#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "openscarlett/gpu/xg_gpu_untile.h"
#include "openscarlett/kernel/vram_residency_manager.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_map>

#include "openscarlett/gpu/d3d12_adapter.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>

#if defined(_MSC_VER)
#define OS_UUIDOF(T) __uuidof(T)
#elif defined(__CRT_UUID_DECL)
#define OS_UUIDOF(T) __mingw_uuidof<T>()
#else
#define OS_UUIDOF(T) IID_##T
#endif
#endif

namespace openscarlett {

namespace {

constexpr uint32_t kUntileGroupSize = 64u;
constexpr uint32_t kSpvHeaderWords = 64u;  // 256 bytes reserved at start of SPIR-V input SSBO

constexpr uint32_t AlignUpU32(uint32_t v, uint32_t a) noexcept {
    return (a <= 1u) ? v : ((v + a - 1u) & ~(a - 1u));
}

constexpr uint64_t AlignUpU64(uint64_t v, uint64_t a) noexcept {
    return (a <= 1ull) ? v : ((v + a - 1ull) & ~(a - 1ull));
}

constexpr uint32_t Log2Pow2(uint32_t v) noexcept {
    uint32_t r = 0u;
    while (v > 1u) {
        v >>= 1u;
        ++r;
    }
    return r;
}

// Mirrors TileGeometry in src/gpu/xg_layout.cpp bit-for-bit.
struct LocalTileGeom {
    uint32_t tile_size_bytes = 256u;
    uint32_t tile_w = 1u;
    uint32_t tile_h = 1u;
    uint32_t tile_d = 1u;
    uint32_t micro_w = 1u;
    uint32_t micro_h = 1u;
    uint32_t micro_size_bytes = 64u;
    uint32_t family = 0u;  // 0=Linear, 1=Std, 2=Disp, 3=DepthZ, 4=Rot
    bool is_3d = false;
    bool has_xor = false;
};

bool QueryLocalTileGeom(XG_TILE_MODE mode, uint32_t bpe, LocalTileGeom& out) noexcept {
    out = LocalTileGeom{};
    if (bpe != 1u && bpe != 2u && bpe != 4u && bpe != 8u && bpe != 16u) return false;
    if (mode == XG_TILE_MODE_LINEAR) {
        out.tile_size_bytes = 256u;
        out.micro_size_bytes = bpe;
        out.family = 0u;
        return true;
    }
    if (mode == XG_TILE_MODE_2D_THIN_256B_S ||
        mode == XG_TILE_MODE_2D_THIN_256B_D ||
        mode == XG_TILE_MODE_2D_THIN_256B_R) {
        out.tile_size_bytes = 256u;
        out.micro_size_bytes = 64u;
        out.family = (mode == XG_TILE_MODE_2D_THIN_256B_S) ? 1u
                     : ((mode == XG_TILE_MODE_2D_THIN_256B_D) ? 2u : 4u);
        switch (bpe) {
            case 1u:  out.tile_w = 16u; out.tile_h = 16u; out.micro_w = 8u; out.micro_h = 8u; return true;
            case 2u:  out.tile_w = 16u; out.tile_h = 8u;  out.micro_w = 8u; out.micro_h = 4u; return true;
            case 4u:  out.tile_w = 8u;  out.tile_h = 8u;  out.micro_w = 4u; out.micro_h = 4u; return true;
            case 8u:  out.tile_w = 8u;  out.tile_h = 4u;  out.micro_w = 4u; out.micro_h = 2u; return true;
            case 16u: out.tile_w = 4u;  out.tile_h = 4u;  out.micro_w = 2u; out.micro_h = 2u; return true;
            default: return false;
        }
    }
    if (mode == XG_TILE_MODE_2D_THIN_4KB_Z ||
        mode == XG_TILE_MODE_2D_THIN_4KB_S ||
        mode == XG_TILE_MODE_2D_THIN_4KB_D ||
        mode == XG_TILE_MODE_2D_THIN_4KB_R) {
        out.tile_size_bytes = 4096u;
        out.micro_size_bytes = 64u;
        switch (mode) {
            case XG_TILE_MODE_2D_THIN_4KB_Z: out.family = 3u; break;
            case XG_TILE_MODE_2D_THIN_4KB_S: out.family = 1u; break;
            case XG_TILE_MODE_2D_THIN_4KB_D: out.family = 2u; break;
            case XG_TILE_MODE_2D_THIN_4KB_R: out.family = 4u; break;
            default: return false;
        }
        switch (bpe) {
            case 1u:  out.tile_w = 64u; out.tile_h = 64u; out.micro_w = 8u; out.micro_h = 8u; return true;
            case 2u:  out.tile_w = 64u; out.tile_h = 32u; out.micro_w = 8u; out.micro_h = 4u; return true;
            case 4u:  out.tile_w = 32u; out.tile_h = 32u; out.micro_w = 4u; out.micro_h = 4u; return true;
            case 8u:  out.tile_w = 32u; out.tile_h = 16u; out.micro_w = 4u; out.micro_h = 2u; return true;
            case 16u: out.tile_w = 16u; out.tile_h = 16u; out.micro_w = 2u; out.micro_h = 2u; return true;
            default: return false;
        }
    }
    if (mode == XG_TILE_MODE_2D_THIN_64KB_Z ||
        mode == XG_TILE_MODE_2D_THIN_64KB_S ||
        mode == XG_TILE_MODE_2D_THIN_64KB_D ||
        mode == XG_TILE_MODE_2D_THIN_64KB_R ||
        mode == XG_TILE_MODE_2D_THIN_64KB_Z_X ||
        mode == XG_TILE_MODE_2D_THIN_64KB_S_X ||
        mode == XG_TILE_MODE_2D_THIN_64KB_D_X ||
        mode == XG_TILE_MODE_2D_THIN_64KB_R_X) {
        out.tile_size_bytes = 65536u;
        out.micro_size_bytes = 256u;
        switch (mode) {
            case XG_TILE_MODE_2D_THIN_64KB_Z:   out.family = 3u; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_S:   out.family = 1u; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_D:   out.family = 2u; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_R:   out.family = 4u; out.has_xor = false; break;
            case XG_TILE_MODE_2D_THIN_64KB_Z_X: out.family = 3u; out.has_xor = true; break;
            case XG_TILE_MODE_2D_THIN_64KB_S_X: out.family = 1u; out.has_xor = true; break;
            case XG_TILE_MODE_2D_THIN_64KB_D_X: out.family = 2u; out.has_xor = true; break;
            case XG_TILE_MODE_2D_THIN_64KB_R_X: out.family = 4u; out.has_xor = true; break;
            default: return false;
        }
        switch (bpe) {
            case 1u:  out.tile_w = 256u; out.tile_h = 256u; out.micro_w = 16u; out.micro_h = 16u; return true;
            case 2u:  out.tile_w = 256u; out.tile_h = 128u; out.micro_w = 16u; out.micro_h = 8u;  return true;
            case 4u:  out.tile_w = 128u; out.tile_h = 128u; out.micro_w = 8u;  out.micro_h = 8u;  return true;
            case 8u:  out.tile_w = 128u; out.tile_h = 64u;  out.micro_w = 8u;  out.micro_h = 4u;  return true;
            case 16u: out.tile_w = 64u;  out.tile_h = 64u;  out.micro_w = 4u;  out.micro_h = 4u;  return true;
            default: return false;
        }
    }
    if (mode == XG_TILE_MODE_3D_THICK_4KB_S) {
        out.tile_size_bytes = 4096u;
        out.family = 1u;
        out.is_3d = true;
        switch (bpe) {
            case 1u:  out.tile_w = 16u; out.tile_h = 16u; out.tile_d = 16u; return true;
            case 2u:  out.tile_w = 16u; out.tile_h = 16u; out.tile_d = 8u;  return true;
            case 4u:  out.tile_w = 16u; out.tile_h = 8u;  out.tile_d = 8u;  return true;
            case 8u:  out.tile_w = 8u;  out.tile_h = 8u;  out.tile_d = 8u;  return true;
            case 16u: out.tile_w = 8u;  out.tile_h = 8u;  out.tile_d = 4u;  return true;
            default: return false;
        }
    }
    if (mode == XG_TILE_MODE_3D_THICK_64KB_Z ||
        mode == XG_TILE_MODE_3D_THICK_64KB_S ||
        mode == XG_TILE_MODE_3D_THICK_64KB_Z_X ||
        mode == XG_TILE_MODE_3D_THICK_64KB_S_X ||
        mode == XG_TILE_MODE_3D_THICK_64KB_R_X) {
        out.tile_size_bytes = 65536u;
        out.is_3d = true;
        switch (mode) {
            case XG_TILE_MODE_3D_THICK_64KB_Z:   out.family = 3u; out.has_xor = false; break;
            case XG_TILE_MODE_3D_THICK_64KB_S:   out.family = 1u; out.has_xor = false; break;
            case XG_TILE_MODE_3D_THICK_64KB_Z_X: out.family = 3u; out.has_xor = true; break;
            case XG_TILE_MODE_3D_THICK_64KB_S_X: out.family = 1u; out.has_xor = true; break;
            case XG_TILE_MODE_3D_THICK_64KB_R_X: out.family = 4u; out.has_xor = true; break;
            default: return false;
        }
        switch (bpe) {
            case 1u:  out.tile_w = 64u; out.tile_h = 32u; out.tile_d = 32u; return true;
            case 2u:  out.tile_w = 32u; out.tile_h = 32u; out.tile_d = 32u; return true;
            case 4u:  out.tile_w = 32u; out.tile_h = 32u; out.tile_d = 16u; return true;
            case 8u:  out.tile_w = 32u; out.tile_h = 16u; out.tile_d = 16u; return true;
            case 16u: out.tile_w = 16u; out.tile_h = 16u; out.tile_d = 16u; return true;
            default: return false;
        }
    }
    return false;
}

#if defined(_WIN32)

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            Reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }

    void Attach(T* p) noexcept {
        Reset();
        ptr_ = p;
    }

    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    T** Put() noexcept {
        Reset();
        return &ptr_;
    }

    void** PutVoid() noexcept {
        Reset();
        return reinterpret_cast<void**>(&ptr_);
    }

private:
    T* ptr_ = nullptr;
};

bool IsSoftwareOrWarpAdapterLocal(const DXGI_ADAPTER_DESC1& desc) noexcept {
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
           (desc.VendorId == 0x1414u &&
            (desc.DeviceId == 0x008Cu || desc.DedicatedVideoMemory == 0));
}

std::string WideToUtf8Local(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
    return out;
}

using PfnD3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PfnD3D12SerializeRootSignature =
    HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

Status CreateD3D12DevicePortable(
    bool prefer_warp,
    ComPtr<ID3D12Device>& out_dev,
    std::string& out_adapter_name,
    uint32_t& out_highest_sm,
    bool& out_is_warp) {
    out_dev.Reset();
    out_adapter_name.clear();
    out_highest_sm = 0x62u;
    out_is_warp = prefer_warp;

    auto create_fn = reinterpret_cast<PfnD3D12CreateDevice>(ResolveHostD3D12Proc("D3D12CreateDevice"));
    if (!create_fn) {
        return Status::Error(StatusCode::DeviceError, "Failed to resolve host d3d12.dll!D3D12CreateDevice");
    }

    ComPtr<IDXGIFactory6> factory6;
    ComPtr<IDXGIFactory4> factory4;
    if (SUCCEEDED(CreateDXGIFactory2(0, OS_UUIDOF(IDXGIFactory6), factory6.PutVoid())) && factory6) {
        factory6->QueryInterface(OS_UUIDOF(IDXGIFactory4), factory4.PutVoid());
    } else if (FAILED(CreateDXGIFactory1(OS_UUIDOF(IDXGIFactory4), factory4.PutVoid())) || !factory4) {
        return Status::Error(StatusCode::DeviceError, "CreateDXGIFactory failed");
    }

    ComPtr<IDXGIAdapter1> chosen;
    if (prefer_warp) {
        if (FAILED(factory4->EnumWarpAdapter(OS_UUIDOF(IDXGIAdapter1), chosen.PutVoid())) || !chosen) {
            return Status::Error(StatusCode::DeviceError, "EnumWarpAdapter failed");
        }
        HRESULT hr = create_fn(chosen.Get(), D3D_FEATURE_LEVEL_12_0, OS_UUIDOF(ID3D12Device), out_dev.PutVoid());
        if (FAILED(hr) || !out_dev) {
            hr = create_fn(chosen.Get(), D3D_FEATURE_LEVEL_11_0, OS_UUIDOF(ID3D12Device), out_dev.PutVoid());
        }
        if (FAILED(hr) || !out_dev) {
            return Status::Error(StatusCode::DeviceError, "D3D12CreateDevice(WARP) failed");
        }
        out_is_warp = true;
    } else {
        for (UINT idx = 0;; ++idx) {
            ComPtr<IDXGIAdapter1> cand;
            HRESULT ehr = factory6
                              ? factory6->EnumAdapterByGpuPreference(
                                    idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                    OS_UUIDOF(IDXGIAdapter1), cand.PutVoid())
                              : factory4->EnumAdapters1(idx, cand.Put());
            if (ehr == DXGI_ERROR_NOT_FOUND || FAILED(ehr) || !cand) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(cand->GetDesc1(&desc))) continue;
            if (IsSoftwareOrWarpAdapterLocal(desc)) continue;

            if (SUCCEEDED(create_fn(cand.Get(), D3D_FEATURE_LEVEL_12_0, OS_UUIDOF(ID3D12Device), out_dev.PutVoid())) &&
                out_dev) {
                chosen = std::move(cand);
                out_adapter_name = WideToUtf8Local(desc.Description);
                out_is_warp = false;
                break;
            }
        }
        if (!out_dev) {
            return Status::Error(StatusCode::DeviceError, "No hardware D3D12 GPU adapter available");
        }
    }

    D3D12AdapterInfo info{};
    if (QueryD3D12DeviceAdapterInfo(out_dev.Get(), chosen.Get(), info).ok()) {
        if (!info.description.empty()) out_adapter_name = info.description;
        out_highest_sm = info.highest_shader_model;
        out_is_warp = info.is_warp || prefer_warp;
    }
    return Status::Ok();
}

Status CreateBufferResource(
    ID3D12Device* device,
    D3D12_HEAP_TYPE heap_type,
    uint64_t size_bytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ComPtr<ID3D12Resource>& out_res) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap_type;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Alignment = 0;
    rd.Width = std::max<uint64_t>(size_bytes, 256ull);
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;

    HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, initial_state, nullptr,
        OS_UUIDOF(ID3D12Resource), out_res.PutVoid());
    if (FAILED(hr) || !out_res) {
        return Status::Error(StatusCode::DeviceError, "CreateCommittedResource(buffer) failed");
    }
    return Status::Ok();
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// Compact Structured SPIR-V 1.3 Compute Shader Builder for GFX10 Untile/Retile
// ---------------------------------------------------------------------------
class SpvUntileBuilder {
public:
    enum Op : uint16_t {
        OpMemoryModel = 14,
        OpEntryPoint = 15,
        OpExecutionMode = 16,
        OpCapability = 17,
        OpTypeVoid = 19,
        OpTypeBool = 20,
        OpTypeInt = 21,
        OpTypeVector = 23,
        OpTypeRuntimeArray = 29,
        OpTypeStruct = 30,
        OpTypePointer = 32,
        OpTypeFunction = 33,
        OpConstant = 43,
        OpFunction = 54,
        OpFunctionParameter = 55,
        OpFunctionEnd = 56,
        OpFunctionCall = 57,
        OpVariable = 59,
        OpLoad = 61,
        OpStore = 62,
        OpAccessChain = 65,
        OpDecorate = 71,
        OpMemberDecorate = 72,
        OpCompositeExtract = 81,
        OpIAdd = 128,
        OpISub = 130,
        OpIMul = 132,
        OpUDiv = 134,
        OpUMod = 137,
        OpLogicalOr = 166,
        OpLogicalAnd = 167,
        OpSelect = 169,
        OpIEqual = 170,
        OpINotEqual = 171,
        OpUGreaterThanEqual = 174,
        OpULessThan = 176,
        OpShiftRightLogical = 194,
        OpShiftLeftLogical = 196,
        OpBitwiseOr = 197,
        OpBitwiseXor = 198,
        OpBitwiseAnd = 199,
        OpNot = 200,
        OpAtomicCompareExchange = 230,
        OpLoopMerge = 246,
        OpSelectionMerge = 247,
        OpLabel = 248,
        OpBranch = 249,
        OpBranchConditional = 250,
        OpReturn = 253,
        OpReturnValue = 254,
    };

    uint32_t NewId() { return next_id_++; }

    static void Emit(std::vector<uint32_t>& dst, uint16_t op, std::initializer_list<uint32_t> ops) {
        const uint32_t wc = static_cast<uint32_t>(ops.size() + 1u);
        dst.push_back((wc << 16) | static_cast<uint32_t>(op));
        dst.insert(dst.end(), ops.begin(), ops.end());
    }

    static void EmitVec(std::vector<uint32_t>& dst, uint16_t op, const std::vector<uint32_t>& ops) {
        const uint32_t wc = static_cast<uint32_t>(ops.size() + 1u);
        dst.push_back((wc << 16) | static_cast<uint32_t>(op));
        dst.insert(dst.end(), ops.begin(), ops.end());
    }

    uint32_t ConstU32(uint32_t v) {
        auto it = u32_consts_.find(v);
        if (it != u32_consts_.end()) return it->second;
        const uint32_t id = NewId();
        Emit(types_consts_, OpConstant, {ty_u32_, id, v});
        u32_consts_[v] = id;
        return id;
    }

    uint32_t Bin(std::vector<uint32_t>& b, uint16_t op, uint32_t ty, uint32_t a, uint32_t c) {
        const uint32_t r = NewId();
        Emit(b, op, {ty, r, a, c});
        return r;
    }

    uint32_t Un(std::vector<uint32_t>& b, uint16_t op, uint32_t ty, uint32_t a) {
        const uint32_t r = NewId();
        Emit(b, op, {ty, r, a});
        return r;
    }

    uint32_t Sel(std::vector<uint32_t>& b, uint32_t cond, uint32_t t_val, uint32_t f_val) {
        const uint32_t r = NewId();
        Emit(b, OpSelect, {ty_u32_, r, cond, t_val, f_val});
        return r;
    }

    uint32_t LoadIn(std::vector<uint32_t>& b, uint32_t idx) {
        const uint32_t ptr = NewId();
        Emit(b, OpAccessChain, {ty_ptr_sb_u32_, ptr, var_in_, ConstU32(0u), idx});
        const uint32_t val = NewId();
        Emit(b, OpLoad, {ty_u32_, val, ptr});
        return val;
    }

    void StoreOut(std::vector<uint32_t>& b, uint32_t idx, uint32_t val) {
        const uint32_t ptr = NewId();
        Emit(b, OpAccessChain, {ty_ptr_sb_u32_, ptr, var_out_, ConstU32(0u), idx});
        Emit(b, OpStore, {ptr, val});
    }

    uint32_t LoadVar(std::vector<uint32_t>& b, uint32_t var_ptr) {
        const uint32_t val = NewId();
        Emit(b, OpLoad, {ty_u32_, val, var_ptr});
        return val;
    }

    void StoreVar(std::vector<uint32_t>& b, uint32_t var_ptr, uint32_t val) {
        Emit(b, OpStore, {var_ptr, val});
    }

    uint32_t FuncVar(std::vector<uint32_t>& b) {
        const uint32_t v = NewId();
        Emit(b, OpVariable, {ty_ptr_fn_u32_, v, 7u});  // StorageClass Function = 7
        return v;
    }

    // Helper: uint Log2Pow2(uint v)
    uint32_t BuildLog2Func() {
        const uint32_t ty_fn = NewId();
        Emit(types_consts_, OpTypeFunction, {ty_fn, ty_u32_, ty_u32_});
        const uint32_t fn = NewId();
        Emit(funcs_, OpFunction, {ty_u32_, fn, 0u, ty_fn});
        const uint32_t p_v = NewId();
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_v});
        const uint32_t l_entry = NewId(), l_hdr = NewId(), l_body = NewId(), l_cont = NewId(), l_exit = NewId();
        Emit(funcs_, OpLabel, {l_entry});
        const uint32_t v_cur = FuncVar(funcs_);
        const uint32_t v_res = FuncVar(funcs_);
        StoreVar(funcs_, v_cur, p_v);
        StoreVar(funcs_, v_res, ConstU32(0u));
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_hdr});
        const uint32_t cur = LoadVar(funcs_, v_cur);
        const uint32_t gt1 = Bin(funcs_, OpUGreaterThanEqual, ty_bool_, cur, ConstU32(2u));
        Emit(funcs_, OpLoopMerge, {l_exit, l_cont, 0u});
        Emit(funcs_, OpBranchConditional, {gt1, l_body, l_exit});

        Emit(funcs_, OpLabel, {l_body});
        StoreVar(funcs_, v_cur, Bin(funcs_, OpShiftRightLogical, ty_u32_, cur, ConstU32(1u)));
        const uint32_t r = LoadVar(funcs_, v_res);
        StoreVar(funcs_, v_res, Bin(funcs_, OpIAdd, ty_u32_, r, ConstU32(1u)));
        Emit(funcs_, OpBranch, {l_cont});

        Emit(funcs_, OpLabel, {l_cont});
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_exit});
        Emit(funcs_, OpReturnValue, {LoadVar(funcs_, v_res)});
        Emit(funcs_, OpFunctionEnd, {});
        return fn;
    }

    // Helper: uint Morton2D(uint x, uint y, uint bx, uint by, uint y_first)
    uint32_t BuildMorton2DFunc() {
        const uint32_t ty_fn = NewId();
        Emit(types_consts_, OpTypeFunction, {ty_fn, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_});
        const uint32_t fn = NewId();
        Emit(funcs_, OpFunction, {ty_u32_, fn, 0u, ty_fn});
        const uint32_t px = NewId(), py = NewId(), pbx = NewId(), pby = NewId(), pyf = NewId();
        Emit(funcs_, OpFunctionParameter, {ty_u32_, px});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, py});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pbx});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pby});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pyf});

        const uint32_t l_entry = NewId(), l_hdr = NewId(), l_body = NewId(), l_cont = NewId(), l_exit = NewId();
        Emit(funcs_, OpLabel, {l_entry});
        const uint32_t v_res = FuncVar(funcs_);
        const uint32_t v_bit = FuncVar(funcs_);
        const uint32_t v_i = FuncVar(funcs_);
        StoreVar(funcs_, v_res, ConstU32(0u));
        StoreVar(funcs_, v_bit, ConstU32(0u));
        StoreVar(funcs_, v_i, ConstU32(0u));
        const uint32_t max_b = Sel(funcs_, Bin(funcs_, OpUGreaterThanEqual, ty_bool_, pbx, pby), pbx, pby);
        const uint32_t is_yf = Bin(funcs_, OpINotEqual, ty_bool_, pyf, ConstU32(0u));
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_hdr});
        const uint32_t i = LoadVar(funcs_, v_i);
        const uint32_t cond = Bin(funcs_, OpULessThan, ty_bool_, i, max_b);
        Emit(funcs_, OpLoopMerge, {l_exit, l_cont, 0u});
        Emit(funcs_, OpBranchConditional, {cond, l_body, l_exit});

        Emit(funcs_, OpLabel, {l_body});
        // Order: if (!y_first): x then y; else y then x.
        // We can emit bit0 = y_first ? ((y>>i)&1) : ((x>>i)&1), has0 = y_first ? (i<by) : (i<bx)
        //              bit1 = y_first ? ((x>>i)&1) : ((y>>i)&1), has1 = y_first ? (i<bx) : (i<by)
        const uint32_t hx = Bin(funcs_, OpULessThan, ty_bool_, i, pbx);
        const uint32_t hy = Bin(funcs_, OpULessThan, ty_bool_, i, pby);
        const uint32_t bx_val = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                    Bin(funcs_, OpShiftRightLogical, ty_u32_, px, i), ConstU32(1u));
        const uint32_t by_val = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                    Bin(funcs_, OpShiftRightLogical, ty_u32_, py, i), ConstU32(1u));

        const uint32_t has0 = Bin(funcs_, OpINotEqual, ty_bool_,
                                  Sel(funcs_, is_yf, Sel(funcs_, hy, ConstU32(1u), ConstU32(0u)),
                                      Sel(funcs_, hx, ConstU32(1u), ConstU32(0u))),
                                  ConstU32(0u));
        const uint32_t bit0 = Sel(funcs_, is_yf, by_val, bx_val);
        const uint32_t has1 = Bin(funcs_, OpINotEqual, ty_bool_,
                                  Sel(funcs_, is_yf, Sel(funcs_, hx, ConstU32(1u), ConstU32(0u)),
                                      Sel(funcs_, hy, ConstU32(1u), ConstU32(0u))),
                                  ConstU32(0u));
        const uint32_t bit1 = Sel(funcs_, is_yf, bx_val, by_val);

        uint32_t res = LoadVar(funcs_, v_res);
        uint32_t ob = LoadVar(funcs_, v_bit);
        const uint32_t res_a = Bin(funcs_, OpBitwiseOr, ty_u32_, res,
                                   Bin(funcs_, OpShiftLeftLogical, ty_u32_, bit0, ob));
        res = Sel(funcs_, has0, res_a, res);
        ob = Sel(funcs_, has0, Bin(funcs_, OpIAdd, ty_u32_, ob, ConstU32(1u)), ob);

        const uint32_t res_b = Bin(funcs_, OpBitwiseOr, ty_u32_, res,
                                   Bin(funcs_, OpShiftLeftLogical, ty_u32_, bit1, ob));
        res = Sel(funcs_, has1, res_b, res);
        ob = Sel(funcs_, has1, Bin(funcs_, OpIAdd, ty_u32_, ob, ConstU32(1u)), ob);

        StoreVar(funcs_, v_res, res);
        StoreVar(funcs_, v_bit, ob);
        Emit(funcs_, OpBranch, {l_cont});

        Emit(funcs_, OpLabel, {l_cont});
        StoreVar(funcs_, v_i, Bin(funcs_, OpIAdd, ty_u32_, i, ConstU32(1u)));
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_exit});
        Emit(funcs_, OpReturnValue, {LoadVar(funcs_, v_res)});
        Emit(funcs_, OpFunctionEnd, {});
        return fn;
    }

    // Helper: uint Morton3D(uint x, uint y, uint z, uint bx, uint by, uint bz, uint order)
    uint32_t BuildMorton3DFunc() {
        const uint32_t ty_fn = NewId();
        Emit(types_consts_, OpTypeFunction,
             {ty_fn, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_});
        const uint32_t fn = NewId();
        Emit(funcs_, OpFunction, {ty_u32_, fn, 0u, ty_fn});
        const uint32_t px = NewId(), py = NewId(), pz = NewId();
        const uint32_t pbx = NewId(), pby = NewId(), pbz = NewId(), pord = NewId();
        Emit(funcs_, OpFunctionParameter, {ty_u32_, px});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, py});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pz});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pbx});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pby});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pbz});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, pord});

        const uint32_t l_entry = NewId(), l_hdr = NewId(), l_body = NewId(), l_cont = NewId(), l_exit = NewId();
        Emit(funcs_, OpLabel, {l_entry});
        const uint32_t v_res = FuncVar(funcs_);
        const uint32_t v_bit = FuncVar(funcs_);
        const uint32_t v_i = FuncVar(funcs_);
        StoreVar(funcs_, v_res, ConstU32(0u));
        StoreVar(funcs_, v_bit, ConstU32(0u));
        StoreVar(funcs_, v_i, ConstU32(0u));
        const uint32_t mxy = Sel(funcs_, Bin(funcs_, OpUGreaterThanEqual, ty_bool_, pbx, pby), pbx, pby);
        const uint32_t max_b = Sel(funcs_, Bin(funcs_, OpUGreaterThanEqual, ty_bool_, mxy, pbz), mxy, pbz);
        const uint32_t is_o0 = Bin(funcs_, OpIEqual, ty_bool_, pord, ConstU32(0u));
        const uint32_t is_o1 = Bin(funcs_, OpIEqual, ty_bool_, pord, ConstU32(1u));
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_hdr});
        const uint32_t i = LoadVar(funcs_, v_i);
        const uint32_t cond = Bin(funcs_, OpULessThan, ty_bool_, i, max_b);
        Emit(funcs_, OpLoopMerge, {l_exit, l_cont, 0u});
        Emit(funcs_, OpBranchConditional, {cond, l_body, l_exit});

        Emit(funcs_, OpLabel, {l_body});
        const uint32_t hx = Sel(funcs_, Bin(funcs_, OpULessThan, ty_bool_, i, pbx), ConstU32(1u), ConstU32(0u));
        const uint32_t hy = Sel(funcs_, Bin(funcs_, OpULessThan, ty_bool_, i, pby), ConstU32(1u), ConstU32(0u));
        const uint32_t hz = Sel(funcs_, Bin(funcs_, OpULessThan, ty_bool_, i, pbz), ConstU32(1u), ConstU32(0u));
        const uint32_t bx_v = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, px, i), ConstU32(1u));
        const uint32_t by_v = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, py, i), ConstU32(1u));
        const uint32_t bz_v = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, pz, i), ConstU32(1u));

        // order 0: (x,y,z), order 1: (y,x,z), order 2: (z,x,y)
        const uint32_t h0 = Sel(funcs_, is_o0, hx, Sel(funcs_, is_o1, hy, hz));
        const uint32_t b0 = Sel(funcs_, is_o0, bx_v, Sel(funcs_, is_o1, by_v, bz_v));
        const uint32_t h1 = Sel(funcs_, is_o0, hy, hx);
        const uint32_t b1 = Sel(funcs_, is_o0, by_v, bx_v);
        const uint32_t h2 = Sel(funcs_, is_o0, hz, Sel(funcs_, is_o1, hz, hy));
        const uint32_t b2 = Sel(funcs_, is_o0, bz_v, Sel(funcs_, is_o1, bz_v, by_v));

        uint32_t res = LoadVar(funcs_, v_res);
        uint32_t ob = LoadVar(funcs_, v_bit);
        auto append_bit = [&](uint32_t h_u32, uint32_t b_u32) {
            const uint32_t h_b = Bin(funcs_, OpINotEqual, ty_bool_, h_u32, ConstU32(0u));
            const uint32_t nr = Bin(funcs_, OpBitwiseOr, ty_u32_, res,
                                    Bin(funcs_, OpShiftLeftLogical, ty_u32_, b_u32, ob));
            res = Sel(funcs_, h_b, nr, res);
            ob = Sel(funcs_, h_b, Bin(funcs_, OpIAdd, ty_u32_, ob, ConstU32(1u)), ob);
        };
        append_bit(h0, b0);
        append_bit(h1, b1);
        append_bit(h2, b2);

        StoreVar(funcs_, v_res, res);
        StoreVar(funcs_, v_bit, ob);
        Emit(funcs_, OpBranch, {l_cont});

        Emit(funcs_, OpLabel, {l_cont});
        StoreVar(funcs_, v_i, Bin(funcs_, OpIAdd, ty_u32_, i, ConstU32(1u)));
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_exit});
        Emit(funcs_, OpReturnValue, {LoadVar(funcs_, v_res)});
        Emit(funcs_, OpFunctionEnd, {});
        return fn;
    }

    // Helper: void AtomicWriteSubWord(uint byte_off, uint val, uint mask)
    uint32_t BuildAtomicSubWordFunc() {
        const uint32_t ty_fn = NewId();
        Emit(types_consts_, OpTypeFunction, {ty_fn, ty_void_, ty_u32_, ty_u32_, ty_u32_});
        const uint32_t fn = NewId();
        Emit(funcs_, OpFunction, {ty_void_, fn, 0u, ty_fn});
        const uint32_t p_boff = NewId(), p_val = NewId(), p_mask = NewId();
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_boff});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_val});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_mask});

        const uint32_t l_entry = NewId(), l_hdr = NewId(), l_body = NewId(), l_cont = NewId(), l_exit = NewId();
        Emit(funcs_, OpLabel, {l_entry});
        const uint32_t v_exp = FuncVar(funcs_);
        const uint32_t word_idx = Bin(funcs_, OpShiftRightLogical, ty_u32_, p_boff, ConstU32(2u));
        const uint32_t shift = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                   Bin(funcs_, OpBitwiseAnd, ty_u32_, p_boff, ConstU32(3u)), ConstU32(3u));
        const uint32_t smask = Bin(funcs_, OpShiftLeftLogical, ty_u32_, p_mask, shift);
        const uint32_t inv_smask = Un(funcs_, OpNot, ty_u32_, smask);
        const uint32_t ins = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                 Bin(funcs_, OpBitwiseAnd, ty_u32_, p_val, p_mask), shift);
        const uint32_t ptr = NewId();
        Emit(funcs_, OpAccessChain, {ty_ptr_sb_u32_, ptr, var_out_, ConstU32(0u), word_idx});
        const uint32_t init_val = NewId();
        Emit(funcs_, OpLoad, {ty_u32_, init_val, ptr});
        StoreVar(funcs_, v_exp, init_val);
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_hdr});
        Emit(funcs_, OpLoopMerge, {l_exit, l_cont, 0u});
        Emit(funcs_, OpBranch, {l_body});

        Emit(funcs_, OpLabel, {l_body});
        const uint32_t exp = LoadVar(funcs_, v_exp);
        const uint32_t desired = Bin(funcs_, OpBitwiseOr, ty_u32_,
                                     Bin(funcs_, OpBitwiseAnd, ty_u32_, exp, inv_smask), ins);
        const uint32_t prev = NewId();
        // OpAtomicCompareExchange: ResultType, ResultId, Pointer, Scope(Device=1), Equal(0), Unequal(0), Value, Comparator
        Emit(funcs_, OpAtomicCompareExchange,
             {ty_u32_, prev, ptr, ConstU32(1u), ConstU32(0u), ConstU32(0u), desired, exp});
        const uint32_t done = Bin(funcs_, OpIEqual, ty_bool_, prev, exp);
        StoreVar(funcs_, v_exp, prev);
        Emit(funcs_, OpBranchConditional, {done, l_exit, l_cont});

        Emit(funcs_, OpLabel, {l_cont});
        Emit(funcs_, OpBranch, {l_hdr});

        Emit(funcs_, OpLabel, {l_exit});
        Emit(funcs_, OpReturn, {});
        Emit(funcs_, OpFunctionEnd, {});
        return fn;
    }

    std::vector<uint32_t> Build() {
        // Basic types
        ty_void_ = NewId();
        ty_bool_ = NewId();
        ty_u32_ = NewId();
        ty_v3u32_ = NewId();
        ty_rt_u32_ = NewId();
        ty_buf_struct_ = NewId();
        ty_ptr_sb_struct_ = NewId();
        ty_ptr_sb_u32_ = NewId();
        ty_ptr_in_v3u32_ = NewId();
        ty_ptr_in_u32_ = NewId();
        ty_ptr_fn_u32_ = NewId();
        ty_main_fn_ = NewId();

        Emit(types_consts_, OpTypeVoid, {ty_void_});
        Emit(types_consts_, OpTypeBool, {ty_bool_});
        Emit(types_consts_, OpTypeInt, {ty_u32_, 32u, 0u});
        Emit(types_consts_, OpTypeVector, {ty_v3u32_, ty_u32_, 3u});
        Emit(types_consts_, OpTypeRuntimeArray, {ty_rt_u32_, ty_u32_});
        Emit(types_consts_, OpTypeStruct, {ty_buf_struct_, ty_rt_u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_sb_struct_, 12u, ty_buf_struct_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_sb_u32_, 12u, ty_u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_in_v3u32_, 1u, ty_v3u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_in_u32_, 1u, ty_u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_fn_u32_, 7u, ty_u32_});
        Emit(types_consts_, OpTypeFunction, {ty_main_fn_, ty_void_});

        // Global variables (StorageBuffer = 12, Input = 1)
        var_in_ = NewId();
        var_out_ = NewId();
        var_gid_ = NewId();
        Emit(globals_, OpVariable, {ty_ptr_sb_struct_, var_in_, 12u});
        Emit(globals_, OpVariable, {ty_ptr_sb_struct_, var_out_, 12u});
        Emit(globals_, OpVariable, {ty_ptr_in_v3u32_, var_gid_, 1u});

        // Decorations
        Emit(decs_, OpDecorate, {ty_rt_u32_, 6u, 4u});          // ArrayStride 4
        Emit(decs_, OpDecorate, {ty_buf_struct_, 2u});          // Block
        Emit(decs_, OpMemberDecorate, {ty_buf_struct_, 0u, 35u, 0u});  // Offset 0
        Emit(decs_, OpDecorate, {var_in_, 34u, 0u});            // DescriptorSet 0
        Emit(decs_, OpDecorate, {var_in_, 33u, 0u});            // Binding 0
        Emit(decs_, OpDecorate, {var_in_, 24u});                // NonWritable
        Emit(decs_, OpDecorate, {var_out_, 34u, 0u});           // DescriptorSet 0
        Emit(decs_, OpDecorate, {var_out_, 33u, 1u});           // Binding 1
        Emit(decs_, OpDecorate, {var_gid_, 11u, 28u});          // BuiltIn GlobalInvocationId

        const uint32_t fn_log2 = BuildLog2Func();
        const uint32_t fn_m2d = BuildMorton2DFunc();
        const uint32_t fn_m3d = BuildMorton3DFunc();
        const uint32_t fn_asub = BuildAtomicSubWordFunc();

        // Main entry point
        const uint32_t fn_main = NewId();
        Emit(funcs_, OpFunction, {ty_void_, fn_main, 0u, ty_main_fn_});
        const uint32_t l_entry = NewId();
        Emit(funcs_, OpLabel, {l_entry});

        // Local variables (must be at start of entry block!)
        const uint32_t v_tiled_off = FuncVar(funcs_);
        const uint32_t v_use_clear = FuncVar(funcs_);
        const uint32_t v_d0 = FuncVar(funcs_);
        const uint32_t v_d1 = FuncVar(funcs_);
        const uint32_t v_d2 = FuncVar(funcs_);
        const uint32_t v_d3 = FuncVar(funcs_);

        // Load GlobalInvocationId.x
        const uint32_t gid_ptr = NewId();
        Emit(funcs_, OpAccessChain, {ty_ptr_in_u32_, gid_ptr, var_gid_, ConstU32(0u)});
        const uint32_t tid = NewId();
        Emit(funcs_, OpLoad, {ty_u32_, tid, gid_ptr});

        // Load header parameters from input SSBO words [0..59]
        const uint32_t p_dir = LoadIn(funcs_, ConstU32(0u));
        const uint32_t p_mode = LoadIn(funcs_, ConstU32(1u));
        const uint32_t p_is3d = LoadIn(funcs_, ConstU32(2u));
        const uint32_t p_bpe = LoadIn(funcs_, ConstU32(3u));
        const uint32_t p_w = LoadIn(funcs_, ConstU32(4u));
        const uint32_t p_h = LoadIn(funcs_, ConstU32(5u));
        const uint32_t p_d = LoadIn(funcs_, ConstU32(6u));
        const uint32_t p_lin_rp = LoadIn(funcs_, ConstU32(7u));
        const uint32_t p_lin_sp = LoadIn(funcs_, ConstU32(8u));
        const uint32_t p_tbase = LoadIn(funcs_, ConstU32(9u));
        const uint32_t p_in_mt = LoadIn(funcs_, ConstU32(10u));
        const uint32_t p_mip = LoadIn(funcs_, ConstU32(11u));
        const uint32_t p_slice = LoadIn(funcs_, ConstU32(12u));
        const uint32_t p_tx = LoadIn(funcs_, ConstU32(13u));
        const uint32_t p_ty = LoadIn(funcs_, ConstU32(14u));
        const uint32_t p_tw = LoadIn(funcs_, ConstU32(15u));
        const uint32_t p_th = LoadIn(funcs_, ConstU32(16u));
        const uint32_t p_td = LoadIn(funcs_, ConstU32(17u));
        const uint32_t p_tsz = LoadIn(funcs_, ConstU32(18u));
        const uint32_t p_mw = LoadIn(funcs_, ConstU32(19u));
        const uint32_t p_mh = LoadIn(funcs_, ConstU32(20u));
        const uint32_t p_msz = LoadIn(funcs_, ConstU32(21u));
        const uint32_t p_fam = LoadIn(funcs_, ConstU32(22u));
        const uint32_t p_xor = LoadIn(funcs_, ConstU32(23u));
        const uint32_t p_mtbx = LoadIn(funcs_, ConstU32(24u));
        const uint32_t p_mtby = LoadIn(funcs_, ConstU32(25u));
        const uint32_t p_mtbz = LoadIn(funcs_, ConstU32(26u));
        const uint32_t p_mtsb = LoadIn(funcs_, ConstU32(27u));
        const uint32_t p_trp = LoadIn(funcs_, ConstU32(28u));
        const uint32_t p_tdp = LoadIn(funcs_, ConstU32(29u));
        const uint32_t p_mmode = LoadIn(funcs_, ConstU32(30u));
        const uint32_t p_mbx = LoadIn(funcs_, ConstU32(31u));
        const uint32_t p_mpipe = LoadIn(funcs_, ConstU32(32u));
        const uint32_t p_mnpipe = LoadIn(funcs_, ConstU32(33u));
        const uint32_t p_msp = LoadIn(funcs_, ConstU32(34u));
        const uint32_t p_mbase = LoadIn(funcs_, ConstU32(35u));
        const uint32_t p_src_wo = LoadIn(funcs_, ConstU32(56u));
        const uint32_t p_meta_wo = LoadIn(funcs_, ConstU32(57u));

        const uint32_t wh = Bin(funcs_, OpIMul, ty_u32_, p_w, p_h);
        const uint32_t total = Bin(funcs_, OpIMul, ty_u32_, wh, p_d);
        const uint32_t in_bounds = Bin(funcs_, OpULessThan, ty_bool_, tid, total);
        const uint32_t l_work = NewId(), l_ret = NewId();
        Emit(funcs_, OpSelectionMerge, {l_ret, 0u});
        Emit(funcs_, OpBranchConditional, {in_bounds, l_work, l_ret});

        Emit(funcs_, OpLabel, {l_work});
        // Decode (x, y, z) from tid
        const uint32_t z = Bin(funcs_, OpUDiv, ty_u32_, tid, wh);
        const uint32_t rem = Bin(funcs_, OpUMod, ty_u32_, tid, wh);
        const uint32_t y = Bin(funcs_, OpUDiv, ty_u32_, rem, p_w);
        const uint32_t x = Bin(funcs_, OpUMod, ty_u32_, rem, p_w);

        // Linear byte offset
        const uint32_t lin_off = Bin(
            funcs_, OpIAdd, ty_u32_,
            Bin(funcs_, OpIAdd, ty_u32_,
                Bin(funcs_, OpIMul, ty_u32_, z, p_lin_sp),
                Bin(funcs_, OpIMul, ty_u32_, y, p_lin_rp)),
            Bin(funcs_, OpIMul, ty_u32_, x, p_bpe));

        // Compute tiled byte offset (branch: Linear vs MipTail vs FullTile)
        StoreVar(funcs_, v_tiled_off, ConstU32(0u));
        const uint32_t is_lin = Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(0u));
        const uint32_t l_lin = NewId(), l_notlin = NewId(), l_addr_done = NewId();
        Emit(funcs_, OpSelectionMerge, {l_addr_done, 0u});
        Emit(funcs_, OpBranchConditional, {is_lin, l_lin, l_notlin});

        // Case 1: XG_TILE_MODE_LINEAR
        Emit(funcs_, OpLabel, {l_lin});
        {
            const uint32_t zterm = Bin(funcs_, OpIMul, ty_u32_, z, p_tdp);
            const uint32_t yterm = Bin(funcs_, OpIMul, ty_u32_, y, p_trp);
            const uint32_t xterm = Bin(funcs_, OpIMul, ty_u32_, x, p_bpe);
            const uint32_t off = Bin(funcs_, OpIAdd, ty_u32_, p_tbase,
                                     Bin(funcs_, OpIAdd, ty_u32_, zterm,
                                         Bin(funcs_, OpIAdd, ty_u32_, yterm, xterm)));
            StoreVar(funcs_, v_tiled_off, off);
        }
        Emit(funcs_, OpBranch, {l_addr_done});

        // Case 2: Tiled (MipTail or FullTile)
        Emit(funcs_, OpLabel, {l_notlin});
        {
            const uint32_t is_mt = Bin(funcs_, OpINotEqual, ty_bool_, p_in_mt, ConstU32(0u));
            const uint32_t l_mt = NewId(), l_full = NewId(), l_tmerge = NewId();
            Emit(funcs_, OpSelectionMerge, {l_tmerge, 0u});
            Emit(funcs_, OpBranchConditional, {is_mt, l_mt, l_full});

            // 2A: MipTail (2D or 3D)
            Emit(funcs_, OpLabel, {l_mt});
            {
                const uint32_t c3d = Bin(funcs_, OpINotEqual, ty_bool_, p_is3d, ConstU32(0u));
                const uint32_t l_mt3 = NewId(), l_mt2 = NewId(), l_mtm = NewId();
                Emit(funcs_, OpSelectionMerge, {l_mtm, 0u});
                Emit(funcs_, OpBranchConditional, {c3d, l_mt3, l_mt2});

                Emit(funcs_, OpLabel, {l_mt3});
                {
                    const uint32_t is_rot = Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(4u));
                    const uint32_t is_dz = Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(3u));
                    const uint32_t ord = Sel(funcs_, is_rot, ConstU32(2u),
                                             Sel(funcs_, is_dz, ConstU32(1u), ConstU32(0u)));
                    const uint32_t elem = NewId();
                    Emit(funcs_, OpFunctionCall, {ty_u32_, elem, fn_m3d, x, y, z, p_mtbx, p_mtby, p_mtbz, ord});
                    uint32_t soff = Bin(funcs_, OpIMul, ty_u32_, elem, p_bpe);
                    const uint32_t do_x = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                              Bin(funcs_, OpINotEqual, ty_bool_, p_xor, ConstU32(0u)),
                                              Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_mtsb, ConstU32(256u)));
                    const uint32_t sxor = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                              Bin(funcs_, OpBitwiseAnd, ty_u32_, p_mip, ConstU32(0xFu)), ConstU32(4u));
                    soff = Sel(funcs_, do_x, Bin(funcs_, OpBitwiseXor, ty_u32_, soff, sxor), soff);
                    StoreVar(funcs_, v_tiled_off, Bin(funcs_, OpIAdd, ty_u32_, p_tbase, soff));
                }
                Emit(funcs_, OpBranch, {l_mtm});

                Emit(funcs_, OpLabel, {l_mt2});
                {
                    const uint32_t yf_b = Bin(funcs_, OpLogicalOr, ty_bool_,
                                              Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(2u)),
                                              Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(4u)));
                    const uint32_t yf = Sel(funcs_, yf_b, ConstU32(1u), ConstU32(0u));
                    const uint32_t elem = NewId();
                    Emit(funcs_, OpFunctionCall, {ty_u32_, elem, fn_m2d, x, y, p_mtbx, p_mtby, yf});
                    uint32_t soff = Bin(funcs_, OpIMul, ty_u32_, elem, p_bpe);
                    const uint32_t do_x = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                              Bin(funcs_, OpINotEqual, ty_bool_, p_xor, ConstU32(0u)),
                                              Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_mtsb, ConstU32(256u)));
                    const uint32_t sl1 = Bin(funcs_, OpShiftLeftLogical, ty_u32_, p_slice, ConstU32(1u));
                    const uint32_t sxor = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                              Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                                  Bin(funcs_, OpBitwiseXor, ty_u32_, p_mip, sl1), ConstU32(0xFu)),
                                              ConstU32(4u));
                    soff = Sel(funcs_, do_x, Bin(funcs_, OpBitwiseXor, ty_u32_, soff, sxor), soff);
                    StoreVar(funcs_, v_tiled_off, Bin(funcs_, OpIAdd, ty_u32_, p_tbase, soff));
                }
                Emit(funcs_, OpBranch, {l_mtm});

                Emit(funcs_, OpLabel, {l_mtm});
            }
            Emit(funcs_, OpBranch, {l_tmerge});

            // 2B: Full Tile (2D or 3D)
            Emit(funcs_, OpLabel, {l_full});
            {
                const uint32_t c3d = Bin(funcs_, OpINotEqual, ty_bool_, p_is3d, ConstU32(0u));
                const uint32_t l_f3 = NewId(), l_f2 = NewId(), l_fm = NewId();
                Emit(funcs_, OpSelectionMerge, {l_fm, 0u});
                Emit(funcs_, OpBranchConditional, {c3d, l_f3, l_f2});

                // Full 3D
                Emit(funcs_, OpLabel, {l_f3});
                {
                    const uint32_t tx = Bin(funcs_, OpUDiv, ty_u32_, x, p_tw);
                    const uint32_t ty = Bin(funcs_, OpUDiv, ty_u32_, y, p_th);
                    const uint32_t tz = Bin(funcs_, OpUDiv, ty_u32_, z, p_td);
                    const uint32_t ix = Bin(funcs_, OpUMod, ty_u32_, x, p_tw);
                    const uint32_t iy = Bin(funcs_, OpUMod, ty_u32_, y, p_th);
                    const uint32_t iz = Bin(funcs_, OpUMod, ty_u32_, z, p_td);
                    const uint32_t tidx = Bin(
                        funcs_, OpIAdd, ty_u32_,
                        Bin(funcs_, OpIMul, ty_u32_,
                            Bin(funcs_, OpIAdd, ty_u32_, Bin(funcs_, OpIMul, ty_u32_, tz, p_ty), ty), p_tx),
                        tx);
                    const uint32_t bx = NewId(), by = NewId(), bz = NewId();
                    Emit(funcs_, OpFunctionCall, {ty_u32_, bx, fn_log2, p_tw});
                    Emit(funcs_, OpFunctionCall, {ty_u32_, by, fn_log2, p_th});
                    Emit(funcs_, OpFunctionCall, {ty_u32_, bz, fn_log2, p_td});

                    // DepthZ vs Rotated vs Standard
                    const uint32_t vox = Bin(
                        funcs_, OpBitwiseOr, ty_u32_,
                        Bin(funcs_, OpBitwiseOr, ty_u32_,
                            Bin(funcs_, OpBitwiseAnd, ty_u32_, ix, ConstU32(1u)),
                            Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                Bin(funcs_, OpBitwiseAnd, ty_u32_, iy, ConstU32(1u)), ConstU32(1u))),
                        Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                            Bin(funcs_, OpBitwiseAnd, ty_u32_, iz, ConstU32(1u)), ConstU32(2u)));
                    const uint32_t bx1 = Bin(funcs_, OpISub, ty_u32_, bx, ConstU32(1u));
                    const uint32_t by1 = Bin(funcs_, OpISub, ty_u32_, by, ConstU32(1u));
                    const uint32_t bz1 = Bin(funcs_, OpISub, ty_u32_, bz, ConstU32(1u));
                    const uint32_t outer_dz = NewId();
                    Emit(funcs_, OpFunctionCall,
                         {ty_u32_, outer_dz, fn_m3d,
                          Bin(funcs_, OpShiftRightLogical, ty_u32_, ix, ConstU32(1u)),
                          Bin(funcs_, OpShiftRightLogical, ty_u32_, iy, ConstU32(1u)),
                          Bin(funcs_, OpShiftRightLogical, ty_u32_, iz, ConstU32(1u)),
                          bx1, by1, bz1, ConstU32(1u)});
                    const uint32_t e_dz = Bin(funcs_, OpBitwiseOr, ty_u32_,
                                              Bin(funcs_, OpShiftLeftLogical, ty_u32_, outer_dz, ConstU32(3u)), vox);
                    const uint32_t is_rot = Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(4u));
                    const uint32_t ord_sr = Sel(funcs_, is_rot, ConstU32(2u), ConstU32(0u));
                    const uint32_t e_sr = NewId();
                    Emit(funcs_, OpFunctionCall, {ty_u32_, e_sr, fn_m3d, ix, iy, iz, bx, by, bz, ord_sr});
                    const uint32_t is_dz = Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(3u));
                    const uint32_t elem = Sel(funcs_, is_dz, e_dz, e_sr);
                    uint32_t ioff = Bin(funcs_, OpIMul, ty_u32_, elem, p_bpe);

                    // 3D XOR nibble
                    const uint32_t x_sx = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpBitwiseXor, ty_u32_, tx,
                                Bin(funcs_, OpShiftLeftLogical, ty_u32_, ty, ConstU32(1u))),
                            Bin(funcs_, OpShiftLeftLogical, ty_u32_, tz, ConstU32(2u))),
                        ConstU32(0xFu));
                    const uint32_t x_zx = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpBitwiseXor, ty_u32_,
                                Bin(funcs_, OpShiftLeftLogical, ty_u32_, tx, ConstU32(1u)), ty),
                            Bin(funcs_, OpIMul, ty_u32_, tz, ConstU32(3u))),
                        ConstU32(0xFu));
                    const uint32_t x_rx = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpBitwiseXor, ty_u32_, tz,
                                Bin(funcs_, OpShiftLeftLogical, ty_u32_, tx, ConstU32(1u))),
                            Bin(funcs_, OpShiftLeftLogical, ty_u32_, ty, ConstU32(2u))),
                        ConstU32(0xFu));
                    const uint32_t xn = Sel(
                        funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(20u)), x_sx,
                        Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(19u)), x_zx,
                            Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(21u)), x_rx, ConstU32(0u))));
                    const uint32_t do_x = Bin(funcs_, OpINotEqual, ty_bool_, p_xor, ConstU32(0u));
                    ioff = Sel(funcs_, do_x,
                               Bin(funcs_, OpBitwiseXor, ty_u32_, ioff,
                                   Bin(funcs_, OpShiftLeftLogical, ty_u32_, xn, ConstU32(12u))),
                               ioff);
                    StoreVar(funcs_, v_tiled_off,
                             Bin(funcs_, OpIAdd, ty_u32_, p_tbase,
                                 Bin(funcs_, OpIAdd, ty_u32_,
                                     Bin(funcs_, OpIMul, ty_u32_, tidx, p_tsz), ioff)));
                }
                Emit(funcs_, OpBranch, {l_fm});

                // Full 2D
                Emit(funcs_, OpLabel, {l_f2});
                {
                    const uint32_t tx = Bin(funcs_, OpUDiv, ty_u32_, x, p_tw);
                    const uint32_t ty = Bin(funcs_, OpUDiv, ty_u32_, y, p_th);
                    const uint32_t ix = Bin(funcs_, OpUMod, ty_u32_, x, p_tw);
                    const uint32_t iy = Bin(funcs_, OpUMod, ty_u32_, y, p_th);
                    const uint32_t tidx = Bin(funcs_, OpIAdd, ty_u32_, Bin(funcs_, OpIMul, ty_u32_, ty, p_tx), tx);
                    const uint32_t bx = NewId(), by = NewId();
                    Emit(funcs_, OpFunctionCall, {ty_u32_, bx, fn_log2, p_tw});
                    Emit(funcs_, OpFunctionCall, {ty_u32_, by, fn_log2, p_th});

                    // Branch Displayable(2) vs DepthZ(3) vs TwoLevel(Std/Rot)
                    const uint32_t v_ioff = v_d0;  // Temp reuse before payload phase
                    StoreVar(funcs_, v_ioff, ConstU32(0u));
                    const uint32_t is_disp = Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(2u));
                    const uint32_t l_disp = NewId(), l_ndisp = NewId(), l_i2m = NewId();
                    Emit(funcs_, OpSelectionMerge, {l_i2m, 0u});
                    Emit(funcs_, OpBranchConditional, {is_disp, l_disp, l_ndisp});

                    Emit(funcs_, OpLabel, {l_disp});
                    {
                        const uint32_t em = NewId();
                        Emit(funcs_, OpFunctionCall, {ty_u32_, em, fn_m2d, ix, iy, bx, by, ConstU32(1u)});
                        StoreVar(funcs_, v_ioff, Bin(funcs_, OpIMul, ty_u32_, em, p_bpe));
                    }
                    Emit(funcs_, OpBranch, {l_i2m});

                    Emit(funcs_, OpLabel, {l_ndisp});
                    {
                        const uint32_t is_dz = Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(3u));
                        const uint32_t l_dz = NewId(), l_tl = NewId(), l_i2n = NewId();
                        Emit(funcs_, OpSelectionMerge, {l_i2n, 0u});
                        Emit(funcs_, OpBranchConditional, {is_dz, l_dz, l_tl});

                        Emit(funcs_, OpLabel, {l_dz});
                        {
                            const uint32_t qe = Bin(
                                funcs_, OpBitwiseOr, ty_u32_,
                                Bin(funcs_, OpBitwiseAnd, ty_u32_, ix, ConstU32(1u)),
                                Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                    Bin(funcs_, OpBitwiseAnd, ty_u32_, iy, ConstU32(1u)), ConstU32(1u)));
                            const uint32_t ux = Bin(funcs_, OpShiftRightLogical, ty_u32_, ix, ConstU32(1u));
                            const uint32_t uy = Bin(funcs_, OpShiftRightLogical, ty_u32_, iy, ConstU32(1u));
                            const uint32_t bux = Bin(funcs_, OpISub, ty_u32_, bx, ConstU32(1u));
                            const uint32_t buy = Bin(funcs_, OpISub, ty_u32_, by, ConstU32(1u));
                            const uint32_t mux = Bin(funcs_, OpISub, ty_u32_,
                                                     Bin(funcs_, OpShiftLeftLogical, ty_u32_, ConstU32(1u), bux),
                                                     ConstU32(1u));
                            const uint32_t uxs = Bin(funcs_, OpBitwiseXor, ty_u32_, ux,
                                                     Bin(funcs_, OpBitwiseAnd, ty_u32_, uy, mux));
                            const uint32_t qidx = NewId();
                            Emit(funcs_, OpFunctionCall, {ty_u32_, qidx, fn_m2d, uxs, uy, bux, buy, ConstU32(1u)});
                            const uint32_t elem = Bin(funcs_, OpBitwiseOr, ty_u32_,
                                                      Bin(funcs_, OpShiftLeftLogical, ty_u32_, qidx, ConstU32(2u)), qe);
                            StoreVar(funcs_, v_ioff, Bin(funcs_, OpIMul, ty_u32_, elem, p_bpe));
                        }
                        Emit(funcs_, OpBranch, {l_i2n});

                        Emit(funcs_, OpLabel, {l_tl});
                        {
                            const uint32_t mx = Bin(funcs_, OpUDiv, ty_u32_, ix, p_mw);
                            const uint32_t my = Bin(funcs_, OpUDiv, ty_u32_, iy, p_mh);
                            const uint32_t lx = Bin(funcs_, OpUMod, ty_u32_, ix, p_mw);
                            const uint32_t ly = Bin(funcs_, OpUMod, ty_u32_, iy, p_mh);
                            const uint32_t mgw = Bin(funcs_, OpUDiv, ty_u32_, p_tw, p_mw);
                            const uint32_t mgh = Bin(funcs_, OpUDiv, ty_u32_, p_th, p_mh);
                            const uint32_t bmgw = NewId(), bmgh = NewId(), bmw = NewId(), bmh = NewId();
                            Emit(funcs_, OpFunctionCall, {ty_u32_, bmgw, fn_log2, mgw});
                            Emit(funcs_, OpFunctionCall, {ty_u32_, bmgh, fn_log2, mgh});
                            Emit(funcs_, OpFunctionCall, {ty_u32_, bmw, fn_log2, p_mw});
                            Emit(funcs_, OpFunctionCall, {ty_u32_, bmh, fn_log2, p_mh});
                            const uint32_t rot_no_rx = Bin(
                                funcs_, OpLogicalAnd, ty_bool_,
                                Bin(funcs_, OpIEqual, ty_bool_, p_fam, ConstU32(4u)),
                                Bin(funcs_, OpINotEqual, ty_bool_, p_mode, ConstU32(4u)));
                            const uint32_t myf = Sel(funcs_, rot_no_rx, ConstU32(1u), ConstU32(0u));
                            const uint32_t midx = NewId(), emic = NewId();
                            Emit(funcs_, OpFunctionCall, {ty_u32_, midx, fn_m2d, mx, my, bmgw, bmgh, myf});
                            Emit(funcs_, OpFunctionCall, {ty_u32_, emic, fn_m2d, lx, ly, bmw, bmh, ConstU32(0u)});
                            StoreVar(funcs_, v_ioff,
                                     Bin(funcs_, OpIAdd, ty_u32_,
                                         Bin(funcs_, OpIMul, ty_u32_, midx, p_msz),
                                         Bin(funcs_, OpIMul, ty_u32_, emic, p_bpe)));
                        }
                        Emit(funcs_, OpBranch, {l_i2n});

                        Emit(funcs_, OpLabel, {l_i2n});
                    }
                    Emit(funcs_, OpBranch, {l_i2m});

                    Emit(funcs_, OpLabel, {l_i2m});
                    uint32_t ioff = LoadVar(funcs_, v_ioff);
                    // 2D XOR nibble
                    const uint32_t x_rx = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpBitwiseXor, ty_u32_, tx,
                                Bin(funcs_, OpShiftLeftLogical, ty_u32_, ty, ConstU32(1u))),
                            Bin(funcs_, OpShiftLeftLogical, ty_u32_, p_slice, ConstU32(2u))),
                        ConstU32(0xFu));
                    const uint32_t x_sx = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpBitwiseXor, ty_u32_,
                                Bin(funcs_, OpShiftLeftLogical, ty_u32_, tx, ConstU32(1u)), ty),
                            p_slice),
                        ConstU32(0xFu));
                    const uint32_t x_dx = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpBitwiseXor, ty_u32_, tx, ty),
                            Bin(funcs_, OpShiftLeftLogical, ty_u32_, p_slice, ConstU32(1u))),
                        ConstU32(0xFu));
                    const uint32_t x_zx = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpBitwiseXor, ty_u32_, tx,
                                Bin(funcs_, OpIMul, ty_u32_, ty, ConstU32(3u))),
                            Bin(funcs_, OpShiftLeftLogical, ty_u32_, p_slice, ConstU32(2u))),
                        ConstU32(0xFu));
                    const uint32_t xn = Sel(
                        funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(4u)), x_rx,
                        Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(14u)), x_sx,
                            Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(15u)), x_dx,
                                Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_mode, ConstU32(13u)), x_zx,
                                    ConstU32(0u)))));
                    const uint32_t do_x = Bin(funcs_, OpINotEqual, ty_bool_, p_xor, ConstU32(0u));
                    ioff = Sel(funcs_, do_x,
                               Bin(funcs_, OpBitwiseXor, ty_u32_, ioff,
                                   Bin(funcs_, OpShiftLeftLogical, ty_u32_, xn, ConstU32(12u))),
                               ioff);
                    StoreVar(funcs_, v_tiled_off,
                             Bin(funcs_, OpIAdd, ty_u32_, p_tbase,
                                 Bin(funcs_, OpIAdd, ty_u32_,
                                     Bin(funcs_, OpIMul, ty_u32_, tidx, p_tsz), ioff)));
                }
                Emit(funcs_, OpBranch, {l_fm});

                Emit(funcs_, OpLabel, {l_fm});
            }
            Emit(funcs_, OpBranch, {l_tmerge});

            Emit(funcs_, OpLabel, {l_tmerge});
        }
        Emit(funcs_, OpBranch, {l_addr_done});

        // Address computed! Now check metadata fast-clear (only when direction == 0 && meta_mode != 0)
        Emit(funcs_, OpLabel, {l_addr_done});
        const uint32_t tiled_off = LoadVar(funcs_, v_tiled_off);
        const uint32_t is_retile = Bin(funcs_, OpINotEqual, ty_bool_, p_dir, ConstU32(0u));
        const uint32_t src_off = Sel(funcs_, is_retile, lin_off, tiled_off);
        const uint32_t dst_off = Sel(funcs_, is_retile, tiled_off, lin_off);

        StoreVar(funcs_, v_use_clear, ConstU32(0u));
        StoreVar(funcs_, v_d0, ConstU32(0u));
        StoreVar(funcs_, v_d1, ConstU32(0u));
        StoreVar(funcs_, v_d2, ConstU32(0u));
        StoreVar(funcs_, v_d3, ConstU32(0u));

        const uint32_t check_meta = Bin(
            funcs_, OpLogicalAnd, ty_bool_,
            Bin(funcs_, OpIEqual, ty_bool_, p_dir, ConstU32(0u)),
            Bin(funcs_, OpINotEqual, ty_bool_, p_mmode, ConstU32(0u)));
        const uint32_t l_meta = NewId(), l_meta_done = NewId();
        Emit(funcs_, OpSelectionMerge, {l_meta_done, 0u});
        Emit(funcs_, OpBranchConditional, {check_meta, l_meta, l_meta_done});

        Emit(funcs_, OpLabel, {l_meta});
        {
            auto load_meta_byte = [&](uint32_t byte_off) -> uint32_t {
                const uint32_t abs_b = Bin(funcs_, OpIAdd, ty_u32_, p_mbase, byte_off);
                const uint32_t widx = Bin(funcs_, OpIAdd, ty_u32_, p_meta_wo,
                                          Bin(funcs_, OpShiftRightLogical, ty_u32_, abs_b, ConstU32(2u)));
                const uint32_t w = LoadIn(funcs_, widx);
                const uint32_t sh = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                        Bin(funcs_, OpBitwiseAnd, ty_u32_, abs_b, ConstU32(3u)), ConstU32(3u));
                return Bin(funcs_, OpBitwiseAnd, ty_u32_,
                           Bin(funcs_, OpShiftRightLogical, ty_u32_, w, sh), ConstU32(0xFFu));
            };
            auto load_clear_payload = [&](uint32_t base_word) {
                StoreVar(funcs_, v_d0, LoadIn(funcs_, ConstU32(base_word + 0u)));
                StoreVar(funcs_, v_d1, LoadIn(funcs_, ConstU32(base_word + 1u)));
                StoreVar(funcs_, v_d2, LoadIn(funcs_, ConstU32(base_word + 2u)));
                StoreVar(funcs_, v_d3, LoadIn(funcs_, ConstU32(base_word + 3u)));
                StoreVar(funcs_, v_use_clear, ConstU32(1u));
            };

            const uint32_t is_dcc = Bin(funcs_, OpIEqual, ty_bool_, p_mmode, ConstU32(1u));
            const uint32_t l_dcc = NewId(), l_ndcc = NewId(), l_mm = NewId();
            Emit(funcs_, OpSelectionMerge, {l_mm, 0u});
            Emit(funcs_, OpBranchConditional, {is_dcc, l_dcc, l_ndcc});

            // DCC fast-clear passthrough
            Emit(funcs_, OpLabel, {l_dcc});
            {
                const uint32_t bw = Sel(funcs_, Bin(funcs_, OpULessThan, ty_bool_, p_bpe, ConstU32(4u)),
                                        ConstU32(16u),
                                        Sel(funcs_, Bin(funcs_, OpULessThan, ty_bool_, p_bpe, ConstU32(16u)),
                                            ConstU32(8u), ConstU32(4u)));
                const uint32_t bh = Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_bpe, ConstU32(1u)),
                                        ConstU32(16u),
                                        Sel(funcs_, Bin(funcs_, OpULessThan, ty_bool_, p_bpe, ConstU32(8u)),
                                            ConstU32(8u), ConstU32(4u)));
                const uint32_t bx = Bin(funcs_, OpUDiv, ty_u32_, x, bw);
                const uint32_t by = Bin(funcs_, OpUDiv, ty_u32_, y, bh);
                uint32_t moff = Bin(funcs_, OpIAdd, ty_u32_, Bin(funcs_, OpIMul, ty_u32_, by, p_mbx), bx);
                const uint32_t do_pa = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                           Bin(funcs_, OpINotEqual, ty_bool_, p_mpipe, ConstU32(0u)),
                                           Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_msp, ConstU32(256u)));
                const uint32_t pmask = Sel(funcs_, Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_mnpipe, ConstU32(2u)),
                                           Bin(funcs_, OpISub, ty_u32_, p_mnpipe, ConstU32(1u)), ConstU32(0u));
                const uint32_t xb = Bin(
                    funcs_, OpBitwiseAnd, ty_u32_,
                    Bin(funcs_, OpBitwiseXor, ty_u32_,
                        Bin(funcs_, OpBitwiseXor, ty_u32_,
                            Bin(funcs_, OpShiftRightLogical, ty_u32_, bx, ConstU32(2u)),
                            Bin(funcs_, OpShiftRightLogical, ty_u32_, by, ConstU32(2u))),
                        Bin(funcs_, OpBitwiseXor, ty_u32_, p_mip, p_slice)),
                    pmask);
                const uint32_t cand = Bin(funcs_, OpBitwiseXor, ty_u32_, moff, xb);
                const uint32_t use_c = Bin(funcs_, OpLogicalAnd, ty_bool_, do_pa,
                                           Bin(funcs_, OpULessThan, ty_bool_, cand, p_msp));
                moff = Sel(funcs_, use_c, cand, moff);
                const uint32_t key = load_meta_byte(moff);

                // Select clear color words based on key (0x00, 0x40, 0x80, 0xC0, 0x20)
                const uint32_t k00 = Bin(funcs_, OpIEqual, ty_bool_, key, ConstU32(0x00u));
                const uint32_t k40 = Bin(funcs_, OpIEqual, ty_bool_, key, ConstU32(0x40u));
                const uint32_t k80 = Bin(funcs_, OpIEqual, ty_bool_, key, ConstU32(0x80u));
                const uint32_t kc0 = Bin(funcs_, OpIEqual, ty_bool_, key, ConstU32(0xC0u));
                const uint32_t k20 = Bin(funcs_, OpIEqual, ty_bool_, key, ConstU32(0x20u));
                const uint32_t any_k = Bin(
                    funcs_, OpLogicalOr, ty_bool_,
                    Bin(funcs_, OpLogicalOr, ty_bool_, k00, k40),
                    Bin(funcs_, OpLogicalOr, ty_bool_, Bin(funcs_, OpLogicalOr, ty_bool_, k80, kc0), k20));
                for (uint32_t ci = 0u; ci < 4u; ++ci) {
                    const uint32_t w00 = LoadIn(funcs_, ConstU32(36u + ci));
                    const uint32_t w40 = LoadIn(funcs_, ConstU32(40u + ci));
                    const uint32_t w80 = LoadIn(funcs_, ConstU32(44u + ci));
                    const uint32_t wc0 = LoadIn(funcs_, ConstU32(48u + ci));
                    const uint32_t w20 = LoadIn(funcs_, ConstU32(52u + ci));
                    const uint32_t chosen = Sel(
                        funcs_, k00, w00,
                        Sel(funcs_, k40, w40,
                            Sel(funcs_, k80, w80, Sel(funcs_, kc0, wc0, w20))));
                    StoreVar(funcs_, ci == 0u ? v_d0 : (ci == 1u ? v_d1 : (ci == 2u ? v_d2 : v_d3)), chosen);
                }
                StoreVar(funcs_, v_use_clear, Sel(funcs_, any_k, ConstU32(1u), ConstU32(0u)));
            }
            Emit(funcs_, OpBranch, {l_mm});

            // HTILE (2) or CMASK (3)
            Emit(funcs_, OpLabel, {l_ndcc});
            {
                const uint32_t is_ht = Bin(funcs_, OpIEqual, ty_bool_, p_mmode, ConstU32(2u));
                const uint32_t bx8 = Bin(funcs_, OpShiftRightLogical, ty_u32_, x, ConstU32(3u));
                const uint32_t by8 = Bin(funcs_, OpShiftRightLogical, ty_u32_, y, ConstU32(3u));
                const uint32_t bidx = Bin(funcs_, OpIAdd, ty_u32_, Bin(funcs_, OpIMul, ty_u32_, by8, p_mbx), bx8);

                const uint32_t l_ht = NewId(), l_cm = NewId(), l_hc = NewId();
                Emit(funcs_, OpSelectionMerge, {l_hc, 0u});
                Emit(funcs_, OpBranchConditional, {is_ht, l_ht, l_cm});

                // HTILE: 4B per 8x8 block; zmask == 0 => fast z-clear
                Emit(funcs_, OpLabel, {l_ht});
                {
                    const uint32_t boff = Bin(funcs_, OpIAdd, ty_u32_, p_mbase,
                                              Bin(funcs_, OpShiftLeftLogical, ty_u32_, bidx, ConstU32(2u)));
                    const uint32_t widx = Bin(funcs_, OpIAdd, ty_u32_, p_meta_wo,
                                              Bin(funcs_, OpShiftRightLogical, ty_u32_, boff, ConstU32(2u)));
                    const uint32_t hw = LoadIn(funcs_, widx);
                    const uint32_t zclear = Bin(funcs_, OpIEqual, ty_bool_,
                                                Bin(funcs_, OpBitwiseAnd, ty_u32_, hw, ConstU32(0xFu)), ConstU32(0u));
                    const uint32_t l_hc_t = NewId(), l_hc_f = NewId();
                    Emit(funcs_, OpSelectionMerge, {l_hc_f, 0u});
                    Emit(funcs_, OpBranchConditional, {zclear, l_hc_t, l_hc_f});
                    Emit(funcs_, OpLabel, {l_hc_t});
                    load_clear_payload(52u);  // clear_reg
                    Emit(funcs_, OpBranch, {l_hc_f});
                    Emit(funcs_, OpLabel, {l_hc_f});
                }
                Emit(funcs_, OpBranch, {l_hc});

                // CMASK: 4 bits per 8x8 block; nibble == 0 => fast color clear
                Emit(funcs_, OpLabel, {l_cm});
                {
                    const uint32_t cbyte = load_meta_byte(Bin(funcs_, OpShiftRightLogical, ty_u32_, bidx, ConstU32(1u)));
                    const uint32_t odd = Bin(funcs_, OpINotEqual, ty_bool_,
                                             Bin(funcs_, OpBitwiseAnd, ty_u32_, bidx, ConstU32(1u)), ConstU32(0u));
                    const uint32_t nib = Bin(
                        funcs_, OpBitwiseAnd, ty_u32_,
                        Sel(funcs_, odd, Bin(funcs_, OpShiftRightLogical, ty_u32_, cbyte, ConstU32(4u)), cbyte),
                        ConstU32(0xFu));
                    const uint32_t cclear = Bin(funcs_, OpIEqual, ty_bool_, nib, ConstU32(0u));
                    const uint32_t l_cm_t = NewId(), l_cm_f = NewId();
                    Emit(funcs_, OpSelectionMerge, {l_cm_f, 0u});
                    Emit(funcs_, OpBranchConditional, {cclear, l_cm_t, l_cm_f});
                    Emit(funcs_, OpLabel, {l_cm_t});
                    load_clear_payload(52u);  // clear_reg
                    Emit(funcs_, OpBranch, {l_cm_f});
                    Emit(funcs_, OpLabel, {l_cm_f});
                }
                Emit(funcs_, OpBranch, {l_hc});

                Emit(funcs_, OpLabel, {l_hc});
            }
            Emit(funcs_, OpBranch, {l_mm});

            Emit(funcs_, OpLabel, {l_mm});
        }
        Emit(funcs_, OpBranch, {l_meta_done});

        // If NOT fast-cleared, load element from src_off
        Emit(funcs_, OpLabel, {l_meta_done});
        const uint32_t need_load = Bin(funcs_, OpIEqual, ty_bool_, LoadVar(funcs_, v_use_clear), ConstU32(0u));
        const uint32_t l_ld = NewId(), l_st = NewId();
        Emit(funcs_, OpSelectionMerge, {l_st, 0u});
        Emit(funcs_, OpBranchConditional, {need_load, l_ld, l_st});

        Emit(funcs_, OpLabel, {l_ld});
        {
            const uint32_t sw = Bin(funcs_, OpIAdd, ty_u32_, p_src_wo,
                                    Bin(funcs_, OpShiftRightLogical, ty_u32_, src_off, ConstU32(2u)));
            const uint32_t b_lt4 = Bin(funcs_, OpULessThan, ty_bool_, p_bpe, ConstU32(4u));
            const uint32_t l_ls = NewId(), l_lw = NewId(), l_lm = NewId();
            Emit(funcs_, OpSelectionMerge, {l_lm, 0u});
            Emit(funcs_, OpBranchConditional, {b_lt4, l_ls, l_lw});

            Emit(funcs_, OpLabel, {l_ls});
            {
                const uint32_t w = LoadIn(funcs_, sw);
                const uint32_t sh = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                        Bin(funcs_, OpBitwiseAnd, ty_u32_, src_off, ConstU32(3u)), ConstU32(3u));
                const uint32_t m = Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_bpe, ConstU32(1u)),
                                       ConstU32(0xFFu), ConstU32(0xFFFFu));
                StoreVar(funcs_, v_d0,
                         Bin(funcs_, OpBitwiseAnd, ty_u32_, Bin(funcs_, OpShiftRightLogical, ty_u32_, w, sh), m));
            }
            Emit(funcs_, OpBranch, {l_lm});

            Emit(funcs_, OpLabel, {l_lw});
            {
                StoreVar(funcs_, v_d0, LoadIn(funcs_, sw));
                const uint32_t ge8 = Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_bpe, ConstU32(8u));
                const uint32_t l_l8 = NewId(), l_l8m = NewId();
                Emit(funcs_, OpSelectionMerge, {l_l8m, 0u});
                Emit(funcs_, OpBranchConditional, {ge8, l_l8, l_l8m});
                Emit(funcs_, OpLabel, {l_l8});
                StoreVar(funcs_, v_d1, LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, sw, ConstU32(1u))));
                const uint32_t ge16 = Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_bpe, ConstU32(16u));
                const uint32_t l_l16 = NewId(), l_l16m = NewId();
                Emit(funcs_, OpSelectionMerge, {l_l16m, 0u});
                Emit(funcs_, OpBranchConditional, {ge16, l_l16, l_l16m});
                Emit(funcs_, OpLabel, {l_l16});
                StoreVar(funcs_, v_d2, LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, sw, ConstU32(2u))));
                StoreVar(funcs_, v_d3, LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, sw, ConstU32(3u))));
                Emit(funcs_, OpBranch, {l_l16m});
                Emit(funcs_, OpLabel, {l_l16m});
                Emit(funcs_, OpBranch, {l_l8m});
                Emit(funcs_, OpLabel, {l_l8m});
            }
            Emit(funcs_, OpBranch, {l_lm});

            Emit(funcs_, OpLabel, {l_lm});
        }
        Emit(funcs_, OpBranch, {l_st});

        // Store element to dst_off
        Emit(funcs_, OpLabel, {l_st});
        {
            const uint32_t dw = Bin(funcs_, OpShiftRightLogical, ty_u32_, dst_off, ConstU32(2u));
            const uint32_t b_lt4 = Bin(funcs_, OpULessThan, ty_bool_, p_bpe, ConstU32(4u));
            const uint32_t l_ss = NewId(), l_sw = NewId(), l_sm = NewId();
            Emit(funcs_, OpSelectionMerge, {l_sm, 0u});
            Emit(funcs_, OpBranchConditional, {b_lt4, l_ss, l_sw});

            Emit(funcs_, OpLabel, {l_ss});
            {
                const uint32_t m = Sel(funcs_, Bin(funcs_, OpIEqual, ty_bool_, p_bpe, ConstU32(1u)),
                                       ConstU32(0xFFu), ConstU32(0xFFFFu));
                const uint32_t call_r = NewId();
                Emit(funcs_, OpFunctionCall, {ty_void_, call_r, fn_asub, dst_off, LoadVar(funcs_, v_d0), m});
            }
            Emit(funcs_, OpBranch, {l_sm});

            Emit(funcs_, OpLabel, {l_sw});
            {
                StoreOut(funcs_, dw, LoadVar(funcs_, v_d0));
                const uint32_t ge8 = Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_bpe, ConstU32(8u));
                const uint32_t l_s8 = NewId(), l_s8m = NewId();
                Emit(funcs_, OpSelectionMerge, {l_s8m, 0u});
                Emit(funcs_, OpBranchConditional, {ge8, l_s8, l_s8m});
                Emit(funcs_, OpLabel, {l_s8});
                StoreOut(funcs_, Bin(funcs_, OpIAdd, ty_u32_, dw, ConstU32(1u)), LoadVar(funcs_, v_d1));
                const uint32_t ge16 = Bin(funcs_, OpUGreaterThanEqual, ty_bool_, p_bpe, ConstU32(16u));
                const uint32_t l_s16 = NewId(), l_s16m = NewId();
                Emit(funcs_, OpSelectionMerge, {l_s16m, 0u});
                Emit(funcs_, OpBranchConditional, {ge16, l_s16, l_s16m});
                Emit(funcs_, OpLabel, {l_s16});
                StoreOut(funcs_, Bin(funcs_, OpIAdd, ty_u32_, dw, ConstU32(2u)), LoadVar(funcs_, v_d2));
                StoreOut(funcs_, Bin(funcs_, OpIAdd, ty_u32_, dw, ConstU32(3u)), LoadVar(funcs_, v_d3));
                Emit(funcs_, OpBranch, {l_s16m});
                Emit(funcs_, OpLabel, {l_s16m});
                Emit(funcs_, OpBranch, {l_s8m});
                Emit(funcs_, OpLabel, {l_s8m});
            }
            Emit(funcs_, OpBranch, {l_sm});

            Emit(funcs_, OpLabel, {l_sm});
        }
        Emit(funcs_, OpBranch, {l_ret});

        Emit(funcs_, OpLabel, {l_ret});
        Emit(funcs_, OpReturn, {});
        Emit(funcs_, OpFunctionEnd, {});

        // Assemble final SPIR-V 1.3 binary
        std::vector<uint32_t> spv;
        spv.push_back(0x07230203u);  // Magic
        spv.push_back(0x00010300u);  // SPIR-V 1.3
        spv.push_back(0x4F533200u);  // Generator "OS2\0"
        spv.push_back(next_id_);     // Bound
        spv.push_back(0u);           // Schema

        Emit(spv, OpCapability, {1u});          // Shader
        Emit(spv, OpMemoryModel, {0u, 1u});     // Logical, GLSL450
        // OpEntryPoint GLCompute(5) %fn_main "main" %var_gid
        Emit(spv, OpEntryPoint, {5u, fn_main, 0x6E69616Du, 0u, var_gid_});
        Emit(spv, OpExecutionMode, {fn_main, 17u, kUntileGroupSize, 1u, 1u});  // LocalSize

        spv.insert(spv.end(), decs_.begin(), decs_.end());
        spv.insert(spv.end(), types_consts_.begin(), types_consts_.end());
        spv.insert(spv.end(), globals_.begin(), globals_.end());
        spv.insert(spv.end(), funcs_.begin(), funcs_.end());
        return spv;
    }

private:
    uint32_t next_id_ = 1u;
    uint32_t ty_void_ = 0u, ty_bool_ = 0u, ty_u32_ = 0u, ty_v3u32_ = 0u;
    uint32_t ty_rt_u32_ = 0u, ty_buf_struct_ = 0u;
    uint32_t ty_ptr_sb_struct_ = 0u, ty_ptr_sb_u32_ = 0u;
    uint32_t ty_ptr_in_v3u32_ = 0u, ty_ptr_in_u32_ = 0u, ty_ptr_fn_u32_ = 0u, ty_main_fn_ = 0u;
    uint32_t var_in_ = 0u, var_out_ = 0u, var_gid_ = 0u;
    std::unordered_map<uint32_t, uint32_t> u32_consts_;
    std::vector<uint32_t> decs_, types_consts_, globals_, funcs_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Public Helper Functions
// ---------------------------------------------------------------------------

void PopulateFormatFastClearPayloads(
    uint32_t dxgi_format,
    uint32_t bpe,
    uint32_t out_0000[4],
    uint32_t out_0001[4],
    uint32_t out_1110[4],
    uint32_t out_1111[4]) noexcept {
    std::memset(out_0000, 0, sizeof(uint32_t) * 4u);
    std::memset(out_0001, 0, sizeof(uint32_t) * 4u);
    std::memset(out_1110, 0, sizeof(uint32_t) * 4u);
    std::memset(out_1111, 0, sizeof(uint32_t) * 4u);

#if defined(_WIN32)
    switch (static_cast<DXGI_FORMAT>(dxgi_format)) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            out_0001[0] = 0x00000000u; out_0001[1] = 0x3C000000u;
            out_1110[0] = 0x3C003C00u; out_1110[1] = 0x00003C00u;
            out_1111[0] = 0x3C003C00u; out_1111[1] = 0x3C003C00u;
            return;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            out_0001[3] = 0x3F800000u;
            out_1110[0] = 0x3F800000u; out_1110[1] = 0x3F800000u; out_1110[2] = 0x3F800000u;
            out_1111[0] = 0x3F800000u; out_1111[1] = 0x3F800000u;
            out_1111[2] = 0x3F800000u; out_1111[3] = 0x3F800000u;
            return;
        case DXGI_FORMAT_R16_FLOAT:
            out_0001[0] = 0x0000u; out_1110[0] = 0x3C00u; out_1111[0] = 0x3C00u;
            return;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:
            out_0001[0] = 0x00000000u; out_1110[0] = 0x3F800000u; out_1111[0] = 0x3F800000u;
            return;
        default:
            break;
    }
#else
    (void)dxgi_format;
#endif

    // Default UNORM/integer encoding by bytes-per-element
    switch (bpe) {
        case 1u:
            out_0001[0] = 0x00u; out_1110[0] = 0xFFu; out_1111[0] = 0xFFu;
            break;
        case 2u:
            out_0001[0] = 0x0000u; out_1110[0] = 0xFFFFu; out_1111[0] = 0xFFFFu;
            break;
        case 4u:
            out_0001[0] = 0xFF000000u; out_1110[0] = 0x00FFFFFFu; out_1111[0] = 0xFFFFFFFFu;
            break;
        case 8u:
            out_0001[0] = 0x00000000u; out_0001[1] = 0xFFFF0000u;
            out_1110[0] = 0xFFFFFFFFu; out_1110[1] = 0x0000FFFFu;
            out_1111[0] = 0xFFFFFFFFu; out_1111[1] = 0xFFFFFFFFu;
            break;
        case 16u:
        default:
            out_0001[3] = 0xFFFFFFFFu;
            out_1110[0] = 0xFFFFFFFFu; out_1110[1] = 0xFFFFFFFFu; out_1110[2] = 0xFFFFFFFFu;
            out_1111[0] = 0xFFFFFFFFu; out_1111[1] = 0xFFFFFFFFu;
            out_1111[2] = 0xFFFFFFFFu; out_1111[3] = 0xFFFFFFFFu;
            break;
    }
}

Status BuildXgGpuUntileParams2D(
    const XgGpuSurface2DRequest& req,
    XgGpuUntileParams& out_params,
    uint64_t* out_linear_bytes,
    uint64_t* out_tiled_bytes) {
    out_params = XgGpuUntileParams{};
    XG_RESOURCE_LAYOUT layout{};
    if (!xg::ComputeTexture2DLayout(req.desc, layout)) {
        return Status::Error(StatusCode::InvalidArgument, "ComputeTexture2DLayout failed");
    }
    if (req.plane >= layout.PlaneCount || req.mip_level >= layout.MipLevels ||
        req.slice_index >= layout.ArraySize) {
        return Status::Error(StatusCode::InvalidArgument, "Subresource plane/mip/slice out of range");
    }

    const XG_MIPLEVEL_LAYOUT& ml = (req.plane == 0u) ? layout.Mips[req.mip_level] : layout.Plane1Mips[req.mip_level];
    const uint32_t bpe = (req.plane == 0u) ? layout.FormatInfo.bytes_per_block
                                           : layout.PlaneFormatInfo[1].bytes_per_block;

    LocalTileGeom geom{};
    if (!QueryLocalTileGeom(layout.TileMode, bpe, geom) || geom.is_3d) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported 2D tile mode or BPE");
    }

    const uint32_t min_row = ml.WidthElements * bpe;
    const uint32_t lin_row_pitch = (req.linear_row_pitch_bytes == 0u) ? min_row : req.linear_row_pitch_bytes;
    if (lin_row_pitch < min_row) {
        return Status::Error(StatusCode::InvalidArgument, "linear_row_pitch_bytes smaller than row width");
    }

    uint64_t tiled_base = 0ull;
    if (ml.InMipTail != 0u) {
        const uint64_t tail_base = (req.plane == 0u) ? layout.MipTailOffsetBytes : layout.Plane1MipTailOffsetBytes;
        const uint64_t tail_pitch =
            (req.plane == 0u) ? layout.MipTailSlicePitchBytes : layout.Plane1MipTailSlicePitchBytes;
        tiled_base = tail_base + static_cast<uint64_t>(req.slice_index) * tail_pitch + ml.MipTailSubTileOffsetBytes;
    } else {
        tiled_base = ml.OffsetBytes + static_cast<uint64_t>(req.slice_index) * ml.SlicePitchBytes;
    }

    if (tiled_base > UINT32_MAX || layout.SizeBytes > UINT32_MAX) {
        return Status::Error(StatusCode::InvalidArgument, "Surface offset exceeds 32-bit GPU buffer bound");
    }

    out_params.direction = static_cast<uint32_t>(req.direction);
    out_params.tile_mode = static_cast<uint32_t>(layout.TileMode);
    out_params.is_3d = 0u;
    out_params.bpe = bpe;
    out_params.width_elems = ml.WidthElements;
    out_params.height_elems = ml.HeightElements;
    out_params.depth_elems = 1u;
    out_params.linear_row_pitch = lin_row_pitch;
    out_params.linear_slice_pitch = lin_row_pitch * ml.HeightElements;
    out_params.tiled_base_offset = static_cast<uint32_t>(tiled_base);
    out_params.in_mip_tail = ml.InMipTail;
    out_params.mip_level = req.mip_level;
    out_params.slice_index = req.slice_index;
    out_params.tiles_x = ml.TilesX;
    out_params.tiles_y = ml.TilesY;
    out_params.tile_w_elems = geom.tile_w;
    out_params.tile_h_elems = geom.tile_h;
    out_params.tile_d_elems = 1u;
    out_params.tile_size_bytes = geom.tile_size_bytes;
    out_params.micro_w_elems = geom.micro_w;
    out_params.micro_h_elems = geom.micro_h;
    out_params.micro_size_bytes = geom.micro_size_bytes;
    out_params.swizzle_family = geom.family;
    out_params.has_xor = geom.has_xor ? 1u : 0u;
    out_params.mip_tail_bits_x = (ml.InMipTail != 0u) ? Log2Pow2(ml.MipTailPaddedWidthElements) : 0u;
    out_params.mip_tail_bits_y = (ml.InMipTail != 0u) ? Log2Pow2(ml.MipTailPaddedHeightElements) : 0u;
    out_params.mip_tail_bits_z = 0u;
    out_params.mip_tail_sub_block_bytes = ml.MipTailSubBlockBytes;
    out_params.tiled_row_pitch = ml.RowPitchBytes;
    out_params.tiled_depth_pitch = 0u;

    out_params.meta_mode =
        (req.direction == XgGpuDirection::Untile) ? static_cast<uint32_t>(req.metadata_mode) : 0u;
    if (out_params.meta_mode != 0u) {
        XG_RESOURCE_METADATA_LAYOUT meta_layout{};
        xg::ComputeTexture2DMetadataLayout(req.desc, req.console_type, meta_layout);
        XG_SCARLETT_PIPE_CONFIG pipe_cfg{};
        xg::QueryScarlettPipeConfig(req.console_type, pipe_cfg);
        out_params.meta_num_pipes = pipe_cfg.NumPipes;
        out_params.meta_pipe_aligned = req.dcc_pipe_aligned;

        if (req.metadata_mode == XgGpuMetadataMode::Dcc) {
            uint32_t blk_w = 8u;
            switch (bpe) {
                case 1u:  blk_w = 16u; break;
                case 2u:  blk_w = 16u; break;
                case 4u:  blk_w = 8u;  break;
                case 8u:  blk_w = 8u;  break;
                case 16u: blk_w = 4u;  break;
                default:  blk_w = 8u;  break;
            }
            out_params.meta_blocks_x = std::max(1u, (ml.WidthElements + blk_w - 1u) / blk_w);
            out_params.meta_slice_pitch = static_cast<uint32_t>(meta_layout.Dcc.SlicePitchBytes);
            out_params.meta_base_offset = static_cast<uint32_t>(req.slice_index * meta_layout.Dcc.SlicePitchBytes);
        } else if (req.metadata_mode == XgGpuMetadataMode::Htile) {
            out_params.meta_blocks_x = std::max(1u, (ml.WidthElements + 7u) / 8u);
            out_params.meta_slice_pitch = static_cast<uint32_t>(meta_layout.Htile.SlicePitchBytes);
            out_params.meta_base_offset = static_cast<uint32_t>(req.slice_index * meta_layout.Htile.SlicePitchBytes);
        } else if (req.metadata_mode == XgGpuMetadataMode::Cmask) {
            out_params.meta_blocks_x = std::max(1u, (ml.WidthElements + 7u) / 8u);
            out_params.meta_slice_pitch = static_cast<uint32_t>(meta_layout.Cmask.SlicePitchBytes);
            out_params.meta_base_offset = static_cast<uint32_t>(req.slice_index * meta_layout.Cmask.SlicePitchBytes);
        }
    }

    PopulateFormatFastClearPayloads(
        req.desc.Format, bpe,
        out_params.clear_0000, out_params.clear_0001, out_params.clear_1110, out_params.clear_1111);
    for (uint32_t i = 0u; i < 4u; ++i) {
        out_params.clear_reg[i] = req.clear_reg_value[i];
    }

    if (out_linear_bytes) {
        *out_linear_bytes = static_cast<uint64_t>(lin_row_pitch) * ml.HeightElements;
    }
    if (out_tiled_bytes) {
        *out_tiled_bytes = layout.SizeBytes;
    }
    return Status::Ok();
}

Status BuildXgGpuUntileParams3D(
    const XgGpuSurface3DRequest& req,
    XgGpuUntileParams& out_params,
    uint64_t* out_linear_bytes,
    uint64_t* out_tiled_bytes) {
    out_params = XgGpuUntileParams{};
    XG_RESOURCE_LAYOUT layout{};
    if (!xg::ComputeTexture3DLayout(req.desc, layout)) {
        return Status::Error(StatusCode::InvalidArgument, "ComputeTexture3DLayout failed");
    }
    if (req.mip_level >= layout.MipLevels) {
        return Status::Error(StatusCode::InvalidArgument, "3D mip_level out of range");
    }

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[req.mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;

    LocalTileGeom geom{};
    if (!QueryLocalTileGeom(layout.TileMode, bpe, geom)) {
        return Status::Error(StatusCode::UnsupportedFormat, "Unsupported 3D tile mode or BPE");
    }
    if (layout.TileMode != XG_TILE_MODE_LINEAR && !geom.is_3d) {
        return Status::Error(StatusCode::InvalidArgument, "2D tile mode supplied for 3D texture");
    }

    const uint32_t min_row = ml.WidthElements * bpe;
    const uint32_t lin_row_pitch = (req.linear_row_pitch_bytes == 0u) ? min_row : req.linear_row_pitch_bytes;
    if (lin_row_pitch < min_row) {
        return Status::Error(StatusCode::InvalidArgument, "3D linear_row_pitch_bytes too small");
    }
    const uint32_t min_slice = lin_row_pitch * ml.HeightElements;
    const uint32_t lin_slice_pitch = (req.linear_slice_pitch_bytes == 0u) ? min_slice : req.linear_slice_pitch_bytes;
    if (lin_slice_pitch < min_slice) {
        return Status::Error(StatusCode::InvalidArgument, "3D linear_slice_pitch_bytes too small");
    }

    const uint64_t tiled_base = (ml.InMipTail != 0u)
                                    ? (layout.MipTailOffsetBytes + ml.MipTailSubTileOffsetBytes)
                                    : ml.OffsetBytes;
    if (tiled_base > UINT32_MAX || layout.SizeBytes > UINT32_MAX) {
        return Status::Error(StatusCode::InvalidArgument, "3D surface offset exceeds 32-bit GPU buffer bound");
    }

    out_params.direction = static_cast<uint32_t>(req.direction);
    out_params.tile_mode = static_cast<uint32_t>(layout.TileMode);
    out_params.is_3d = 1u;
    out_params.bpe = bpe;
    out_params.width_elems = ml.WidthElements;
    out_params.height_elems = ml.HeightElements;
    out_params.depth_elems = ml.DepthElements;
    out_params.linear_row_pitch = lin_row_pitch;
    out_params.linear_slice_pitch = lin_slice_pitch;
    out_params.tiled_base_offset = static_cast<uint32_t>(tiled_base);
    out_params.in_mip_tail = ml.InMipTail;
    out_params.mip_level = req.mip_level;
    out_params.slice_index = 0u;
    out_params.tiles_x = ml.TilesX;
    out_params.tiles_y = ml.TilesY;
    out_params.tile_w_elems = geom.tile_w;
    out_params.tile_h_elems = geom.tile_h;
    out_params.tile_d_elems = geom.tile_d;
    out_params.tile_size_bytes = geom.tile_size_bytes;
    out_params.micro_w_elems = geom.micro_w;
    out_params.micro_h_elems = geom.micro_h;
    out_params.micro_size_bytes = geom.micro_size_bytes;
    out_params.swizzle_family = geom.family;
    out_params.has_xor = geom.has_xor ? 1u : 0u;
    out_params.mip_tail_bits_x = (ml.InMipTail != 0u) ? Log2Pow2(ml.MipTailPaddedWidthElements) : 0u;
    out_params.mip_tail_bits_y = (ml.InMipTail != 0u) ? Log2Pow2(ml.MipTailPaddedHeightElements) : 0u;
    out_params.mip_tail_bits_z = (ml.InMipTail != 0u) ? Log2Pow2(ml.MipTailPaddedDepthElements) : 0u;
    out_params.mip_tail_sub_block_bytes = ml.MipTailSubBlockBytes;
    out_params.tiled_row_pitch = ml.RowPitchBytes;
    out_params.tiled_depth_pitch = static_cast<uint32_t>(ml.DepthPitchBytes);

    if (out_linear_bytes) {
        *out_linear_bytes = static_cast<uint64_t>(lin_slice_pitch) * ml.DepthElements;
    }
    if (out_tiled_bytes) {
        *out_tiled_bytes = layout.SizeBytes;
    }
    return Status::Ok();
}

std::string GenerateXgGpuUntileHlsl() {
    return R"HLSL(
// OpenScarlett GFX10 / RDNA2 GPU Compute Untiler & Retiler (cs_6_2 / cs_6_5)
cbuffer UntileParams : register(b0) {
    uint g_direction;
    uint g_tile_mode;
    uint g_is_3d;
    uint g_bpe;

    uint g_width_elems;
    uint g_height_elems;
    uint g_depth_elems;
    uint g_linear_row_pitch;

    uint g_linear_slice_pitch;
    uint g_tiled_base_offset;
    uint g_in_mip_tail;
    uint g_mip_level;

    uint g_slice_index;
    uint g_tiles_x;
    uint g_tiles_y;
    uint g_tile_w_elems;

    uint g_tile_h_elems;
    uint g_tile_d_elems;
    uint g_tile_size_bytes;
    uint g_micro_w_elems;

    uint g_micro_h_elems;
    uint g_micro_size_bytes;
    uint g_swizzle_family;
    uint g_has_xor;

    uint g_mip_tail_bits_x;
    uint g_mip_tail_bits_y;
    uint g_mip_tail_bits_z;
    uint g_mip_tail_sub_block_bytes;

    uint g_tiled_row_pitch;
    uint g_tiled_depth_pitch;
    uint g_meta_mode;
    uint g_meta_blocks_x;

    uint g_meta_pipe_aligned;
    uint g_meta_num_pipes;
    uint g_meta_slice_pitch;
    uint g_meta_base_offset;

    uint4 g_clear_0000;
    uint4 g_clear_0001;
    uint4 g_clear_1110;
    uint4 g_clear_1111;
    uint4 g_clear_reg;

    uint4 g_spv_reserved;
};

ByteAddressBuffer g_Src : register(t0);
ByteAddressBuffer g_Meta : register(t1);
RWByteAddressBuffer g_Dst : register(u0);

uint Log2Pow2(uint v) {
    uint r = 0u;
    while (v > 1u) {
        v >>= 1u;
        ++r;
    }
    return r;
}

uint MortonEncode2D(uint x, uint y, uint bits_x, uint bits_y, bool y_first) {
    uint result = 0u;
    uint out_bit = 0u;
    uint common = (bits_x < bits_y) ? bits_x : bits_y;
    for (uint i = 0u; i < common; ++i) {
        uint bx = (x >> i) & 1u;
        uint by = (y >> i) & 1u;
        if (!y_first) {
            result |= (bx << out_bit); out_bit += 1u;
            result |= (by << out_bit); out_bit += 1u;
        } else {
            result |= (by << out_bit); out_bit += 1u;
            result |= (bx << out_bit); out_bit += 1u;
        }
    }
    for (uint ix = common; ix < bits_x; ++ix) {
        result |= (((x >> ix) & 1u) << out_bit); out_bit += 1u;
    }
    for (uint iy = common; iy < bits_y; ++iy) {
        result |= (((y >> iy) & 1u) << out_bit); out_bit += 1u;
    }
    return result;
}

uint MortonEncode3D(uint x, uint y, uint z, uint bx, uint by, uint bz, uint axis_order) {
    uint result = 0u;
    uint out_bit = 0u;
    uint max_bits = max(bx, max(by, bz));
    for (uint i = 0u; i < max_bits; ++i) {
        if (axis_order == 0u) {
            if (i < bx) { result |= (((x >> i) & 1u) << out_bit); out_bit += 1u; }
            if (i < by) { result |= (((y >> i) & 1u) << out_bit); out_bit += 1u; }
            if (i < bz) { result |= (((z >> i) & 1u) << out_bit); out_bit += 1u; }
        } else if (axis_order == 1u) {
            if (i < by) { result |= (((y >> i) & 1u) << out_bit); out_bit += 1u; }
            if (i < bx) { result |= (((x >> i) & 1u) << out_bit); out_bit += 1u; }
            if (i < bz) { result |= (((z >> i) & 1u) << out_bit); out_bit += 1u; }
        } else {
            if (i < bz) { result |= (((z >> i) & 1u) << out_bit); out_bit += 1u; }
            if (i < bx) { result |= (((x >> i) & 1u) << out_bit); out_bit += 1u; }
            if (i < by) { result |= (((y >> i) & 1u) << out_bit); out_bit += 1u; }
        }
    }
    return result;
}

uint ReadByteFromBuf(ByteAddressBuffer buf, uint byte_off) {
    uint w = buf.Load(byte_off & ~3u);
    return (w >> ((byte_off & 3u) * 8u)) & 0xFFu;
}

void AtomicWriteSubWord(uint byte_off, uint val, uint mask) {
    uint aligned = byte_off & ~3u;
    uint shift = (byte_off & 3u) * 8u;
    uint smask = mask << shift;
    uint ins = (val & mask) << shift;
    uint expected = g_Dst.Load(aligned);
    for (;;) {
        uint desired = (expected & ~smask) | ins;
        uint prev = 0u;
        g_Dst.InterlockedCompareExchange(aligned, expected, desired, prev);
        if (prev == expected) break;
        expected = prev;
    }
}

uint ComputeTiledOffset(uint x, uint y, uint z) {
    if (g_tile_mode == 0u) {
        return g_tiled_base_offset + z * g_tiled_depth_pitch + y * g_tiled_row_pitch + x * g_bpe;
    }
    if (g_in_mip_tail != 0u) {
        if (g_is_3d != 0u) {
            uint order = (g_swizzle_family == 4u) ? 2u : ((g_swizzle_family == 3u) ? 1u : 0u);
            uint sub_off = MortonEncode3D(x, y, z, g_mip_tail_bits_x, g_mip_tail_bits_y, g_mip_tail_bits_z, order) * g_bpe;
            if (g_has_xor != 0u && g_mip_tail_sub_block_bytes >= 256u) {
                sub_off ^= ((g_mip_level & 0xFu) << 4u);
            }
            return g_tiled_base_offset + sub_off;
        } else {
            bool y_first = (g_swizzle_family == 2u || g_swizzle_family == 4u);
            uint sub_off = MortonEncode2D(x, y, g_mip_tail_bits_x, g_mip_tail_bits_y, y_first) * g_bpe;
            if (g_has_xor != 0u && g_mip_tail_sub_block_bytes >= 256u) {
                sub_off ^= (((g_mip_level ^ (g_slice_index << 1u)) & 0xFu) << 4u);
            }
            return g_tiled_base_offset + sub_off;
        }
    }
    if (g_is_3d != 0u) {
        uint tile_x = x / g_tile_w_elems;
        uint tile_y = y / g_tile_h_elems;
        uint tile_z = z / g_tile_d_elems;
        uint in_x = x % g_tile_w_elems;
        uint in_y = y % g_tile_h_elems;
        uint in_z = z % g_tile_d_elems;
        uint tile_index = (tile_z * g_tiles_y + tile_y) * g_tiles_x + tile_x;
        uint bx = Log2Pow2(g_tile_w_elems);
        uint by = Log2Pow2(g_tile_h_elems);
        uint bz = Log2Pow2(g_tile_d_elems);
        uint elem_index = 0u;
        if (g_swizzle_family == 3u && bx >= 1u && by >= 1u && bz >= 1u) {
            uint voxel = (in_x & 1u) | ((in_y & 1u) << 1u) | ((in_z & 1u) << 2u);
            uint outer = MortonEncode3D(in_x >> 1u, in_y >> 1u, in_z >> 1u, bx - 1u, by - 1u, bz - 1u, 1u);
            elem_index = (outer << 3u) | voxel;
        } else if (g_swizzle_family == 4u) {
            elem_index = MortonEncode3D(in_x, in_y, in_z, bx, by, bz, 2u);
        } else {
            elem_index = MortonEncode3D(in_x, in_y, in_z, bx, by, bz, 0u);
        }
        uint offset_in_tile = elem_index * g_bpe;
        if (g_has_xor != 0u) {
            uint xor_nibble = 0u;
            if (g_tile_mode == 20u) xor_nibble = (tile_x ^ (tile_y << 1u) ^ (tile_z << 2u)) & 0xFu;
            else if (g_tile_mode == 19u) xor_nibble = ((tile_x << 1u) ^ tile_y ^ (tile_z * 3u)) & 0xFu;
            else if (g_tile_mode == 21u) xor_nibble = (tile_z ^ (tile_x << 1u) ^ (tile_y << 2u)) & 0xFu;
            offset_in_tile ^= (xor_nibble << 12u);
        }
        return g_tiled_base_offset + tile_index * g_tile_size_bytes + offset_in_tile;
    } else {
        uint tile_x = x / g_tile_w_elems;
        uint tile_y = y / g_tile_h_elems;
        uint in_x = x % g_tile_w_elems;
        uint in_y = y % g_tile_h_elems;
        uint tile_index = tile_y * g_tiles_x + tile_x;
        uint bits_x = Log2Pow2(g_tile_w_elems);
        uint bits_y = Log2Pow2(g_tile_h_elems);
        uint offset_in_tile = 0u;
        if (g_swizzle_family == 2u) {
            offset_in_tile = MortonEncode2D(in_x, in_y, bits_x, bits_y, true) * g_bpe;
        } else if (g_swizzle_family == 3u) {
            if (bits_x >= 1u && bits_y >= 1u) {
                uint quad_elem = (in_x & 1u) | ((in_y & 1u) << 1u);
                uint ux = in_x >> 1u;
                uint uy = in_y >> 1u;
                uint bits_ux = bits_x - 1u;
                uint bits_uy = bits_y - 1u;
                uint mask_ux = (bits_ux > 0u) ? ((1u << bits_ux) - 1u) : 0u;
                uint ux_sheared = ux ^ (uy & mask_ux);
                uint quad_idx = MortonEncode2D(ux_sheared, uy, bits_ux, bits_uy, true);
                offset_in_tile = ((quad_idx << 2u) | quad_elem) * g_bpe;
            } else {
                offset_in_tile = MortonEncode2D(in_x, in_y, bits_x, bits_y, false) * g_bpe;
            }
        } else {
            uint micro_x = in_x / g_micro_w_elems;
            uint micro_y = in_y / g_micro_h_elems;
            uint local_x = in_x % g_micro_w_elems;
            uint local_y = in_y % g_micro_h_elems;
            uint micro_grid_w = g_tile_w_elems / g_micro_w_elems;
            uint micro_grid_h = g_tile_h_elems / g_micro_h_elems;
            bool micro_y_first = (g_swizzle_family == 4u && g_tile_mode != 4u);
            uint micro_idx = MortonEncode2D(micro_x, micro_y, Log2Pow2(micro_grid_w), Log2Pow2(micro_grid_h), micro_y_first);
            uint elem_in_micro = MortonEncode2D(local_x, local_y, Log2Pow2(g_micro_w_elems), Log2Pow2(g_micro_h_elems), false);
            offset_in_tile = micro_idx * g_micro_size_bytes + elem_in_micro * g_bpe;
        }
        if (g_has_xor != 0u) {
            uint xor_nibble = 0u;
            if (g_tile_mode == 4u) xor_nibble = (tile_x ^ (tile_y << 1u) ^ (g_slice_index << 2u)) & 0xFu;
            else if (g_tile_mode == 14u) xor_nibble = ((tile_x << 1u) ^ tile_y ^ g_slice_index) & 0xFu;
            else if (g_tile_mode == 15u) xor_nibble = (tile_x ^ tile_y ^ (g_slice_index << 1u)) & 0xFu;
            else if (g_tile_mode == 13u) xor_nibble = (tile_x ^ (tile_y * 3u) ^ (g_slice_index << 2u)) & 0xFu;
            offset_in_tile ^= (xor_nibble << 12u);
        }
        return g_tiled_base_offset + tile_index * g_tile_size_bytes + offset_in_tile;
    }
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    uint wh = g_width_elems * g_height_elems;
    uint total = wh * g_depth_elems;
    uint tid = gid.x;
    if (tid >= total) return;

    uint z = tid / wh;
    uint rem = tid % wh;
    uint y = rem / g_width_elems;
    uint x = rem % g_width_elems;

    uint linear_off = z * g_linear_slice_pitch + y * g_linear_row_pitch + x * g_bpe;
    uint tiled_off = ComputeTiledOffset(x, y, z);
    uint src_off = (g_direction == 0u) ? tiled_off : linear_off;
    uint dst_off = (g_direction == 0u) ? linear_off : tiled_off;

    bool use_fast_clear = false;
    uint4 payload = uint4(0u, 0u, 0u, 0u);

    if (g_direction == 0u && g_meta_mode != 0u) {
        if (g_meta_mode == 1u) {
            uint blk_w = (g_bpe < 4u) ? 16u : ((g_bpe < 16u) ? 8u : 4u);
            uint blk_h = (g_bpe == 1u) ? 16u : ((g_bpe < 8u) ? 8u : 4u);
            uint bx = x / blk_w;
            uint by = y / blk_h;
            uint moff = by * g_meta_blocks_x + bx;
            if (g_meta_pipe_aligned != 0u && g_meta_slice_pitch >= 256u) {
                uint pipe_mask = (g_meta_num_pipes > 1u) ? (g_meta_num_pipes - 1u) : 0u;
                uint xor_bits = ((bx >> 2u) ^ (by >> 2u) ^ g_mip_level ^ g_slice_index) & pipe_mask;
                uint cand = moff ^ xor_bits;
                if (cand < g_meta_slice_pitch) moff = cand;
            }
            uint key = ReadByteFromBuf(g_Meta, g_meta_base_offset + moff);
            if (key == 0x00u)      { payload = g_clear_0000; use_fast_clear = true; }
            else if (key == 0x40u) { payload = g_clear_0001; use_fast_clear = true; }
            else if (key == 0x80u) { payload = g_clear_1110; use_fast_clear = true; }
            else if (key == 0xC0u) { payload = g_clear_1111; use_fast_clear = true; }
            else if (key == 0x20u) { payload = g_clear_reg;  use_fast_clear = true; }
        } else if (g_meta_mode == 2u) {
            uint bx = x >> 3u;
            uint by = y >> 3u;
            uint hword = g_Meta.Load(g_meta_base_offset + (by * g_meta_blocks_x + bx) * 4u);
            if ((hword & 0xFu) == 0u) {
                payload = g_clear_reg;
                use_fast_clear = true;
            }
        } else if (g_meta_mode == 3u) {
            uint bx = x >> 3u;
            uint by = y >> 3u;
            uint bidx = by * g_meta_blocks_x + bx;
            uint cbyte = ReadByteFromBuf(g_Meta, g_meta_base_offset + (bidx >> 1u));
            uint nib = ((bidx & 1u) == 0u) ? (cbyte & 0xFu) : ((cbyte >> 4u) & 0xFu);
            if (nib == 0u) {
                payload = g_clear_reg;
                use_fast_clear = true;
            }
        }
    }

    if (!use_fast_clear) {
        if (g_bpe == 1u) {
            payload.x = ReadByteFromBuf(g_Src, src_off);
        } else if (g_bpe == 2u) {
            uint w = g_Src.Load(src_off & ~3u);
            payload.x = (w >> ((src_off & 3u) * 8u)) & 0xFFFFu;
        } else if (g_bpe == 4u) {
            payload.x = g_Src.Load(src_off);
        } else if (g_bpe == 8u) {
            uint2 v2 = g_Src.Load2(src_off);
            payload.xy = v2;
        } else {
            payload = g_Src.Load4(src_off);
        }
    }

    if (g_bpe == 1u) {
        AtomicWriteSubWord(dst_off, payload.x, 0xFFu);
    } else if (g_bpe == 2u) {
        AtomicWriteSubWord(dst_off, payload.x, 0xFFFFu);
    } else if (g_bpe == 4u) {
        g_Dst.Store(dst_off, payload.x);
    } else if (g_bpe == 8u) {
        g_Dst.Store2(dst_off, payload.xy);
    } else {
        g_Dst.Store4(dst_off, payload);
    }
}
)HLSL";
}

Status CompileXgGpuUntileDxil(
    const std::string& target_profile,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path) {
    const std::string target = target_profile.empty() ? "cs_6_5" : target_profile;
    Status st = CompileHlslWithDxc(GenerateXgGpuUntileHlsl(), target, "main", out_dxil, out_diagnostics, dxc_path);
    if (!st.ok()) return st;
    return ResignDxilContainer(out_dxil);
}

Status EmitXgGpuUntileSpirv(std::vector<uint32_t>& out_spirv) {
    SpvUntileBuilder builder;
    out_spirv = builder.Build();
    return ValidateSpirvBinary(out_spirv);
}

// ---------------------------------------------------------------------------
// XgGpuUntiler Implementation
// ---------------------------------------------------------------------------

struct XgGpuUntiler::Impl {
#if defined(_WIN32)
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_value = 0ull;
    ComPtr<ID3D12RootSignature> root_sig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12PipelineState> resolve_pso;
    std::vector<uint8_t> dxil;
    std::vector<uint8_t> resolve_dxil;
    std::string dxc_path_cached;
    std::string adapter_name;
    std::string shader_target;
    bool is_warp = false;
    bool d3d12_ready = false;
#endif
    VulkanBackend vk_backend;
    std::vector<uint32_t> spirv;
    std::vector<uint32_t> resolve_spirv;
    std::string vk_device_name;
    bool vk_ready = false;
    bool vk_is_hw = false;

    void ReleaseD3D12() {
#if defined(_WIN32)
        resolve_pso.Reset();
        pso.Reset();
        root_sig.Reset();
        cmd_list.Reset();
        allocator.Reset();
        queue.Reset();
        fence.Reset();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        device.Reset();
        dxil.clear();
        resolve_dxil.clear();
        d3d12_ready = false;
#endif
    }
};

XgGpuUntiler::XgGpuUntiler() : impl_(std::make_unique<Impl>()) {}
XgGpuUntiler::~XgGpuUntiler() { Shutdown(); }

void XgGpuUntiler::Shutdown() {
    if (!impl_) return;
    impl_->ReleaseD3D12();
    impl_->vk_backend.Shutdown();
    impl_->vk_ready = false;
    impl_->vk_is_hw = false;
}

#if defined(_WIN32)

Status XgGpuUntiler::InitializeD3D12WithDevice(
    ID3D12Device* device,
    const std::string& target_profile_override,
    const std::string& dxc_path) {
    if (!device) {
        return Status::Error(StatusCode::InvalidArgument, "InitializeD3D12WithDevice: null device");
    }
    impl_->ReleaseD3D12();

    device->AddRef();
    impl_->device.Attach(device);

    D3D12AdapterInfo info{};
    if (QueryD3D12DeviceAdapterInfo(device, nullptr, info).ok()) {
        impl_->adapter_name = info.description;
        impl_->is_warp = info.is_warp;
    } else {
        impl_->adapter_name = "External D3D12 Device";
        impl_->is_warp = false;
    }

    const std::string target = !target_profile_override.empty()
                                   ? target_profile_override
                                   : ((info.highest_shader_model >= 0x65u && !info.is_warp) ? "cs_6_5" : "cs_6_2");
    impl_->shader_target = target;
    impl_->dxc_path_cached = dxc_path;

    std::string diag;
    Status st = CompileXgGpuUntileDxil(target, impl_->dxil, diag, dxc_path);
    if (!st.ok()) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::ShaderError, "Failed to compile untiler DXIL (" + target + "): " + diag);
    }

    // Create Root Signature: [0]=CBV b0, [1]=SRV t0, [2]=SRV t1, [3]=UAV u0
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].Descriptor.RegisterSpace = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[3].Descriptor.ShaderRegister = 0;
    params[3].Descriptor.RegisterSpace = 0;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 4;
    rs_desc.pParameters = params;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    auto serialize_fn = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        ResolveHostD3D12Proc("D3D12SerializeRootSignature"));
    if (!serialize_fn) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "Failed to resolve D3D12SerializeRootSignature");
    }

    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> err_blob;
    HRESULT hr = serialize_fn(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put());
    if (FAILED(hr) || !sig_blob) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "D3D12SerializeRootSignature failed");
    }

    hr = device->CreateRootSignature(
        0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
        OS_UUIDOF(ID3D12RootSignature), impl_->root_sig.PutVoid());
    if (FAILED(hr) || !impl_->root_sig) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "CreateRootSignature failed");
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = impl_->root_sig.Get();
    pso_desc.CS.pShaderBytecode = impl_->dxil.data();
    pso_desc.CS.BytecodeLength = impl_->dxil.size();
    hr = device->CreateComputePipelineState(&pso_desc, OS_UUIDOF(ID3D12PipelineState), impl_->pso.PutVoid());
    if (FAILED(hr) || !impl_->pso) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::ShaderError, "CreateComputePipelineState(untiler) failed");
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&qd, OS_UUIDOF(ID3D12CommandQueue), impl_->queue.PutVoid());
    if (FAILED(hr) || !impl_->queue) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "CreateCommandQueue failed");
    }

    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, OS_UUIDOF(ID3D12CommandAllocator), impl_->allocator.PutVoid());
    if (FAILED(hr) || !impl_->allocator) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "CreateCommandAllocator failed");
    }

    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl_->allocator.Get(), nullptr,
        OS_UUIDOF(ID3D12GraphicsCommandList), impl_->cmd_list.PutVoid());
    if (FAILED(hr) || !impl_->cmd_list) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "CreateCommandList failed");
    }
    impl_->cmd_list->Close();

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, OS_UUIDOF(ID3D12Fence), impl_->fence.PutVoid());
    if (FAILED(hr) || !impl_->fence) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "CreateFence failed");
    }
    impl_->fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->fence_event) {
        impl_->ReleaseD3D12();
        return Status::Error(StatusCode::DeviceError, "CreateEventA failed");
    }

    impl_->d3d12_ready = true;
    return Status::Ok();
}

Status XgGpuUntiler::InitializeD3D12(
    bool prefer_warp,
    const std::string& target_profile_override,
    const std::string& dxc_path) {
    ComPtr<ID3D12Device> dev;
    std::string adapter_name;
    uint32_t highest_sm = 0x62u;
    bool is_warp = prefer_warp;
    Status st = CreateD3D12DevicePortable(prefer_warp, dev, adapter_name, highest_sm, is_warp);
    if (!st.ok()) return st;

    const std::string target = !target_profile_override.empty()
                                   ? target_profile_override
                                   : ((highest_sm >= 0x65u && !is_warp) ? "cs_6_5" : "cs_6_2");
    st = InitializeD3D12WithDevice(dev.Get(), target, dxc_path);
    if (st.ok()) {
        impl_->adapter_name = adapter_name;
        impl_->is_warp = is_warp;
    }
    return st;
}

Status XgGpuUntiler::RecordDispatchD3D12(
    ID3D12GraphicsCommandList* cmd_list,
    const XgGpuUntileParams& params,
    uint64_t cbv_gpu_va,
    uint64_t src_srv_gpu_va,
    uint64_t meta_srv_gpu_va,
    uint64_t dst_uav_gpu_va) const {
    if (!impl_->d3d12_ready || !cmd_list) {
        return Status::Error(StatusCode::DeviceError, "RecordDispatchD3D12: untiler or command list not ready");
    }
    const uint64_t total_elems = static_cast<uint64_t>(params.width_elems) *
                                 static_cast<uint64_t>(params.height_elems) *
                                 static_cast<uint64_t>(params.depth_elems);
    if (total_elems == 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "RecordDispatchD3D12: zero elements");
    }
    const uint32_t groups_x = static_cast<uint32_t>((total_elems + kUntileGroupSize - 1ull) / kUntileGroupSize);

    cmd_list->SetComputeRootSignature(impl_->root_sig.Get());
    cmd_list->SetPipelineState(impl_->pso.Get());
    cmd_list->SetComputeRootConstantBufferView(0, cbv_gpu_va);
    cmd_list->SetComputeRootShaderResourceView(1, src_srv_gpu_va);
    cmd_list->SetComputeRootShaderResourceView(2, meta_srv_gpu_va != 0ull ? meta_srv_gpu_va : src_srv_gpu_va);
    cmd_list->SetComputeRootUnorderedAccessView(3, dst_uav_gpu_va);
    cmd_list->Dispatch(groups_x, 1u, 1u);
    return Status::Ok();
}

Status XgGpuUntiler::ExecuteD3D12(
    const XgGpuUntileParams& params,
    const std::vector<uint8_t>& src_bytes,
    std::vector<uint8_t>& dst_bytes,
    const std::vector<uint8_t>* meta_bytes,
    XgGpuDispatchMetrics* out_metrics) {
    if (!impl_->d3d12_ready) {
        return Status::Error(StatusCode::DeviceError, "XgGpuUntiler D3D12 backend not initialized");
    }
    if (src_bytes.empty() || dst_bytes.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "src_bytes and dst_bytes must be non-empty");
    }

    const uint64_t src_sz = AlignUpU64(src_bytes.size(), 256ull);
    const uint64_t dst_sz = AlignUpU64(dst_bytes.size(), 256ull);
    const uint64_t meta_sz = (meta_bytes && !meta_bytes->empty()) ? AlignUpU64(meta_bytes->size(), 256ull) : 256ull;

    ComPtr<ID3D12Resource> cb_up, src_up, src_gpu, meta_up, meta_gpu, dst_init_up, dst_gpu, dst_rb;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256ull,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, cb_up); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_UPLOAD, src_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, src_up); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_DEFAULT, src_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, src_gpu); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_UPLOAD, meta_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, meta_up); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_DEFAULT, meta_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, meta_gpu); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_UPLOAD, dst_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, dst_init_up); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_DEFAULT, dst_sz,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST, dst_gpu); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_READBACK, dst_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, dst_rb); !st.ok()) return st;

    auto upload = [](ID3D12Resource* res, const void* data, size_t len, uint64_t total_sz) -> Status {
        void* mapped = nullptr;
        D3D12_RANGE r{0, 0};
        if (FAILED(res->Map(0, &r, &mapped)) || !mapped) {
            return Status::Error(StatusCode::DeviceError, "Map(upload) failed");
        }
        std::memset(mapped, 0, static_cast<size_t>(total_sz));
        if (data && len > 0) std::memcpy(mapped, data, len);
        res->Unmap(0, nullptr);
        return Status::Ok();
    };

    if (auto st = upload(cb_up.Get(), &params, sizeof(params), 256ull); !st.ok()) return st;
    if (auto st = upload(src_up.Get(), src_bytes.data(), src_bytes.size(), src_sz); !st.ok()) return st;
    if (meta_bytes && !meta_bytes->empty()) {
        if (auto st = upload(meta_up.Get(), meta_bytes->data(), meta_bytes->size(), meta_sz); !st.ok()) return st;
    } else {
        if (auto st = upload(meta_up.Get(), nullptr, 0, meta_sz); !st.ok()) return st;
    }
    if (auto st = upload(dst_init_up.Get(), dst_bytes.data(), dst_bytes.size(), dst_sz); !st.ok()) return st;

    impl_->allocator->Reset();
    impl_->cmd_list->Reset(impl_->allocator.Get(), nullptr);

    impl_->cmd_list->CopyBufferRegion(src_gpu.Get(), 0, src_up.Get(), 0, src_sz);
    impl_->cmd_list->CopyBufferRegion(meta_gpu.Get(), 0, meta_up.Get(), 0, meta_sz);
    impl_->cmd_list->CopyBufferRegion(dst_gpu.Get(), 0, dst_init_up.Get(), 0, dst_sz);

    D3D12_RESOURCE_BARRIER pre[3]{};
    pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[0].Transition = {src_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[1].Transition = {meta_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    pre[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[2].Transition = {dst_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    impl_->cmd_list->ResourceBarrier(3, pre);

    Status rec = RecordDispatchD3D12(
        impl_->cmd_list.Get(), params,
        cb_up->GetGPUVirtualAddress(),
        src_gpu->GetGPUVirtualAddress(),
        meta_gpu->GetGPUVirtualAddress(),
        dst_gpu->GetGPUVirtualAddress());
    if (!rec.ok()) return rec;

    D3D12_RESOURCE_BARRIER post[2]{};
    post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    post[0].UAV.pResource = dst_gpu.Get();
    post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition = {dst_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    impl_->cmd_list->ResourceBarrier(2, post);

    impl_->cmd_list->CopyBufferRegion(dst_rb.Get(), 0, dst_gpu.Get(), 0, dst_sz);
    if (FAILED(impl_->cmd_list->Close())) {
        return Status::Error(StatusCode::DeviceError, "Close(cmd_list) failed");
    }

    ID3D12CommandList* lists[] = {impl_->cmd_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    const uint64_t wait_val = ++impl_->fence_value;
    if (FAILED(impl_->queue->Signal(impl_->fence.Get(), wait_val))) {
        return Status::Error(StatusCode::DeviceError, "Queue::Signal failed");
    }
    if (impl_->fence->GetCompletedValue() < wait_val) {
        impl_->fence->SetEventOnCompletion(wait_val, impl_->fence_event);
        WaitForSingleObject(impl_->fence_event, 15000);
    }

    void* mapped = nullptr;
    D3D12_RANGE rr{0, dst_bytes.size()};
    if (FAILED(dst_rb->Map(0, &rr, &mapped)) || !mapped) {
        return Status::Error(StatusCode::DeviceError, "Map(readback) failed");
    }
    std::memcpy(dst_bytes.data(), mapped, dst_bytes.size());
    D3D12_RANGE wr{0, 0};
    dst_rb->Unmap(0, &wr);

    if (out_metrics) {
        const uint32_t total = params.width_elems * params.height_elems * params.depth_elems;
        out_metrics->backend_name = impl_->is_warp ? "D3D12-WARP" : "D3D12-HW";
        out_metrics->adapter_name = impl_->adapter_name;
        out_metrics->shader_target = impl_->shader_target;
        out_metrics->dispatch_groups_x = (total + kUntileGroupSize - 1u) / kUntileGroupSize;
        out_metrics->total_elements = total;
        out_metrics->bytes_per_element = params.bpe;
        out_metrics->executed_on_hardware_gpu = !impl_->is_warp;
        out_metrics->used_warp = impl_->is_warp;
        out_metrics->used_vulkan_icd = false;
    }
    return Status::Ok();
}

bool XgGpuUntiler::IsD3D12Ready() const noexcept { return impl_->d3d12_ready; }
bool XgGpuUntiler::IsD3D12Warp() const noexcept { return impl_->is_warp; }
const std::string& XgGpuUntiler::D3D12AdapterName() const noexcept { return impl_->adapter_name; }
const std::string& XgGpuUntiler::D3D12ShaderTarget() const noexcept { return impl_->shader_target; }

#endif  // _WIN32

Status XgGpuUntiler::InitializeVulkan(bool allow_software_fallback) {
    impl_->vk_backend.Shutdown();
    impl_->vk_ready = false;
    impl_->vk_is_hw = false;

    Status st = EmitXgGpuUntileSpirv(impl_->spirv);
    if (!st.ok()) return st;

    st = impl_->vk_backend.Initialize(allow_software_fallback);
    if (!st.ok()) return st;

    impl_->vk_is_hw = !impl_->vk_backend.is_fallback_mode();
    impl_->vk_device_name = impl_->vk_backend.GetDeviceInfo().device_name;
    impl_->vk_ready = true;
    return Status::Ok();
}

bool XgGpuUntiler::IsVulkanReady() const noexcept { return impl_->vk_ready; }
bool XgGpuUntiler::IsVulkanHardwareIcd() const noexcept { return impl_->vk_ready && impl_->vk_is_hw; }
const std::string& XgGpuUntiler::VulkanDeviceName() const noexcept { return impl_->vk_device_name; }

Status XgGpuUntiler::ExecuteVulkan(
    const XgGpuUntileParams& params,
    const std::vector<uint8_t>& src_bytes,
    std::vector<uint8_t>& dst_bytes,
    const std::vector<uint8_t>* meta_bytes,
    XgGpuDispatchMetrics* out_metrics) {
    if (!impl_->vk_ready) {
        return Status::Error(StatusCode::DeviceError, "XgGpuUntiler Vulkan backend not initialized");
    }
    if (!impl_->vk_is_hw) {
        return Status::Error(StatusCode::NotImplemented,
                             "Vulkan hardware ICD required for SPIR-V untiler compute execution");
    }
    if (src_bytes.empty() || dst_bytes.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "src_bytes and dst_bytes must be non-empty");
    }

    const uint32_t src_words = static_cast<uint32_t>((src_bytes.size() + 3u) / 4u);
    const uint32_t meta_words =
        (meta_bytes && !meta_bytes->empty()) ? static_cast<uint32_t>((meta_bytes->size() + 3u) / 4u) : 1u;
    const uint32_t dst_words = static_cast<uint32_t>((dst_bytes.size() + 3u) / 4u);

    XgGpuUntileParams hdr = params;
    hdr.spv_src_word_offset = kSpvHeaderWords;
    hdr.spv_meta_word_offset = kSpvHeaderWords + src_words;
    hdr.spv_dst_byte_size = static_cast<uint32_t>(dst_bytes.size());

    const uint32_t total_in_words = std::max(kSpvHeaderWords + src_words + meta_words, dst_words);
    std::vector<uint32_t> input_u32(total_in_words, 0u);
    std::memcpy(input_u32.data(), &hdr, sizeof(hdr));
    std::memcpy(input_u32.data() + hdr.spv_src_word_offset, src_bytes.data(), src_bytes.size());
    if (meta_bytes && !meta_bytes->empty()) {
        std::memcpy(input_u32.data() + hdr.spv_meta_word_offset, meta_bytes->data(), meta_bytes->size());
    }

    const uint32_t total_elems = params.width_elems * params.height_elems * params.depth_elems;
    const uint32_t groups_x = std::max(1u, (total_elems + kUntileGroupSize - 1u) / kUntileGroupSize);

    std::vector<uint32_t> output_u32;
    VulkanComputeDispatchOptions vk_opts{};
    VulkanComputeDispatchResult vk_res{};
    Status st = impl_->vk_backend.ExecuteComputeShaderEx(
        impl_->spirv, input_u32, output_u32, groups_x, vk_opts, vk_res);
    if (!st.ok()) return st;

    if (output_u32.size() * sizeof(uint32_t) < dst_bytes.size()) {
        return Status::Error(StatusCode::DeviceError, "Vulkan output buffer smaller than expected");
    }
    std::memcpy(dst_bytes.data(), output_u32.data(), dst_bytes.size());

    if (out_metrics) {
        out_metrics->backend_name = "Vulkan-SPIRV";
        out_metrics->adapter_name = impl_->vk_device_name;
        out_metrics->shader_target = "spirv_1_3";
        out_metrics->dispatch_groups_x = groups_x;
        out_metrics->total_elements = total_elems;
        out_metrics->bytes_per_element = params.bpe;
        out_metrics->executed_on_hardware_gpu = vk_res.executed_on_vulkan_icd;
        out_metrics->used_warp = false;
        out_metrics->used_vulkan_icd = vk_res.executed_on_vulkan_icd;
    }
    return Status::Ok();
}

// ============================================================================
// Direct D3D12 / Vulkan HTILE, CMASK & DCC Fast-Clear Metadata Coherency Helpers
// ============================================================================

namespace {

uint16_t FloatToHalfLocal(float f) noexcept {
    uint32_t bits = 0u;
    std::memcpy(&bits, &f, sizeof(bits));
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23u) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x007FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        mant = (mant | 0x00800000u) >> static_cast<uint32_t>(1 - exp);
        return static_cast<uint16_t>(sign | ((mant + 0x00001000u) >> 13u));
    }
    if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10u) | ((mant + 0x00001000u) >> 13u));
}

uint32_t FloatToUnormBits(float v, uint32_t bits) noexcept {
    const float c = std::clamp(v, 0.0f, 1.0f);
    const uint32_t max_val = (1u << bits) - 1u;
    return static_cast<uint32_t>(c * static_cast<float>(max_val) + 0.5f);
}

uint32_t DccBlockWidthForBpe(uint32_t bpe) noexcept {
    if (bpe < 4u) return 16u;
    if (bpe < 16u) return 8u;
    return 4u;
}

uint32_t DccBlockHeightForBpe(uint32_t bpe) noexcept {
    if (bpe == 1u) return 16u;
    if (bpe < 8u) return 8u;
    return 4u;
}

uint8_t DccFastClearKeyForState(XG_DCC_BLOCK_STATE state) noexcept {
    switch (state) {
        case XG_DCC_STATE_CLEAR_0000: return 0x00u;
        case XG_DCC_STATE_CLEAR_0001: return 0x40u;
        case XG_DCC_STATE_CLEAR_1110: return 0x80u;
        case XG_DCC_STATE_CLEAR_1111: return 0xC0u;
        case XG_DCC_STATE_CLEAR_REG:  return 0x20u;
        case XG_DCC_STATE_UNCOMPRESSED:
        default:
            return 0xFFu;
    }
}

bool IsBlockInRect(
    uint32_t bx,
    uint32_t by,
    uint32_t blk_w,
    uint32_t blk_h,
    uint32_t full_w,
    uint32_t full_h,
    const XgFastClearRect& rect) noexcept {
    const uint32_t r_right = (rect.right == 0u) ? full_w : std::min(rect.right, full_w);
    const uint32_t r_bottom = (rect.bottom == 0u) ? full_h : std::min(rect.bottom, full_h);
    const uint32_t px0 = bx * blk_w;
    const uint32_t py0 = by * blk_h;
    const uint32_t px1 = px0 + blk_w;
    const uint32_t py1 = py0 + blk_h;
    return px0 < r_right && px1 > rect.left && py0 < r_bottom && py1 > rect.top;
}

}  // namespace

void EncodeFormatFastClearRegister(
    uint32_t dxgi_format,
    const float rgba_clear[4],
    float depth_clear,
    uint8_t stencil_clear,
    std::array<uint32_t, 4>& out_clear_reg) noexcept {
    out_clear_reg = {0u, 0u, 0u, 0u};
    const float r = rgba_clear ? rgba_clear[0] : 0.0f;
    const float g = rgba_clear ? rgba_clear[1] : 0.0f;
    const float b = rgba_clear ? rgba_clear[2] : 0.0f;
    const float a = rgba_clear ? rgba_clear[3] : 1.0f;

#if defined(_WIN32)
    switch (static_cast<DXGI_FORMAT>(dxgi_format)) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: {
            out_clear_reg[0] = FloatToUnormBits(r, 8u) |
                               (FloatToUnormBits(g, 8u) << 8u) |
                               (FloatToUnormBits(b, 8u) << 16u) |
                               (FloatToUnormBits(a, 8u) << 24u);
            return;
        }
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: {
            out_clear_reg[0] = FloatToUnormBits(b, 8u) |
                               (FloatToUnormBits(g, 8u) << 8u) |
                               (FloatToUnormBits(r, 8u) << 16u) |
                               (FloatToUnormBits(a, 8u) << 24u);
            return;
        }
        case DXGI_FORMAT_R10G10B10A2_UNORM: {
            out_clear_reg[0] = FloatToUnormBits(r, 10u) |
                               (FloatToUnormBits(g, 10u) << 10u) |
                               (FloatToUnormBits(b, 10u) << 20u) |
                               (FloatToUnormBits(a, 2u) << 30u);
            return;
        }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: {
            out_clear_reg[0] = static_cast<uint32_t>(FloatToHalfLocal(r)) |
                               (static_cast<uint32_t>(FloatToHalfLocal(g)) << 16u);
            out_clear_reg[1] = static_cast<uint32_t>(FloatToHalfLocal(b)) |
                               (static_cast<uint32_t>(FloatToHalfLocal(a)) << 16u);
            return;
        }
        case DXGI_FORMAT_R32G32B32A32_FLOAT: {
            std::memcpy(&out_clear_reg[0], &r, 4u);
            std::memcpy(&out_clear_reg[1], &g, 4u);
            std::memcpy(&out_clear_reg[2], &b, 4u);
            std::memcpy(&out_clear_reg[3], &a, 4u);
            return;
        }
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: {
            const float val = (dxgi_format == DXGI_FORMAT_R32_FLOAT) ? r : depth_clear;
            std::memcpy(&out_clear_reg[0], &val, 4u);
            if (dxgi_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
                out_clear_reg[1] = static_cast<uint32_t>(stencil_clear);
            }
            return;
        }
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM: {
            const float val = (dxgi_format == DXGI_FORMAT_R16_UNORM) ? r : depth_clear;
            out_clear_reg[0] = FloatToUnormBits(val, 16u);
            return;
        }
        case DXGI_FORMAT_D24_UNORM_S8_UINT: {
            const uint32_t d24 = FloatToUnormBits(depth_clear, 24u);
            out_clear_reg[0] = (d24 & 0x00FFFFFFu) | (static_cast<uint32_t>(stencil_clear) << 24u);
            return;
        }
        case DXGI_FORMAT_R8_UNORM: {
            out_clear_reg[0] = FloatToUnormBits(r, 8u);
            return;
        }
        default:
            break;
    }
#else
    (void)dxgi_format;
    (void)depth_clear;
    (void)stencil_clear;
#endif
    out_clear_reg[0] = FloatToUnormBits(r, 8u) |
                       (FloatToUnormBits(g, 8u) << 8u) |
                       (FloatToUnormBits(b, 8u) << 16u) |
                       (FloatToUnormBits(a, 8u) << 24u);
}

Status BuildDccFastClearMetadata(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    uint32_t mip_level,
    uint32_t slice_index,
    bool pipe_aligned,
    XG_DCC_BLOCK_STATE clear_state,
    const XgFastClearRect& rect,
    std::vector<uint8_t>& out_dcc_bytes) {
    out_dcc_bytes.clear();
    XG_RESOURCE_LAYOUT layout{};
    if (!xg::ComputeTexture2DLayout(desc, layout) ||
        mip_level >= layout.MipLevels || slice_index >= layout.ArraySize) {
        return Status::Error(StatusCode::InvalidArgument, "BuildDccFastClearMetadata invalid texture desc/subresource");
    }
    XG_RESOURCE_METADATA_LAYOUT meta{};
    if (!xg::ComputeTexture2DMetadataLayout(desc, console_type, meta) || meta.Dcc.SizeBytes == 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "BuildDccFastClearMetadata failed computing metadata layout");
    }
    XG_SCARLETT_PIPE_CONFIG pipe_cfg{};
    xg::QueryScarlettPipeConfig(console_type, pipe_cfg);

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip_level];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;
    const uint32_t blk_w = DccBlockWidthForBpe(bpe);
    const uint32_t blk_h = DccBlockHeightForBpe(bpe);
    const uint32_t blocks_x = std::max(1u, (ml.WidthElements + blk_w - 1u) / blk_w);
    const uint32_t blocks_y = std::max(1u, (ml.HeightElements + blk_h - 1u) / blk_h);
    const uint64_t slice_pitch = std::max<uint64_t>(
        meta.Dcc.SlicePitchBytes, static_cast<uint64_t>(blocks_x) * blocks_y);
    const uint64_t base_off = static_cast<uint64_t>(slice_index) * meta.Dcc.SlicePitchBytes;
    const uint64_t total_sz = std::max<uint64_t>(meta.Dcc.SizeBytes, base_off + slice_pitch);

    out_dcc_bytes.assign(static_cast<size_t>(total_sz), 0xFFu);
    const uint8_t key = DccFastClearKeyForState(clear_state);
    const uint32_t pipe_mask = (pipe_cfg.NumPipes > 1u) ? (pipe_cfg.NumPipes - 1u) : 0u;

    for (uint32_t by = 0u; by < blocks_y; ++by) {
        for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
            if (!IsBlockInRect(bx, by, blk_w, blk_h, ml.WidthElements, ml.HeightElements, rect)) {
                continue;
            }
            uint32_t moff = by * blocks_x + bx;
            if (pipe_aligned && meta.Dcc.SlicePitchBytes >= 256ull) {
                const uint32_t xor_bits = ((bx >> 2u) ^ (by >> 2u) ^ mip_level ^ slice_index) & pipe_mask;
                const uint32_t cand = moff ^ xor_bits;
                if (cand < static_cast<uint32_t>(meta.Dcc.SlicePitchBytes)) {
                    moff = cand;
                }
            }
            if (base_off + moff < out_dcc_bytes.size()) {
                out_dcc_bytes[static_cast<size_t>(base_off + moff)] = key;
            }
        }
    }
    return Status::Ok();
}

Status BuildHtileFastClearMetadata(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    uint32_t slice_index,
    const XgFastClearRect& rect,
    float depth_clear,
    uint8_t stencil_clear,
    std::vector<uint8_t>& out_htile_bytes,
    std::array<uint32_t, 4>& out_clear_reg) {
    out_htile_bytes.clear();
    EncodeFormatFastClearRegister(desc.Format, nullptr, depth_clear, stencil_clear, out_clear_reg);

    XG_RESOURCE_LAYOUT layout{};
    if (!xg::ComputeTexture2DLayout(desc, layout) || slice_index >= layout.ArraySize) {
        return Status::Error(StatusCode::InvalidArgument, "BuildHtileFastClearMetadata invalid texture desc/slice");
    }
    XG_RESOURCE_METADATA_LAYOUT meta{};
    if (!xg::ComputeTexture2DMetadataLayout(desc, console_type, meta)) {
        return Status::Error(StatusCode::InvalidArgument, "BuildHtileFastClearMetadata failed computing metadata layout");
    }

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[0];
    const uint32_t blocks_x = std::max(1u, (ml.WidthElements + 7u) / 8u);
    const uint32_t blocks_y = std::max(1u, (ml.HeightElements + 7u) / 8u);
    const uint64_t min_slice = static_cast<uint64_t>(blocks_x) * blocks_y * 4ull;
    const uint64_t slice_pitch = std::max<uint64_t>(meta.Htile.SlicePitchBytes, min_slice);
    const uint64_t base_off = static_cast<uint64_t>(slice_index) * meta.Htile.SlicePitchBytes;
    const uint64_t total_sz = std::max<uint64_t>(meta.Htile.SizeBytes, base_off + slice_pitch);

    out_htile_bytes.assign(static_cast<size_t>(total_sz), 0u);
    // Default expanded HTILE word: zmask = 0xF (uncompressed/expanded), full [0..1] Hi-Z range.
    const uint32_t expanded_word = 0xFFFFFC0Fu;
    // Fast-clear HTILE word: zmask = 0x0, minZ = maxZ = quantized clear depth (14-bit Hi-Z).
    const uint32_t qz = FloatToUnormBits(depth_clear, 14u) & 0x3FFFu;
    const uint32_t clear_word = (qz << 18u) | (qz << 4u);

    for (size_t off = 0u; off + 4u <= out_htile_bytes.size(); off += 4u) {
        std::memcpy(out_htile_bytes.data() + off, &expanded_word, 4u);
    }
    for (uint32_t by = 0u; by < blocks_y; ++by) {
        for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
            if (!IsBlockInRect(bx, by, 8u, 8u, ml.WidthElements, ml.HeightElements, rect)) {
                continue;
            }
            const uint64_t off = base_off + static_cast<uint64_t>(by * blocks_x + bx) * 4ull;
            if (off + 4ull <= out_htile_bytes.size()) {
                std::memcpy(out_htile_bytes.data() + off, &clear_word, 4u);
            }
        }
    }
    return Status::Ok();
}

Status BuildCmaskFastClearMetadata(
    const XG_TEXTURE2D_DESC& desc,
    XG_CONSOLE_TYPE console_type,
    uint32_t slice_index,
    const XgFastClearRect& rect,
    std::vector<uint8_t>& out_cmask_bytes) {
    out_cmask_bytes.clear();
    XG_RESOURCE_LAYOUT layout{};
    if (!xg::ComputeTexture2DLayout(desc, layout) || slice_index >= layout.ArraySize) {
        return Status::Error(StatusCode::InvalidArgument, "BuildCmaskFastClearMetadata invalid texture desc/slice");
    }
    XG_RESOURCE_METADATA_LAYOUT meta{};
    if (!xg::ComputeTexture2DMetadataLayout(desc, console_type, meta)) {
        return Status::Error(StatusCode::InvalidArgument, "BuildCmaskFastClearMetadata failed computing metadata layout");
    }

    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[0];
    const uint32_t blocks_x = std::max(1u, (ml.WidthElements + 7u) / 8u);
    const uint32_t blocks_y = std::max(1u, (ml.HeightElements + 7u) / 8u);
    const uint64_t min_slice = (static_cast<uint64_t>(blocks_x) * blocks_y + 1ull) / 2ull;
    const uint64_t slice_pitch = std::max<uint64_t>(meta.Cmask.SlicePitchBytes, min_slice);
    const uint64_t base_off = static_cast<uint64_t>(slice_index) * meta.Cmask.SlicePitchBytes;
    const uint64_t total_sz = std::max<uint64_t>(meta.Cmask.SizeBytes, base_off + slice_pitch);

    // 0xFF => both nibbles 0xF (expanded / uncompressed)
    out_cmask_bytes.assign(static_cast<size_t>(total_sz), 0xFFu);
    for (uint32_t by = 0u; by < blocks_y; ++by) {
        for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
            if (!IsBlockInRect(bx, by, 8u, 8u, ml.WidthElements, ml.HeightElements, rect)) {
                continue;
            }
            const uint32_t bidx = by * blocks_x + bx;
            const uint64_t byte_off = base_off + static_cast<uint64_t>(bidx >> 1u);
            if (byte_off < out_cmask_bytes.size()) {
                if ((bidx & 1u) == 0u) {
                    out_cmask_bytes[static_cast<size_t>(byte_off)] &= 0xF0u;  // low nibble = 0x0 (fast clear)
                } else {
                    out_cmask_bytes[static_cast<size_t>(byte_off)] &= 0x0Fu;  // high nibble = 0x0 (fast clear)
                }
            }
        }
    }
    return Status::Ok();
}

Status ResetFastClearMetadataToExpanded(
    XgGpuMetadataMode mode,
    std::vector<uint8_t>& io_meta_bytes) noexcept {
    if (mode == XgGpuMetadataMode::None || io_meta_bytes.empty()) {
        return Status::Ok();
    }
    if (mode == XgGpuMetadataMode::Dcc || mode == XgGpuMetadataMode::Cmask) {
        std::fill(io_meta_bytes.begin(), io_meta_bytes.end(), static_cast<uint8_t>(0xFFu));
        return Status::Ok();
    }
    if (mode == XgGpuMetadataMode::Htile) {
        const uint32_t expanded_word = 0xFFFFFC0Fu;
        for (size_t off = 0u; off + 4u <= io_meta_bytes.size(); off += 4u) {
            std::memcpy(io_meta_bytes.data() + off, &expanded_word, 4u);
        }
        return Status::Ok();
    }
    return Status::Error(StatusCode::InvalidArgument, "Unknown XgGpuMetadataMode");
}

Status ExpandAndSyncFastClearMetadata2D(
    XgGpuUntiler* untiler,
    const XgGpuSurface2DRequest& req,
    const std::vector<uint8_t>& src_tiled_bytes,
    std::vector<uint8_t>& io_meta_bytes,
    std::vector<uint8_t>& out_linear_bytes,
    bool reset_metadata_after_expand,
    bool use_vulkan,
    XgFastClearMetadataSyncStats* out_stats) {
    XgGpuSurface2DRequest untile_req = req;
    untile_req.direction = XgGpuDirection::Untile;

    XgGpuUntileParams params{};
    uint64_t linear_bytes = 0ull;
    uint64_t tiled_bytes = 0ull;
    Status st = BuildXgGpuUntileParams2D(untile_req, params, &linear_bytes, &tiled_bytes);
    if (!st.ok()) return st;

    if (src_tiled_bytes.size() < tiled_bytes) {
        return Status::Error(StatusCode::InvalidArgument, "ExpandAndSyncFastClearMetadata2D: tiled source too small");
    }

    // Count fast-cleared vs uncompressed metadata blocks before resetting.
    XgFastClearMetadataSyncStats local_stats{};
    if (untile_req.metadata_mode == XgGpuMetadataMode::Dcc) {
        const uint32_t blk_w = DccBlockWidthForBpe(params.bpe);
        const uint32_t blk_h = DccBlockHeightForBpe(params.bpe);
        const uint32_t blocks_x = params.meta_blocks_x;
        const uint32_t blocks_y = std::max(1u, (params.height_elems + blk_h - 1u) / blk_h);
        const uint32_t pipe_mask = (params.meta_num_pipes > 1u) ? (params.meta_num_pipes - 1u) : 0u;
        local_stats.total_meta_blocks = blocks_x * blocks_y;
        for (uint32_t by = 0u; by < blocks_y; ++by) {
            for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
                uint32_t moff = by * blocks_x + bx;
                if (params.meta_pipe_aligned != 0u && params.meta_slice_pitch >= 256u) {
                    const uint32_t xor_bits =
                        ((bx >> 2u) ^ (by >> 2u) ^ params.mip_level ^ params.slice_index) & pipe_mask;
                    const uint32_t cand = moff ^ xor_bits;
                    if (cand < params.meta_slice_pitch) moff = cand;
                }
                const size_t idx = static_cast<size_t>(params.meta_base_offset) + moff;
                const uint8_t key = (idx < io_meta_bytes.size()) ? io_meta_bytes[idx] : 0xFFu;
                if (key == 0x00u || key == 0x40u || key == 0x80u || key == 0xC0u || key == 0x20u) {
                    ++local_stats.fast_cleared_blocks_expanded;
                } else {
                    ++local_stats.uncompressed_blocks_preserved;
                }
            }
        }
        (void)blk_w;
    } else if (untile_req.metadata_mode == XgGpuMetadataMode::Htile) {
        const uint32_t blocks_x = params.meta_blocks_x;
        const uint32_t blocks_y = std::max(1u, (params.height_elems + 7u) / 8u);
        local_stats.total_meta_blocks = blocks_x * blocks_y;
        for (uint32_t by = 0u; by < blocks_y; ++by) {
            for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
                const size_t off =
                    static_cast<size_t>(params.meta_base_offset) +
                    static_cast<size_t>(by * blocks_x + bx) * 4u;
                uint32_t hword = 0xFFFFFC0Fu;
                if (off + 4u <= io_meta_bytes.size()) {
                    std::memcpy(&hword, io_meta_bytes.data() + off, 4u);
                }
                if ((hword & 0xFu) == 0u) {
                    ++local_stats.fast_cleared_blocks_expanded;
                } else {
                    ++local_stats.uncompressed_blocks_preserved;
                }
            }
        }
    } else if (untile_req.metadata_mode == XgGpuMetadataMode::Cmask) {
        const uint32_t blocks_x = params.meta_blocks_x;
        const uint32_t blocks_y = std::max(1u, (params.height_elems + 7u) / 8u);
        local_stats.total_meta_blocks = blocks_x * blocks_y;
        for (uint32_t by = 0u; by < blocks_y; ++by) {
            for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
                const uint32_t bidx = by * blocks_x + bx;
                const size_t off = static_cast<size_t>(params.meta_base_offset) + (bidx >> 1u);
                const uint8_t cbyte = (off < io_meta_bytes.size()) ? io_meta_bytes[off] : 0xFFu;
                const uint8_t nib = ((bidx & 1u) == 0u) ? (cbyte & 0x0Fu) : ((cbyte >> 4u) & 0x0Fu);
                if (nib == 0u) {
                    ++local_stats.fast_cleared_blocks_expanded;
                } else {
                    ++local_stats.uncompressed_blocks_preserved;
                }
            }
        }
    }

    out_linear_bytes.assign(static_cast<size_t>(linear_bytes), 0u);
    bool gpu_done = false;
    XgGpuDispatchMetrics metrics{};

    if (untiler) {
        if (use_vulkan && untiler->IsVulkanHardwareIcd()) {
            st = untiler->ExecuteVulkan(params, src_tiled_bytes, out_linear_bytes, &io_meta_bytes, &metrics);
            if (!st.ok()) return st;
            gpu_done = true;
            local_stats.executed_on_gpu = true;
            local_stats.backend_name = metrics.backend_name;
        }
#if defined(_WIN32)
        else if (!use_vulkan && untiler->IsD3D12Ready()) {
            st = untiler->ExecuteD3D12(params, src_tiled_bytes, out_linear_bytes, &io_meta_bytes, &metrics);
            if (!st.ok()) return st;
            gpu_done = true;
            local_stats.executed_on_gpu = true;
            local_stats.backend_name = metrics.backend_name;
        }
#endif
    }

    if (!gpu_done) {
        // CPU fallback: untile base surface then coherently apply fast-clear metadata blocks.
        if (!xg::UntileSurface2DPlane(
                untile_req.desc,
                untile_req.plane,
                untile_req.mip_level,
                untile_req.slice_index,
                src_tiled_bytes.data(),
                src_tiled_bytes.size(),
                out_linear_bytes.data(),
                params.linear_row_pitch)) {
            return Status::Error(StatusCode::DeviceError, "CPU fallback untile failed during metadata sync");
        }
        const uint32_t blk_w = DccBlockWidthForBpe(params.bpe);
        const uint32_t blk_h = DccBlockHeightForBpe(params.bpe);
        const uint32_t pipe_mask = (params.meta_num_pipes > 1u) ? (params.meta_num_pipes - 1u) : 0u;
        for (uint32_t y = 0u; y < params.height_elems; ++y) {
            for (uint32_t x = 0u; x < params.width_elems; ++x) {
                const uint32_t* clear_src = nullptr;
                if (untile_req.metadata_mode == XgGpuMetadataMode::Dcc) {
                    const uint32_t bx = x / blk_w;
                    const uint32_t by = y / blk_h;
                    uint32_t moff = by * params.meta_blocks_x + bx;
                    if (params.meta_pipe_aligned != 0u && params.meta_slice_pitch >= 256u) {
                        const uint32_t xor_bits =
                            ((bx >> 2u) ^ (by >> 2u) ^ params.mip_level ^ params.slice_index) & pipe_mask;
                        const uint32_t cand = moff ^ xor_bits;
                        if (cand < params.meta_slice_pitch) moff = cand;
                    }
                    const size_t idx = static_cast<size_t>(params.meta_base_offset) + moff;
                    const uint8_t key = (idx < io_meta_bytes.size()) ? io_meta_bytes[idx] : 0xFFu;
                    if (key == 0x00u) clear_src = params.clear_0000;
                    else if (key == 0x40u) clear_src = params.clear_0001;
                    else if (key == 0x80u) clear_src = params.clear_1110;
                    else if (key == 0xC0u) clear_src = params.clear_1111;
                    else if (key == 0x20u) clear_src = params.clear_reg;
                } else if (untile_req.metadata_mode == XgGpuMetadataMode::Htile) {
                    const size_t off = static_cast<size_t>(params.meta_base_offset) +
                                       static_cast<size_t>((y >> 3u) * params.meta_blocks_x + (x >> 3u)) * 4u;
                    uint32_t hword = 0xFFFFFC0Fu;
                    if (off + 4u <= io_meta_bytes.size()) {
                        std::memcpy(&hword, io_meta_bytes.data() + off, 4u);
                    }
                    if ((hword & 0xFu) == 0u) clear_src = params.clear_reg;
                } else if (untile_req.metadata_mode == XgGpuMetadataMode::Cmask) {
                    const uint32_t bidx = (y >> 3u) * params.meta_blocks_x + (x >> 3u);
                    const size_t off = static_cast<size_t>(params.meta_base_offset) + (bidx >> 1u);
                    const uint8_t cbyte = (off < io_meta_bytes.size()) ? io_meta_bytes[off] : 0xFFu;
                    const uint8_t nib = ((bidx & 1u) == 0u) ? (cbyte & 0x0Fu) : ((cbyte >> 4u) & 0x0Fu);
                    if (nib == 0u) clear_src = params.clear_reg;
                }
                if (clear_src) {
                    const size_t dst_off =
                        static_cast<size_t>(y) * params.linear_row_pitch + static_cast<size_t>(x) * params.bpe;
                    std::memcpy(out_linear_bytes.data() + dst_off, clear_src, params.bpe);
                }
            }
        }
        local_stats.executed_on_gpu = false;
        local_stats.backend_name = "CPU-CoherentFallback";
    }

    if (reset_metadata_after_expand) {
        st = ResetFastClearMetadataToExpanded(untile_req.metadata_mode, io_meta_bytes);
        if (!st.ok()) return st;
    }

    if (out_stats) {
        *out_stats = std::move(local_stats);
    }
    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Placed Tiled Texture 64KB Tile-Pool Helpers (Leonida #1674)
// Shared by CommandListX (d3d12_xs) and unit/live tests via openscarlett_core.
// ---------------------------------------------------------------------------
struct XgPlacedTiledSubresourceDesc {
    uint32_t subresource_index = 0u;
    uint32_t mip_level = 0u;
    uint32_t array_slice = 0u;
    uint32_t width_elements = 0u;
    uint32_t height_elements = 0u;
    uint32_t depth_elements = 1u;
    uint32_t tiles_x = 0u;
    uint32_t tiles_y = 0u;
    uint32_t tiles_z = 1u;
    uint32_t tile_size_bytes = 65536u;
    uint32_t bytes_per_element = 4u;
    uint32_t linear_row_pitch_bytes = 256u;
    uint64_t subresource_offset_bytes = 0ull;
    uint64_t heap_subresource_offset_bytes = 0ull;
    uint64_t subresource_size_bytes = 0ull;
    uint32_t tile_pool_start_index_64kb = 0u;
    uint32_t tile_pool_count_64kb = 0u;
};

struct XgPlacedTilePoolEntry {
    uint32_t logical_tile_index = 0u;
    uint32_t heap_tile_index_64kb = 0u;
    uint64_t pool_byte_offset = 0ull;
    bool mapped = true;
};

bool ComputePlacedTiledSubresourceLayout2D(
    const XG_TEXTURE2D_DESC& desc,
    uint64_t placed_heap_offset_bytes,
    uint32_t subresource_index,
    XgPlacedTiledSubresourceDesc& out_sub) noexcept {
    out_sub = XgPlacedTiledSubresourceDesc{};
    XG_RESOURCE_LAYOUT layout{};
    if (!xg::ComputeTexture2DLayout(desc, layout) || layout.MipLevels == 0u || layout.ArraySize == 0u) {
        return false;
    }
    const uint32_t total_subs = layout.MipLevels * layout.ArraySize;
    if (subresource_index >= total_subs) {
        return false;
    }
    const uint32_t mip = subresource_index % layout.MipLevels;
    const uint32_t slice = subresource_index / layout.MipLevels;
    const XG_MIPLEVEL_LAYOUT& ml = layout.Mips[mip];
    const uint32_t bpe = layout.FormatInfo.bytes_per_block;

    const uint64_t sub_off = (ml.InMipTail != 0u)
        ? (layout.MipTailOffsetBytes +
           static_cast<uint64_t>(slice) * layout.MipTailSlicePitchBytes +
           ml.MipTailSubTileOffsetBytes)
        : (ml.OffsetBytes + static_cast<uint64_t>(slice) * ml.SlicePitchBytes);
    const uint64_t sub_size = (ml.InMipTail != 0u)
        ? static_cast<uint64_t>(ml.MipTailSubBlockBytes)
        : ml.SlicePitchBytes;

    constexpr uint64_t kPoolTile64KB = 65536ull;
    const uint64_t heap_sub_off = placed_heap_offset_bytes + sub_off;

    const uint32_t sample_count = (desc.SampleCount == 0u) ? 1u : desc.SampleCount;
    const uint64_t base_row_bytes =
        static_cast<uint64_t>(ml.WidthElements) * static_cast<uint64_t>(bpe) * static_cast<uint64_t>(sample_count);

    out_sub.subresource_index = subresource_index;
    out_sub.mip_level = mip;
    out_sub.array_slice = slice;
    out_sub.width_elements = ml.WidthElements;
    out_sub.height_elements = ml.HeightElements;
    out_sub.depth_elements = sample_count;
    out_sub.tiles_x = std::max<uint32_t>(1u, ml.TilesX);
    out_sub.tiles_y = std::max<uint32_t>(1u, ml.TilesY);
    out_sub.tiles_z = 1u;
    out_sub.tile_size_bytes = ml.TileSizeBytes;
    out_sub.bytes_per_element = bpe;
    // D3D12 placed texture subresource row pitch is 256-byte aligned.
    out_sub.linear_row_pitch_bytes = AlignUpU32(static_cast<uint32_t>(base_row_bytes), 256u);
    out_sub.subresource_offset_bytes = sub_off;
    out_sub.heap_subresource_offset_bytes = heap_sub_off;
    out_sub.subresource_size_bytes = sub_size;
    out_sub.tile_pool_start_index_64kb = static_cast<uint32_t>(heap_sub_off / kPoolTile64KB);
    out_sub.tile_pool_count_64kb = static_cast<uint32_t>(
        std::max<uint64_t>(1ull, (sub_size + kPoolTile64KB - 1ull) / kPoolTile64KB));
    return true;
}

bool GatherPlacedTiledPool64KB(
    const void* src_pool_base,
    uint64_t src_pool_size_bytes,
    uint64_t pool_base_offset_bytes,
    const std::vector<XgPlacedTilePoolEntry>& mappings,
    uint32_t tile_size_bytes,
    std::vector<uint8_t>& out_contiguous_tiled) {
    if (!src_pool_base || mappings.empty() || tile_size_bytes == 0u) {
        return false;
    }
    const auto* src = static_cast<const uint8_t*>(src_pool_base);
    uint32_t max_logical = 0u;
    for (const auto& m : mappings) {
        max_logical = std::max(max_logical, m.logical_tile_index);
    }
    const size_t total_out = static_cast<size_t>(max_logical + 1u) * static_cast<size_t>(tile_size_bytes);
    out_contiguous_tiled.assign(total_out, 0u);

    constexpr uint64_t kPoolTile64KB = 65536ull;
    const uint64_t tile_stride = (tile_size_bytes >= kPoolTile64KB) ? kPoolTile64KB : static_cast<uint64_t>(tile_size_bytes);

    for (const auto& m : mappings) {
        const size_t dst_off = static_cast<size_t>(m.logical_tile_index) * static_cast<size_t>(tile_size_bytes);
        if (dst_off + tile_size_bytes > out_contiguous_tiled.size()) {
            return false;
        }
        if (!m.mapped) {
            std::memset(out_contiguous_tiled.data() + dst_off, 0, tile_size_bytes);
            continue;
        }
        const uint64_t src_off = (m.pool_byte_offset != 0ull)
            ? m.pool_byte_offset
            : (pool_base_offset_bytes + static_cast<uint64_t>(m.heap_tile_index_64kb) * tile_stride);
        if (src_off + tile_size_bytes > src_pool_size_bytes) {
            return false;
        }
        std::memcpy(out_contiguous_tiled.data() + dst_off, src + src_off, tile_size_bytes);
    }
    return true;
}

bool ScatterPlacedTiledPool64KB(
    const void* src_contiguous_tiled,
    uint64_t src_size_bytes,
    uint64_t pool_base_offset_bytes,
    const std::vector<XgPlacedTilePoolEntry>& mappings,
    uint32_t tile_size_bytes,
    void* dst_pool_base,
    uint64_t dst_pool_size_bytes) {
    if (!src_contiguous_tiled || !dst_pool_base || mappings.empty() || tile_size_bytes == 0u) {
        return false;
    }
    const auto* src = static_cast<const uint8_t*>(src_contiguous_tiled);
    auto* dst = static_cast<uint8_t*>(dst_pool_base);
    constexpr uint64_t kPoolTile64KB = 65536ull;
    const uint64_t tile_stride = (tile_size_bytes >= kPoolTile64KB) ? kPoolTile64KB : static_cast<uint64_t>(tile_size_bytes);

    for (const auto& m : mappings) {
        if (!m.mapped) {
            continue;
        }
        const uint64_t src_off = static_cast<uint64_t>(m.logical_tile_index) * static_cast<uint64_t>(tile_size_bytes);
        if (src_off + tile_size_bytes > src_size_bytes) {
            return false;
        }
        const uint64_t dst_off = (m.pool_byte_offset != 0ull)
            ? m.pool_byte_offset
            : (pool_base_offset_bytes + static_cast<uint64_t>(m.heap_tile_index_64kb) * tile_stride);
        if (dst_off + tile_size_bytes > dst_pool_size_bytes) {
            return false;
        }
        std::memcpy(dst + dst_off, src + src_off, tile_size_bytes);
    }
    return true;
}

// ============================================================================
// Sprint 16: Live FMASK / DCC (2:1 & 4:1) / HTILE GPU Compute Resolve & Decompression
// ============================================================================

namespace {

constexpr uint32_t kResolveSpvHeaderWords = 16u;

inline uint32_t ReadU32Le(const std::vector<uint8_t>& buf, size_t byte_off) noexcept {
    if (byte_off + 4u > buf.size()) return 0u;
    uint32_t v = 0u;
    std::memcpy(&v, buf.data() + byte_off, 4u);
    return v;
}

inline void WriteU32Le(std::vector<uint8_t>& buf, size_t byte_off, uint32_t v) noexcept {
    if (byte_off + 4u > buf.size()) return;
    std::memcpy(buf.data() + byte_off, &v, 4u);
}

inline uint32_t ApplyBiasedDeltaU32(
    uint32_t base_ch,
    uint32_t raw_delta,
    uint32_t bias,
    uint32_t shift,
    uint32_t max_val) noexcept {
    const uint32_t pos = base_ch + (raw_delta << shift);
    const uint32_t neg = bias << shift;
    if (pos < neg) return 0u;
    const uint32_t diff = pos - neg;
    return (diff > max_val) ? max_val : diff;
}

inline uint32_t FloatBitsFromZ24(uint32_t z24) noexcept {
    // 1.0f / 16777216.0f = 2^-24: exact exponent subtraction for every 24-bit integer [0..0xFFFFFF]
    // with zero mantissa rounding error across x86 CPU, D3D12 HLSL, and Vulkan SPIR-V.
    const float f = static_cast<float>(z24) * (1.0f / 16777216.0f);
    uint32_t bits = 0u;
    std::memcpy(&bits, &f, 4u);
    return bits;
}

// Minimal self-contained SPIR-V 1.3 builder for FMASK / DCC / HTILE resolve compute shader.
class SpvResolveBuilder {
public:
    enum : uint16_t {
        OpCapability = 17,
        OpMemoryModel = 14,
        OpEntryPoint = 15,
        OpExecutionMode = 16,
        OpTypeVoid = 19,
        OpTypeBool = 20,
        OpTypeInt = 21,
        OpTypeFloat = 22,
        OpTypeVector = 23,
        OpTypeRuntimeArray = 29,
        OpTypeStruct = 30,
        OpTypePointer = 32,
        OpTypeFunction = 33,
        OpConstant = 43,
        OpFunction = 54,
        OpFunctionParameter = 55,
        OpFunctionEnd = 56,
        OpFunctionCall = 57,
        OpVariable = 59,
        OpLoad = 61,
        OpStore = 62,
        OpAccessChain = 65,
        OpDecorate = 71,
        OpMemberDecorate = 72,
        OpConvertUToF = 112,
        OpBitcast = 124,
        OpIAdd = 128,
        OpISub = 130,
        OpIMul = 132,
        OpFMul = 133,
        OpUDiv = 134,
        OpUMod = 137,
        OpLogicalOr = 166,
        OpLogicalAnd = 167,
        OpLogicalNot = 168,
        OpSelect = 169,
        OpIEqual = 170,
        OpINotEqual = 171,
        OpUGreaterThan = 172,
        OpUGreaterThanEqual = 174,
        OpULessThan = 176,
        OpULessThanEqual = 178,
        OpShiftRightLogical = 194,
        OpShiftLeftLogical = 196,
        OpBitwiseOr = 197,
        OpBitwiseXor = 198,
        OpBitwiseAnd = 199,
        OpLoopMerge = 246,
        OpSelectionMerge = 247,
        OpLabel = 248,
        OpBranch = 249,
        OpBranchConditional = 250,
        OpReturn = 253,
        OpReturnValue = 254,
    };

    uint32_t NewId() { return next_id_++; }

    static void Emit(std::vector<uint32_t>& dst, uint16_t op, std::initializer_list<uint32_t> ops) {
        const uint32_t wc = static_cast<uint32_t>(ops.size() + 1u);
        dst.push_back((wc << 16u) | static_cast<uint32_t>(op));
        dst.insert(dst.end(), ops.begin(), ops.end());
    }

    static void EmitVec(std::vector<uint32_t>& dst, uint16_t op, const std::vector<uint32_t>& ops) {
        const uint32_t wc = static_cast<uint32_t>(ops.size() + 1u);
        dst.push_back((wc << 16u) | static_cast<uint32_t>(op));
        dst.insert(dst.end(), ops.begin(), ops.end());
    }

    uint32_t ConstU32(uint32_t v) {
        auto it = u32_consts_.find(v);
        if (it != u32_consts_.end()) return it->second;
        const uint32_t id = NewId();
        Emit(types_consts_, OpConstant, {ty_u32_, id, v});
        u32_consts_[v] = id;
        return id;
    }

    uint32_t Bin(std::vector<uint32_t>& b, uint16_t op, uint32_t ty, uint32_t a, uint32_t c) {
        const uint32_t r = NewId();
        Emit(b, op, {ty, r, a, c});
        return r;
    }

    uint32_t Un(std::vector<uint32_t>& b, uint16_t op, uint32_t ty, uint32_t a) {
        const uint32_t r = NewId();
        Emit(b, op, {ty, r, a});
        return r;
    }

    uint32_t Sel(std::vector<uint32_t>& b, uint32_t cond, uint32_t t_val, uint32_t f_val) {
        const uint32_t r = NewId();
        Emit(b, OpSelect, {ty_u32_, r, cond, t_val, f_val});
        return r;
    }

    uint32_t LoadIn(std::vector<uint32_t>& b, uint32_t idx) {
        const uint32_t ptr = NewId();
        Emit(b, OpAccessChain, {ty_ptr_sb_u32_, ptr, var_in_, ConstU32(0u), idx});
        const uint32_t val = NewId();
        Emit(b, OpLoad, {ty_u32_, val, ptr});
        return val;
    }

    void StoreOut(std::vector<uint32_t>& b, uint32_t idx, uint32_t val) {
        const uint32_t ptr = NewId();
        Emit(b, OpAccessChain, {ty_ptr_sb_u32_, ptr, var_out_, ConstU32(0u), idx});
        Emit(b, OpStore, {ptr, val});
    }

    uint32_t LoadVar(std::vector<uint32_t>& b, uint32_t var_ptr) {
        const uint32_t val = NewId();
        Emit(b, OpLoad, {ty_u32_, val, var_ptr});
        return val;
    }

    void StoreVar(std::vector<uint32_t>& b, uint32_t var_ptr, uint32_t val) {
        Emit(b, OpStore, {var_ptr, val});
    }

    uint32_t FuncVar(std::vector<uint32_t>& b) {
        const uint32_t v = NewId();
        Emit(b, OpVariable, {ty_ptr_fn_u32_, v, 7u});
        return v;
    }

    // uint ReadByte(uint base_word_off, uint byte_off)
    uint32_t BuildReadByteFunc() {
        const uint32_t ty_fn = NewId();
        Emit(types_consts_, OpTypeFunction, {ty_fn, ty_u32_, ty_u32_, ty_u32_});
        const uint32_t fn = NewId();
        Emit(funcs_, OpFunction, {ty_u32_, fn, 0u, ty_fn});
        const uint32_t p_base = NewId(), p_boff = NewId();
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_base});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_boff});
        Emit(funcs_, OpLabel, {NewId()});
        const uint32_t widx = Bin(funcs_, OpIAdd, ty_u32_, p_base,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, p_boff, ConstU32(2u)));
        const uint32_t w = LoadIn(funcs_, widx);
        const uint32_t sh = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                Bin(funcs_, OpBitwiseAnd, ty_u32_, p_boff, ConstU32(3u)), ConstU32(3u));
        const uint32_t res = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_, w, sh), ConstU32(0xFFu));
        Emit(funcs_, OpReturnValue, {res});
        Emit(funcs_, OpFunctionEnd, {});
        return fn;
    }

    // uint ApplyDelta(uint base_ch, uint raw_delta, uint bias, uint shift, uint max_val)
    uint32_t BuildApplyDeltaFunc() {
        const uint32_t ty_fn = NewId();
        Emit(types_consts_, OpTypeFunction,
             {ty_fn, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_, ty_u32_});
        const uint32_t fn = NewId();
        Emit(funcs_, OpFunction, {ty_u32_, fn, 0u, ty_fn});
        const uint32_t p_base = NewId(), p_raw = NewId(), p_bias = NewId();
        const uint32_t p_sh = NewId(), p_max = NewId();
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_base});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_raw});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_bias});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_sh});
        Emit(funcs_, OpFunctionParameter, {ty_u32_, p_max});
        Emit(funcs_, OpLabel, {NewId()});
        const uint32_t pos = Bin(funcs_, OpIAdd, ty_u32_, p_base,
                                 Bin(funcs_, OpShiftLeftLogical, ty_u32_, p_raw, p_sh));
        const uint32_t neg = Bin(funcs_, OpShiftLeftLogical, ty_u32_, p_bias, p_sh);
        const uint32_t under = Bin(funcs_, OpULessThan, ty_bool_, pos, neg);
        const uint32_t diff = Bin(funcs_, OpISub, ty_u32_, pos, neg);
        const uint32_t over = Bin(funcs_, OpUGreaterThan, ty_bool_, diff, p_max);
        const uint32_t clamped = Sel(funcs_, over, p_max, diff);
        const uint32_t res = Sel(funcs_, under, ConstU32(0u), clamped);
        Emit(funcs_, OpReturnValue, {res});
        Emit(funcs_, OpFunctionEnd, {});
        return fn;
    }

    std::vector<uint32_t> Build() {
        ty_void_ = NewId();
        ty_bool_ = NewId();
        ty_u32_ = NewId();
        ty_f32_ = NewId();
        ty_v3u32_ = NewId();
        ty_rt_u32_ = NewId();
        ty_buf_struct_ = NewId();
        ty_ptr_sb_struct_ = NewId();
        ty_ptr_sb_u32_ = NewId();
        ty_ptr_in_v3u32_ = NewId();
        ty_ptr_in_u32_ = NewId();
        ty_ptr_fn_u32_ = NewId();
        ty_main_fn_ = NewId();

        Emit(types_consts_, OpTypeVoid, {ty_void_});
        Emit(types_consts_, OpTypeBool, {ty_bool_});
        Emit(types_consts_, OpTypeInt, {ty_u32_, 32u, 0u});
        Emit(types_consts_, OpTypeFloat, {ty_f32_, 32u});
        Emit(types_consts_, OpTypeVector, {ty_v3u32_, ty_u32_, 3u});
        Emit(types_consts_, OpTypeRuntimeArray, {ty_rt_u32_, ty_u32_});
        Emit(types_consts_, OpTypeStruct, {ty_buf_struct_, ty_rt_u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_sb_struct_, 12u, ty_buf_struct_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_sb_u32_, 12u, ty_u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_in_v3u32_, 1u, ty_v3u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_in_u32_, 1u, ty_u32_});
        Emit(types_consts_, OpTypePointer, {ty_ptr_fn_u32_, 7u, ty_u32_});
        Emit(types_consts_, OpTypeFunction, {ty_main_fn_, ty_void_});

        const uint32_t c_inv_2_24 = NewId();
        Emit(types_consts_, OpConstant, {ty_f32_, c_inv_2_24, 0x33800000u});  // 2^-24

        var_in_ = NewId();
        var_out_ = NewId();
        var_gid_ = NewId();
        Emit(types_consts_, OpVariable, {ty_ptr_sb_struct_, var_in_, 12u});
        Emit(types_consts_, OpVariable, {ty_ptr_sb_struct_, var_out_, 12u});
        Emit(types_consts_, OpVariable, {ty_ptr_in_v3u32_, var_gid_, 1u});

        Emit(decorations_, OpDecorate, {ty_rt_u32_, 6u, 4u});
        Emit(decorations_, OpDecorate, {ty_buf_struct_, 2u});
        Emit(decorations_, OpMemberDecorate, {ty_buf_struct_, 0u, 35u, 0u});
        Emit(decorations_, OpDecorate, {var_in_, 34u, 0u});
        Emit(decorations_, OpDecorate, {var_in_, 33u, 0u});
        Emit(decorations_, OpDecorate, {var_in_, 24u});
        Emit(decorations_, OpDecorate, {var_out_, 34u, 0u});
        Emit(decorations_, OpDecorate, {var_out_, 33u, 1u});
        Emit(decorations_, OpDecorate, {var_gid_, 11u, 28u});

        const uint32_t fn_read_byte = BuildReadByteFunc();
        const uint32_t fn_apply_delta = BuildApplyDeltaFunc();

        const uint32_t main_fn = NewId();
        Emit(funcs_, OpFunction, {ty_void_, main_fn, 0u, ty_main_fn_});

        const uint32_t l_entry = NewId();
        Emit(funcs_, OpLabel, {l_entry});

        // All Function-storage variables MUST be declared at the top of the entry block.
        const uint32_t v_s = FuncVar(funcs_);
        const uint32_t v_sr = FuncVar(funcs_);
        const uint32_t v_sg = FuncVar(funcs_);
        const uint32_t v_sb = FuncVar(funcs_);
        const uint32_t v_sa = FuncVar(funcs_);
        const uint32_t v_centroid = FuncVar(funcs_);
        const uint32_t v_has_valid = FuncVar(funcs_);
        const uint32_t v_out_word = FuncVar(funcs_);

        // GlobalInvocationID.x
        const uint32_t gid_x_ptr = NewId();
        Emit(funcs_, OpAccessChain, {ty_ptr_in_u32_, gid_x_ptr, var_gid_, ConstU32(0u)});
        const uint32_t tid = NewId();
        Emit(funcs_, OpLoad, {ty_u32_, tid, gid_x_ptr});

        // Load XgGpuResolveParams header (16 dwords)
        const uint32_t h_op = LoadIn(funcs_, ConstU32(0u));
        const uint32_t h_w = LoadIn(funcs_, ConstU32(1u));
        const uint32_t h_h = LoadIn(funcs_, ConstU32(2u));
        const uint32_t h_samples = LoadIn(funcs_, ConstU32(3u));
        const uint32_t h_mode = LoadIn(funcs_, ConstU32(4u));
        const uint32_t h_dst_pitch = LoadIn(funcs_, ConstU32(7u));
        const uint32_t h_meta_bx = LoadIn(funcs_, ConstU32(8u));
        const uint32_t h_pipes = LoadIn(funcs_, ConstU32(9u));
        const uint32_t h_meta_pitch = LoadIn(funcs_, ConstU32(10u));
        const uint32_t h_clear0 = LoadIn(funcs_, ConstU32(11u));
        const uint32_t h_clear1 = LoadIn(funcs_, ConstU32(12u));
        const uint32_t h_src_woff = LoadIn(funcs_, ConstU32(13u));
        const uint32_t h_meta_woff = LoadIn(funcs_, ConstU32(14u));
        const uint32_t h_aux_woff = LoadIn(funcs_, ConstU32(15u));

        const uint32_t total = Bin(funcs_, OpIMul, ty_u32_, h_w, h_h);
        const uint32_t in_bounds = Bin(funcs_, OpULessThan, ty_bool_, tid, total);
        const uint32_t l_body = NewId(), l_done = NewId();
        Emit(funcs_, OpSelectionMerge, {l_done, 0u});
        Emit(funcs_, OpBranchConditional, {in_bounds, l_body, l_done});

        Emit(funcs_, OpLabel, {l_body});
        const uint32_t y = Bin(funcs_, OpUDiv, ty_u32_, tid, h_w);
        const uint32_t x = Bin(funcs_, OpUMod, ty_u32_, tid, h_w);
        const uint32_t dst_stride_w = Bin(funcs_, OpShiftRightLogical, ty_u32_, h_dst_pitch, ConstU32(2u));
        const uint32_t dst_pix_idx = Bin(funcs_, OpIAdd, ty_u32_,
                                         Bin(funcs_, OpIMul, ty_u32_, y, dst_stride_w), x);

        const uint32_t is_fmask = Bin(funcs_, OpIEqual, ty_bool_, h_op, ConstU32(0u));
        const uint32_t l_fmask = NewId(), l_not_fmask = NewId(), l_after_op = NewId();
        Emit(funcs_, OpSelectionMerge, {l_after_op, 0u});
        Emit(funcs_, OpBranchConditional, {is_fmask, l_fmask, l_not_fmask});

        // ---------------- OP 0: FMASK MSAA Resolve & Decompress ----------------
        Emit(funcs_, OpLabel, {l_fmask});
        const uint32_t fmask_word = LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, h_meta_woff, tid));
        const uint32_t base_pix_samp = Bin(funcs_, OpIMul, ty_u32_, tid, h_samples);
        StoreVar(funcs_, v_s, ConstU32(0u));
        StoreVar(funcs_, v_sr, ConstU32(0u));
        StoreVar(funcs_, v_sg, ConstU32(0u));
        StoreVar(funcs_, v_sb, ConstU32(0u));
        StoreVar(funcs_, v_sa, ConstU32(0u));
        StoreVar(funcs_, v_centroid, h_clear0);
        StoreVar(funcs_, v_has_valid, ConstU32(0u));

        const uint32_t l_floop_hdr = NewId(), l_floop_body = NewId();
        const uint32_t l_floop_cont = NewId(), l_floop_exit = NewId();
        Emit(funcs_, OpBranch, {l_floop_hdr});

        Emit(funcs_, OpLabel, {l_floop_hdr});
        const uint32_t cur_s = LoadVar(funcs_, v_s);
        const uint32_t s_lt = Bin(funcs_, OpULessThan, ty_bool_, cur_s, h_samples);
        Emit(funcs_, OpLoopMerge, {l_floop_exit, l_floop_cont, 0u});
        Emit(funcs_, OpBranchConditional, {s_lt, l_floop_body, l_floop_exit});

        Emit(funcs_, OpLabel, {l_floop_body});
        const uint32_t s_sh = Bin(funcs_, OpShiftLeftLogical, ty_u32_, cur_s, ConstU32(2u));
        const uint32_t fidx = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, fmask_word, s_sh), ConstU32(0xFu));
        const uint32_t is_ge = Bin(funcs_, OpUGreaterThanEqual, ty_bool_, fidx, h_samples);
        const uint32_t is_e = Bin(funcs_, OpIEqual, ty_bool_, fidx, ConstU32(0xEu));
        const uint32_t is_f = Bin(funcs_, OpIEqual, ty_bool_, fidx, ConstU32(0xFu));
        const uint32_t is_sent = Bin(funcs_, OpLogicalOr, ty_bool_, is_ge,
                                     Bin(funcs_, OpLogicalOr, ty_bool_, is_e, is_f));
        const uint32_t safe_fidx = Sel(funcs_, is_sent, ConstU32(0u), fidx);
        const uint32_t src_col = LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, h_src_woff,
                                                    Bin(funcs_, OpIAdd, ty_u32_, base_pix_samp, safe_fidx)));
        const uint32_t scol = Sel(funcs_, is_sent, h_clear0, src_col);

        const uint32_t cr = Bin(funcs_, OpBitwiseAnd, ty_u32_, scol, ConstU32(0xFFu));
        const uint32_t cg = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                Bin(funcs_, OpShiftRightLogical, ty_u32_, scol, ConstU32(8u)), ConstU32(0xFFu));
        const uint32_t cb = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                Bin(funcs_, OpShiftRightLogical, ty_u32_, scol, ConstU32(16u)), ConstU32(0xFFu));
        const uint32_t ca = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                Bin(funcs_, OpShiftRightLogical, ty_u32_, scol, ConstU32(24u)), ConstU32(0xFFu));

        StoreVar(funcs_, v_sr, Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sr), cr));
        StoreVar(funcs_, v_sg, Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sg), cg));
        StoreVar(funcs_, v_sb, Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sb), cb));
        StoreVar(funcs_, v_sa, Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sa), ca));

        const uint32_t hv_prev = LoadVar(funcs_, v_has_valid);
        const uint32_t take_c = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                    Bin(funcs_, OpIEqual, ty_bool_, hv_prev, ConstU32(0u)),
                                    Un(funcs_, OpLogicalNot, ty_bool_, is_sent));
        StoreVar(funcs_, v_centroid, Sel(funcs_, take_c, scol, LoadVar(funcs_, v_centroid)));
        StoreVar(funcs_, v_has_valid, Sel(funcs_, take_c, ConstU32(1u), hv_prev));

        const uint32_t is_persamp = Bin(funcs_, OpIEqual, ty_bool_, h_mode, ConstU32(2u));
        const uint32_t l_ps_wr = NewId(), l_ps_end = NewId();
        Emit(funcs_, OpSelectionMerge, {l_ps_end, 0u});
        Emit(funcs_, OpBranchConditional, {is_persamp, l_ps_wr, l_ps_end});
        Emit(funcs_, OpLabel, {l_ps_wr});
        StoreOut(funcs_, Bin(funcs_, OpIAdd, ty_u32_, base_pix_samp, cur_s), scol);
        Emit(funcs_, OpBranch, {l_ps_end});
        Emit(funcs_, OpLabel, {l_ps_end});
        Emit(funcs_, OpBranch, {l_floop_cont});

        Emit(funcs_, OpLabel, {l_floop_cont});
        StoreVar(funcs_, v_s, Bin(funcs_, OpIAdd, ty_u32_, cur_s, ConstU32(1u)));
        Emit(funcs_, OpBranch, {l_floop_hdr});

        Emit(funcs_, OpLabel, {l_floop_exit});
        const uint32_t not_ps = Bin(funcs_, OpINotEqual, ty_bool_, h_mode, ConstU32(2u));
        const uint32_t l_fm_wr = NewId(), l_fm_end = NewId();
        Emit(funcs_, OpSelectionMerge, {l_fm_end, 0u});
        Emit(funcs_, OpBranchConditional, {not_ps, l_fm_wr, l_fm_end});

        Emit(funcs_, OpLabel, {l_fm_wr});
        const uint32_t half_s = Bin(funcs_, OpShiftRightLogical, ty_u32_, h_samples, ConstU32(1u));
        const uint32_t ar = Bin(funcs_, OpUDiv, ty_u32_,
                                Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sr), half_s), h_samples);
        const uint32_t ag = Bin(funcs_, OpUDiv, ty_u32_,
                                Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sg), half_s), h_samples);
        const uint32_t ab = Bin(funcs_, OpUDiv, ty_u32_,
                                Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sb), half_s), h_samples);
        const uint32_t aa = Bin(funcs_, OpUDiv, ty_u32_,
                                Bin(funcs_, OpIAdd, ty_u32_, LoadVar(funcs_, v_sa), half_s), h_samples);
        const uint32_t avg_col = Bin(funcs_, OpBitwiseOr, ty_u32_, ar,
                                     Bin(funcs_, OpBitwiseOr, ty_u32_,
                                         Bin(funcs_, OpShiftLeftLogical, ty_u32_, ag, ConstU32(8u)),
                                         Bin(funcs_, OpBitwiseOr, ty_u32_,
                                             Bin(funcs_, OpShiftLeftLogical, ty_u32_, ab, ConstU32(16u)),
                                             Bin(funcs_, OpShiftLeftLogical, ty_u32_, aa, ConstU32(24u)))));
        const uint32_t is_cent = Bin(funcs_, OpIEqual, ty_bool_, h_mode, ConstU32(1u));
        const uint32_t fm_final = Sel(funcs_, is_cent, LoadVar(funcs_, v_centroid), avg_col);
        StoreOut(funcs_, dst_pix_idx, fm_final);
        Emit(funcs_, OpBranch, {l_fm_end});

        Emit(funcs_, OpLabel, {l_fm_end});
        Emit(funcs_, OpBranch, {l_after_op});

        // ---------------- OP 1 (DCC) vs OP 2 (HTILE) ----------------
        Emit(funcs_, OpLabel, {l_not_fmask});
        const uint32_t bx = Bin(funcs_, OpShiftRightLogical, ty_u32_, x, ConstU32(3u));
        const uint32_t by = Bin(funcs_, OpShiftRightLogical, ty_u32_, y, ConstU32(3u));
        const uint32_t lx = Bin(funcs_, OpBitwiseAnd, ty_u32_, x, ConstU32(7u));
        const uint32_t ly = Bin(funcs_, OpBitwiseAnd, ty_u32_, y, ConstU32(7u));
        const uint32_t pix_in_blk = Bin(funcs_, OpBitwiseOr, ty_u32_,
                                        Bin(funcs_, OpShiftLeftLogical, ty_u32_, ly, ConstU32(3u)), lx);
        const uint32_t blk_idx = Bin(funcs_, OpIAdd, ty_u32_,
                                     Bin(funcs_, OpIMul, ty_u32_, by, h_meta_bx), bx);
        const uint32_t blk_wbase = Bin(funcs_, OpIAdd, ty_u32_, h_src_woff,
                                       Bin(funcs_, OpShiftLeftLogical, ty_u32_, blk_idx, ConstU32(6u)));

        const uint32_t is_dcc = Bin(funcs_, OpIEqual, ty_bool_, h_op, ConstU32(1u));
        const uint32_t l_dcc = NewId(), l_htile = NewId(), l_after_dh = NewId();
        Emit(funcs_, OpSelectionMerge, {l_after_dh, 0u});
        Emit(funcs_, OpBranchConditional, {is_dcc, l_dcc, l_htile});

        // ---------------- OP 1: DCC Compressed Block Decompression ----------------
        Emit(funcs_, OpLabel, {l_dcc});
        const uint32_t pa_on = Bin(funcs_, OpINotEqual, ty_bool_,
                                   Bin(funcs_, OpBitwiseAnd, ty_u32_, h_mode, ConstU32(1u)), ConstU32(0u));
        const uint32_t pitch_ok = Bin(funcs_, OpUGreaterThanEqual, ty_bool_, h_meta_pitch, ConstU32(256u));
        const uint32_t use_pa = Bin(funcs_, OpLogicalAnd, ty_bool_, pa_on, pitch_ok);
        const uint32_t pmask = Sel(funcs_, Bin(funcs_, OpUGreaterThan, ty_bool_, h_pipes, ConstU32(1u)),
                                   Bin(funcs_, OpISub, ty_u32_, h_pipes, ConstU32(1u)), ConstU32(0u));
        const uint32_t xorb = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpBitwiseXor, ty_u32_,
                                      Bin(funcs_, OpShiftRightLogical, ty_u32_, bx, ConstU32(2u)),
                                      Bin(funcs_, OpShiftRightLogical, ty_u32_, by, ConstU32(2u))),
                                  pmask);
        const uint32_t cand_moff = Bin(funcs_, OpBitwiseXor, ty_u32_, blk_idx, xorb);
        const uint32_t cand_ok = Bin(funcs_, OpLogicalAnd, ty_bool_, use_pa,
                                     Bin(funcs_, OpULessThan, ty_bool_, cand_moff, h_meta_pitch));
        const uint32_t moff = Sel(funcs_, cand_ok, cand_moff, blk_idx);

        const uint32_t dcc_key = NewId();
        Emit(funcs_, OpFunctionCall, {ty_u32_, dcc_key, fn_read_byte, h_meta_woff, moff});

        // Default uncompressed fallback
        StoreVar(funcs_, v_out_word, LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase, pix_in_blk)));

        // Fast-clear sentinels
        const uint32_t k00 = Bin(funcs_, OpIEqual, ty_bool_, dcc_key, ConstU32(0x00u));
        const uint32_t k40 = Bin(funcs_, OpIEqual, ty_bool_, dcc_key, ConstU32(0x40u));
        const uint32_t k80 = Bin(funcs_, OpIEqual, ty_bool_, dcc_key, ConstU32(0x80u));
        const uint32_t kc0 = Bin(funcs_, OpIEqual, ty_bool_, dcc_key, ConstU32(0xC0u));
        const uint32_t k20 = Bin(funcs_, OpIEqual, ty_bool_, dcc_key, ConstU32(0x20u));

        const uint32_t is_4to1 = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                     Bin(funcs_, OpUGreaterThanEqual, ty_bool_, dcc_key, ConstU32(0x02u)),
                                     Bin(funcs_, OpULessThanEqual, ty_bool_, dcc_key, ConstU32(0x07u)));
        const uint32_t is_2to1 = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                     Bin(funcs_, OpUGreaterThanEqual, ty_bool_, dcc_key, ConstU32(0x08u)),
                                     Bin(funcs_, OpULessThanEqual, ty_bool_, dcc_key, ConstU32(0x12u)));
        const uint32_t is_comp = Bin(funcs_, OpLogicalOr, ty_bool_, is_4to1, is_2to1);

        const uint32_t l_dcc_comp = NewId(), l_dcc_after_comp = NewId();
        Emit(funcs_, OpSelectionMerge, {l_dcc_after_comp, 0u});
        Emit(funcs_, OpBranchConditional, {is_comp, l_dcc_comp, l_dcc_after_comp});

        Emit(funcs_, OpLabel, {l_dcc_comp});
        const uint32_t base_rgba = LoadIn(funcs_, blk_wbase);
        const uint32_t sh_word = LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase, ConstU32(1u)));
        const uint32_t r0 = Bin(funcs_, OpBitwiseAnd, ty_u32_, base_rgba, ConstU32(0xFFu));
        const uint32_t g0 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                Bin(funcs_, OpShiftRightLogical, ty_u32_, base_rgba, ConstU32(8u)), ConstU32(0xFFu));
        const uint32_t b0 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                Bin(funcs_, OpShiftRightLogical, ty_u32_, base_rgba, ConstU32(16u)), ConstU32(0xFFu));
        const uint32_t a0 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                Bin(funcs_, OpShiftRightLogical, ty_u32_, base_rgba, ConstU32(24u)), ConstU32(0xFFu));
        const uint32_t sh_r = Bin(funcs_, OpBitwiseAnd, ty_u32_, sh_word, ConstU32(0xFFu));
        const uint32_t sh_g = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, sh_word, ConstU32(8u)), ConstU32(0xFFu));
        const uint32_t sh_b = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, sh_word, ConstU32(16u)), ConstU32(0xFFu));
        const uint32_t sh_a = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                  Bin(funcs_, OpShiftRightLogical, ty_u32_, sh_word, ConstU32(24u)), ConstU32(0xFFu));

        const uint32_t w4 = Bin(funcs_, OpShiftRightLogical, ty_u32_, pix_in_blk, ConstU32(3u));
        const uint32_t b4 = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                Bin(funcs_, OpBitwiseAnd, ty_u32_, pix_in_blk, ConstU32(7u)), ConstU32(2u));
        const uint32_t w2 = Bin(funcs_, OpShiftRightLogical, ty_u32_, pix_in_blk, ConstU32(4u));
        const uint32_t b2 = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                Bin(funcs_, OpBitwiseAnd, ty_u32_, pix_in_blk, ConstU32(15u)), ConstU32(1u));
        const uint32_t w1 = Bin(funcs_, OpShiftRightLogical, ty_u32_, pix_in_blk, ConstU32(5u));
        const uint32_t b1 = Bin(funcs_, OpBitwiseAnd, ty_u32_, pix_in_blk, ConstU32(31u));

        // 2:1 raw deltas
        const uint32_t dr2 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(2u), w4))),
                                     b4),
                                 ConstU32(0xFu));
        const uint32_t dg2 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(10u), w4))),
                                     b4),
                                 ConstU32(0xFu));
        const uint32_t db2 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(18u), w4))),
                                     b4),
                                 ConstU32(0xFu));
        const uint32_t da2 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(26u), w2))),
                                     b2),
                                 ConstU32(0x3u));

        // 4:1 raw deltas
        const uint32_t dr4 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(2u), w2))),
                                     b2),
                                 ConstU32(0x3u));
        const uint32_t dg4 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(6u), w2))),
                                     b2),
                                 ConstU32(0x3u));
        const uint32_t db4 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(10u), w2))),
                                     b2),
                                 ConstU32(0x3u));
        const uint32_t da4 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                 Bin(funcs_, OpShiftRightLogical, ty_u32_,
                                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                                        Bin(funcs_, OpIAdd, ty_u32_, ConstU32(14u), w1))),
                                     b1),
                                 ConstU32(0x1u));

        const uint32_t dr_sel = Sel(funcs_, is_4to1, dr4, dr2);
        const uint32_t dg_sel = Sel(funcs_, is_4to1, dg4, dg2);
        const uint32_t db_sel = Sel(funcs_, is_4to1, db4, db2);
        const uint32_t da_sel = Sel(funcs_, is_4to1, da4, da2);
        const uint32_t rgb_bias = Sel(funcs_, is_4to1, ConstU32(2u), ConstU32(8u));
        const uint32_t a_bias = Sel(funcs_, is_4to1, ConstU32(0u), ConstU32(2u));

        const uint32_t rr = NewId(), gg = NewId(), bb = NewId(), aa_d = NewId();
        Emit(funcs_, OpFunctionCall, {ty_u32_, rr, fn_apply_delta, r0, dr_sel, rgb_bias, sh_r, ConstU32(255u)});
        Emit(funcs_, OpFunctionCall, {ty_u32_, gg, fn_apply_delta, g0, dg_sel, rgb_bias, sh_g, ConstU32(255u)});
        Emit(funcs_, OpFunctionCall, {ty_u32_, bb, fn_apply_delta, b0, db_sel, rgb_bias, sh_b, ConstU32(255u)});
        Emit(funcs_, OpFunctionCall, {ty_u32_, aa_d, fn_apply_delta, a0, da_sel, a_bias, sh_a, ConstU32(255u)});
        const uint32_t dec_col = Bin(funcs_, OpBitwiseOr, ty_u32_, rr,
                                     Bin(funcs_, OpBitwiseOr, ty_u32_,
                                         Bin(funcs_, OpShiftLeftLogical, ty_u32_, gg, ConstU32(8u)),
                                         Bin(funcs_, OpBitwiseOr, ty_u32_,
                                             Bin(funcs_, OpShiftLeftLogical, ty_u32_, bb, ConstU32(16u)),
                                             Bin(funcs_, OpShiftLeftLogical, ty_u32_, aa_d, ConstU32(24u)))));
        StoreVar(funcs_, v_out_word, dec_col);
        Emit(funcs_, OpBranch, {l_dcc_after_comp});

        Emit(funcs_, OpLabel, {l_dcc_after_comp});
        uint32_t dcc_col = LoadVar(funcs_, v_out_word);
        dcc_col = Sel(funcs_, k00, ConstU32(0x00000000u), dcc_col);
        dcc_col = Sel(funcs_, k40, ConstU32(0xFF000000u), dcc_col);
        dcc_col = Sel(funcs_, k80, ConstU32(0x00FFFFFFu), dcc_col);
        dcc_col = Sel(funcs_, kc0, ConstU32(0xFFFFFFFFu), dcc_col);
        dcc_col = Sel(funcs_, k20, h_clear0, dcc_col);
        StoreOut(funcs_, dst_pix_idx, dcc_col);
        Emit(funcs_, OpBranch, {l_after_dh});

        // ---------------- OP 2: HTILE Depth/Stencil Decompression & HiZ Bounds ----------------
        Emit(funcs_, OpLabel, {l_htile});
        const uint32_t hword = LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, h_meta_woff, blk_idx));
        const uint32_t zmask = Bin(funcs_, OpBitwiseAnd, ty_u32_, hword, ConstU32(0xFu));
        const uint32_t is_d24s8 = Bin(funcs_, OpINotEqual, ty_bool_,
                                      Bin(funcs_, OpBitwiseAnd, ty_u32_, h_mode, ConstU32(2u)), ConstU32(0u));
        const uint32_t clear_d24s8 = Bin(funcs_, OpBitwiseOr, ty_u32_,
                                         Bin(funcs_, OpBitwiseAnd, ty_u32_, h_clear0, ConstU32(0x00FFFFFFu)),
                                         Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                             Bin(funcs_, OpBitwiseAnd, ty_u32_, h_clear1, ConstU32(0xFFu)),
                                             ConstU32(24u)));
        const uint32_t clear_depth_w = Sel(funcs_, is_d24s8, clear_d24s8, h_clear0);

        const uint32_t is_zclear = Bin(funcs_, OpIEqual, ty_bool_, zmask, ConstU32(0u));
        const uint32_t is_zexp = Bin(funcs_, OpIEqual, ty_bool_, zmask, ConstU32(15u));
        const uint32_t is_zcomp = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                      Un(funcs_, OpLogicalNot, ty_bool_, is_zclear),
                                      Un(funcs_, OpLogicalNot, ty_bool_, is_zexp));

        StoreVar(funcs_, v_out_word,
                 Sel(funcs_, is_zclear, clear_depth_w,
                     LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase, pix_in_blk))));

        const uint32_t l_ht_comp = NewId(), l_ht_after_comp = NewId();
        Emit(funcs_, OpSelectionMerge, {l_ht_after_comp, 0u});
        Emit(funcs_, OpBranchConditional, {is_zcomp, l_ht_comp, l_ht_after_comp});

        Emit(funcs_, OpLabel, {l_ht_comp});
        const uint32_t hdr0 = LoadIn(funcs_, blk_wbase);
        const uint32_t hdr1 = LoadIn(funcs_, Bin(funcs_, OpIAdd, ty_u32_, blk_wbase, ConstU32(1u)));
        const uint32_t base_z = Bin(funcs_, OpBitwiseAnd, ty_u32_, hdr0, ConstU32(0x00FFFFFFu));
        const uint32_t stencil = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                     Bin(funcs_, OpShiftRightLogical, ty_u32_, hdr0, ConstU32(24u)), ConstU32(0xFFu));
        const uint32_t raw_dx = Bin(funcs_, OpBitwiseAnd, ty_u32_, hdr1, ConstU32(0xFFFFu));
        const uint32_t raw_dy = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                    Bin(funcs_, OpShiftRightLogical, ty_u32_, hdr1, ConstU32(16u)), ConstU32(0xFFFFu));
        const uint32_t rw_idx = Bin(funcs_, OpIAdd, ty_u32_, blk_wbase,
                                    Bin(funcs_, OpIAdd, ty_u32_, ConstU32(2u),
                                        Bin(funcs_, OpShiftRightLogical, ty_u32_, pix_in_blk, ConstU32(3u))));
        const uint32_t rshift_bit = Bin(funcs_, OpShiftLeftLogical, ty_u32_,
                                        Bin(funcs_, OpBitwiseAnd, ty_u32_, pix_in_blk, ConstU32(7u)), ConstU32(2u));
        const uint32_t raw_res = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                     Bin(funcs_, OpShiftRightLogical, ty_u32_, LoadIn(funcs_, rw_idx), rshift_bit),
                                     ConstU32(0xFu));
        const uint32_t rshift = Bin(funcs_, OpISub, ty_u32_, zmask, ConstU32(1u));
        const uint32_t pos_z = Bin(funcs_, OpIAdd, ty_u32_, base_z,
                                   Bin(funcs_, OpIAdd, ty_u32_,
                                       Bin(funcs_, OpIAdd, ty_u32_,
                                           Bin(funcs_, OpIMul, ty_u32_, lx, raw_dx),
                                           Bin(funcs_, OpIMul, ty_u32_, ly, raw_dy)),
                                       Bin(funcs_, OpShiftLeftLogical, ty_u32_, raw_res, rshift)));
        const uint32_t neg_z = Bin(funcs_, OpIAdd, ty_u32_,
                                   Bin(funcs_, OpIMul, ty_u32_,
                                       Bin(funcs_, OpIAdd, ty_u32_, lx, ly), ConstU32(32768u)),
                                   Bin(funcs_, OpShiftLeftLogical, ty_u32_, ConstU32(8u), rshift));
        const uint32_t under_z = Bin(funcs_, OpULessThan, ty_bool_, pos_z, neg_z);
        const uint32_t diff_z = Bin(funcs_, OpISub, ty_u32_, pos_z, neg_z);
        const uint32_t over_z = Bin(funcs_, OpUGreaterThan, ty_bool_, diff_z, ConstU32(0x00FFFFFFu));
        const uint32_t z24 = Sel(funcs_, under_z, ConstU32(0u),
                                 Sel(funcs_, over_z, ConstU32(0x00FFFFFFu), diff_z));
        const uint32_t d24s8_val = Bin(funcs_, OpBitwiseOr, ty_u32_, z24,
                                       Bin(funcs_, OpShiftLeftLogical, ty_u32_, stencil, ConstU32(24u)));
        const uint32_t z_f32 = Un(funcs_, OpConvertUToF, ty_f32_, z24);
        const uint32_t z_norm = Bin(funcs_, OpFMul, ty_f32_, z_f32, c_inv_2_24);
        const uint32_t d32f_val = Un(funcs_, OpBitcast, ty_u32_, z_norm);
        StoreVar(funcs_, v_out_word, Sel(funcs_, is_d24s8, d24s8_val, d32f_val));
        Emit(funcs_, OpBranch, {l_ht_after_comp});

        Emit(funcs_, OpLabel, {l_ht_after_comp});
        StoreOut(funcs_, dst_pix_idx, LoadVar(funcs_, v_out_word));

        const uint32_t emit_hiz = Bin(funcs_, OpINotEqual, ty_bool_,
                                      Bin(funcs_, OpBitwiseAnd, ty_u32_, h_mode, ConstU32(1u)), ConstU32(0u));
        const uint32_t is_tile0 = Bin(funcs_, OpLogicalAnd, ty_bool_,
                                      Bin(funcs_, OpIEqual, ty_bool_, lx, ConstU32(0u)),
                                      Bin(funcs_, OpIEqual, ty_bool_, ly, ConstU32(0u)));
        const uint32_t do_hiz = Bin(funcs_, OpLogicalAnd, ty_bool_, emit_hiz, is_tile0);
        const uint32_t l_hiz_wr = NewId(), l_hiz_end = NewId();
        Emit(funcs_, OpSelectionMerge, {l_hiz_end, 0u});
        Emit(funcs_, OpBranchConditional, {do_hiz, l_hiz_wr, l_hiz_end});

        Emit(funcs_, OpLabel, {l_hiz_wr});
        const uint32_t min14 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                   Bin(funcs_, OpShiftRightLogical, ty_u32_, hword, ConstU32(4u)), ConstU32(0x3FFFu));
        const uint32_t max14 = Bin(funcs_, OpBitwiseAnd, ty_u32_,
                                   Bin(funcs_, OpShiftRightLogical, ty_u32_, hword, ConstU32(18u)), ConstU32(0x3FFFu));
        const uint32_t hiz_w = Bin(funcs_, OpBitwiseOr, ty_u32_, min14,
                                   Bin(funcs_, OpShiftLeftLogical, ty_u32_, max14, ConstU32(16u)));
        StoreOut(funcs_, Bin(funcs_, OpIAdd, ty_u32_, h_aux_woff, blk_idx), hiz_w);
        Emit(funcs_, OpBranch, {l_hiz_end});

        Emit(funcs_, OpLabel, {l_hiz_end});
        Emit(funcs_, OpBranch, {l_after_dh});

        Emit(funcs_, OpLabel, {l_after_dh});
        Emit(funcs_, OpBranch, {l_after_op});

        Emit(funcs_, OpLabel, {l_after_op});
        Emit(funcs_, OpBranch, {l_done});

        Emit(funcs_, OpLabel, {l_done});
        Emit(funcs_, OpReturn, {});
        Emit(funcs_, OpFunctionEnd, {});

        std::vector<uint32_t> spv;
        spv.push_back(0x07230203u);
        spv.push_back(0x00010300u);  // SPIR-V 1.3
        spv.push_back(0u);
        spv.push_back(next_id_);
        spv.push_back(0u);

        Emit(spv, OpCapability, {1u});  // Shader
        Emit(spv, OpMemoryModel, {0u, 1u});  // Logical GLSL450
        EmitVec(spv, OpEntryPoint, {5u, main_fn, 0x6E69616Du, 0x00000000u, var_gid_});
        Emit(spv, OpExecutionMode, {main_fn, 17u, kUntileGroupSize, 1u, 1u});
        spv.insert(spv.end(), decorations_.begin(), decorations_.end());
        spv.insert(spv.end(), types_consts_.begin(), types_consts_.end());
        spv.insert(spv.end(), funcs_.begin(), funcs_.end());
        return spv;
    }

private:
    uint32_t next_id_ = 1u;
    uint32_t ty_void_ = 0u, ty_bool_ = 0u, ty_u32_ = 0u, ty_f32_ = 0u, ty_v3u32_ = 0u;
    uint32_t ty_rt_u32_ = 0u, ty_buf_struct_ = 0u;
    uint32_t ty_ptr_sb_struct_ = 0u, ty_ptr_sb_u32_ = 0u;
    uint32_t ty_ptr_in_v3u32_ = 0u, ty_ptr_in_u32_ = 0u, ty_ptr_fn_u32_ = 0u, ty_main_fn_ = 0u;
    uint32_t var_in_ = 0u, var_out_ = 0u, var_gid_ = 0u;
    std::unordered_map<uint32_t, uint32_t> u32_consts_;
    std::vector<uint32_t> decorations_;
    std::vector<uint32_t> types_consts_;
    std::vector<uint32_t> funcs_;
};

}  // namespace

Gfx10HtileDecoded DecodeGfx10HtileWord(uint32_t htile_word, bool has_stencil) noexcept {
    Gfx10HtileDecoded out{};
    out.zmask = htile_word & 0xFu;
    if (has_stencil) {
        out.smem = (htile_word >> 4u) & 0x3u;
        out.tc_compat = ((htile_word >> 10u) & 0x1u) != 0u;
        out.min_z_14 = ((htile_word >> 12u) & 0x3Fu) << 8u;
        out.max_z_14 = (htile_word >> 18u) & 0x3FFFu;
    } else {
        out.smem = 0x3u;
        out.tc_compat = false;
        out.min_z_14 = (htile_word >> 4u) & 0x3FFFu;
        out.max_z_14 = (htile_word >> 18u) & 0x3FFFu;
    }
    out.min_z_float = static_cast<float>(out.min_z_14) / 16383.0f;
    out.max_z_float = static_cast<float>(out.max_z_14) / 16383.0f;
    return out;
}

uint32_t EncodeGfx10HtileWord(
    uint32_t zmask,
    uint32_t min_z_14,
    uint32_t max_z_14,
    uint32_t smem,
    bool tc_compat,
    bool has_stencil) noexcept {
    const uint32_t zm = zmask & 0xFu;
    const uint32_t mn = min_z_14 & 0x3FFFu;
    const uint32_t mx = max_z_14 & 0x3FFFu;
    if (has_stencil) {
        const uint32_t zbase6 = (mn >> 8u) & 0x3Fu;
        return zm |
               ((smem & 0x3u) << 4u) |
               (tc_compat ? (1u << 10u) : 0u) |
               (zbase6 << 12u) |
               (mx << 18u);
    }
    return zm | (mn << 4u) | (mx << 18u);
}

uint32_t PackGfx10FmaskPixelWord(const uint8_t* sample_frags, uint32_t sample_count) noexcept {
    if (!sample_frags) return 0xFFFFFFFFu;
    const uint32_t n = std::min<uint32_t>(sample_count, 8u);
    uint32_t word = 0u;
    for (uint32_t s = 0u; s < n; ++s) {
        word |= (static_cast<uint32_t>(sample_frags[s] & 0xFu) << (s * 4u));
    }
    return word;
}

Status ResolveFmaskCpuReference(
    const XgGpuResolveParams& params,
    const std::vector<uint8_t>& msaa_color_bytes,
    const std::vector<uint8_t>& fmask_bytes,
    std::vector<uint8_t>& out_rgba8) {
    if (params.width == 0u || params.height == 0u ||
        (params.sample_count != 2u && params.sample_count != 4u && params.sample_count != 8u)) {
        return Status::Error(StatusCode::InvalidArgument, "ResolveFmaskCpuReference: invalid dimensions or sample_count");
    }
    const size_t total_pixels = static_cast<size_t>(params.width) * params.height;
    const uint32_t dst_pitch = (params.dst_row_pitch >= params.width * 4u) ? params.dst_row_pitch : (params.width * 4u);
    const bool per_sample = (params.mode == static_cast<uint32_t>(XgFmaskResolveMode::PerSampleExpand));
    const size_t expected_out = per_sample
                                    ? (total_pixels * params.sample_count * 4u)
                                    : (static_cast<size_t>(params.height) * dst_pitch);
    out_rgba8.assign(expected_out, 0u);

    for (uint32_t y = 0u; y < params.height; ++y) {
        for (uint32_t x = 0u; x < params.width; ++x) {
            const size_t tid = static_cast<size_t>(y) * params.width + x;
            const uint32_t fmask_word = ReadU32Le(fmask_bytes, tid * 4u);
            uint32_t sr = 0u, sg = 0u, sb = 0u, sa = 0u;
            uint32_t centroid_col = params.clear_reg_0;
            bool has_valid = false;

            for (uint32_t s = 0u; s < params.sample_count; ++s) {
                const uint32_t fidx = (fmask_word >> (s * 4u)) & 0xFu;
                const bool is_sent = (fidx >= params.sample_count || fidx == 0xEu || fidx == 0xFu);
                const uint32_t scol =
                    is_sent ? params.clear_reg_0
                            : ReadU32Le(msaa_color_bytes, (tid * params.sample_count + fidx) * 4u);
                sr += (scol & 0xFFu);
                sg += ((scol >> 8u) & 0xFFu);
                sb += ((scol >> 16u) & 0xFFu);
                sa += ((scol >> 24u) & 0xFFu);
                if (!has_valid && !is_sent) {
                    centroid_col = scol;
                    has_valid = true;
                }
                if (per_sample) {
                    WriteU32Le(out_rgba8, (tid * params.sample_count + s) * 4u, scol);
                }
            }
            if (!per_sample) {
                const uint32_t half_s = params.sample_count >> 1u;
                const uint32_t avg = ((sr + half_s) / params.sample_count) |
                                     (((sg + half_s) / params.sample_count) << 8u) |
                                     (((sb + half_s) / params.sample_count) << 16u) |
                                     (((sa + half_s) / params.sample_count) << 24u);
                const uint32_t final_col =
                    (params.mode == static_cast<uint32_t>(XgFmaskResolveMode::Sample0Centroid))
                        ? centroid_col
                        : avg;
                WriteU32Le(out_rgba8, static_cast<size_t>(y) * dst_pitch + static_cast<size_t>(x) * 4u, final_col);
            }
        }
    }
    return Status::Ok();
}

Status EncodeDccCompressedSurface2D(
    uint32_t width,
    uint32_t height,
    const std::vector<uint8_t>& linear_rgba8,
    uint32_t clear_reg_rgba8,
    bool pipe_aligned,
    uint32_t num_pipes,
    std::vector<uint8_t>& out_compressed_blocks,
    std::vector<uint8_t>& out_dcc_keys,
    uint32_t* out_fast_clear_blocks,
    uint32_t* out_blocks_4to1,
    uint32_t* out_blocks_2to1,
    uint32_t* out_blocks_uncompressed) {
    if (width == 0u || height == 0u || linear_rgba8.size() < static_cast<size_t>(width) * height * 4u) {
        return Status::Error(StatusCode::InvalidArgument, "EncodeDccCompressedSurface2D: invalid input dimensions");
    }
    const uint32_t blocks_x = (width + 7u) / 8u;
    const uint32_t blocks_y = (height + 7u) / 8u;
    const uint32_t total_blocks = blocks_x * blocks_y;
    const uint32_t meta_pitch = std::max<uint32_t>(total_blocks, pipe_aligned ? 256u : total_blocks);

    out_compressed_blocks.assign(static_cast<size_t>(total_blocks) * 256u, 0u);
    out_dcc_keys.assign(meta_pitch, 0xFFu);

    uint32_t cnt_clear = 0u, cnt_4to1 = 0u, cnt_2to1 = 0u, cnt_uncomp = 0u;
    const uint32_t pipe_mask = (num_pipes > 1u) ? (num_pipes - 1u) : 0u;

    for (uint32_t by = 0u; by < blocks_y; ++by) {
        for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
            const uint32_t blk_idx = by * blocks_x + bx;
            uint32_t pixels[64]{};
            uint32_t min_ch[4] = {255u, 255u, 255u, 255u};
            uint32_t max_ch[4] = {0u, 0u, 0u, 0u};
            bool all_same = true;
            uint32_t first_col = 0u;

            for (uint32_t ly = 0u; ly < 8u; ++ly) {
                for (uint32_t lx = 0u; lx < 8u; ++lx) {
                    const uint32_t px = std::min(bx * 8u + lx, width - 1u);
                    const uint32_t py = std::min(by * 8u + ly, height - 1u);
                    const uint32_t col = ReadU32Le(linear_rgba8, (static_cast<size_t>(py) * width + px) * 4u);
                    const uint32_t p = ly * 8u + lx;
                    pixels[p] = col;
                    if (p == 0u) first_col = col;
                    else if (col != first_col) all_same = false;
                    for (uint32_t c = 0u; c < 4u; ++c) {
                        const uint32_t v = (col >> (c * 8u)) & 0xFFu;
                        min_ch[c] = std::min(min_ch[c], v);
                        max_ch[c] = std::max(max_ch[c], v);
                    }
                }
            }

            uint32_t moff = blk_idx;
            if (pipe_aligned && meta_pitch >= 256u) {
                const uint32_t xorb = ((bx >> 2u) ^ (by >> 2u)) & pipe_mask;
                const uint32_t cand = blk_idx ^ xorb;
                if (cand < meta_pitch) moff = cand;
            }

            const size_t blk_base = static_cast<size_t>(blk_idx) * 256u;

            // 1. Check fast-clear sentinels
            if (all_same) {
                uint8_t fc_key = 0xFFu;
                if (first_col == 0x00000000u) fc_key = 0x00u;
                else if (first_col == 0xFF000000u) fc_key = 0x40u;
                else if (first_col == 0x00FFFFFFu) fc_key = 0x80u;
                else if (first_col == 0xFFFFFFFFu) fc_key = 0xC0u;
                else if (first_col == clear_reg_rgba8) fc_key = 0x20u;
                if (fc_key != 0xFFu) {
                    out_dcc_keys[moff] = fc_key;
                    ++cnt_clear;
                    continue;
                }
            }

            // 2. Check lossless 4:1 delta-block compression (RGB span <= 3, A span <= 1)
            const bool fits_4to1 =
                (max_ch[0] - min_ch[0] <= 3u) &&
                (max_ch[1] - min_ch[1] <= 3u) &&
                (max_ch[2] - min_ch[2] <= 3u) &&
                (max_ch[3] - min_ch[3] <= 1u);
            if (fits_4to1) {
                const uint32_t r0 = std::min<uint32_t>(min_ch[0] + 2u, 255u);
                const uint32_t g0 = std::min<uint32_t>(min_ch[1] + 2u, 255u);
                const uint32_t b0 = std::min<uint32_t>(min_ch[2] + 2u, 255u);
                const uint32_t a0 = min_ch[3];
                // Verify exact round-trip (base near 255 could clip if min_ch == 254 -> base 255 -> delta +2..+1)
                bool exact = true;
                uint32_t rw[4]{}, gw[4]{}, bw[4]{}, aw[2]{};
                for (uint32_t p = 0u; p < 64u; ++p) {
                    const uint32_t cr = pixels[p] & 0xFFu;
                    const uint32_t cg = (pixels[p] >> 8u) & 0xFFu;
                    const uint32_t cb = (pixels[p] >> 16u) & 0xFFu;
                    const uint32_t ca = (pixels[p] >> 24u) & 0xFFu;
                    const int32_t dr = static_cast<int32_t>(cr) - static_cast<int32_t>(r0) + 2;
                    const int32_t dg = static_cast<int32_t>(cg) - static_cast<int32_t>(g0) + 2;
                    const int32_t db = static_cast<int32_t>(cb) - static_cast<int32_t>(b0) + 2;
                    const int32_t da = static_cast<int32_t>(ca) - static_cast<int32_t>(a0);
                    if (dr < 0 || dr > 3 || dg < 0 || dg > 3 || db < 0 || db > 3 || da < 0 || da > 1) {
                        exact = false;
                        break;
                    }
                    const uint32_t sh2 = (p & 15u) * 2u;
                    rw[p >> 4u] |= (static_cast<uint32_t>(dr) << sh2);
                    gw[p >> 4u] |= (static_cast<uint32_t>(dg) << sh2);
                    bw[p >> 4u] |= (static_cast<uint32_t>(db) << sh2);
                    aw[p >> 5u] |= (static_cast<uint32_t>(da) << (p & 31u));
                }
                if (exact) {
                    out_dcc_keys[moff] = 0x04u;  // XG_DCC_STATE_COMPRESSED_4TO1
                    WriteU32Le(out_compressed_blocks, blk_base + 0u, r0 | (g0 << 8u) | (b0 << 16u) | (a0 << 24u));
                    WriteU32Le(out_compressed_blocks, blk_base + 4u, 0u);
                    for (uint32_t i = 0u; i < 4u; ++i) {
                        WriteU32Le(out_compressed_blocks, blk_base + 8u + i * 4u, rw[i]);
                        WriteU32Le(out_compressed_blocks, blk_base + 24u + i * 4u, gw[i]);
                        WriteU32Le(out_compressed_blocks, blk_base + 40u + i * 4u, bw[i]);
                    }
                    for (uint32_t i = 0u; i < 2u; ++i) {
                        WriteU32Le(out_compressed_blocks, blk_base + 56u + i * 4u, aw[i]);
                    }
                    ++cnt_4to1;
                    continue;
                }
            }

            // 3. Check lossless 2:1 delta-block compression (RGB span <= 15, A span <= 3)
            const bool fits_2to1 =
                (max_ch[0] - min_ch[0] <= 15u) &&
                (max_ch[1] - min_ch[1] <= 15u) &&
                (max_ch[2] - min_ch[2] <= 15u) &&
                (max_ch[3] - min_ch[3] <= 3u);
            if (fits_2to1) {
                const uint32_t r0 = std::min<uint32_t>(min_ch[0] + 8u, 255u);
                const uint32_t g0 = std::min<uint32_t>(min_ch[1] + 8u, 255u);
                const uint32_t b0 = std::min<uint32_t>(min_ch[2] + 8u, 255u);
                const uint32_t a0 = std::min<uint32_t>(min_ch[3] + 2u, 255u);
                bool exact = true;
                uint32_t rw[8]{}, gw[8]{}, bw[8]{}, aw[4]{};
                for (uint32_t p = 0u; p < 64u; ++p) {
                    const uint32_t cr = pixels[p] & 0xFFu;
                    const uint32_t cg = (pixels[p] >> 8u) & 0xFFu;
                    const uint32_t cb = (pixels[p] >> 16u) & 0xFFu;
                    const uint32_t ca = (pixels[p] >> 24u) & 0xFFu;
                    const int32_t dr = static_cast<int32_t>(cr) - static_cast<int32_t>(r0) + 8;
                    const int32_t dg = static_cast<int32_t>(cg) - static_cast<int32_t>(g0) + 8;
                    const int32_t db = static_cast<int32_t>(cb) - static_cast<int32_t>(b0) + 8;
                    const int32_t da = static_cast<int32_t>(ca) - static_cast<int32_t>(a0) + 2;
                    if (dr < 0 || dr > 15 || dg < 0 || dg > 15 || db < 0 || db > 15 || da < 0 || da > 3) {
                        exact = false;
                        break;
                    }
                    const uint32_t sh4 = (p & 7u) * 4u;
                    const uint32_t sh2 = (p & 15u) * 2u;
                    rw[p >> 3u] |= (static_cast<uint32_t>(dr) << sh4);
                    gw[p >> 3u] |= (static_cast<uint32_t>(dg) << sh4);
                    bw[p >> 3u] |= (static_cast<uint32_t>(db) << sh4);
                    aw[p >> 4u] |= (static_cast<uint32_t>(da) << sh2);
                }
                if (exact) {
                    out_dcc_keys[moff] = 0x0Cu;  // XG_DCC_STATE_COMPRESSED_2TO1
                    WriteU32Le(out_compressed_blocks, blk_base + 0u, r0 | (g0 << 8u) | (b0 << 16u) | (a0 << 24u));
                    WriteU32Le(out_compressed_blocks, blk_base + 4u, 0u);
                    for (uint32_t i = 0u; i < 8u; ++i) {
                        WriteU32Le(out_compressed_blocks, blk_base + 8u + i * 4u, rw[i]);
                        WriteU32Le(out_compressed_blocks, blk_base + 40u + i * 4u, gw[i]);
                        WriteU32Le(out_compressed_blocks, blk_base + 72u + i * 4u, bw[i]);
                    }
                    for (uint32_t i = 0u; i < 4u; ++i) {
                        WriteU32Le(out_compressed_blocks, blk_base + 104u + i * 4u, aw[i]);
                    }
                    ++cnt_2to1;
                    continue;
                }
            }

            // 4. Uncompressed fallback (0xFF)
            out_dcc_keys[moff] = 0xFFu;
            for (uint32_t p = 0u; p < 64u; ++p) {
                WriteU32Le(out_compressed_blocks, blk_base + p * 4u, pixels[p]);
            }
            ++cnt_uncomp;
        }
    }

    if (out_fast_clear_blocks) *out_fast_clear_blocks = cnt_clear;
    if (out_blocks_4to1) *out_blocks_4to1 = cnt_4to1;
    if (out_blocks_2to1) *out_blocks_2to1 = cnt_2to1;
    if (out_blocks_uncompressed) *out_blocks_uncompressed = cnt_uncomp;
    return Status::Ok();
}

Status DecompressDccCpuReference(
    const XgGpuResolveParams& params,
    const std::vector<uint8_t>& src_blocks,
    const std::vector<uint8_t>& dcc_keys,
    std::vector<uint8_t>& out_rgba8) {
    if (params.width == 0u || params.height == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "DecompressDccCpuReference: zero dimensions");
    }
    const uint32_t dst_pitch = (params.dst_row_pitch >= params.width * 4u) ? params.dst_row_pitch : (params.width * 4u);
    out_rgba8.assign(static_cast<size_t>(params.height) * dst_pitch, 0u);
    const uint32_t pipe_mask = (params.meta_num_pipes > 1u) ? (params.meta_num_pipes - 1u) : 0u;
    const bool pipe_aligned = ((params.mode & 1u) != 0u) && (params.meta_slice_pitch >= 256u);

    for (uint32_t y = 0u; y < params.height; ++y) {
        for (uint32_t x = 0u; x < params.width; ++x) {
            const uint32_t bx = x >> 3u;
            const uint32_t by = y >> 3u;
            const uint32_t lx = x & 7u;
            const uint32_t ly = y & 7u;
            const uint32_t pix_in_blk = (ly << 3u) | lx;
            const uint32_t blk_idx = by * params.meta_blocks_x + bx;
            uint32_t moff = blk_idx;
            if (pipe_aligned) {
                const uint32_t xorb = ((bx >> 2u) ^ (by >> 2u)) & pipe_mask;
                const uint32_t cand = blk_idx ^ xorb;
                if (cand < params.meta_slice_pitch) moff = cand;
            }
            const uint8_t key = (moff < dcc_keys.size()) ? dcc_keys[moff] : 0xFFu;
            const size_t blk_base = static_cast<size_t>(blk_idx) * 256u;

            uint32_t col = 0u;
            if (key == 0x00u) col = 0x00000000u;
            else if (key == 0x40u) col = 0xFF000000u;
            else if (key == 0x80u) col = 0x00FFFFFFu;
            else if (key == 0xC0u) col = 0xFFFFFFFFu;
            else if (key == 0x20u) col = params.clear_reg_0;
            else if (key >= 0x02u && key <= 0x12u) {
                const bool is_4to1 = (key <= 0x07u);
                const uint32_t base_rgba = ReadU32Le(src_blocks, blk_base + 0u);
                const uint32_t sh_word = ReadU32Le(src_blocks, blk_base + 4u);
                const uint32_t r0 = base_rgba & 0xFFu;
                const uint32_t g0 = (base_rgba >> 8u) & 0xFFu;
                const uint32_t b0 = (base_rgba >> 16u) & 0xFFu;
                const uint32_t a0 = (base_rgba >> 24u) & 0xFFu;
                const uint32_t sr = sh_word & 0xFFu;
                const uint32_t sg = (sh_word >> 8u) & 0xFFu;
                const uint32_t sb = (sh_word >> 16u) & 0xFFu;
                const uint32_t sa = (sh_word >> 24u) & 0xFFu;

                uint32_t dr = 0u, dg = 0u, db = 0u, da = 0u;
                if (is_4to1) {
                    const uint32_t w2 = pix_in_blk >> 4u;
                    const uint32_t b2 = (pix_in_blk & 15u) * 2u;
                    const uint32_t w1 = pix_in_blk >> 5u;
                    const uint32_t b1 = pix_in_blk & 31u;
                    dr = (ReadU32Le(src_blocks, blk_base + 8u + w2 * 4u) >> b2) & 0x3u;
                    dg = (ReadU32Le(src_blocks, blk_base + 24u + w2 * 4u) >> b2) & 0x3u;
                    db = (ReadU32Le(src_blocks, blk_base + 40u + w2 * 4u) >> b2) & 0x3u;
                    da = (ReadU32Le(src_blocks, blk_base + 56u + w1 * 4u) >> b1) & 0x1u;
                    col = ApplyBiasedDeltaU32(r0, dr, 2u, sr, 255u) |
                          (ApplyBiasedDeltaU32(g0, dg, 2u, sg, 255u) << 8u) |
                          (ApplyBiasedDeltaU32(b0, db, 2u, sb, 255u) << 16u) |
                          (ApplyBiasedDeltaU32(a0, da, 0u, sa, 255u) << 24u);
                } else {
                    const uint32_t w4 = pix_in_blk >> 3u;
                    const uint32_t b4 = (pix_in_blk & 7u) * 4u;
                    const uint32_t w2 = pix_in_blk >> 4u;
                    const uint32_t b2 = (pix_in_blk & 15u) * 2u;
                    dr = (ReadU32Le(src_blocks, blk_base + 8u + w4 * 4u) >> b4) & 0xFu;
                    dg = (ReadU32Le(src_blocks, blk_base + 40u + w4 * 4u) >> b4) & 0xFu;
                    db = (ReadU32Le(src_blocks, blk_base + 72u + w4 * 4u) >> b4) & 0xFu;
                    da = (ReadU32Le(src_blocks, blk_base + 104u + w2 * 4u) >> b2) & 0x3u;
                    col = ApplyBiasedDeltaU32(r0, dr, 8u, sr, 255u) |
                          (ApplyBiasedDeltaU32(g0, dg, 8u, sg, 255u) << 8u) |
                          (ApplyBiasedDeltaU32(b0, db, 8u, sb, 255u) << 16u) |
                          (ApplyBiasedDeltaU32(a0, da, 2u, sa, 255u) << 24u);
                }
            } else {
                col = ReadU32Le(src_blocks, blk_base + pix_in_blk * 4u);
            }
            WriteU32Le(out_rgba8, static_cast<size_t>(y) * dst_pitch + static_cast<size_t>(x) * 4u, col);
        }
    }
    return Status::Ok();
}

Status EncodeHtileCompressedSurface2D(
    uint32_t width,
    uint32_t height,
    const std::vector<float>& depth_f32,
    const std::vector<uint8_t>* stencil_s8,
    float clear_depth,
    uint8_t clear_stencil,
    bool output_d24s8,
    std::vector<uint8_t>& out_compressed_tiles,
    std::vector<uint8_t>& out_htile_words,
    uint32_t* out_clear_tiles,
    uint32_t* out_compressed_tile_count,
    uint32_t* out_expanded_tiles) {
    if (width == 0u || height == 0u || depth_f32.size() < static_cast<size_t>(width) * height) {
        return Status::Error(StatusCode::InvalidArgument, "EncodeHtileCompressedSurface2D: invalid input dimensions");
    }
    const uint32_t blocks_x = (width + 7u) / 8u;
    const uint32_t blocks_y = (height + 7u) / 8u;
    const uint32_t total_tiles = blocks_x * blocks_y;
    out_compressed_tiles.assign(static_cast<size_t>(total_tiles) * 256u, 0u);
    out_htile_words.assign(static_cast<size_t>(total_tiles) * 4u, 0u);

    uint32_t cnt_clear = 0u, cnt_comp = 0u, cnt_exp = 0u;
    const auto to_z24 = [](float d) noexcept -> uint32_t {
        const float c = std::clamp(d, 0.0f, 1.0f);
        const uint32_t v = static_cast<uint32_t>(c * 16777216.0f + 0.5f);
        return std::min<uint32_t>(v, 0x00FFFFFFu);
    };
    const uint32_t clear_z24 = to_z24(clear_depth);

    for (uint32_t by = 0u; by < blocks_y; ++by) {
        for (uint32_t bx = 0u; bx < blocks_x; ++bx) {
            const uint32_t tile_idx = by * blocks_x + bx;
            const size_t tile_base = static_cast<size_t>(tile_idx) * 256u;
            uint32_t z24_pix[64]{};
            uint8_t s_pix[64]{};
            uint32_t min_z24 = 0x00FFFFFFu;
            uint32_t max_z24 = 0u;
            bool all_clear = true;
            bool same_stencil = true;

            for (uint32_t ly = 0u; ly < 8u; ++ly) {
                for (uint32_t lx = 0u; lx < 8u; ++lx) {
                    const uint32_t px = std::min(bx * 8u + lx, width - 1u);
                    const uint32_t py = std::min(by * 8u + ly, height - 1u);
                    const size_t src_i = static_cast<size_t>(py) * width + px;
                    const uint32_t p = ly * 8u + lx;
                    const uint32_t zv = to_z24(depth_f32[src_i]);
                    const uint8_t sv = (stencil_s8 && src_i < stencil_s8->size()) ? (*stencil_s8)[src_i] : clear_stencil;
                    z24_pix[p] = zv;
                    s_pix[p] = sv;
                    min_z24 = std::min(min_z24, zv);
                    max_z24 = std::max(max_z24, zv);
                    if (zv != clear_z24 || sv != clear_stencil) all_clear = false;
                    if (sv != s_pix[0]) same_stencil = false;
                }
            }

            const uint32_t min14 = (min_z24 >> 10u) & 0x3FFFu;
            const uint32_t max14 = std::min<uint32_t>(0x3FFFu, (max_z24 + 0x3FFu) >> 10u);

            if (all_clear) {
                const uint32_t qz = (clear_z24 >> 10u) & 0x3FFFu;
                const uint32_t hword = EncodeGfx10HtileWord(0u, qz, qz, 0u, false, false);
                WriteU32Le(out_htile_words, static_cast<size_t>(tile_idx) * 4u, hword);
                ++cnt_clear;
                continue;
            }

            // Try planar + 4-bit residual compression (zmask in 1..14) when stencil is uniform across tile.
            if (same_stencil) {
                const int32_t base_z = static_cast<int32_t>(z24_pix[0]);
                const int32_t dz_dx = static_cast<int32_t>(z24_pix[1]) - base_z;
                const int32_t dz_dy = static_cast<int32_t>(z24_pix[8]) - base_z;
                if (dz_dx >= -32768 && dz_dx <= 32767 && dz_dy >= -32768 && dz_dy <= 32767) {
                    int32_t residuals[64]{};
                    bool exact_plane = true;
                    uint32_t chosen_zmask = 0u;
                    for (uint32_t zm = 1u; zm <= 14u; ++zm) {
                        const uint32_t rshift = zm - 1u;
                        const uint32_t step = 1u << rshift;
                        exact_plane = true;
                        for (uint32_t ly = 0u; ly < 8u && exact_plane; ++ly) {
                            for (uint32_t lx = 0u; lx < 8u; ++lx) {
                                const uint32_t p = ly * 8u + lx;
                                const int32_t pred = base_z + static_cast<int32_t>(lx) * dz_dx +
                                                     static_cast<int32_t>(ly) * dz_dy;
                                const int32_t diff = static_cast<int32_t>(z24_pix[p]) - pred;
                                if ((diff % static_cast<int32_t>(step)) != 0) {
                                    exact_plane = false;
                                    break;
                                }
                                const int32_t q = (diff / static_cast<int32_t>(step)) + 8;
                                if (q < 0 || q > 15) {
                                    exact_plane = false;
                                    break;
                                }
                                residuals[p] = q;
                            }
                        }
                        if (exact_plane) {
                            chosen_zmask = zm;
                            break;
                        }
                    }
                    if (chosen_zmask != 0u) {
                        const uint32_t hword = EncodeGfx10HtileWord(chosen_zmask, min14, max14, 0x3u, false, false);
                        WriteU32Le(out_htile_words, static_cast<size_t>(tile_idx) * 4u, hword);
                        WriteU32Le(out_compressed_tiles, tile_base + 0u,
                                   (static_cast<uint32_t>(base_z) & 0x00FFFFFFu) |
                                       (static_cast<uint32_t>(s_pix[0]) << 24u));
                        WriteU32Le(out_compressed_tiles, tile_base + 4u,
                                   static_cast<uint32_t>(dz_dx + 32768) |
                                       (static_cast<uint32_t>(dz_dy + 32768) << 16u));
                        uint32_t rw[8]{};
                        for (uint32_t p = 0u; p < 64u; ++p) {
                            rw[p >> 3u] |= (static_cast<uint32_t>(residuals[p]) << ((p & 7u) * 4u));
                        }
                        for (uint32_t i = 0u; i < 8u; ++i) {
                            WriteU32Le(out_compressed_tiles, tile_base + 8u + i * 4u, rw[i]);
                        }
                        ++cnt_comp;
                        continue;
                    }
                }
            }

            // Expanded tile fallback (zmask == 15)
            const uint32_t hword = EncodeGfx10HtileWord(15u, min14, max14, 0x3u, false, false);
            WriteU32Le(out_htile_words, static_cast<size_t>(tile_idx) * 4u, hword);
            for (uint32_t p = 0u; p < 64u; ++p) {
                const uint32_t val = output_d24s8
                                         ? ((static_cast<uint32_t>(s_pix[p]) << 24u) | z24_pix[p])
                                         : FloatBitsFromZ24(z24_pix[p]);
                WriteU32Le(out_compressed_tiles, tile_base + p * 4u, val);
            }
            ++cnt_exp;
        }
    }

    if (out_clear_tiles) *out_clear_tiles = cnt_clear;
    if (out_compressed_tile_count) *out_compressed_tile_count = cnt_comp;
    if (out_expanded_tiles) *out_expanded_tiles = cnt_exp;
    return Status::Ok();
}

Status DecompressHtileCpuReference(
    const XgGpuResolveParams& params,
    const std::vector<uint8_t>& src_tiles,
    const std::vector<uint8_t>& htile_words,
    std::vector<uint8_t>& out_depth_bytes,
    std::vector<uint32_t>* out_hiz_bounds) {
    if (params.width == 0u || params.height == 0u) {
        return Status::Error(StatusCode::InvalidArgument, "DecompressHtileCpuReference: zero dimensions");
    }
    const uint32_t dst_pitch = (params.dst_row_pitch >= params.width * 4u) ? params.dst_row_pitch : (params.width * 4u);
    const uint32_t blocks_x = params.meta_blocks_x;
    const uint32_t blocks_y = (params.height + 7u) / 8u;
    const bool emit_hiz = ((params.mode & 1u) != 0u);
    const bool is_d24s8 = ((params.mode & 2u) != 0u);

    const size_t pix_bytes = static_cast<size_t>(params.height) * dst_pitch;
    const size_t total_bytes = emit_hiz ? (pix_bytes + static_cast<size_t>(blocks_x) * blocks_y * 4u) : pix_bytes;
    out_depth_bytes.assign(total_bytes, 0u);
    if (out_hiz_bounds) {
        out_hiz_bounds->assign(static_cast<size_t>(blocks_x) * blocks_y, 0u);
    }

    const uint32_t clear_w = is_d24s8
                                 ? (((params.clear_reg_1 & 0xFFu) << 24u) | (params.clear_reg_0 & 0x00FFFFFFu))
                                 : params.clear_reg_0;

    for (uint32_t y = 0u; y < params.height; ++y) {
        for (uint32_t x = 0u; x < params.width; ++x) {
            const uint32_t bx = x >> 3u;
            const uint32_t by = y >> 3u;
            const uint32_t lx = x & 7u;
            const uint32_t ly = y & 7u;
            const uint32_t pix_in_tile = (ly << 3u) | lx;
            const uint32_t tile_idx = by * blocks_x + bx;
            const uint32_t hword = ReadU32Le(htile_words, static_cast<size_t>(tile_idx) * 4u);
            const uint32_t zmask = hword & 0xFu;
            const size_t tile_base = static_cast<size_t>(tile_idx) * 256u;

            uint32_t out_w = 0u;
            if (zmask == 0u) {
                out_w = clear_w;
            } else if (zmask < 15u) {
                const uint32_t hdr0 = ReadU32Le(src_tiles, tile_base + 0u);
                const uint32_t hdr1 = ReadU32Le(src_tiles, tile_base + 4u);
                const uint32_t base_z = hdr0 & 0x00FFFFFFu;
                const uint32_t stencil = (hdr0 >> 24u) & 0xFFu;
                const uint32_t raw_dx = hdr1 & 0xFFFFu;
                const uint32_t raw_dy = (hdr1 >> 16u) & 0xFFFFu;
                const uint32_t rword = ReadU32Le(src_tiles, tile_base + 8u + (pix_in_tile >> 3u) * 4u);
                const uint32_t raw_res = (rword >> ((pix_in_tile & 7u) * 4u)) & 0xFu;
                const uint32_t rshift = zmask - 1u;
                const uint32_t pos = base_z + lx * raw_dx + ly * raw_dy + (raw_res << rshift);
                const uint32_t neg = (lx + ly) * 32768u + (8u << rshift);
                const uint32_t z24 = (pos < neg) ? 0u : std::min<uint32_t>(pos - neg, 0x00FFFFFFu);
                out_w = is_d24s8 ? ((stencil << 24u) | z24) : FloatBitsFromZ24(z24);
            } else {
                out_w = ReadU32Le(src_tiles, tile_base + pix_in_tile * 4u);
            }
            WriteU32Le(out_depth_bytes, static_cast<size_t>(y) * dst_pitch + static_cast<size_t>(x) * 4u, out_w);

            if (emit_hiz && lx == 0u && ly == 0u) {
                const uint32_t min14 = (hword >> 4u) & 0x3FFFu;
                const uint32_t max14 = (hword >> 18u) & 0x3FFFu;
                const uint32_t hiz_w = (max14 << 16u) | min14;
                WriteU32Le(out_depth_bytes, pix_bytes + static_cast<size_t>(tile_idx) * 4u, hiz_w);
                if (out_hiz_bounds && tile_idx < out_hiz_bounds->size()) {
                    (*out_hiz_bounds)[tile_idx] = hiz_w;
                }
            }
        }
    }
    return Status::Ok();
}

std::string GenerateXgGpuResolveHlsl() {
    return R"HLSL(
cbuffer ResolveParams : register(b0) {
    uint g_op;
    uint g_width;
    uint g_height;
    uint g_sample_count;
    uint g_mode;
    uint g_bpe;
    uint g_src_row_pitch;
    uint g_dst_row_pitch;
    uint g_meta_blocks_x;
    uint g_meta_num_pipes;
    uint g_meta_slice_pitch;
    uint g_clear_reg_0;
    uint g_clear_reg_1;
    uint g_spv_src_woff;
    uint g_spv_meta_woff;
    uint g_aux_word_offset;
};

ByteAddressBuffer g_Src : register(t0);
ByteAddressBuffer g_Meta : register(t1);
RWByteAddressBuffer g_Dst : register(u0);

uint ReadByteAt(ByteAddressBuffer buf, uint byte_off) {
    uint w = buf.Load(byte_off & ~3u);
    return (w >> ((byte_off & 3u) * 8u)) & 0xFFu;
}

uint ApplyBiasedDelta(uint base_ch, uint raw_delta, uint bias, uint shift, uint max_val) {
    uint pos = base_ch + (raw_delta << shift);
    uint neg = bias << shift;
    if (pos < neg) return 0u;
    uint diff = pos - neg;
    return (diff > max_val) ? max_val : diff;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_DispatchThreadID) {
    uint total = g_width * g_height;
    uint tid = gid.x;
    if (tid >= total) return;

    uint y = tid / g_width;
    uint x = tid % g_width;
    uint dst_byte_off = y * g_dst_row_pitch + x * 4u;

    if (g_op == 0u) {
        // FMASK MSAA Resolve & Decompress (2x/4x/8x)
        uint fmask_word = g_Meta.Load(tid * 4u);
        uint sr = 0u, sg = 0u, sb = 0u, sa = 0u;
        uint centroid_col = g_clear_reg_0;
        bool has_valid = false;
        uint base_samp = tid * g_sample_count;
        for (uint s = 0u; s < g_sample_count; ++s) {
            uint fidx = (fmask_word >> (s * 4u)) & 0xFu;
            bool is_sent = (fidx >= g_sample_count || fidx == 0xEu || fidx == 0xFu);
            uint scol = is_sent ? g_clear_reg_0 : g_Src.Load((base_samp + fidx) * 4u);
            sr += (scol & 0xFFu);
            sg += ((scol >> 8u) & 0xFFu);
            sb += ((scol >> 16u) & 0xFFu);
            sa += ((scol >> 24u) & 0xFFu);
            if (!has_valid && !is_sent) {
                centroid_col = scol;
                has_valid = true;
            }
            if (g_mode == 2u) {
                g_Dst.Store((base_samp + s) * 4u, scol);
            }
        }
        if (g_mode != 2u) {
            uint half_s = g_sample_count >> 1u;
            uint avg = ((sr + half_s) / g_sample_count) |
                       (((sg + half_s) / g_sample_count) << 8u) |
                       (((sb + half_s) / g_sample_count) << 16u) |
                       (((sa + half_s) / g_sample_count) << 24u);
            g_Dst.Store(dst_byte_off, (g_mode == 1u) ? centroid_col : avg);
        }
        return;
    }

    uint bx = x >> 3u;
    uint by = y >> 3u;
    uint lx = x & 7u;
    uint ly = y & 7u;
    uint pix_in_blk = (ly << 3u) | lx;
    uint blk_idx = by * g_meta_blocks_x + bx;
    uint blk_byte_base = blk_idx * 256u;

    if (g_op == 1u) {
        // DCC Compressed Block Decompression (fast-clear + 4:1 + 2:1 + uncompressed)
        uint moff = blk_idx;
        if ((g_mode & 1u) != 0u && g_meta_slice_pitch >= 256u) {
            uint pipe_mask = (g_meta_num_pipes > 1u) ? (g_meta_num_pipes - 1u) : 0u;
            uint xorb = ((bx >> 2u) ^ (by >> 2u)) & pipe_mask;
            uint cand = blk_idx ^ xorb;
            if (cand < g_meta_slice_pitch) moff = cand;
        }
        uint key = ReadByteAt(g_Meta, moff);
        uint col = 0u;
        if (key == 0x00u)      col = 0x00000000u;
        else if (key == 0x40u) col = 0xFF000000u;
        else if (key == 0x80u) col = 0x00FFFFFFu;
        else if (key == 0xC0u) col = 0xFFFFFFFFu;
        else if (key == 0x20u) col = g_clear_reg_0;
        else if (key >= 0x02u && key <= 0x12u) {
            bool is_4to1 = (key <= 0x07u);
            uint base_rgba = g_Src.Load(blk_byte_base);
            uint sh_word = g_Src.Load(blk_byte_base + 4u);
            uint r0 = base_rgba & 0xFFu;
            uint g0 = (base_rgba >> 8u) & 0xFFu;
            uint b0 = (base_rgba >> 16u) & 0xFFu;
            uint a0 = (base_rgba >> 24u) & 0xFFu;
            uint sr = sh_word & 0xFFu;
            uint sg = (sh_word >> 8u) & 0xFFu;
            uint sb = (sh_word >> 16u) & 0xFFu;
            uint sa = (sh_word >> 24u) & 0xFFu;
            if (is_4to1) {
                uint w2 = pix_in_blk >> 4u;
                uint b2 = (pix_in_blk & 15u) * 2u;
                uint w1 = pix_in_blk >> 5u;
                uint b1 = pix_in_blk & 31u;
                uint dr = (g_Src.Load(blk_byte_base + 8u + w2 * 4u) >> b2) & 0x3u;
                uint dg = (g_Src.Load(blk_byte_base + 24u + w2 * 4u) >> b2) & 0x3u;
                uint db = (g_Src.Load(blk_byte_base + 40u + w2 * 4u) >> b2) & 0x3u;
                uint da = (g_Src.Load(blk_byte_base + 56u + w1 * 4u) >> b1) & 0x1u;
                col = ApplyBiasedDelta(r0, dr, 2u, sr, 255u) |
                      (ApplyBiasedDelta(g0, dg, 2u, sg, 255u) << 8u) |
                      (ApplyBiasedDelta(b0, db, 2u, sb, 255u) << 16u) |
                      (ApplyBiasedDelta(a0, da, 0u, sa, 255u) << 24u);
            } else {
                uint w4 = pix_in_blk >> 3u;
                uint b4 = (pix_in_blk & 7u) * 4u;
                uint w2 = pix_in_blk >> 4u;
                uint b2 = (pix_in_blk & 15u) * 2u;
                uint dr = (g_Src.Load(blk_byte_base + 8u + w4 * 4u) >> b4) & 0xFu;
                uint dg = (g_Src.Load(blk_byte_base + 40u + w4 * 4u) >> b4) & 0xFu;
                uint db = (g_Src.Load(blk_byte_base + 72u + w4 * 4u) >> b4) & 0xFu;
                uint da = (g_Src.Load(blk_byte_base + 104u + w2 * 4u) >> b2) & 0x3u;
                col = ApplyBiasedDelta(r0, dr, 8u, sr, 255u) |
                      (ApplyBiasedDelta(g0, dg, 8u, sg, 255u) << 8u) |
                      (ApplyBiasedDelta(b0, db, 8u, sb, 255u) << 16u) |
                      (ApplyBiasedDelta(a0, da, 2u, sa, 255u) << 24u);
            }
        } else {
            col = g_Src.Load(blk_byte_base + pix_in_blk * 4u);
        }
        g_Dst.Store(dst_byte_off, col);
        return;
    }

    // OP 2: HTILE Depth/Stencil Decompression & HiZ Bounds Resolve
    uint hword = g_Meta.Load(blk_idx * 4u);
    uint zmask = hword & 0xFu;
    bool is_d24s8 = ((g_mode & 2u) != 0u);
    uint out_w = 0u;
    if (zmask == 0u) {
        out_w = is_d24s8 ? (((g_clear_reg_1 & 0xFFu) << 24u) | (g_clear_reg_0 & 0x00FFFFFFu)) : g_clear_reg_0;
    } else if (zmask < 15u) {
        uint hdr0 = g_Src.Load(blk_byte_base);
        uint hdr1 = g_Src.Load(blk_byte_base + 4u);
        uint base_z = hdr0 & 0x00FFFFFFu;
        uint stencil = (hdr0 >> 24u) & 0xFFu;
        uint raw_dx = hdr1 & 0xFFFFu;
        uint raw_dy = (hdr1 >> 16u) & 0xFFFFu;
        uint rword = g_Src.Load(blk_byte_base + 8u + (pix_in_blk >> 3u) * 4u);
        uint raw_res = (rword >> ((pix_in_blk & 7u) * 4u)) & 0xFu;
        uint rshift = zmask - 1u;
        uint pos = base_z + lx * raw_dx + ly * raw_dy + (raw_res << rshift);
        uint neg = (lx + ly) * 32768u + (8u << rshift);
        uint z24 = (pos < neg) ? 0u : min(pos - neg, 0x00FFFFFFu);
        out_w = is_d24s8 ? ((stencil << 24u) | z24) : asuint(float(z24) * (1.0f / 16777216.0f));
    } else {
        out_w = g_Src.Load(blk_byte_base + pix_in_blk * 4u);
    }
    g_Dst.Store(dst_byte_off, out_w);

    if ((g_mode & 1u) != 0u && lx == 0u && ly == 0u) {
        uint min14 = (hword >> 4u) & 0x3FFFu;
        uint max14 = (hword >> 18u) & 0x3FFFu;
        g_Dst.Store((g_aux_word_offset + blk_idx) * 4u, (max14 << 16u) | min14);
    }
}
)HLSL";
}

Status CompileXgGpuResolveDxil(
    const std::string& target_profile,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path) {
    const std::string target = target_profile.empty() ? "cs_6_5" : target_profile;
    Status st = CompileHlslWithDxc(GenerateXgGpuResolveHlsl(), target, "main", out_dxil, out_diagnostics, dxc_path);
    if (!st.ok()) return st;
    return ResignDxilContainer(out_dxil);
}

Status EmitXgGpuResolveSpirv(std::vector<uint32_t>& out_spirv) {
    SpvResolveBuilder builder;
    out_spirv = builder.Build();
    return VulkanBackend::ValidateSpirvBinary(out_spirv);
}

#if defined(_WIN32)
Status XgGpuUntiler::RecordResolveDispatchD3D12(
    ID3D12GraphicsCommandList* cmd_list,
    const XgGpuResolveParams& params,
    uint64_t cbv_gpu_va,
    uint64_t src_srv_gpu_va,
    uint64_t meta_srv_gpu_va,
    uint64_t dst_uav_gpu_va) {
    if (!impl_->d3d12_ready || !cmd_list) {
        return Status::Error(StatusCode::DeviceError, "RecordResolveDispatchD3D12: untiler or command list not ready");
    }
    if (!impl_->resolve_pso) {
        std::string diag;
        Status st = CompileXgGpuResolveDxil(impl_->shader_target, impl_->resolve_dxil, diag, impl_->dxc_path_cached);
        if (!st.ok()) {
            return Status::Error(StatusCode::ShaderError, "Failed to compile resolve DXIL: " + diag);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = impl_->root_sig.Get();
        pso_desc.CS.pShaderBytecode = impl_->resolve_dxil.data();
        pso_desc.CS.BytecodeLength = impl_->resolve_dxil.size();
        const HRESULT hr = impl_->device->CreateComputePipelineState(
            &pso_desc, OS_UUIDOF(ID3D12PipelineState), impl_->resolve_pso.PutVoid());
        if (FAILED(hr) || !impl_->resolve_pso) {
            return Status::Error(StatusCode::ShaderError, "CreateComputePipelineState(resolve) failed");
        }
    }
    const uint64_t total_pixels = static_cast<uint64_t>(params.width) * static_cast<uint64_t>(params.height);
    if (total_pixels == 0ull) {
        return Status::Error(StatusCode::InvalidArgument, "RecordResolveDispatchD3D12: zero pixels");
    }
    const uint32_t groups_x = static_cast<uint32_t>((total_pixels + kUntileGroupSize - 1ull) / kUntileGroupSize);
    cmd_list->SetComputeRootSignature(impl_->root_sig.Get());
    cmd_list->SetPipelineState(impl_->resolve_pso.Get());
    cmd_list->SetComputeRootConstantBufferView(0, cbv_gpu_va);
    cmd_list->SetComputeRootShaderResourceView(1, src_srv_gpu_va);
    cmd_list->SetComputeRootShaderResourceView(2, meta_srv_gpu_va != 0ull ? meta_srv_gpu_va : src_srv_gpu_va);
    cmd_list->SetComputeRootUnorderedAccessView(3, dst_uav_gpu_va);
    cmd_list->Dispatch(groups_x, 1u, 1u);
    return Status::Ok();
}

Status XgGpuUntiler::ExecuteResolveD3D12(
    const XgGpuResolveParams& params,
    const std::vector<uint8_t>& src_bytes,
    const std::vector<uint8_t>& meta_bytes,
    std::vector<uint8_t>& dst_bytes,
    XgGpuDispatchMetrics* out_metrics) {
    if (!impl_->d3d12_ready) {
        return Status::Error(StatusCode::DeviceError, "XgGpuUntiler D3D12 backend not initialized");
    }
    if (params.width == 0u || params.height == 0u || dst_bytes.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "ExecuteResolveD3D12: invalid dimensions or empty dst_bytes");
    }

    XgGpuResolveParams eff = params;
    if (eff.dst_row_pitch < eff.width * 4u) eff.dst_row_pitch = eff.width * 4u;
    if (eff.op == static_cast<uint32_t>(XgGpuResolveOp::HtileDecompress) &&
        (eff.mode & 1u) != 0u && eff.spv_aux_word_offset == 0u) {
        eff.spv_aux_word_offset = (eff.dst_row_pitch * eff.height) / 4u;
    }

    const uint64_t src_sz = AlignUpU64(std::max<size_t>(src_bytes.size(), 256u), 256ull);
    const uint64_t meta_sz = AlignUpU64(std::max<size_t>(meta_bytes.size(), 256u), 256ull);
    const uint64_t dst_sz = AlignUpU64(dst_bytes.size(), 256ull);

    ComPtr<ID3D12Resource> cb_up, src_up, src_gpu, meta_up, meta_gpu, dst_gpu, dst_rb;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256ull,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, cb_up); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_UPLOAD, src_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, src_up); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_DEFAULT, src_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, src_gpu); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_UPLOAD, meta_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, meta_up); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_DEFAULT, meta_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, meta_gpu); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_DEFAULT, dst_sz,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, dst_gpu); !st.ok()) return st;
    if (auto st = CreateBufferResource(impl_->device.Get(), D3D12_HEAP_TYPE_READBACK, dst_sz,
                                       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, dst_rb); !st.ok()) return st;

    auto upload = [](ID3D12Resource* res, const void* data, size_t len, uint64_t total_sz) -> Status {
        void* mapped = nullptr;
        D3D12_RANGE r{0, 0};
        if (FAILED(res->Map(0, &r, &mapped)) || !mapped) {
            return Status::Error(StatusCode::DeviceError, "Map(upload) failed");
        }
        std::memset(mapped, 0, static_cast<size_t>(total_sz));
        if (data && len > 0) std::memcpy(mapped, data, len);
        res->Unmap(0, nullptr);
        return Status::Ok();
    };

    if (auto st = upload(cb_up.Get(), &eff, sizeof(eff), 256ull); !st.ok()) return st;
    if (auto st = upload(src_up.Get(), src_bytes.data(), src_bytes.size(), src_sz); !st.ok()) return st;
    if (auto st = upload(meta_up.Get(), meta_bytes.data(), meta_bytes.size(), meta_sz); !st.ok()) return st;

    impl_->allocator->Reset();
    impl_->cmd_list->Reset(impl_->allocator.Get(), nullptr);
    impl_->cmd_list->CopyBufferRegion(src_gpu.Get(), 0, src_up.Get(), 0, src_sz);
    impl_->cmd_list->CopyBufferRegion(meta_gpu.Get(), 0, meta_up.Get(), 0, meta_sz);

    D3D12_RESOURCE_BARRIER pre[2]{};
    pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[0].Transition = {src_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[1].Transition = {meta_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    impl_->cmd_list->ResourceBarrier(2, pre);

    Status rec = RecordResolveDispatchD3D12(
        impl_->cmd_list.Get(), eff,
        cb_up->GetGPUVirtualAddress(),
        src_gpu->GetGPUVirtualAddress(),
        meta_gpu->GetGPUVirtualAddress(),
        dst_gpu->GetGPUVirtualAddress());
    if (!rec.ok()) return rec;

    D3D12_RESOURCE_BARRIER post[2]{};
    post[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    post[0].UAV.pResource = dst_gpu.Get();
    post[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    post[1].Transition = {dst_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    impl_->cmd_list->ResourceBarrier(2, post);

    impl_->cmd_list->CopyBufferRegion(dst_rb.Get(), 0, dst_gpu.Get(), 0, dst_sz);
    if (FAILED(impl_->cmd_list->Close())) {
        return Status::Error(StatusCode::DeviceError, "Close(cmd_list) failed");
    }

    ID3D12CommandList* lists[] = {impl_->cmd_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    const uint64_t wait_val = ++impl_->fence_value;
    if (FAILED(impl_->queue->Signal(impl_->fence.Get(), wait_val))) {
        return Status::Error(StatusCode::DeviceError, "Queue::Signal failed");
    }
    if (impl_->fence->GetCompletedValue() < wait_val) {
        impl_->fence->SetEventOnCompletion(wait_val, impl_->fence_event);
        WaitForSingleObject(impl_->fence_event, 15000);
    }

    void* mapped = nullptr;
    D3D12_RANGE rr{0, dst_bytes.size()};
    if (FAILED(dst_rb->Map(0, &rr, &mapped)) || !mapped) {
        return Status::Error(StatusCode::DeviceError, "Map(readback) failed");
    }
    std::memcpy(dst_bytes.data(), mapped, dst_bytes.size());
    D3D12_RANGE wr{0, 0};
    dst_rb->Unmap(0, &wr);

    if (out_metrics) {
        const uint32_t total = eff.width * eff.height;
        out_metrics->backend_name = impl_->is_warp ? "D3D12-WARP" : "D3D12-HW";
        out_metrics->adapter_name = impl_->adapter_name;
        out_metrics->shader_target = impl_->shader_target;
        out_metrics->dispatch_groups_x = (total + kUntileGroupSize - 1u) / kUntileGroupSize;
        out_metrics->total_elements = total;
        out_metrics->bytes_per_element = 4u;
        out_metrics->executed_on_hardware_gpu = !impl_->is_warp;
        out_metrics->used_warp = impl_->is_warp;
        out_metrics->used_vulkan_icd = false;
    }
    return Status::Ok();
}
#endif

Status XgGpuUntiler::ExecuteResolveVulkan(
    const XgGpuResolveParams& params,
    const std::vector<uint8_t>& src_bytes,
    const std::vector<uint8_t>& meta_bytes,
    std::vector<uint8_t>& dst_bytes,
    XgGpuDispatchMetrics* out_metrics) {
    if (!impl_->vk_ready) {
        return Status::Error(StatusCode::DeviceError, "XgGpuUntiler Vulkan backend not initialized");
    }
    if (!impl_->vk_is_hw) {
        return Status::Error(StatusCode::NotImplemented,
                             "Vulkan hardware ICD required for SPIR-V resolve compute execution");
    }
    if (params.width == 0u || params.height == 0u || dst_bytes.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "ExecuteResolveVulkan: invalid dimensions or empty dst_bytes");
    }
    if (impl_->resolve_spirv.empty()) {
        Status st = EmitXgGpuResolveSpirv(impl_->resolve_spirv);
        if (!st.ok()) return st;
    }

    const uint32_t src_words = static_cast<uint32_t>((std::max<size_t>(src_bytes.size(), 4u) + 3u) / 4u);
    const uint32_t meta_words = static_cast<uint32_t>((std::max<size_t>(meta_bytes.size(), 4u) + 3u) / 4u);
    const uint32_t dst_words = static_cast<uint32_t>((dst_bytes.size() + 3u) / 4u);

    XgGpuResolveParams hdr = params;
    if (hdr.dst_row_pitch < hdr.width * 4u) hdr.dst_row_pitch = hdr.width * 4u;
    hdr.spv_src_word_offset = kResolveSpvHeaderWords;
    hdr.spv_meta_word_offset = kResolveSpvHeaderWords + src_words;
    if (hdr.op == static_cast<uint32_t>(XgGpuResolveOp::HtileDecompress) &&
        (hdr.mode & 1u) != 0u && hdr.spv_aux_word_offset == 0u) {
        hdr.spv_aux_word_offset = (hdr.dst_row_pitch * hdr.height) / 4u;
    }

    const uint32_t total_in_words = std::max(kResolveSpvHeaderWords + src_words + meta_words, dst_words);
    std::vector<uint32_t> input_u32(total_in_words, 0u);
    std::memcpy(input_u32.data(), &hdr, sizeof(hdr));
    if (!src_bytes.empty()) {
        std::memcpy(input_u32.data() + hdr.spv_src_word_offset, src_bytes.data(), src_bytes.size());
    }
    if (!meta_bytes.empty()) {
        std::memcpy(input_u32.data() + hdr.spv_meta_word_offset, meta_bytes.data(), meta_bytes.size());
    }

    const uint32_t total_pixels = hdr.width * hdr.height;
    const uint32_t groups_x = std::max(1u, (total_pixels + kUntileGroupSize - 1u) / kUntileGroupSize);

    std::vector<uint32_t> output_u32;
    VulkanComputeDispatchOptions vk_opts{};
    VulkanComputeDispatchResult vk_res{};
    Status st = impl_->vk_backend.ExecuteComputeShaderEx(
        impl_->resolve_spirv, input_u32, output_u32, groups_x, vk_opts, vk_res);
    if (!st.ok()) return st;

    if (output_u32.size() * sizeof(uint32_t) < dst_bytes.size()) {
        return Status::Error(StatusCode::DeviceError, "Vulkan resolve output buffer smaller than expected");
    }
    std::memcpy(dst_bytes.data(), output_u32.data(), dst_bytes.size());

    if (out_metrics) {
        out_metrics->backend_name = "Vulkan-SPIRV";
        out_metrics->adapter_name = impl_->vk_device_name;
        out_metrics->shader_target = "spirv_1_3";
        out_metrics->dispatch_groups_x = groups_x;
        out_metrics->total_elements = total_pixels;
        out_metrics->bytes_per_element = 4u;
        out_metrics->executed_on_hardware_gpu = vk_res.executed_on_vulkan_icd;
        out_metrics->used_warp = false;
        out_metrics->used_vulkan_icd = vk_res.executed_on_vulkan_icd;
    }
    return Status::Ok();
}

}  // namespace openscarlett
