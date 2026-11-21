#include "openscarlett/gpu/rdna2_lifter.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "openscarlett/core/telemetry.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace openscarlett {

namespace {

uint32_t FloatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

std::string HexU32(uint32_t v) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08xu", v);
    return buf;
}

uint64_t HashBytesFnv1a(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Constants (Khronos SPIR-V 1.3 Specification)
// ---------------------------------------------------------------------------
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint32_t kSpirvVersion13 = 0x00010300u;
constexpr uint32_t kSpirvGeneratorOpenScarlett = 0x4F533200u;  // "OS2\0"

enum SpirvOp : uint16_t {
    SpvOpExtInstImport = 11,
    SpvOpExtInst = 12,
    SpvOpMemoryModel = 14,
    SpvOpEntryPoint = 15,
    SpvOpExecutionMode = 16,
    SpvOpCapability = 17,
    SpvOpTypeVoid = 19,
    SpvOpTypeBool = 20,
    SpvOpTypeInt = 21,
    SpvOpTypeFloat = 22,
    SpvOpTypeVector = 23,
    SpvOpTypeImage = 25,
    SpvOpTypeSampler = 26,
    SpvOpTypeSampledImage = 27,
    SpvOpTypeArray = 28,
    SpvOpTypeRuntimeArray = 29,
    SpvOpTypeStruct = 30,
    SpvOpTypePointer = 32,
    SpvOpTypeFunction = 33,
    SpvOpConstantTrue = 41,
    SpvOpConstantFalse = 42,
    SpvOpConstant = 43,
    SpvOpConstantNull = 46,
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpVariable = 59,
    SpvOpImageTexelPointer = 60,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpCompositeConstruct = 80,
    SpvOpCompositeExtract = 81,
    SpvOpSampledImage = 86,
    SpvOpImageSampleExplicitLod = 88,
    SpvOpImageFetch = 95,
    SpvOpImageRead = 98,
    SpvOpImageWrite = 99,
    SpvOpConvertFToU = 109,
    SpvOpConvertFToS = 110,
    SpvOpConvertSToF = 111,
    SpvOpConvertUToF = 112,
    SpvOpBitcast = 124,
    SpvOpFNegate = 127,
    SpvOpIAdd = 128,
    SpvOpFAdd = 129,
    SpvOpISub = 130,
    SpvOpFSub = 131,
    SpvOpIMul = 132,
    SpvOpFMul = 133,
    SpvOpFDiv = 136,
    SpvOpDot = 148,
    SpvOpLogicalOr = 166,
    SpvOpLogicalAnd = 167,
    SpvOpSelect = 169,
    SpvOpIEqual = 170,
    SpvOpINotEqual = 171,
    SpvOpUGreaterThan = 172,
    SpvOpSGreaterThan = 173,
    SpvOpUGreaterThanEqual = 174,
    SpvOpSGreaterThanEqual = 175,
    SpvOpULessThan = 176,
    SpvOpSLessThan = 177,
    SpvOpFOrdEqual = 180,
    SpvOpFOrdNotEqual = 182,
    SpvOpFOrdLessThan = 184,
    SpvOpFOrdGreaterThan = 186,
    SpvOpFOrdLessThanEqual = 188,
    SpvOpFOrdGreaterThanEqual = 190,
    SpvOpShiftRightLogical = 194,
    SpvOpShiftRightArithmetic = 195,
    SpvOpShiftLeftLogical = 196,
    SpvOpBitwiseOr = 197,
    SpvOpBitwiseXor = 198,
    SpvOpBitwiseAnd = 199,
    SpvOpNot = 200,
    SpvOpBitCount = 205,
    SpvOpControlBarrier = 224,
    SpvOpAtomicCompareExchange = 230,
    SpvOpAtomicIAdd = 234,
    SpvOpAtomicAnd = 240,
    SpvOpAtomicOr = 241,
    SpvOpPhi = 245,
    SpvOpSelectionMerge = 247,
    SpvOpLabel = 248,
    SpvOpBranch = 249,
    SpvOpBranchConditional = 250,
    SpvOpReturn = 253,
};

enum GlslStd450Op : uint32_t {
    GlslFAbs = 4,
    GlslFloor = 8,
    GlslFract = 10,
    GlslSqrt = 31,
    GlslInverseSqrt = 32,
    GlslFMin = 37,
    GlslUMin = 38,
    GlslSMin = 39,
    GlslFMax = 40,
    GlslUMax = 41,
    GlslSMax = 42,
    GlslFma = 50,
    GlslPackHalf2x16 = 58,
    GlslUnpackHalf2x16 = 62,
};

// SPIR-V Storage Classes
constexpr uint32_t kSpvStorageUniformConstant = 0u;
constexpr uint32_t kSpvStorageInput = 1u;
constexpr uint32_t kSpvStorageUniform = 2u;
constexpr uint32_t kSpvStorageOutput = 3u;
constexpr uint32_t kSpvStorageWorkgroup = 4u;
constexpr uint32_t kSpvStoragePrivate = 6u;
constexpr uint32_t kSpvStorageImage = 11u;
constexpr uint32_t kSpvStorageStorageBuffer = 12u;

// SPIR-V Image Formats
constexpr uint32_t kSpvImgFmtUnknown = 0u;
constexpr uint32_t kSpvImgFmtRgba32f = 1u;
constexpr uint32_t kSpvImgFmtR32ui = 33u;

// SPIR-V Decorations
constexpr uint32_t kSpvDecBlock = 2u;
constexpr uint32_t kSpvDecArrayStride = 6u;
constexpr uint32_t kSpvDecBuiltIn = 11u;
constexpr uint32_t kSpvDecNonWritable = 24u;
constexpr uint32_t kSpvDecNonReadable = 25u;
constexpr uint32_t kSpvDecLocation = 30u;
constexpr uint32_t kSpvDecBinding = 33u;
constexpr uint32_t kSpvDecDescriptorSet = 34u;
constexpr uint32_t kSpvDecOffset = 35u;

// SPIR-V BuiltIns
constexpr uint32_t kSpvBuiltInPosition = 0u;
constexpr uint32_t kSpvBuiltInGlobalInvocationId = 28u;
constexpr uint32_t kSpvBuiltInLocalInvocationIndex = 29u;
constexpr uint32_t kSpvBuiltInVertexIndex = 42u;

// SPIR-V Scopes & Semantics
constexpr uint32_t kSpvScopeDevice = 1u;
constexpr uint32_t kSpvScopeWorkgroup = 2u;
constexpr uint32_t kSpvMemSemNone = 0u;
constexpr uint32_t kSpvMemSemAcqRelWorkgroup = 0x108u;

void EmitSpvInst(std::vector<uint32_t>& buf, uint16_t opcode, const std::vector<uint32_t>& operands) {
    const uint32_t wc = static_cast<uint32_t>(operands.size() + 1u);
    buf.push_back((wc << 16) | static_cast<uint32_t>(opcode));
    buf.insert(buf.end(), operands.begin(), operands.end());
}

void AppendSpvString(std::vector<uint32_t>& operands, const char* str) {
    const size_t len = std::strlen(str) + 1;
    const size_t words = (len + 3) / 4;
    for (size_t w = 0; w < words; ++w) {
        uint32_t word = 0;
        for (size_t b = 0; b < 4; ++b) {
            const size_t idx = w * 4 + b;
            if (idx < len) {
                word |= static_cast<uint32_t>(static_cast<uint8_t>(str[idx])) << (b * 8);
            }
        }
        operands.push_back(word);
    }
}

bool ModuleUsesStageInput(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::StageInput) return true;
        }
    }
    return false;
}

bool ModuleUsesThreadId(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::ThreadIdX) return true;
        }
    }
    return false;
}

bool ModuleHasCondBranch(const Rdna2IrModule& ir) {
    for (const auto& bb : ir.blocks) {
        if (!bb.instructions.empty() && bb.instructions.back().op == IrOp::CondBranch) {
            return true;
        }
    }
    return false;
}

enum class SpvValKind : uint8_t {
    U32,
    F32,
    Bool,
};

SpvValKind CanonicalSpvKind(IrType t) {
    if (t == IrType::F32) return SpvValKind::F32;
    if (t == IrType::Bool) return SpvValKind::Bool;
    return SpvValKind::U32;
}

// Computes immediate post-dominators for each block in [0..N-1], where N is the virtual exit node.
std::vector<uint32_t> ComputeImmediatePostDominators(const Rdna2IrModule& ir) {
    const size_t n = ir.blocks.size();
    const uint32_t exit_node = static_cast<uint32_t>(n);

    std::vector<std::vector<uint32_t>> succs(n);
    for (size_t b = 0; b < n; ++b) {
        const auto& ins = ir.blocks[b].instructions;
        if (ins.empty() || ins.back().op == IrOp::Return) {
            succs[b].push_back(exit_node);
        } else if (ins.back().op == IrOp::Branch) {
            succs[b].push_back(ins.back().true_block);
        } else if (ins.back().op == IrOp::CondBranch) {
            succs[b].push_back(ins.back().true_block);
            if (ins.back().false_block != ins.back().true_block) {
                succs[b].push_back(ins.back().false_block);
            }
        } else {
            succs[b].push_back(exit_node);
        }
    }

    std::vector<std::vector<uint8_t>> pdom(n + 1, std::vector<uint8_t>(n + 1, 1u));
    pdom[exit_node].assign(n + 1, 0u);
    pdom[exit_node][exit_node] = 1u;

    bool changed = true;
    for (size_t iter = 0; changed && iter < (n + 2) * 8; ++iter) {
        changed = false;
        for (size_t b = n; b-- > 0;) {
            std::vector<uint8_t> next_set(n + 1, 1u);
            if (succs[b].empty()) {
                next_set.assign(n + 1, 0u);
            } else {
                for (uint32_t s : succs[b]) {
                    if (s > exit_node) continue;
                    for (size_t k = 0; k <= n; ++k) {
                        next_set[k] = static_cast<uint8_t>(next_set[k] & pdom[s][k]);
                    }
                }
            }
            next_set[b] = 1u;
            if (next_set != pdom[b]) {
                pdom[b] = std::move(next_set);
                changed = true;
            }
        }
    }

    std::vector<size_t> pdom_size(n + 1, 0);
    for (size_t i = 0; i <= n; ++i) {
        size_t c = 0;
        for (uint8_t bit : pdom[i]) c += bit;
        pdom_size[i] = c;
    }

    std::vector<uint32_t> ipdom(n, exit_node);
    for (size_t b = 0; b < n; ++b) {
        uint32_t best = exit_node;
        size_t best_size = 0;
        for (size_t d = 0; d <= n; ++d) {
            if (d == b || !pdom[b][d]) continue;
            if (pdom_size[d] > best_size) {
                best_size = pdom_size[d];
                best = static_cast<uint32_t>(d);
            }
        }
        ipdom[b] = best;
    }
    return ipdom;
}

std::string MakeUniqueTempPath(const char* prefix, const char* ext) {
    static std::atomic<uint64_t> g_seq{1};
    std::error_code ec;
    std::filesystem::path temp_dir;
#if defined(_WIN32)
    char env_buf[1024]{};
    DWORD env_len = GetEnvironmentVariableA("TEMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (env_len == 0 || env_len >= sizeof(env_buf)) {
        env_len = GetEnvironmentVariableA("TMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    }
    if (env_len > 0 && env_len < sizeof(env_buf)) {
        temp_dir = std::filesystem::path(env_buf) / "openscarlett_lifter";
    } else if (std::filesystem::exists("E:\\OpenScarlett", ec)) {
        temp_dir = std::filesystem::path(kLifterTempDir);
    } else {
        temp_dir = std::filesystem::temp_directory_path(ec) / "openscarlett_lifter";
    }
#else
    temp_dir = std::filesystem::temp_directory_path(ec) / "openscarlett_lifter";
#endif
    std::filesystem::create_directories(temp_dir, ec);
    uint32_t pid = 0;
#if defined(_WIN32)
    pid = static_cast<uint32_t>(GetCurrentProcessId());
#endif
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << temp_dir.string() << "\\" << prefix << "_" << pid << "_" << seq << ext;
    return oss.str();
}

uint32_t ComputeWaveLanesCapacity(const Rdna2IrModule& ir) noexcept {
    const uint32_t tg = ir.threadgroup_x * ir.threadgroup_y * ir.threadgroup_z;
    uint32_t cap = 64u;
    while (cap < tg && cap < 1024u) {
        cap <<= 1;
    }
    return cap;
}

#if defined(_WIN32)
std::string ResolveDxcExecutable(const std::string& requested_path) {
    std::error_code ec;
    // 1. Prefer bundled dxc.exe right beside the running executable (portable tester bundles)
    char self_buf[MAX_PATH]{};
    const DWORD self_len = GetModuleFileNameA(nullptr, self_buf, static_cast<DWORD>(sizeof(self_buf)));
    if (self_len > 0 && self_len < sizeof(self_buf)) {
        const auto beside_exe = std::filesystem::path(self_buf).parent_path() / "dxc.exe";
        if (std::filesystem::exists(beside_exe, ec)) {
            return beside_exe.string();
        }
    }
    // 2. Explicit environment override
    char env_dxc[1024]{};
    const DWORD env_len = GetEnvironmentVariableA("OPENSCARLETT_DXC_PATH", env_dxc, static_cast<DWORD>(sizeof(env_dxc)));
    if (env_len > 0 && env_len < sizeof(env_dxc) && std::filesystem::exists(env_dxc, ec)) {
        return std::string(env_dxc);
    }
    // 3. Explicit caller-requested path
    if (!requested_path.empty() && std::filesystem::exists(requested_path, ec)) {
        return requested_path;
    }
    // 4. System PATH
    char path_buf[MAX_PATH]{};
    const DWORD path_len = SearchPathA(nullptr, "dxc.exe", nullptr, static_cast<DWORD>(sizeof(path_buf)), path_buf, nullptr);
    if (path_len > 0 && path_len < sizeof(path_buf) && std::filesystem::exists(path_buf, ec)) {
        return std::string(path_buf);
    }
    // 5. Windows 10/11 SDK fallback
    const std::filesystem::path kits_bin(R"(C:\Program Files (x86)\Windows Kits\10\bin)");
    if (std::filesystem::exists(kits_bin, ec)) {
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::directory_iterator(kits_bin, ec)) {
            if (!entry.is_directory()) continue;
            const auto cand = entry.path() / "x64" / "dxc.exe";
            if (std::filesystem::exists(cand, ec)) {
                found.push_back(cand);
            }
        }
        std::sort(found.begin(), found.end());
        if (!found.empty()) {
            return found.back().string();
        }
    }
    return requested_path;
}
#endif

#if defined(_WIN32)
Status RunExternalProcess(const std::string& cmdline, uint32_t& out_exit_code, std::string& out_output) {
    out_exit_code = 1;
    out_output.clear();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_read = nullptr;
    HANDLE h_write = nullptr;
    if (!CreatePipe(&h_read, &h_write, &sa, 0)) {
        return Status::Error(StatusCode::IoError, "CreatePipe failed for external shader tool");
    }
    SetHandleInformation(h_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_write;
    si.hStdError = h_write;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmdline;
    const BOOL ok = CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi);

    CloseHandle(h_write);
    if (!ok) {
        const DWORD err = GetLastError();
        CloseHandle(h_read);
        return Status::Error(StatusCode::IoError,
                             "CreateProcessA failed (err=" + std::to_string(err) + ") for: " + cmdline);
    }

    char buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(h_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        out_output.append(buf, bytes_read);
    }
    CloseHandle(h_read);

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    out_exit_code = static_cast<uint32_t>(code);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Status::Ok();
}
#endif

// ---------------------------------------------------------------------------
// HLSL Structured Emitter (SM 6.5 & SM 5.0)
// ---------------------------------------------------------------------------
class HlslEmitter {
public:
    HlslEmitter(const Rdna2IrModule& ir, bool use_sm65) : ir_(ir), use_sm65_(use_sm65) {
        for (const auto& bb : ir_.blocks) {
            for (const auto& inst : bb.instructions) {
                if (inst.id != kInvalidIrValue) {
                    defs_[inst.id] = &inst;
                }
            }
        }
    }

    Status Emit(std::string& out_hlsl, std::string& out_target) {
        if (ir_.blocks.empty()) {
            return Status::Error(StatusCode::ShaderError, "Cannot emit HLSL from empty IR module");
        }

        const bool uses_stage_in = ModuleUsesStageInput(ir_);
        const bool uses_dual_uav = ir_.uses_uav1;
        const bool uses_uav0 = ir_.uses_uav0 || (ir_.uses_uav && !ir_.uses_uav1);

        std::ostringstream hdr;
        hdr << "// Generated by OpenScarlett Clean-Room RDNA2 (XS2) -> HLSL Lifter\n";

        if (ir_.uses_root_constants) {
            hdr << "cbuffer OpenScarlettRootConstants : register(b0) {\n";
            for (uint32_t i = 0; i <= ir_.max_root_const_index; ++i) {
                hdr << "    uint _rc" << i << ";\n";
            }
            hdr << "};\n";
        }
        if (uses_uav0) {
            hdr << "RWByteAddressBuffer _in0 : register(u0);\n";
        }
        if (uses_dual_uav) {
            hdr << "RWByteAddressBuffer _out0 : register(u1);\n";
        }
        uint32_t next_t = 0u;
        if (ir_.uses_texture_srv) {
            hdr << "Texture2D<float4> _tex0 : register(t" << next_t++ << ");\n";
        }
        if (ir_.uses_texture3d_srv) {
            hdr << "Texture3D<float4> _tex3d0 : register(t" << next_t++ << ");\n";
        }
        if (ir_.uses_texture_cube_srv) {
            hdr << "Texture2DArray<float4> _texcube0 : register(t" << next_t++ << ");\n";
        }
        if (ir_.uses_texture_sample) {
            hdr << "SamplerState _samp0 : register(s0);\n";
        }
        uint32_t next_rw_u = uses_dual_uav ? 2u : 1u;
        if (ir_.uses_texture_uav) {
            hdr << "RWTexture2D<float4> _rwtex0 : register(u" << next_rw_u++ << ");\n";
        }
        if (ir_.uses_texture_u32_uav) {
            if (ir_.texture_u32_uav_dim == 2u) {
                hdr << "RWTexture3D<uint> _rwimg_u32 : register(u" << next_rw_u++ << ");\n";
            } else if (ir_.texture_u32_uav_dim == 3u) {
                hdr << "RWTexture2DArray<uint> _rwimg_u32 : register(u" << next_rw_u++ << ");\n";
            } else {
                hdr << "RWTexture2D<uint> _rwimg_u32 : register(u" << next_rw_u++ << ");\n";
            }
        }
        if (ir_.uses_lds) {
            const uint32_t lds_n = ir_.lds_size_dwords ? ir_.lds_size_dwords : 1024u;
            hdr << "groupshared uint _lds[" << lds_n << "];\n";
        }
        if (ir_.uses_wave64_emulation) {
            const uint32_t w_cap = ComputeWaveLanesCapacity(ir_);
            hdr << "groupshared uint _w64_ballot[2];\n";
            hdr << "groupshared uint _w64_lanes[" << w_cap << "];\n";
        }
        if (ir_.uses_bvh_ray) {
            hdr << "uint4 _os_bvh_intersect_ray(uint node_ptr, float tmax, float3 orig, float3 dir) {\n"
                   "    uint hdr = _in0.Load(node_ptr);\n"
                   "    uint hit_id = hdr & 0x7FFFFFFFu;\n"
                   "    if ((hdr & 0x80000000u) == 0u) {\n"
                   "        float3 v0 = float3(asfloat(_in0.Load(node_ptr + 4u)), asfloat(_in0.Load(node_ptr + 8u)), asfloat(_in0.Load(node_ptr + 12u)));\n"
                   "        float3 v1 = float3(asfloat(_in0.Load(node_ptr + 16u)), asfloat(_in0.Load(node_ptr + 20u)), asfloat(_in0.Load(node_ptr + 24u)));\n"
                   "        float3 v2 = float3(asfloat(_in0.Load(node_ptr + 28u)), asfloat(_in0.Load(node_ptr + 32u)), asfloat(_in0.Load(node_ptr + 36u)));\n"
                   "        float3 e1 = v1 - v0;\n"
                   "        float3 e2 = v2 - v0;\n"
                   "        float3 pvec = cross(dir, e2);\n"
                   "        float det = dot(e1, pvec);\n"
                   "        if (abs(det) > 1e-7) {\n"
                   "            float inv_det = 1.0 / det;\n"
                   "            float3 tvec = orig - v0;\n"
                   "            float u = dot(tvec, pvec) * inv_det;\n"
                   "            float3 qvec = cross(tvec, e1);\n"
                   "            float v = dot(dir, qvec) * inv_det;\n"
                   "            float t = dot(e2, qvec) * inv_det;\n"
                   "            if (u >= 0.0 && v >= 0.0 && (u + v) <= 1.0 && t >= 0.0 && t <= tmax) {\n"
                   "                return uint4(asuint(t), asuint(u), asuint(v), hit_id);\n"
                   "            }\n"
                   "        }\n"
                   "        return uint4(asuint(tmax), 0u, 0u, 0xFFFFFFFFu);\n"
                   "    } else {\n"
                   "        float3 bmin = float3(asfloat(_in0.Load(node_ptr + 4u)), asfloat(_in0.Load(node_ptr + 8u)), asfloat(_in0.Load(node_ptr + 12u)));\n"
                   "        float3 bmax = float3(asfloat(_in0.Load(node_ptr + 16u)), asfloat(_in0.Load(node_ptr + 20u)), asfloat(_in0.Load(node_ptr + 24u)));\n"
                   "        float t_enter = 0.0;\n"
                   "        float t_exit = tmax;\n"
                   "        if (abs(dir.x) > 1e-12) {\n"
                   "            float inv_d = 1.0 / dir.x;\n"
                   "            float t0 = (bmin.x - orig.x) * inv_d;\n"
                   "            float t1 = (bmax.x - orig.x) * inv_d;\n"
                   "            t_enter = max(t_enter, min(t0, t1));\n"
                   "            t_exit = min(t_exit, max(t0, t1));\n"
                   "        } else if (orig.x < bmin.x || orig.x > bmax.x) {\n"
                   "            t_enter = 1e30; t_exit = -1e30;\n"
                   "        }\n"
                   "        if (abs(dir.y) > 1e-12) {\n"
                   "            float inv_d = 1.0 / dir.y;\n"
                   "            float t0 = (bmin.y - orig.y) * inv_d;\n"
                   "            float t1 = (bmax.y - orig.y) * inv_d;\n"
                   "            t_enter = max(t_enter, min(t0, t1));\n"
                   "            t_exit = min(t_exit, max(t0, t1));\n"
                   "        } else if (orig.y < bmin.y || orig.y > bmax.y) {\n"
                   "            t_enter = 1e30; t_exit = -1e30;\n"
                   "        }\n"
                   "        if (abs(dir.z) > 1e-12) {\n"
                   "            float inv_d = 1.0 / dir.z;\n"
                   "            float t0 = (bmin.z - orig.z) * inv_d;\n"
                   "            float t1 = (bmax.z - orig.z) * inv_d;\n"
                   "            t_enter = max(t_enter, min(t0, t1));\n"
                   "            t_exit = min(t_exit, max(t0, t1));\n"
                   "        } else if (orig.z < bmin.z || orig.z > bmax.z) {\n"
                   "            t_enter = 1e30; t_exit = -1e30;\n"
                   "        }\n"
                   "        if (t_enter <= t_exit) {\n"
                   "            return uint4(asuint(t_enter), asuint(t_exit), 0u, hit_id);\n"
                   "        }\n"
                   "        return uint4(asuint(tmax), 0u, 0u, 0xFFFFFFFFu);\n"
                   "    }\n"
                   "}\n";
        }

        switch (ir_.stage) {
            case ShaderStage::Compute:
                out_target = use_sm65_ ? "cs_6_5" : "cs_5_0";
                hdr << "[numthreads(" << ir_.threadgroup_x << ", " << ir_.threadgroup_y << ", "
                    << ir_.threadgroup_z << ")]\n";
                hdr << "void main(uint3 _tid : SV_DispatchThreadID, uint _gtid : SV_GroupIndex) {\n";
                if (ir_.uses_wave64_emulation) {
                    const uint32_t lane_mask = (ir_.wave_size == 64u) ? 63u : 31u;
                    hdr << "    uint _w64_lane = _gtid & " << lane_mask << "u;\n";
                }
                break;
            case ShaderStage::Vertex:
                out_target = use_sm65_ ? "vs_6_5" : "vs_5_0";
                hdr << "struct VSOut {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                if (ir_.exports_param0) {
                    hdr << "    float4 param0 : TEXCOORD0;\n";
                }
                hdr << "};\n";
                hdr << "VSOut main(uint _vid : SV_VertexID) {\n";
                hdr << "    VSOut _out;\n";
                hdr << "    _out.pos = float4(0.0, 0.0, 0.0, 1.0);\n";
                if (ir_.exports_param0) {
                    hdr << "    _out.param0 = float4(0.0, 0.0, 0.0, 1.0);\n";
                }
                break;
            case ShaderStage::Pixel:
                out_target = use_sm65_ ? "ps_6_5" : "ps_5_0";
                hdr << "struct PSIn {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                if (uses_stage_in) {
                    hdr << "    float4 param0 : TEXCOORD0;\n";
                }
                hdr << "};\n";
                hdr << "float4 main(PSIn _in) : SV_TARGET0 {\n";
                hdr << "    float4 _mrt0 = float4(0.0, 0.0, 0.0, 1.0);\n";
                break;
            case ShaderStage::Mesh: {
                if (!use_sm65_) {
                    return Status::Error(StatusCode::UnsupportedFormat, "Mesh shaders require SM 6.5 (ms_6_5)");
                }
                out_target = "ms_6_5";
                const uint32_t max_v = ir_.mesh_max_vertices ? ir_.mesh_max_vertices : 64u;
                const uint32_t max_p = ir_.mesh_max_primitives ? ir_.mesh_max_primitives : 64u;
                if (ir_.mesh_has_payload) {
                    hdr << "struct ASPayload {\n";
                    hdr << "    uint _data[4];\n";
                    hdr << "};\n";
                }
                hdr << "struct MeshVert {\n";
                hdr << "    float4 pos : SV_POSITION;\n";
                if (ir_.exports_param0) {
                    hdr << "    float4 param0 : TEXCOORD0;\n";
                }
                hdr << "};\n";
                hdr << "[outputtopology(\"triangle\")]\n";
                hdr << "[numthreads(" << ir_.threadgroup_x << ", " << ir_.threadgroup_y << ", "
                    << ir_.threadgroup_z << ")]\n";
                hdr << "void main(\n";
                hdr << "    uint _gtid : SV_GroupIndex,\n";
                if (ir_.mesh_has_payload) {
                    hdr << "    in payload ASPayload _payload,\n";
                }
                hdr << "    out vertices MeshVert _verts[" << max_v << "],\n";
                hdr << "    out indices uint3 _prims[" << max_p << "])\n{\n";
                break;
            }
            case ShaderStage::Amplification:
                if (!use_sm65_) {
                    return Status::Error(StatusCode::UnsupportedFormat, "Amplification shaders require SM 6.5 (as_6_5)");
                }
                out_target = "as_6_5";
                hdr << "struct ASPayload {\n";
                hdr << "    uint _data[4];\n";
                hdr << "};\n";
                hdr << "groupshared ASPayload _payload;\n";
                hdr << "[numthreads(" << ir_.threadgroup_x << ", " << ir_.threadgroup_y << ", "
                    << ir_.threadgroup_z << ")]\n";
                hdr << "void main(uint _gtid : SV_GroupIndex) {\n";
                hdr << "    _payload._data[0] = 0u;\n";
                hdr << "    _payload._data[1] = 0u;\n";
                hdr << "    _payload._data[2] = 0u;\n";
                hdr << "    _payload._data[3] = 0u;\n";
                hdr << "    uint _dm_x = 1u;\n";
                hdr << "    uint _dm_y = 1u;\n";
                hdr << "    uint _dm_z = 1u;\n";
                break;
            default:
                return Status::Error(StatusCode::UnsupportedFormat, "Unsupported shader stage for HLSL emission");
        }

        // Pre-declare thread-private scratch array if used
        if (ir_.uses_scratch) {
            const uint32_t scr_n = ir_.scratch_size_dwords ? ir_.scratch_size_dwords : 64u;
            hdr << "    uint _scratch[" << scr_n << "];\n";
            hdr << "    for (uint _si = 0u; _si < " << scr_n << "u; ++_si) { _scratch[_si] = 0u; }\n";
        }

        // Pre-declare all SSA value locals so multi-block structured/fallback CFG always compiles cleanly.
        for (const auto& bb : ir_.blocks) {
            for (const auto& inst : bb.instructions) {
                if (inst.id == kInvalidIrValue) continue;
                if (inst.type == IrType::F32) {
                    hdr << "    float _v" << inst.id << " = 0.0;\n";
                } else if (inst.type == IrType::Bool) {
                    hdr << "    bool _v" << inst.id << " = false;\n";
                } else {
                    hdr << "    uint _v" << inst.id << " = 0u;\n";
                }
            }
        }

        std::ostringstream body;
        std::unordered_set<uint32_t> visited;
        if (EmitStructured(body, 0u, UINT32_MAX, 1, visited)) {
            hdr << body.str();
        } else {
            EmitStateMachine(hdr);
            if (ir_.stage == ShaderStage::Vertex) {
                hdr << "    return _out;\n";
            } else if (ir_.stage == ShaderStage::Pixel) {
                hdr << "    return _mrt0;\n";
            } else if (ir_.stage == ShaderStage::Amplification) {
                hdr << "    DispatchMesh(_dm_x, _dm_y, _dm_z, _payload);\n";
            }
        }
        hdr << "}\n";

        out_hlsl = hdr.str();
        return Status::Ok();
    }

private:
    std::string Indent(int depth) const {
        return std::string(static_cast<size_t>(depth) * 4u, ' ');
    }

    IrType TypeOf(IrValueId id) const {
        auto it = defs_.find(id);
        return it != defs_.end() ? it->second->type : IrType::U32;
    }

    std::string AsU32(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::F32) return "asuint(_v" + std::to_string(id) + ")";
        if (t == IrType::Bool) return "(_v" + std::to_string(id) + " ? 1u : 0u)";
        return "_v" + std::to_string(id);
    }

    std::string AsI32(IrValueId id) const {
        return "asint(" + AsU32(id) + ")";
    }

    std::string AsF32(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::F32) return "_v" + std::to_string(id);
        return "asfloat(" + AsU32(id) + ")";
    }

    std::string AsBool(IrValueId id) const {
        const IrType t = TypeOf(id);
        if (t == IrType::Bool) return "_v" + std::to_string(id);
        return "(" + AsU32(id) + " != 0u)";
    }

    std::string EmitReturnStmt() const {
        if (ir_.stage == ShaderStage::Vertex) return "return _out;";
        if (ir_.stage == ShaderStage::Pixel) return "return _mrt0;";
        if (ir_.stage == ShaderStage::Amplification) return "DispatchMesh(_dm_x, _dm_y, _dm_z, _payload);";
        return "return;";
    }

    void EmitPhiCopies(std::ostringstream& os, uint32_t from_bb, uint32_t to_bb, int depth) const {
        if (to_bb >= ir_.blocks.size()) return;
        const auto& dst_bb = ir_.blocks[to_bb];
        for (const auto& inst : dst_bb.instructions) {
            if (inst.op != IrOp::Phi) continue;
            for (const auto& inc : inst.phi_incoming) {
                if (inc.first == from_bb) {
                    os << Indent(depth) << "_v" << inst.id << " = ";
                    if (inst.type == IrType::F32) {
                        os << AsF32(inc.second);
                    } else if (inst.type == IrType::Bool) {
                        os << AsBool(inc.second);
                    } else {
                        os << AsU32(inc.second);
                    }
                    os << ";\n";
                    break;
                }
            }
        }
    }

    bool HasPhiCopies(uint32_t from_bb, uint32_t to_bb) const {
        if (to_bb >= ir_.blocks.size()) return false;
        for (const auto& inst : ir_.blocks[to_bb].instructions) {
            if (inst.op != IrOp::Phi) continue;
            for (const auto& inc : inst.phi_incoming) {
                if (inc.first == from_bb) return true;
            }
        }
        return false;
    }

    void EmitNonTerminators(std::ostringstream& os, const IrBasicBlock& bb, int depth) const {
        const std::string ind = Indent(depth);
        const uint32_t lds_mask = (ir_.lds_size_dwords ? ir_.lds_size_dwords : 1024u) - 1u;
        static constexpr char kSwiz[4] = {'x', 'y', 'z', 'w'};

        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::Phi || inst.op == IrOp::Branch ||
                inst.op == IrOp::CondBranch || inst.op == IrOp::Return) {
                continue;
            }

            switch (inst.op) {
                case IrOp::ConstU32:
                    if (inst.type == IrType::Bool) {
                        os << ind << "_v" << inst.id << " = " << (inst.imm_u32 != 0u ? "true" : "false") << ";\n";
                    } else {
                        os << ind << "_v" << inst.id << " = " << HexU32(inst.imm_u32) << ";\n";
                    }
                    break;
                case IrOp::ConstF32:
                    os << ind << "_v" << inst.id << " = asfloat(" << HexU32(FloatToBits(inst.imm_f32)) << ");\n";
                    break;
                case IrOp::ThreadIdX:
                    if (ir_.stage == ShaderStage::Vertex) {
                        os << ind << "_v" << inst.id << " = _vid;\n";
                    } else if (ir_.stage == ShaderStage::Mesh || ir_.stage == ShaderStage::Amplification) {
                        os << ind << "_v" << inst.id << " = _gtid;\n";
                    } else {
                        os << ind << "_v" << inst.id << " = _tid.x;\n";
                    }
                    break;
                case IrOp::StageInput:
                    os << ind << "_v" << inst.id << " = _in.param0." << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                case IrOp::LoadRootConst:
                    os << ind << "_v" << inst.id << " = _rc" << inst.imm_u32 << ";\n";
                    break;
                case IrOp::LoadUav: {
                    const char* buf = (inst.imm_u32 == 1u && ir_.uses_uav1) ? "_out0" : "_in0";
                    os << ind << "_v" << inst.id << " = " << buf << ".Load(" << AsU32(inst.operands[0]) << ");\n";
                    break;
                }
                case IrOp::StoreUav: {
                    const char* buf = (inst.imm_u32 == 1u && ir_.uses_uav1) ? "_out0" : "_in0";
                    os << ind << buf << ".Store(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    break;
                }
                case IrOp::ImageSample:
                    os << ind << "_v" << inst.id << " = _tex0.SampleLevel(_samp0, float2("
                       << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << "), "
                       << AsF32(inst.operands[2]) << ")." << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                case IrOp::ImageLoad:
                    os << ind << "_v" << inst.id << " = _tex0.Load(int3("
                       << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", 0))."
                       << kSwiz[inst.imm_u32 & 3u] << ";\n";
                    break;
                case IrOp::ImageLoadMip: {
                    const uint32_t ch = inst.imm_u32 & 3u;
                    const uint32_t dim = (inst.imm_u32 >> 4) & 7u;
                    if (dim == 2u) {
                        os << ind << "_v" << inst.id << " = _tex3d0.Load(int4("
                           << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", "
                           << AsI32(inst.operands[2]) << ", " << AsI32(inst.operands[3]) << "))."
                           << kSwiz[ch] << ";\n";
                    } else if (dim == 3u) {
                        os << ind << "_v" << inst.id << " = _texcube0.Load(int4("
                           << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", "
                           << AsI32(inst.operands[2]) << ", " << AsI32(inst.operands[3]) << "))."
                           << kSwiz[ch] << ";\n";
                    } else {
                        os << ind << "_v" << inst.id << " = _tex0.Load(int3("
                           << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << ", "
                           << AsI32(inst.operands[3]) << "))." << kSwiz[ch] << ";\n";
                    }
                    break;
                }
                case IrOp::ImageStore:
                    os << ind << "_rwtex0[uint2(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1])
                       << ")] = float4(" << AsF32(inst.operands[2]) << ", " << AsF32(inst.operands[3])
                       << ", " << AsF32(inst.operands[4]) << ", " << AsF32(inst.operands[5]) << ");\n";
                    break;
                case IrOp::ImageAtomicCmpSwap: {
                    const uint32_t dim = inst.imm_u32 & 7u;
                    if (dim == 2u || dim == 3u) {
                        os << ind << "InterlockedCompareExchange(_rwimg_u32[uint3("
                           << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ", "
                           << AsU32(inst.operands[2]) << ")], " << AsU32(inst.operands[3]) << ", "
                           << AsU32(inst.operands[4]) << ", _v" << inst.id << ");\n";
                    } else {
                        os << ind << "InterlockedCompareExchange(_rwimg_u32[uint2("
                           << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ")], "
                           << AsU32(inst.operands[3]) << ", " << AsU32(inst.operands[4]) << ", _v"
                           << inst.id << ");\n";
                    }
                    break;
                }
                case IrOp::BvhIntersectRay: {
                    const uint32_t comp = inst.imm_u32 & 3u;
                    const std::string call = "_os_bvh_intersect_ray(" + AsU32(inst.operands[0]) + ", " +
                                             AsF32(inst.operands[1]) + ", float3(" +
                                             AsF32(inst.operands[2]) + ", " + AsF32(inst.operands[3]) + ", " +
                                             AsF32(inst.operands[4]) + "), float3(" +
                                             AsF32(inst.operands[5]) + ", " + AsF32(inst.operands[6]) + ", " +
                                             AsF32(inst.operands[7]) + "))." + std::string(1, kSwiz[comp]);
                    if (comp < 3u) {
                        os << ind << "_v" << inst.id << " = asfloat(" << call << ");\n";
                    } else {
                        os << ind << "_v" << inst.id << " = " << call << ";\n";
                    }
                    break;
                }
                case IrOp::LdsLoad:
                    os << ind << "_v" << inst.id << " = _lds[((" << AsU32(inst.operands[0]) << ") >> 2u) & "
                       << lds_mask << "u];\n";
                    break;
                case IrOp::LdsStore:
                    os << ind << "_lds[((" << AsU32(inst.operands[0]) << ") >> 2u) & " << lds_mask
                       << "u] = " << AsU32(inst.operands[1]) << ";\n";
                    break;
                case IrOp::LdsAtomicAdd:
                    os << ind << "InterlockedAdd(_lds[((" << AsU32(inst.operands[0]) << ") >> 2u) & "
                       << lds_mask << "u], " << AsU32(inst.operands[1]) << ", _v" << inst.id << ");\n";
                    break;
                case IrOp::ScratchLoad: {
                    const uint32_t scr_mask = (ir_.scratch_size_dwords ? ir_.scratch_size_dwords : 64u) - 1u;
                    os << ind << "_v" << inst.id << " = _scratch[((" << AsU32(inst.operands[0]) << ") >> 2u) & "
                       << scr_mask << "u];\n";
                    break;
                }
                case IrOp::ScratchStore: {
                    const uint32_t scr_mask = (ir_.scratch_size_dwords ? ir_.scratch_size_dwords : 64u) - 1u;
                    os << ind << "_scratch[((" << AsU32(inst.operands[0]) << ") >> 2u) & " << scr_mask
                       << "u] = " << AsU32(inst.operands[1]) << ";\n";
                    break;
                }
                case IrOp::WaveLaneId:
                    os << ind << "_v" << inst.id << " = _w64_lane;\n";
                    break;
                case IrOp::WaveBallotLo:
                case IrOp::WaveBallotHi: {
                    const uint32_t word_idx = (inst.op == IrOp::WaveBallotHi) ? 1u : 0u;
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "if (_w64_lane == 0u) { _w64_ballot[0] = 0u; _w64_ballot[1] = 0u; }\n";
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "InterlockedOr(_w64_ballot[_w64_lane >> 5u], (" << AsBool(inst.operands[0])
                       << " ? (1u << (_w64_lane & 31u)) : 0u));\n";
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "_v" << inst.id << " = _w64_ballot[" << word_idx << "];\n";
                    break;
                }
                case IrOp::WaveActiveCountBits64:
                    os << ind << "_v" << inst.id << " = countbits(" << AsU32(inst.operands[0])
                       << ") + countbits(" << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::WavePrefixCount64:
                    if (inst.imm_u32 == 0u) {
                        os << ind << "_v" << inst.id << " = countbits(" << AsU32(inst.operands[0])
                           << " & ((_w64_lane < 32u) ? ((1u << (_w64_lane & 31u)) - 1u) : 0xFFFFFFFFu)) + "
                           << AsU32(inst.operands[1]) << ";\n";
                    } else {
                        os << ind << "_v" << inst.id << " = countbits(" << AsU32(inst.operands[0])
                           << " & ((_w64_lane >= 32u) ? ((1u << ((_w64_lane - 32u) & 31u)) - 1u) : 0u)) + "
                           << AsU32(inst.operands[1]) << ";\n";
                    }
                    break;
                case IrOp::WaveReadFirstLane:
                case IrOp::WaveReadLaneAt: {
                    const uint32_t w_mask = ComputeWaveLanesCapacity(ir_) - 1u;
                    const std::string lane_expr = (inst.op == IrOp::WaveReadFirstLane)
                                                      ? "0u"
                                                      : ("(" + AsU32(inst.operands[1]) + " & " + std::to_string(w_mask) + "u)");
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "_w64_lanes[_w64_lane] = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    if (inst.type == IrType::F32) {
                        os << ind << "_v" << inst.id << " = asfloat(_w64_lanes[" << lane_expr << "]);\n";
                    } else {
                        os << ind << "_v" << inst.id << " = _w64_lanes[" << lane_expr << "];\n";
                    }
                    break;
                }
                case IrOp::WavePermLane16: {
                    const uint32_t w_mask = ComputeWaveLanesCapacity(ir_) - 1u;
                    const uint32_t x16_xor = (inst.imm_u32 != 0u) ? 16u : 0u;
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "_w64_lanes[_gtid & " << w_mask << "u] = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    os << ind << "{\n";
                    os << ind << "    uint _pl_lane = _gtid & " << w_mask << "u;\n";
                    os << ind << "    uint _pl_row = _pl_lane & 15u;\n";
                    os << ind << "    uint _pl_base = (_pl_lane & ~15u) ^ " << x16_xor << "u;\n";
                    os << ind << "    uint _pl_word = (_pl_row < 8u) ? " << AsU32(inst.operands[1]) << " : " << AsU32(inst.operands[2]) << ";\n";
                    os << ind << "    uint _pl_sel = (_pl_word >> ((_pl_row & 7u) << 2u)) & 15u;\n";
                    if (inst.type == IrType::F32) {
                        os << ind << "    _v" << inst.id << " = asfloat(_w64_lanes[(_pl_base | _pl_sel) & " << w_mask << "u]);\n";
                    } else {
                        os << ind << "    _v" << inst.id << " = _w64_lanes[(_pl_base | _pl_sel) & " << w_mask << "u];\n";
                    }
                    os << ind << "}\n";
                    break;
                }
                case IrOp::SetMeshOutputCounts:
                    os << ind << "SetMeshOutputCounts(" << AsU32(inst.operands[0]) << ", "
                       << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::ExportMeshPrim:
                    if (inst.imm_u32 == 1u) {
                        const std::string p = AsU32(inst.operands[0]);
                        os << ind << "_prims[_gtid] = uint3((" << p << ") & 0x3FFu, ((" << p
                           << ") >> 10u) & 0x3FFu, ((" << p << ") >> 20u) & 0x3FFu);\n";
                    } else {
                        os << ind << "_prims[_gtid] = uint3(" << AsU32(inst.operands[0]) << ", "
                           << AsU32(inst.operands[1]) << ", " << AsU32(inst.operands[2]) << ");\n";
                    }
                    break;
                case IrOp::DispatchMeshGroups:
                    os << ind << "_dm_x = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "_dm_y = " << AsU32(inst.operands[1]) << ";\n";
                    os << ind << "_dm_z = " << AsU32(inst.operands[2]) << ";\n";
                    break;
                case IrOp::FAdd:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " + " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FSub:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " - " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FMul:
                    os << ind << "_v" << inst.id << " = (" << AsF32(inst.operands[0]) << " * " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FFma:
                    os << ind << "_v" << inst.id << " = mad(" << AsF32(inst.operands[0]) << ", "
                       << AsF32(inst.operands[1]) << ", " << AsF32(inst.operands[2]) << ");\n";
                    break;
                case IrOp::FMin:
                    os << ind << "_v" << inst.id << " = min(" << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FMax:
                    os << ind << "_v" << inst.id << " = max(" << AsF32(inst.operands[0]) << ", " << AsF32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::FRcp:
                    os << ind << "_v" << inst.id << " = rcp(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FRsq:
                    os << ind << "_v" << inst.id << " = rsqrt(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FSqrt:
                    os << ind << "_v" << inst.id << " = sqrt(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FFloor:
                    os << ind << "_v" << inst.id << " = floor(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FFract:
                    os << ind << "_v" << inst.id << " = frac(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::FDot2: {
                    const std::string a = AsU32(inst.operands[0]);
                    const std::string b = AsU32(inst.operands[1]);
                    const std::string c = AsF32(inst.operands[2]);
                    os << ind << "_v" << inst.id << " = (f16tof32(" << a << " & 0xFFFFu) * f16tof32(" << b
                       << " & 0xFFFFu) + f16tof32((" << a << " >> 16u) & 0xFFFFu) * f16tof32((" << b
                       << " >> 16u) & 0xFFFFu) + " << c << ");\n";
                    break;
                }
                case IrOp::IAdd:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " + " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::ISub:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " - " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::IMul:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " * " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::IMin:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(min(" << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << "));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = min(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    }
                    break;
                case IrOp::IMax:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(max(" << AsI32(inst.operands[0]) << ", " << AsI32(inst.operands[1]) << "));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = max(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1]) << ");\n";
                    }
                    break;
                case IrOp::BitAnd:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " & " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitOr:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " | " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitXor:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " ^ " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::BitNot:
                    os << ind << "_v" << inst.id << " = (~" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::Shl:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " << (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    break;
                case IrOp::Shr:
                    if (inst.type == IrType::I32) {
                        os << ind << "_v" << inst.id << " = asuint(" << AsI32(inst.operands[0]) << " >> (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    } else {
                        os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " >> (" << AsU32(inst.operands[1]) << " & 31u));\n";
                    }
                    break;
                case IrOp::BfeU32:
                    os << ind << "_v" << inst.id << " = ((" << AsU32(inst.operands[0]) << " >> ("
                       << AsU32(inst.operands[1]) << " & 31u)) & ((1u << ("
                       << AsU32(inst.operands[2]) << " & 31u)) - 1u));\n";
                    break;
                case IrOp::BfeI32:
                    os << ind << "{\n";
                    os << ind << "    uint _bfe_s = " << AsU32(inst.operands[0]) << ";\n";
                    os << ind << "    uint _bfe_o = " << AsU32(inst.operands[1]) << " & 31u;\n";
                    os << ind << "    uint _bfe_w = " << AsU32(inst.operands[2]) << " & 31u;\n";
                    os << ind << "    int _bfe_r = (_bfe_w == 0u) ? 0 : (((_bfe_o + _bfe_w) < 32u) ? (asint(_bfe_s << (32u - _bfe_o - _bfe_w)) >> (32u - _bfe_w)) : (asint(_bfe_s) >> _bfe_o));\n";
                    os << ind << "    _v" << inst.id << " = asuint(_bfe_r);\n";
                    os << ind << "}\n";
                    break;
                case IrOp::BfiB32:
                    os << ind << "_v" << inst.id << " = ((" << AsU32(inst.operands[0]) << " & "
                       << AsU32(inst.operands[1]) << ") | ((~" << AsU32(inst.operands[0]) << ") & "
                       << AsU32(inst.operands[2]) << "));\n";
                    break;
                case IrOp::PkAddF16:
                case IrOp::PkMulF16:
                case IrOp::PkFmaF16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const uint8_t neg_lo = static_cast<uint8_t>((f >> 8) & 7u);
                    const uint8_t neg_hi = static_cast<uint8_t>((f >> 12) & 7u);
                    const bool clamp = ((f >> 16) & 1u) != 0u;
                    auto lane_expr = [&](IrValueId v, int idx, bool hi_out) -> std::string {
                        const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> idx) & 1u) != 0u;
                        const bool do_neg = (((hi_out ? neg_hi : neg_lo) >> idx) & 1u) != 0u;
                        std::string ex = pick_hi
                                             ? ("f16tof32((" + AsU32(v) + " >> 16u) & 0xFFFFu)")
                                             : ("f16tof32(" + AsU32(v) + " & 0xFFFFu)");
                        return do_neg ? ("(-" + ex + ")") : ex;
                    };
                    os << ind << "{\n";
                    os << ind << "    float _a0 = " << lane_expr(inst.operands[0], 0, false) << ";\n";
                    os << ind << "    float _a1 = " << lane_expr(inst.operands[0], 0, true) << ";\n";
                    os << ind << "    float _b0 = " << lane_expr(inst.operands[1], 1, false) << ";\n";
                    os << ind << "    float _b1 = " << lane_expr(inst.operands[1], 1, true) << ";\n";
                    if (inst.op == IrOp::PkFmaF16) {
                        os << ind << "    float _c0 = " << lane_expr(inst.operands[2], 2, false) << ";\n";
                        os << ind << "    float _c1 = " << lane_expr(inst.operands[2], 2, true) << ";\n";
                        os << ind << "    float _r0 = mad(_a0, _b0, _c0);\n";
                        os << ind << "    float _r1 = mad(_a1, _b1, _c1);\n";
                    } else if (inst.op == IrOp::PkAddF16) {
                        os << ind << "    float _r0 = _a0 + _b0;\n";
                        os << ind << "    float _r1 = _a1 + _b1;\n";
                    } else {
                        os << ind << "    float _r0 = _a0 * _b0;\n";
                        os << ind << "    float _r1 = _a1 * _b1;\n";
                    }
                    if (clamp) {
                        os << ind << "    _r0 = saturate(_r0); _r1 = saturate(_r1);\n";
                    }
                    os << ind << "    _v" << inst.id << " = (f32tof16(_r0) & 0xFFFFu) | ((f32tof16(_r1) & 0xFFFFu) << 16u);\n";
                    os << ind << "}\n";
                    break;
                }
                case IrOp::PkAddU16:
                case IrOp::PkShl16:
                case IrOp::PkShr16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const bool is_shift = (inst.op == IrOp::PkShl16 || inst.op == IrOp::PkShr16);
                    auto u16_expr = [&](IrValueId v, int isa_idx, bool hi_out) -> std::string {
                        const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> isa_idx) & 1u) != 0u;
                        return pick_hi ? ("((" + AsU32(v) + " >> 16u) & 0xFFFFu)")
                                       : ("(" + AsU32(v) + " & 0xFFFFu)");
                    };
                    os << ind << "{\n";
                    os << ind << "    uint _a0 = " << u16_expr(inst.operands[0], is_shift ? 1 : 0, false) << ";\n";
                    os << ind << "    uint _a1 = " << u16_expr(inst.operands[0], is_shift ? 1 : 0, true) << ";\n";
                    os << ind << "    uint _b0 = " << u16_expr(inst.operands[1], is_shift ? 0 : 1, false) << ";\n";
                    os << ind << "    uint _b1 = " << u16_expr(inst.operands[1], is_shift ? 0 : 1, true) << ";\n";
                    if (inst.op == IrOp::PkAddU16) {
                        os << ind << "    uint _r0 = (_a0 + _b0) & 0xFFFFu;\n";
                        os << ind << "    uint _r1 = (_a1 + _b1) & 0xFFFFu;\n";
                    } else if (inst.op == IrOp::PkShl16) {
                        os << ind << "    uint _r0 = (_a0 << (_b0 & 15u)) & 0xFFFFu;\n";
                        os << ind << "    uint _r1 = (_a1 << (_b1 & 15u)) & 0xFFFFu;\n";
                    } else {
                        os << ind << "    uint _r0 = (_a0 >> (_b0 & 15u)) & 0xFFFFu;\n";
                        os << ind << "    uint _r1 = (_a1 >> (_b1 & 15u)) & 0xFFFFu;\n";
                    }
                    os << ind << "    _v" << inst.id << " = _r0 | (_r1 << 16u);\n";
                    os << ind << "}\n";
                    break;
                }
                case IrOp::I2F:
                    os << ind << "_v" << inst.id << " = (float)(" << AsI32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::U2F:
                    os << ind << "_v" << inst.id << " = (float)(" << AsU32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::F2I:
                    os << ind << "_v" << inst.id << " = asuint((int)(" << AsF32(inst.operands[0]) << "));\n";
                    break;
                case IrOp::F2U:
                    os << ind << "_v" << inst.id << " = (uint)(" << AsF32(inst.operands[0]) << ");\n";
                    break;
                case IrOp::CmpEq:
                case IrOp::CmpNe:
                case IrOp::CmpLt:
                case IrOp::CmpGt:
                case IrOp::CmpGe: {
                    const bool is_flt = (TypeOf(inst.operands[0]) == IrType::F32 || TypeOf(inst.operands[1]) == IrType::F32);
                    const std::string a = is_flt ? AsF32(inst.operands[0]) : AsI32(inst.operands[0]);
                    const std::string b = is_flt ? AsF32(inst.operands[1]) : AsI32(inst.operands[1]);
                    const char* sym = "==";
                    if (inst.op == IrOp::CmpNe) sym = "!=";
                    else if (inst.op == IrOp::CmpLt) sym = "<";
                    else if (inst.op == IrOp::CmpGt) sym = ">";
                    else if (inst.op == IrOp::CmpGe) sym = ">=";
                    os << ind << "_v" << inst.id << " = (" << a << " " << sym << " " << b << ");\n";
                    break;
                }
                case IrOp::CmpLtU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " < " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpEqU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " == " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpGtU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " > " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::CmpGeU:
                    os << ind << "_v" << inst.id << " = (" << AsU32(inst.operands[0]) << " >= " << AsU32(inst.operands[1]) << ");\n";
                    break;
                case IrOp::Select:
                    if (inst.type == IrType::F32) {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsF32(inst.operands[1]) << " : " << AsF32(inst.operands[2]) << ");\n";
                    } else if (inst.type == IrType::Bool) {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsBool(inst.operands[1]) << " : " << AsBool(inst.operands[2]) << ");\n";
                    } else {
                        os << ind << "_v" << inst.id << " = (" << AsBool(inst.operands[0]) << " ? "
                           << AsU32(inst.operands[1]) << " : " << AsU32(inst.operands[2]) << ");\n";
                    }
                    break;
                case IrOp::Barrier:
                    if (ir_.stage == ShaderStage::Compute || ir_.stage == ShaderStage::Mesh ||
                        ir_.stage == ShaderStage::Amplification) {
                        os << ind << "GroupMemoryBarrierWithGroupSync();\n";
                    }
                    break;
                case IrOp::Export: {
                    const std::string vec = "float4(" + AsF32(inst.operands[0]) + ", " + AsF32(inst.operands[1]) +
                                            ", " + AsF32(inst.operands[2]) + ", " + AsF32(inst.operands[3]) + ")";
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0)) {
                        if (ir_.stage == ShaderStage::Vertex) {
                            os << ind << "_out.pos = " << vec << ";\n";
                        } else if (ir_.stage == ShaderStage::Mesh) {
                            os << ind << "_verts[_gtid].pos = " << vec << ";\n";
                        }
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0)) {
                        if (ir_.stage == ShaderStage::Vertex) {
                            os << ind << "_out.param0 = " << vec << ";\n";
                        } else if (ir_.stage == ShaderStage::Mesh) {
                            os << ind << "_verts[_gtid].param0 = " << vec << ";\n";
                        }
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0) &&
                               ir_.stage == ShaderStage::Pixel) {
                        os << ind << "_mrt0 = " << vec << ";\n";
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    uint32_t SingleBranchTarget(uint32_t bb_id) const {
        if (bb_id >= ir_.blocks.size()) return UINT32_MAX;
        const auto& ins = ir_.blocks[bb_id].instructions;
        if (ins.empty()) return UINT32_MAX;
        if (ins.back().op == IrOp::Branch) return ins.back().true_block;
        return UINT32_MAX;
    }

    bool IsReturnBlock(uint32_t bb_id) const {
        if (bb_id >= ir_.blocks.size()) return false;
        const auto& ins = ir_.blocks[bb_id].instructions;
        return !ins.empty() && ins.back().op == IrOp::Return;
    }

    bool EmitStructured(
        std::ostringstream& os,
        uint32_t bb_id,
        uint32_t stop_bb,
        int depth,
        std::unordered_set<uint32_t>& active) const {
        while (bb_id != stop_bb) {
            if (bb_id >= ir_.blocks.size()) return false;
            if (!active.insert(bb_id).second) return false;

            const auto& bb = ir_.blocks[bb_id];
            EmitNonTerminators(os, bb, depth);
            if (bb.instructions.empty()) {
                active.erase(bb_id);
                return true;
            }

            const auto& term = bb.instructions.back();
            if (term.op == IrOp::Return) {
                os << Indent(depth) << EmitReturnStmt() << "\n";
                active.erase(bb_id);
                return true;
            }
            if (term.op == IrOp::Branch) {
                const uint32_t next = term.true_block;
                EmitPhiCopies(os, bb_id, next, depth);
                active.erase(bb_id);
                bb_id = next;
                continue;
            }
            if (term.op == IrOp::CondBranch) {
                const uint32_t t_bb = term.true_block;
                const uint32_t f_bb = term.false_block;
                const std::string cond = AsBool(term.operands[0]);

                const uint32_t t_next = SingleBranchTarget(t_bb);
                const uint32_t f_next = SingleBranchTarget(f_bb);

                if (t_bb != f_bb && t_next == f_bb && t_bb != stop_bb) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, f_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}";
                    if (HasPhiCopies(bb_id, f_bb)) {
                        os << " else {\n";
                        EmitPhiCopies(os, bb_id, f_bb, depth + 1);
                        os << Indent(depth) << "}";
                    }
                    os << "\n";
                    active.erase(bb_id);
                    bb_id = f_bb;
                    continue;
                }

                if (t_bb != f_bb && f_next == t_bb && f_bb != stop_bb) {
                    os << Indent(depth) << "if (!(" << cond << ")) {\n";
                    if (!EmitStructured(os, f_bb, t_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}";
                    if (HasPhiCopies(bb_id, t_bb)) {
                        os << " else {\n";
                        EmitPhiCopies(os, bb_id, t_bb, depth + 1);
                        os << Indent(depth) << "}";
                    }
                    os << "\n";
                    active.erase(bb_id);
                    bb_id = t_bb;
                    continue;
                }

                if (t_bb != f_bb && t_next != UINT32_MAX && t_next == f_next) {
                    const uint32_t join_bb = t_next;
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, join_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "} else {\n";
                    if (!EmitStructured(os, f_bb, join_bb, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = join_bb;
                    continue;
                }

                if (IsReturnBlock(t_bb) && !IsReturnBlock(f_bb)) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = f_bb;
                    continue;
                }
                if (IsReturnBlock(f_bb) && !IsReturnBlock(t_bb)) {
                    os << Indent(depth) << "if (!(" << cond << ")) {\n";
                    if (!EmitStructured(os, f_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    bb_id = t_bb;
                    continue;
                }
                if (IsReturnBlock(t_bb) && IsReturnBlock(f_bb)) {
                    os << Indent(depth) << "if (" << cond << ") {\n";
                    if (!EmitStructured(os, t_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "} else {\n";
                    if (!EmitStructured(os, f_bb, UINT32_MAX, depth + 1, active)) return false;
                    os << Indent(depth) << "}\n";
                    active.erase(bb_id);
                    return true;
                }

                return false;
            }

            active.erase(bb_id);
            return true;
        }
        return true;
    }

    void EmitStateMachine(std::ostringstream& os) const {
        os << "    uint _bb = 0u;\n";
        os << "    [loop] while (_bb != 0xFFFFFFFFu) {\n";
        for (size_t i = 0; i < ir_.blocks.size(); ++i) {
            const auto& bb = ir_.blocks[i];
            os << "        " << (i == 0 ? "if" : "else if") << " (_bb == " << bb.id << "u) {\n";
            EmitNonTerminators(os, bb, 3);
            if (!bb.instructions.empty()) {
                const auto& term = bb.instructions.back();
                if (term.op == IrOp::Return) {
                    os << "            _bb = 0xFFFFFFFFu;\n";
                } else if (term.op == IrOp::Branch) {
                    EmitPhiCopies(os, bb.id, term.true_block, 3);
                    os << "            _bb = " << term.true_block << "u;\n";
                } else if (term.op == IrOp::CondBranch) {
                    os << "            if (" << AsBool(term.operands[0]) << ") {\n";
                    EmitPhiCopies(os, bb.id, term.true_block, 4);
                    os << "                _bb = " << term.true_block << "u;\n";
                    os << "            } else {\n";
                    EmitPhiCopies(os, bb.id, term.false_block, 4);
                    os << "                _bb = " << term.false_block << "u;\n";
                    os << "            }\n";
                } else {
                    os << "            _bb = 0xFFFFFFFFu;\n";
                }
            } else {
                os << "            _bb = 0xFFFFFFFFu;\n";
            }
            os << "        }\n";
        }
        os << "        else { _bb = 0xFFFFFFFFu; }\n";
        os << "    }\n";
    }

    const Rdna2IrModule& ir_;
    bool use_sm65_ = true;
    std::unordered_map<IrValueId, const IrInstruction*> defs_;
};

}  // namespace

// ---------------------------------------------------------------------------
// SPIR-V 1.3 Binary Emitter (Strict Vulkan 1.1 / 1.2 spirv-val Compliance)
// ---------------------------------------------------------------------------
Status EmitSpirv(const Rdna2IrModule& ir, std::vector<uint32_t>& out_spirv) {
    out_spirv.clear();
    if (ir.blocks.empty()) {
        CallTelemetry::Global().Record("rdna2_lifter", "EmitSpirv", CallOutcome::Error, -1);
        return Status::Error(StatusCode::ShaderError, "Cannot emit SPIR-V from empty IR module");
    }
    if (ir.stage != ShaderStage::Compute && ir.stage != ShaderStage::Vertex && ir.stage != ShaderStage::Pixel) {
        CallTelemetry::Global().Record("rdna2_lifter", "EmitSpirv", CallOutcome::NotImplemented, -1);
        return Status::Error(StatusCode::UnsupportedFormat,
                             "SPIR-V 1.3 emitter supports Compute/Vertex/Pixel (use DXC SM 6.5 for Mesh/AS)");
    }

    uint32_t next_id = 1;
    auto alloc_id = [&]() -> uint32_t { return next_id++; };

    std::vector<uint32_t> sec_caps;
    std::vector<uint32_t> sec_ext;
    std::vector<uint32_t> sec_mem;
    std::vector<uint32_t> sec_entry;
    std::vector<uint32_t> sec_exec;
    std::vector<uint32_t> sec_decor;
    std::vector<uint32_t> sec_types;
    std::vector<uint32_t> sec_globals;
    std::vector<uint32_t> sec_fn;

    // 1. Capabilities, ExtInstImport, MemoryModel
    EmitSpvInst(sec_caps, SpvOpCapability, {1u});  // Shader
    const uint32_t glsl_ext_id = alloc_id();
    {
        std::vector<uint32_t> ops = {glsl_ext_id};
        AppendSpvString(ops, "GLSL.std.450");
        EmitSpvInst(sec_ext, SpvOpExtInstImport, ops);
    }
    EmitSpvInst(sec_mem, SpvOpMemoryModel, {0u, 1u});  // Logical, GLSL450

    // 2. Core scalar / vector / function types
    const uint32_t t_void = alloc_id();
    const uint32_t t_bool = alloc_id();
    const uint32_t t_u32 = alloc_id();
    const uint32_t t_i32 = alloc_id();
    const uint32_t t_f32 = alloc_id();
    const uint32_t t_v2f32 = alloc_id();
    const uint32_t t_v2i32 = alloc_id();
    const uint32_t t_v3i32 = alloc_id();
    const uint32_t t_v3u32 = alloc_id();
    const uint32_t t_v4u32 = alloc_id();
    const uint32_t t_v4f32 = alloc_id();
    const uint32_t t_fn_void = alloc_id();

    EmitSpvInst(sec_types, SpvOpTypeVoid, {t_void});
    EmitSpvInst(sec_types, SpvOpTypeBool, {t_bool});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_u32, 32u, 0u});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_i32, 32u, 1u});
    EmitSpvInst(sec_types, SpvOpTypeFloat, {t_f32, 32u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v2f32, t_f32, 2u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v2i32, t_i32, 2u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v3i32, t_i32, 3u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v3u32, t_u32, 3u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v4u32, t_u32, 4u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v4f32, t_f32, 4u});
    EmitSpvInst(sec_types, SpvOpTypeFunction, {t_fn_void, t_void});

    // Deduplicated constant pool (emitted into sec_types)
    std::unordered_map<uint32_t, uint32_t> const_u32_map;
    std::unordered_map<int32_t, uint32_t> const_i32_map;
    std::unordered_map<uint32_t, uint32_t> const_f32_map;
    uint32_t const_true_id = 0;
    uint32_t const_false_id = 0;

    auto get_const_u32 = [&](uint32_t v) -> uint32_t {
        auto it = const_u32_map.find(v);
        if (it != const_u32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_u32, id, v});
        const_u32_map[v] = id;
        return id;
    };

    auto get_const_i32 = [&](int32_t v) -> uint32_t {
        auto it = const_i32_map.find(v);
        if (it != const_i32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_i32, id, static_cast<uint32_t>(v)});
        const_i32_map[v] = id;
        return id;
    };

    auto get_const_f32 = [&](float f) -> uint32_t {
        const uint32_t bits = FloatToBits(f);
        auto it = const_f32_map.find(bits);
        if (it != const_f32_map.end()) return it->second;
        uint32_t id = alloc_id();
        EmitSpvInst(sec_types, SpvOpConstant, {t_f32, id, bits});
        const_f32_map[bits] = id;
        return id;
    };

    auto get_const_bool = [&](bool v) -> uint32_t {
        if (v) {
            if (!const_true_id) {
                const_true_id = alloc_id();
                EmitSpvInst(sec_types, SpvOpConstantTrue, {t_bool, const_true_id});
            }
            return const_true_id;
        }
        if (!const_false_id) {
            const_false_id = alloc_id();
            EmitSpvInst(sec_types, SpvOpConstantFalse, {t_bool, const_false_id});
        }
        return const_false_id;
    };

    const uint32_t c_u32_0 = get_const_u32(0u);
    const uint32_t c_u32_1 = get_const_u32(1u);
    const uint32_t c_u32_2 = get_const_u32(2u);
    const uint32_t c_u32_5 = get_const_u32(5u);
    const uint32_t c_u32_7 = get_const_u32(7u);
    const uint32_t c_u32_8 = get_const_u32(8u);
    const uint32_t c_u32_15 = get_const_u32(15u);
    const uint32_t c_u32_16 = get_const_u32(16u);
    const uint32_t c_u32_31 = get_const_u32(31u);
    const uint32_t c_u32_32 = get_const_u32(32u);
    const uint32_t c_u32_63 = get_const_u32(63u);
    const uint32_t c_u32_65535 = get_const_u32(0xFFFFu);
    const uint32_t c_u32_not15 = get_const_u32(~15u);
    const uint32_t c_u32_allones = get_const_u32(0xFFFFFFFFu);
    const uint32_t c_i32_0 = get_const_i32(0);
    const uint32_t c_f32_0 = get_const_f32(0.0f);
    const uint32_t c_f32_1 = get_const_f32(1.0f);

    // 3. Interface variables, descriptors, and workgroup shared memory
    std::vector<uint32_t> interface_vars;
    uint32_t var_gid = 0;
    uint32_t var_local_idx = 0;
    uint32_t t_ptr_in_u32 = 0;
    uint32_t var_vid = 0;
    uint32_t var_pervertex = 0;
    uint32_t t_ptr_out_v4f32 = 0;
    uint32_t var_param0_out = 0;
    uint32_t var_param0_in = 0;
    uint32_t var_mrt0 = 0;

    // Storage buffers (SPIR-V 1.3 core StorageBuffer + Block)
    uint32_t var_uav0 = 0;
    uint32_t var_uav1 = 0;
    uint32_t t_ptr_sb_u32 = 0;
    const bool uses_uav0 = ir.uses_uav0 || (ir.uses_uav && !ir.uses_uav1);
    const bool uses_uav1 = ir.uses_uav1;

    if (uses_uav0 || uses_uav1) {
        const uint32_t t_rta_u32 = alloc_id();
        const uint32_t t_uav_struct = alloc_id();
        const uint32_t t_ptr_sb_struct = alloc_id();
        t_ptr_sb_u32 = alloc_id();

        EmitSpvInst(sec_types, SpvOpTypeRuntimeArray, {t_rta_u32, t_u32});
        EmitSpvInst(sec_types, SpvOpTypeStruct, {t_uav_struct, t_rta_u32});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_sb_struct, kSpvStorageStorageBuffer, t_uav_struct});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_sb_u32, kSpvStorageStorageBuffer, t_u32});

        EmitSpvInst(sec_decor, SpvOpDecorate, {t_rta_u32, kSpvDecArrayStride, 4u});
        EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_uav_struct, 0u, kSpvDecOffset, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_uav_struct, kSpvDecBlock});

        if (uses_uav0) {
            var_uav0 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_sb_struct, var_uav0, kSpvStorageStorageBuffer});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav0, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav0, kSpvDecBinding, 0u});
        }
        if (uses_uav1) {
            var_uav1 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_sb_struct, var_uav1, kSpvStorageStorageBuffer});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav1, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_uav1, kSpvDecBinding, 1u});
        }
    }

    // Root constants UBO (individual scalar uint members -> 100% std140 compliant)
    uint32_t var_rc = 0;
    uint32_t t_ptr_unif_u32 = 0;
    if (ir.uses_root_constants) {
        const uint32_t rc_count = ir.max_root_const_index + 1u;
        const uint32_t t_rc_struct = alloc_id();
        std::vector<uint32_t> struct_ops = {t_rc_struct};
        for (uint32_t i = 0; i < rc_count; ++i) {
            struct_ops.push_back(t_u32);
            EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_rc_struct, i, kSpvDecOffset, i * 4u});
        }
        EmitSpvInst(sec_types, SpvOpTypeStruct, struct_ops);
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_rc_struct, kSpvDecBlock});

        const uint32_t t_ptr_rc_struct = alloc_id();
        t_ptr_unif_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_rc_struct, kSpvStorageUniform, t_rc_struct});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_unif_u32, kSpvStorageUniform, t_u32});

        var_rc = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_rc_struct, var_rc, kSpvStorageUniform});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rc, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rc, kSpvDecBinding, 2u});
    }

    // MIMG Texture & Sampler descriptors
    uint32_t t_img2d = 0;
    uint32_t t_sampler = 0;
    uint32_t t_sampled_img2d = 0;
    uint32_t var_tex0 = 0;
    uint32_t var_samp0 = 0;
    uint32_t t_rwimg2d = 0;
    uint32_t var_rwtex0 = 0;
    uint32_t var_img_ro = 0;
    uint32_t t_img3d = 0;
    uint32_t var_tex3d = 0;
    uint32_t t_imgcube = 0;
    uint32_t var_texcube = 0;
    uint32_t t_rwimg_u32 = 0;
    uint32_t t_ptr_img_u32 = 0;
    uint32_t var_rwimg_u32 = 0;

    if (ir.uses_texture_sample || ir.uses_texture2d_mip_srv) {
        t_img2d = alloc_id();
        const uint32_t t_ptr_uc_img = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_img2d, t_f32, 1u, 0u, 0u, 0u, 1u, kSpvImgFmtUnknown});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_img, kSpvStorageUniformConstant, t_img2d});
        var_tex0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_img, var_tex0, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex0, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex0, kSpvDecBinding, 3u});

        if (ir.uses_texture_sample) {
            t_sampler = alloc_id();
            t_sampled_img2d = alloc_id();
            const uint32_t t_ptr_uc_samp = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypeSampler, {t_sampler});
            EmitSpvInst(sec_types, SpvOpTypeSampledImage, {t_sampled_img2d, t_img2d});
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_samp, kSpvStorageUniformConstant, t_sampler});
            var_samp0 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_samp, var_samp0, kSpvStorageUniformConstant});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_samp0, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_samp0, kSpvDecBinding, 4u});
        }
    }

    if (ir.uses_texture_uav || ir.uses_texture_load) {
        t_rwimg2d = alloc_id();
        const uint32_t t_ptr_uc_rwimg = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_rwimg2d, t_f32, 1u, 0u, 0u, 0u, 2u, kSpvImgFmtRgba32f});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_rwimg, kSpvStorageUniformConstant, t_rwimg2d});

        if (ir.uses_texture_uav) {
            var_rwtex0 = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_rwimg, var_rwtex0, kSpvStorageUniformConstant});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwtex0, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwtex0, kSpvDecBinding, 5u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwtex0, kSpvDecNonReadable});
        }
        if (ir.uses_texture_load) {
            var_img_ro = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_rwimg, var_img_ro, kSpvStorageUniformConstant});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecDescriptorSet, 0u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecBinding, 6u});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_img_ro, kSpvDecNonWritable});
        }
    }

    if (ir.uses_texture3d_srv) {
        t_img3d = alloc_id();
        const uint32_t t_ptr_uc_img3d = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_img3d, t_f32, 2u, 0u, 0u, 0u, 1u, kSpvImgFmtUnknown});  // 3D
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_img3d, kSpvStorageUniformConstant, t_img3d});
        var_tex3d = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_img3d, var_tex3d, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex3d, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex3d, kSpvDecBinding, 7u});
    }

    if (ir.uses_texture_cube_srv) {
        t_imgcube = alloc_id();
        const uint32_t t_ptr_uc_cube = alloc_id();
        // 2D Arrayed (6 cube faces) for integer texel + mip OpImageFetch
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_imgcube, t_f32, 1u, 0u, 1u, 0u, 1u, kSpvImgFmtUnknown});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_cube, kSpvStorageUniformConstant, t_imgcube});
        var_texcube = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_cube, var_texcube, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_texcube, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_texcube, kSpvDecBinding, 8u});
    }

    if (ir.uses_texture_u32_uav) {
        t_rwimg_u32 = alloc_id();
        const uint32_t t_ptr_uc_u32 = alloc_id();
        t_ptr_img_u32 = alloc_id();
        const uint32_t spv_dim = (ir.texture_u32_uav_dim == 2u) ? 2u : 1u;
        const uint32_t spv_arr = (ir.texture_u32_uav_dim == 3u) ? 1u : 0u;
        EmitSpvInst(sec_types, SpvOpTypeImage,
                    {t_rwimg_u32, t_u32, spv_dim, 0u, spv_arr, 0u, 2u, kSpvImgFmtR32ui});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_u32, kSpvStorageUniformConstant, t_rwimg_u32});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_img_u32, kSpvStorageImage, t_u32});
        var_rwimg_u32 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_u32, var_rwimg_u32, kSpvStorageUniformConstant});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwimg_u32, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_rwimg_u32, kSpvDecBinding, 9u});
    }

    // Thread-private scratch memory (Private storage class array, zero-initialized)
    uint32_t t_ptr_priv_u32 = 0;
    uint32_t var_scratch = 0;
    const uint32_t scratch_size_dwords = ir.scratch_size_dwords ? ir.scratch_size_dwords : 64u;
    const uint32_t c_scratch_mask = get_const_u32(scratch_size_dwords - 1u);
    if (ir.uses_scratch) {
        const uint32_t c_scr_len = get_const_u32(scratch_size_dwords);
        const uint32_t t_scr_arr = alloc_id();
        const uint32_t t_ptr_priv_scr = alloc_id();
        t_ptr_priv_u32 = alloc_id();
        const uint32_t c_null_scr = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_scr_arr, t_u32, c_scr_len});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_priv_scr, kSpvStoragePrivate, t_scr_arr});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_priv_u32, kSpvStoragePrivate, t_u32});
        EmitSpvInst(sec_types, SpvOpConstantNull, {t_scr_arr, c_null_scr});
        var_scratch = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_priv_scr, var_scratch, kSpvStoragePrivate, c_null_scr});
    }

    // Workgroup shared memory for LDS and/or wave64->wave32 emulation
    uint32_t t_ptr_wg_u32 = 0;
    uint32_t var_lds = 0;
    uint32_t var_w64_ballot = 0;
    uint32_t var_w64_lanes = 0;
    const uint32_t lds_size_dwords = ir.lds_size_dwords ? ir.lds_size_dwords : 1024u;
    const uint32_t c_lds_mask = get_const_u32(lds_size_dwords - 1u);
    const uint32_t w_lanes_cap = ComputeWaveLanesCapacity(ir);
    const uint32_t c_w_lanes_mask = get_const_u32(w_lanes_cap - 1u);

    if (ir.uses_lds || ir.uses_wave64_emulation) {
        t_ptr_wg_u32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_u32, kSpvStorageWorkgroup, t_u32});
    }
    if (ir.uses_lds) {
        const uint32_t c_lds_len = get_const_u32(lds_size_dwords);
        const uint32_t t_lds_arr = alloc_id();
        const uint32_t t_ptr_wg_lds = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_lds_arr, t_u32, c_lds_len});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_lds, kSpvStorageWorkgroup, t_lds_arr});
        var_lds = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_lds, var_lds, kSpvStorageWorkgroup});
    }
    if (ir.uses_wave64_emulation) {
        const uint32_t c_len_2 = get_const_u32(2u);
        const uint32_t c_len_lanes = get_const_u32(w_lanes_cap);
        const uint32_t t_arr_2 = alloc_id();
        const uint32_t t_arr_lanes = alloc_id();
        const uint32_t t_ptr_wg_2 = alloc_id();
        const uint32_t t_ptr_wg_lanes = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_2, t_u32, c_len_2});
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_lanes, t_u32, c_len_lanes});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_2, kSpvStorageWorkgroup, t_arr_2});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_lanes, kSpvStorageWorkgroup, t_arr_lanes});
        var_w64_ballot = alloc_id();
        var_w64_lanes = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_2, var_w64_ballot, kSpvStorageWorkgroup});
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_lanes, var_w64_lanes, kSpvStorageWorkgroup});
    }

    const bool uses_tid = ModuleUsesThreadId(ir);
    const bool uses_stage_in = ModuleUsesStageInput(ir);

    if (ir.stage == ShaderStage::Compute) {
        if (uses_tid || ir.uses_wave64_emulation) {
            t_ptr_in_u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_u32, kSpvStorageInput, t_u32});
        }
        if (uses_tid) {
            const uint32_t t_ptr_in_v3u32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v3u32, kSpvStorageInput, t_v3u32});
            var_gid = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v3u32, var_gid, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_gid, kSpvDecBuiltIn, kSpvBuiltInGlobalInvocationId});
            interface_vars.push_back(var_gid);
        }
        if (ir.uses_wave64_emulation) {
            var_local_idx = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_u32, var_local_idx, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_local_idx, kSpvDecBuiltIn, kSpvBuiltInLocalInvocationIndex});
            interface_vars.push_back(var_local_idx);
        }
    }

    if (ir.stage == ShaderStage::Vertex) {
        if (uses_tid) {
            const uint32_t t_ptr_in_i32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_i32, kSpvStorageInput, t_i32});
            var_vid = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_i32, var_vid, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_vid, kSpvDecBuiltIn, kSpvBuiltInVertexIndex});
            interface_vars.push_back(var_vid);
        }
        // Vulkan requires BuiltIn Position in a Block-decorated gl_PerVertex struct
        const uint32_t t_pervertex = alloc_id();
        const uint32_t t_ptr_out_pv = alloc_id();
        t_ptr_out_v4f32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeStruct, {t_pervertex, t_v4f32});
        EmitSpvInst(sec_decor, SpvOpDecorate, {t_pervertex, kSpvDecBlock});
        EmitSpvInst(sec_decor, SpvOpMemberDecorate, {t_pervertex, 0u, kSpvDecBuiltIn, kSpvBuiltInPosition});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_pv, kSpvStorageOutput, t_pervertex});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v4f32, kSpvStorageOutput, t_v4f32});
        var_pervertex = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_pv, var_pervertex, kSpvStorageOutput});
        interface_vars.push_back(var_pervertex);

        if (ir.exports_param0) {
            var_param0_out = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_v4f32, var_param0_out, kSpvStorageOutput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_param0_out, kSpvDecLocation, 0u});
            interface_vars.push_back(var_param0_out);
        }
    }

    if (ir.stage == ShaderStage::Pixel) {
        if (uses_stage_in) {
            const uint32_t t_ptr_in_v4f32 = alloc_id();
            EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_in_v4f32, kSpvStorageInput, t_v4f32});
            var_param0_in = alloc_id();
            EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_in_v4f32, var_param0_in, kSpvStorageInput});
            EmitSpvInst(sec_decor, SpvOpDecorate, {var_param0_in, kSpvDecLocation, 0u});
            interface_vars.push_back(var_param0_in);
        }
        t_ptr_out_v4f32 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_out_v4f32, kSpvStorageOutput, t_v4f32});
        var_mrt0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_out_v4f32, var_mrt0, kSpvStorageOutput});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_mrt0, kSpvDecLocation, 0u});
        interface_vars.push_back(var_mrt0);
    }

    // 4. Entry point & execution mode
    const uint32_t main_fn_id = alloc_id();
    {
        uint32_t exec_model = 5u;  // GLCompute
        if (ir.stage == ShaderStage::Vertex) exec_model = 0u;
        else if (ir.stage == ShaderStage::Pixel) exec_model = 4u;

        std::vector<uint32_t> ops = {exec_model, main_fn_id};
        AppendSpvString(ops, "main");
        ops.insert(ops.end(), interface_vars.begin(), interface_vars.end());
        EmitSpvInst(sec_entry, SpvOpEntryPoint, ops);

        if (ir.stage == ShaderStage::Compute) {
            EmitSpvInst(sec_exec, SpvOpExecutionMode,
                        {main_fn_id, 17u, ir.threadgroup_x, ir.threadgroup_y, ir.threadgroup_z});
        } else if (ir.stage == ShaderStage::Pixel) {
            EmitSpvInst(sec_exec, SpvOpExecutionMode, {main_fn_id, 7u});  // OriginUpperLeft
        }
    }

    // 5. Block labels, immediate post-dominators (for OpSelectionMerge), and value IDs
    const std::vector<uint32_t> ipdom = ComputeImmediatePostDominators(ir);
    const bool has_cond_branch = ModuleHasCondBranch(ir);
    const uint32_t exit_node = static_cast<uint32_t>(ir.blocks.size());
    bool need_epilogue = false;
    if (has_cond_branch) {
        for (size_t b = 0; b < ir.blocks.size(); ++b) {
            const auto& ins = ir.blocks[b].instructions;
            if (!ins.empty() && ins.back().op == IrOp::CondBranch && ipdom[b] == exit_node) {
                need_epilogue = true;
                break;
            }
        }
    }

    std::vector<uint32_t> bb_labels(ir.blocks.size(), 0u);
    for (size_t i = 0; i < ir.blocks.size(); ++i) {
        bb_labels[i] = alloc_id();
    }
    const uint32_t epilogue_label = need_epilogue ? alloc_id() : 0u;

    std::unordered_map<IrValueId, uint32_t> val_spv_id;
    std::unordered_map<IrValueId, SpvValKind> val_kind;
    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.id == kInvalidIrValue) continue;
            val_kind[inst.id] = CanonicalSpvKind(inst.type);
            if (inst.op == IrOp::ConstU32) {
                if (inst.type == IrType::Bool) {
                    val_spv_id[inst.id] = get_const_bool(inst.imm_u32 != 0u);
                } else {
                    val_spv_id[inst.id] = get_const_u32(inst.imm_u32);
                }
            } else if (inst.op == IrOp::ConstF32) {
                val_spv_id[inst.id] = get_const_f32(inst.imm_f32);
            } else {
                val_spv_id[inst.id] = alloc_id();
            }
        }
    }

    auto spv_type_for_kind = [&](SpvValKind k) -> uint32_t {
        if (k == SpvValKind::F32) return t_f32;
        if (k == SpvValKind::Bool) return t_bool;
        return t_u32;
    };

    auto get_as_u32 = [&](IrValueId v) -> uint32_t {
        const uint32_t id = val_spv_id[v];
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::U32) return id;
        if (k == SpvValKind::F32) {
            uint32_t cast_id = alloc_id();
            EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, cast_id, id});
            return cast_id;
        }
        uint32_t sel_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sel_id, id, c_u32_1, c_u32_0});
        return sel_id;
    };

    auto get_as_i32 = [&](IrValueId v) -> uint32_t {
        uint32_t u = get_as_u32(v);
        uint32_t i = alloc_id();
        EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, i, u});
        return i;
    };

    auto get_as_f32 = [&](IrValueId v) -> uint32_t {
        const uint32_t id = val_spv_id[v];
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::F32) return id;
        uint32_t u = get_as_u32(v);
        uint32_t cast_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, cast_id, u});
        return cast_id;
    };

    auto get_as_bool = [&](IrValueId v) -> uint32_t {
        const uint32_t id = val_spv_id[v];
        const SpvValKind k = val_kind[v];
        if (k == SpvValKind::Bool) return id;
        uint32_t u = get_as_u32(v);
        uint32_t cmp_id = alloc_id();
        EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, cmp_id, u, c_u32_0});
        return cmp_id;
    };

    auto emit_wg_barrier = [&]() {
        const uint32_t c_scope = get_const_u32(kSpvScopeWorkgroup);
        const uint32_t c_sem = get_const_u32(kSpvMemSemAcqRelWorkgroup);
        EmitSpvInst(sec_fn, SpvOpControlBarrier, {c_scope, c_scope, c_sem});
    };

    auto emit_lane_id = [&]() -> uint32_t {
        const uint32_t raw_idx = alloc_id();
        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, raw_idx, var_local_idx});
        const uint32_t mask = (ir.wave_size == 64u) ? c_u32_63 : c_u32_31;
        const uint32_t lane = alloc_id();
        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lane, raw_idx, mask});
        return lane;
    };

    // Precompute any Phi incoming edge coercions
    struct PhiEdgeFixup {
        uint32_t target_phi_id;
        SpvValKind expected_kind;
        IrValueId incoming_val;
        uint32_t coerced_spv_id;
    };
    std::unordered_map<uint64_t, uint32_t> phi_edge_override;
    std::vector<std::vector<PhiEdgeFixup>> block_exit_fixups(ir.blocks.size());

    for (const auto& bb : ir.blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.op != IrOp::Phi) continue;
            const SpvValKind expected = CanonicalSpvKind(inst.type);
            for (const auto& inc : inst.phi_incoming) {
                const uint32_t pred = inc.first;
                const IrValueId v = inc.second;
                if (val_kind[v] != expected && pred < ir.blocks.size()) {
                    const uint32_t coerced = alloc_id();
                    block_exit_fixups[pred].push_back({inst.id, expected, v, coerced});
                    phi_edge_override[(static_cast<uint64_t>(inst.id) << 32) | pred] = coerced;
                }
            }
        }
    }

    // 6. Emit Function Body
    EmitSpvInst(sec_fn, SpvOpFunction, {t_void, main_fn_id, 0u, t_fn_void});

    for (size_t bi = 0; bi < ir.blocks.size(); ++bi) {
        const auto& bb = ir.blocks[bi];
        EmitSpvInst(sec_fn, SpvOpLabel, {bb_labels[bi]});

        // OpPhi strictly first after OpLabel
        for (const auto& inst : bb.instructions) {
            if (inst.op != IrOp::Phi) continue;
            const uint32_t res_type = spv_type_for_kind(CanonicalSpvKind(inst.type));
            std::vector<uint32_t> ops = {res_type, val_spv_id[inst.id]};
            for (const auto& inc : inst.phi_incoming) {
                const uint64_t key = (static_cast<uint64_t>(inst.id) << 32) | inc.first;
                auto ov = phi_edge_override.find(key);
                const uint32_t v_id = (ov != phi_edge_override.end()) ? ov->second : val_spv_id[inc.second];
                ops.push_back(v_id);
                ops.push_back(bb_labels[inc.first]);
            }
            EmitSpvInst(sec_fn, SpvOpPhi, ops);
        }

        // Per-block cache for consecutive BvhIntersectRay component extracts
        std::vector<IrValueId> last_bvh_ops;
        uint32_t last_bvh_vec4_id = 0u;

        // Non-Phi, non-terminator instructions
        for (const auto& inst : bb.instructions) {
            if (inst.op == IrOp::Phi || inst.op == IrOp::ConstU32 || inst.op == IrOp::ConstF32 ||
                inst.op == IrOp::Branch || inst.op == IrOp::CondBranch || inst.op == IrOp::Return) {
                continue;
            }
            const uint32_t res = (inst.id != kInvalidIrValue) ? val_spv_id[inst.id] : 0u;

            switch (inst.op) {
                case IrOp::ThreadIdX:
                    if (ir.stage == ShaderStage::Compute) {
                        uint32_t ptr_x = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_in_u32, ptr_x, var_gid, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr_x});
                    } else if (ir.stage == ShaderStage::Vertex) {
                        uint32_t vid_i32 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_i32, vid_i32, var_vid});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, vid_i32});
                    }
                    break;
                case IrOp::StageInput: {
                    uint32_t v4 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_v4f32, v4, var_param0_in});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, v4, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::LoadRootConst: {
                    uint32_t idx = get_const_i32(static_cast<int32_t>(inst.imm_u32));
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_unif_u32, ptr, var_rc, idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::LoadUav: {
                    const uint32_t var_buf = (inst.imm_u32 == 1u && var_uav1 != 0u) ? var_uav1 : var_uav0;
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t dword_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr, var_buf, c_i32_0, dword_idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::StoreUav: {
                    const uint32_t var_buf = (inst.imm_u32 == 1u && var_uav1 != 0u) ? var_uav1 : var_uav0;
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t data_u32 = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr, var_buf, c_i32_0, dword_idx});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                    break;
                }
                case IrOp::ImageSample: {
                    uint32_t img = alloc_id();
                    uint32_t samp = alloc_id();
                    uint32_t si = alloc_id();
                    uint32_t uv = alloc_id();
                    uint32_t rgba = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_img2d, img, var_tex0});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_sampler, samp, var_samp0});
                    EmitSpvInst(sec_fn, SpvOpSampledImage, {t_sampled_img2d, si, img, samp});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v2f32, uv, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpImageSampleExplicitLod,
                                {t_v4f32, rgba, si, uv, 2u, get_as_f32(inst.operands[2])});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, rgba, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::ImageLoad: {
                    uint32_t img = alloc_id();
                    uint32_t coord = alloc_id();
                    uint32_t rgba = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_rwimg2d, img, var_img_ro});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpImageRead, {t_v4f32, rgba, img, coord});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, rgba, inst.imm_u32 & 3u});
                    break;
                }
                case IrOp::ImageLoadMip: {
                    const uint32_t ch = inst.imm_u32 & 3u;
                    const uint32_t dim = (inst.imm_u32 >> 4) & 7u;
                    uint32_t img = alloc_id();
                    uint32_t coord = alloc_id();
                    uint32_t rgba = alloc_id();
                    const uint32_t mip_i32 = get_as_i32(inst.operands[3]);
                    if (dim == 2u) {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_img3d, img, var_tex3d});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v3i32, coord, get_as_i32(inst.operands[0]),
                                     get_as_i32(inst.operands[1]), get_as_i32(inst.operands[2])});
                    } else if (dim == 3u) {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_imgcube, img, var_texcube});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v3i32, coord, get_as_i32(inst.operands[0]),
                                     get_as_i32(inst.operands[1]), get_as_i32(inst.operands[2])});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_img2d, img, var_tex0});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    }
                    // ImageOperands::Lod = 0x2
                    EmitSpvInst(sec_fn, SpvOpImageFetch, {t_v4f32, rgba, img, coord, 2u, mip_i32});
                    EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, res, rgba, ch});
                    break;
                }
                case IrOp::ImageStore: {
                    uint32_t img = alloc_id();
                    uint32_t coord = alloc_id();
                    uint32_t rgba = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_rwimg2d, img, var_rwtex0});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                {t_v4f32, rgba, get_as_f32(inst.operands[2]), get_as_f32(inst.operands[3]),
                                 get_as_f32(inst.operands[4]), get_as_f32(inst.operands[5])});
                    EmitSpvInst(sec_fn, SpvOpImageWrite, {img, coord, rgba});
                    break;
                }
                case IrOp::ImageAtomicCmpSwap: {
                    const uint32_t dim = inst.imm_u32 & 7u;
                    uint32_t coord = alloc_id();
                    if (dim == 2u || dim == 3u) {
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v3i32, coord, get_as_i32(inst.operands[0]),
                                     get_as_i32(inst.operands[1]), get_as_i32(inst.operands[2])});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v2i32, coord, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    }
                    uint32_t texel_ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpImageTexelPointer,
                                {t_ptr_img_u32, texel_ptr, var_rwimg_u32, coord, c_i32_0});
                    const uint32_t c_scope = get_const_u32(kSpvScopeDevice);
                    const uint32_t c_sem = get_const_u32(kSpvMemSemNone);
                    const uint32_t cmp_val = get_as_u32(inst.operands[3]);
                    const uint32_t src_val = get_as_u32(inst.operands[4]);
                    // OpAtomicCompareExchange: Value (new=src) first, Comparator (cmp) second
                    EmitSpvInst(sec_fn, SpvOpAtomicCompareExchange,
                                {t_u32, res, texel_ptr, c_scope, c_sem, c_sem, src_val, cmp_val});
                    break;
                }
                case IrOp::BvhIntersectRay: {
                    if (last_bvh_vec4_id == 0u || last_bvh_ops != inst.operands) {
                        // Branchless clean-room Möller-Trumbore triangle + slab AABB intersection
                        const uint32_t node_ptr = get_as_u32(inst.operands[0]);
                        const uint32_t tmax = get_as_f32(inst.operands[1]);
                        const uint32_t ox = get_as_f32(inst.operands[2]);
                        const uint32_t oy = get_as_f32(inst.operands[3]);
                        const uint32_t oz = get_as_f32(inst.operands[4]);
                        const uint32_t dx = get_as_f32(inst.operands[5]);
                        const uint32_t dy = get_as_f32(inst.operands[6]);
                        const uint32_t dz = get_as_f32(inst.operands[7]);

                        uint32_t base_dw = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, base_dw, node_ptr, c_u32_2});

                        auto load_uav_u32 = [&](uint32_t dw_off) -> uint32_t {
                            uint32_t idx = base_dw;
                            if (dw_off != 0u) {
                                idx = alloc_id();
                                EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, idx, base_dw, get_const_u32(dw_off)});
                            }
                            uint32_t ptr = alloc_id();
                            uint32_t val = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_sb_u32, ptr, var_uav0, c_i32_0, idx});
                            EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, val, ptr});
                            return val;
                        };
                        auto load_uav_f32 = [&](uint32_t dw_off) -> uint32_t {
                            uint32_t u = load_uav_u32(dw_off);
                            uint32_t f = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, f, u});
                            return f;
                        };
                        auto fsub = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFSub, {t_f32, r, a, b});
                            return r;
                        };
                        auto fadd = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, r, a, b});
                            return r;
                        };
                        auto fmul = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, r, a, b});
                            return r;
                        };
                        auto fdiv = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFDiv, {t_f32, r, a, b});
                            return r;
                        };
                        auto fmin = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, r, glsl_ext_id, GlslFMin, a, b});
                            return r;
                        };
                        auto fmax = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, r, glsl_ext_id, GlslFMax, a, b});
                            return r;
                        };
                        auto fabs_f = [&](uint32_t a) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, r, glsl_ext_id, GlslFAbs, a});
                            return r;
                        };
                        auto dot3 = [&](uint32_t ax, uint32_t ay, uint32_t az,
                                        uint32_t bx, uint32_t by, uint32_t bz) -> uint32_t {
                            return fadd(fadd(fmul(ax, bx), fmul(ay, by)), fmul(az, bz));
                        };
                        auto cross3 = [&](uint32_t ax, uint32_t ay, uint32_t az,
                                          uint32_t bx, uint32_t by, uint32_t bz,
                                          uint32_t& rx, uint32_t& ry, uint32_t& rz) {
                            rx = fsub(fmul(ay, bz), fmul(az, by));
                            ry = fsub(fmul(az, bx), fmul(ax, bz));
                            rz = fsub(fmul(ax, by), fmul(ay, bx));
                        };
                        auto land = [&](uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpLogicalAnd, {t_bool, r, a, b});
                            return r;
                        };
                        auto fcmp = [&](uint16_t op, uint32_t a, uint32_t b) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, op, {t_bool, r, a, b});
                            return r;
                        };
                        auto sel_f = [&](uint32_t cond, uint32_t tv, uint32_t fv) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, r, cond, tv, fv});
                            return r;
                        };
                        auto sel_u = [&](uint32_t cond, uint32_t tv, uint32_t fv) -> uint32_t {
                            uint32_t r = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, r, cond, tv, fv});
                            return r;
                        };

                        const uint32_t hdr = load_uav_u32(0u);
                        const uint32_t c_highbit = get_const_u32(0x80000000u);
                        const uint32_t c_idmask = get_const_u32(0x7FFFFFFFu);
                        uint32_t hdr_flag = alloc_id();
                        uint32_t is_box = alloc_id();
                        uint32_t hit_id = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, hdr_flag, hdr, c_highbit});
                        EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, is_box, hdr_flag, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, hit_id, hdr, c_idmask});

                        const uint32_t v0x = load_uav_f32(1u);
                        const uint32_t v0y = load_uav_f32(2u);
                        const uint32_t v0z = load_uav_f32(3u);
                        const uint32_t v1x = load_uav_f32(4u);
                        const uint32_t v1y = load_uav_f32(5u);
                        const uint32_t v1z = load_uav_f32(6u);
                        const uint32_t v2x = load_uav_f32(7u);
                        const uint32_t v2y = load_uav_f32(8u);
                        const uint32_t v2z = load_uav_f32(9u);

                        // Triangle Möller-Trumbore
                        const uint32_t e1x = fsub(v1x, v0x), e1y = fsub(v1y, v0y), e1z = fsub(v1z, v0z);
                        const uint32_t e2x = fsub(v2x, v0x), e2y = fsub(v2y, v0y), e2z = fsub(v2z, v0z);
                        uint32_t px = 0, py = 0, pz = 0;
                        cross3(dx, dy, dz, e2x, e2y, e2z, px, py, pz);
                        const uint32_t det = dot3(e1x, e1y, e1z, px, py, pz);
                        const uint32_t det_ok = fcmp(SpvOpFOrdGreaterThan, fabs_f(det), get_const_f32(1e-7f));
                        const uint32_t inv_det = fdiv(c_f32_1, sel_f(det_ok, det, c_f32_1));
                        const uint32_t tvx = fsub(ox, v0x), tvy = fsub(oy, v0y), tvz = fsub(oz, v0z);
                        const uint32_t u = fmul(dot3(tvx, tvy, tvz, px, py, pz), inv_det);
                        uint32_t qx = 0, qy = 0, qz = 0;
                        cross3(tvx, tvy, tvz, e1x, e1y, e1z, qx, qy, qz);
                        const uint32_t v = fmul(dot3(dx, dy, dz, qx, qy, qz), inv_det);
                        const uint32_t t = fmul(dot3(e2x, e2y, e2z, qx, qy, qz), inv_det);
                        uint32_t tri_hit = det_ok;
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdGreaterThanEqual, u, c_f32_0));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdGreaterThanEqual, v, c_f32_0));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdLessThanEqual, fadd(u, v), c_f32_1));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdGreaterThanEqual, t, c_f32_0));
                        tri_hit = land(tri_hit, fcmp(SpvOpFOrdLessThanEqual, t, tmax));
                        const uint32_t tri_t = sel_f(tri_hit, t, tmax);
                        const uint32_t tri_u = sel_f(tri_hit, u, c_f32_0);
                        const uint32_t tri_v = sel_f(tri_hit, v, c_f32_0);
                        const uint32_t tri_id = sel_u(tri_hit, hit_id, c_u32_allones);

                        // AABB Slab test (bmin = v0, bmax = v1)
                        const uint32_t c_eps = get_const_f32(1e-12f);
                        const uint32_t c_big = get_const_f32(1e30f);
                        const uint32_t c_nbig = get_const_f32(-1e30f);
                        auto slab_axis = [&](uint32_t ok, uint32_t dk, uint32_t bmin_k, uint32_t bmax_k,
                                             uint32_t& out_tmin, uint32_t& out_tmax) {
                            const uint32_t nz = fcmp(SpvOpFOrdGreaterThan, fabs_f(dk), c_eps);
                            const uint32_t inv_d = fdiv(c_f32_1, sel_f(nz, dk, c_f32_1));
                            const uint32_t t0 = fmul(fsub(bmin_k, ok), inv_d);
                            const uint32_t t1 = fmul(fsub(bmax_k, ok), inv_d);
                            const uint32_t smin = fmin(t0, t1);
                            const uint32_t smax = fmax(t0, t1);
                            const uint32_t inside = land(
                                fcmp(SpvOpFOrdGreaterThanEqual, ok, bmin_k),
                                fcmp(SpvOpFOrdLessThanEqual, ok, bmax_k));
                            out_tmin = sel_f(nz, smin, sel_f(inside, c_nbig, c_big));
                            out_tmax = sel_f(nz, smax, sel_f(inside, c_big, c_nbig));
                        };
                        uint32_t tx0 = 0, tx1 = 0, ty0 = 0, ty1 = 0, tz0 = 0, tz1 = 0;
                        slab_axis(ox, dx, v0x, v1x, tx0, tx1);
                        slab_axis(oy, dy, v0y, v1y, ty0, ty1);
                        slab_axis(oz, dz, v0z, v1z, tz0, tz1);
                        const uint32_t t_enter = fmax(fmax(tx0, ty0), fmax(tz0, c_f32_0));
                        const uint32_t t_exit = fmin(fmin(tx1, ty1), fmin(tz1, tmax));
                        const uint32_t box_hit = fcmp(SpvOpFOrdLessThanEqual, t_enter, t_exit);
                        const uint32_t box_t = sel_f(box_hit, t_enter, tmax);
                        const uint32_t box_u = sel_f(box_hit, t_exit, c_f32_0);
                        const uint32_t box_v = c_f32_0;
                        const uint32_t box_id = sel_u(box_hit, hit_id, c_u32_allones);

                        const uint32_t out_t = sel_f(is_box, box_t, tri_t);
                        const uint32_t out_u = sel_f(is_box, box_u, tri_u);
                        const uint32_t out_v = sel_f(is_box, box_v, tri_v);
                        const uint32_t out_id = sel_u(is_box, box_id, tri_id);

                        uint32_t out_t_u = alloc_id();
                        uint32_t out_u_u = alloc_id();
                        uint32_t out_v_u = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, out_t_u, out_t});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, out_u_u, out_u});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, out_v_u, out_v});

                        last_bvh_vec4_id = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct,
                                    {t_v4u32, last_bvh_vec4_id, out_t_u, out_u_u, out_v_u, out_id});
                        last_bvh_ops = inst.operands;
                    }

                    const uint32_t comp = inst.imm_u32 & 3u;
                    if (comp < 3u) {
                        uint32_t ext_u = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_u32, ext_u, last_bvh_vec4_id, comp});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, res, ext_u});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_u32, res, last_bvh_vec4_id, 3u});
                    }
                    break;
                }
                case IrOp::LdsLoad: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_lds_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr, var_lds, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::LdsStore: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t data_u32 = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_lds_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr, var_lds, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                    break;
                }
                case IrOp::LdsAtomicAdd: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t addend = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    const uint32_t c_scope = get_const_u32(kSpvScopeWorkgroup);
                    const uint32_t c_sem = get_const_u32(kSpvMemSemNone);
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_lds_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr, var_lds, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpAtomicIAdd, {t_u32, res, ptr, c_scope, c_sem, addend});
                    break;
                }
                case IrOp::ScratchLoad: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_scratch_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_priv_u32, ptr, var_scratch, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr});
                    break;
                }
                case IrOp::ScratchStore: {
                    uint32_t byte_off = get_as_u32(inst.operands[0]);
                    uint32_t data_u32 = get_as_u32(inst.operands[1]);
                    uint32_t dword_idx = alloc_id();
                    uint32_t masked_idx = alloc_id();
                    uint32_t ptr = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, dword_idx, byte_off, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked_idx, dword_idx, c_scratch_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_priv_u32, ptr, var_scratch, masked_idx});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr, data_u32});
                    break;
                }
                case IrOp::WaveLaneId: {
                    uint32_t raw_idx = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, raw_idx, var_local_idx});
                    const uint32_t mask = (ir.wave_size == 64u) ? c_u32_63 : c_u32_31;
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, res, raw_idx, mask});
                    break;
                }
                case IrOp::WaveBallotLo:
                case IrOp::WaveBallotHi: {
                    // Race-free, branchless workgroup-emulated 64-lane ballot (works on any HW wave size!)
                    const uint32_t c_scope = get_const_u32(kSpvScopeWorkgroup);
                    const uint32_t c_sem_none = get_const_u32(kSpvMemSemNone);
                    uint32_t ptr_b0 = alloc_id();
                    uint32_t ptr_b1 = alloc_id();
                    uint32_t clr0 = alloc_id();
                    uint32_t clr1 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_b0, var_w64_ballot, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_b1, var_w64_ballot, c_u32_1});

                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpAtomicAnd, {t_u32, clr0, ptr_b0, c_scope, c_sem_none, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpAtomicAnd, {t_u32, clr1, ptr_b1, c_scope, c_sem_none, c_u32_0});
                    emit_wg_barrier();

                    uint32_t lane = emit_lane_id();
                    uint32_t word_idx = alloc_id();
                    uint32_t bit_idx = alloc_id();
                    uint32_t bit_mask = alloc_id();
                    uint32_t contrib = alloc_id();
                    uint32_t ptr_bw = alloc_id();
                    uint32_t or_old = alloc_id();
                    uint32_t pred = get_as_bool(inst.operands[0]);

                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, word_idx, lane, c_u32_5});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, bit_idx, lane, c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, bit_mask, c_u32_1, bit_idx});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, contrib, pred, bit_mask, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_bw, var_w64_ballot, word_idx});
                    EmitSpvInst(sec_fn, SpvOpAtomicOr, {t_u32, or_old, ptr_bw, c_scope, c_sem_none, contrib});

                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpLoad,
                                {t_u32, res, (inst.op == IrOp::WaveBallotHi) ? ptr_b1 : ptr_b0});
                    break;
                }
                case IrOp::WaveActiveCountBits64: {
                    uint32_t c0 = alloc_id();
                    uint32_t c1 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitCount, {t_u32, c0, get_as_u32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpBitCount, {t_u32, c1, get_as_u32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, c0, c1});
                    break;
                }
                case IrOp::WavePrefixCount64: {
                    uint32_t lane = emit_lane_id();
                    uint32_t mask_in = get_as_u32(inst.operands[0]);
                    uint32_t init_in = get_as_u32(inst.operands[1]);
                    uint32_t bitmask = alloc_id();
                    if (inst.imm_u32 == 0u) {
                        uint32_t lt32 = alloc_id();
                        uint32_t sh = alloc_id();
                        uint32_t p2 = alloc_id();
                        uint32_t lo_m = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, lt32, lane, c_u32_32});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sh, lt32, lane, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, p2, c_u32_1, sh});
                        EmitSpvInst(sec_fn, SpvOpISub, {t_u32, lo_m, p2, c_u32_1});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, bitmask, lt32, lo_m, c_u32_allones});
                    } else {
                        uint32_t ge32 = alloc_id();
                        uint32_t sub32 = alloc_id();
                        uint32_t sh = alloc_id();
                        uint32_t p2 = alloc_id();
                        uint32_t hi_m = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, ge32, lane, c_u32_32});
                        EmitSpvInst(sec_fn, SpvOpISub, {t_u32, sub32, lane, c_u32_32});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, sh, ge32, sub32, c_u32_0});
                        EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, p2, c_u32_1, sh});
                        EmitSpvInst(sec_fn, SpvOpISub, {t_u32, hi_m, p2, c_u32_1});
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, bitmask, ge32, hi_m, c_u32_0});
                    }
                    uint32_t masked = alloc_id();
                    uint32_t cnt = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, mask_in, bitmask});
                    EmitSpvInst(sec_fn, SpvOpBitCount, {t_u32, cnt, masked});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, cnt, init_in});
                    break;
                }
                case IrOp::WaveReadFirstLane:
                case IrOp::WaveReadLaneAt: {
                    uint32_t val_u32 = get_as_u32(inst.operands[0]);
                    uint32_t lane = emit_lane_id();
                    uint32_t ptr_me = alloc_id();
                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_me, var_w64_lanes, lane});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr_me, val_u32});
                    emit_wg_barrier();

                    uint32_t src_idx = c_u32_0;
                    if (inst.op == IrOp::WaveReadLaneAt) {
                        src_idx = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, src_idx, get_as_u32(inst.operands[1]), c_w_lanes_mask});
                    }
                    uint32_t ptr_src = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_src, var_w64_lanes, src_idx});
                    if (CanonicalSpvKind(inst.type) == SpvValKind::F32) {
                        uint32_t loaded = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, loaded, ptr_src});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, res, loaded});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr_src});
                    }
                    break;
                }
                case IrOp::WavePermLane16: {
                    uint32_t val_u32 = get_as_u32(inst.operands[0]);
                    uint32_t s1_u32 = get_as_u32(inst.operands[1]);
                    uint32_t s2_u32 = get_as_u32(inst.operands[2]);
                    uint32_t raw_idx = alloc_id();
                    uint32_t lane = alloc_id();
                    uint32_t ptr_me = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, raw_idx, var_local_idx});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lane, raw_idx, c_w_lanes_mask});
                    emit_wg_barrier();
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_me, var_w64_lanes, lane});
                    EmitSpvInst(sec_fn, SpvOpStore, {ptr_me, val_u32});
                    emit_wg_barrier();

                    uint32_t row = alloc_id();
                    uint32_t row_base = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, row, lane, c_u32_15});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, row_base, lane, c_u32_not15});
                    if (inst.imm_u32 != 0u) {
                        uint32_t xbase = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseXor, {t_u32, xbase, row_base, c_u32_16});
                        row_base = xbase;
                    }
                    uint32_t is_hi = alloc_id();
                    uint32_t word = alloc_id();
                    uint32_t nib = alloc_id();
                    uint32_t shift = alloc_id();
                    uint32_t shifted = alloc_id();
                    uint32_t sel = alloc_id();
                    uint32_t tgt_raw = alloc_id();
                    uint32_t tgt = alloc_id();
                    uint32_t ptr_tgt = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, is_hi, row, c_u32_8});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, word, is_hi, s2_u32, s1_u32});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, nib, row, c_u32_7});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, shift, nib, c_u32_2});
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, shifted, word, shift});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, sel, shifted, c_u32_15});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, tgt_raw, row_base, sel});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, tgt, tgt_raw, c_w_lanes_mask});
                    EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_wg_u32, ptr_tgt, var_w64_lanes, tgt});
                    if (CanonicalSpvKind(inst.type) == SpvValKind::F32) {
                        uint32_t loaded = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, loaded, ptr_tgt});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, res, loaded});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpLoad, {t_u32, res, ptr_tgt});
                    }
                    break;
                }
                case IrOp::FAdd:
                    EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FSub:
                    EmitSpvInst(sec_fn, SpvOpFSub, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FMul:
                    EmitSpvInst(sec_fn, SpvOpFMul, {t_f32, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FFma:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFma,
                                                       get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1]),
                                                       get_as_f32(inst.operands[2])});
                    break;
                case IrOp::FMin:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFMin,
                                                       get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FMax:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFMax,
                                                       get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    break;
                case IrOp::FRcp:
                    EmitSpvInst(sec_fn, SpvOpFDiv, {t_f32, res, c_f32_1, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FRsq:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslInverseSqrt, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FSqrt:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslSqrt, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FFloor:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFloor, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FFract:
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_f32, res, glsl_ext_id, GlslFract, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::FDot2: {
                    uint32_t va = alloc_id();
                    uint32_t vb = alloc_id();
                    uint32_t dp = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, va, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vb, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(inst.operands[1])});
                    EmitSpvInst(sec_fn, SpvOpDot, {t_f32, dp, va, vb});
                    EmitSpvInst(sec_fn, SpvOpFAdd, {t_f32, res, dp, get_as_f32(inst.operands[2])});
                    break;
                }
                case IrOp::IAdd:
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::ISub:
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::IMul:
                    EmitSpvInst(sec_fn, SpvOpIMul, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::IMin:
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_i32, tmp, glsl_ext_id, GlslSMin,
                                                           get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslUMin,
                                                           get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    }
                    break;
                case IrOp::IMax:
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_i32, tmp, glsl_ext_id, GlslSMax,
                                                           get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslUMax,
                                                           get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    }
                    break;
                case IrOp::BitAnd:
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitOr:
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitXor:
                    EmitSpvInst(sec_fn, SpvOpBitwiseXor, {t_u32, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::BitNot:
                    EmitSpvInst(sec_fn, SpvOpNot, {t_u32, res, get_as_u32(inst.operands[0])});
                    break;
                case IrOp::Shl: {
                    uint32_t masked = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, get_as_u32(inst.operands[1]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, res, get_as_u32(inst.operands[0]), masked});
                    break;
                }
                case IrOp::Shr: {
                    uint32_t masked = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, masked, get_as_u32(inst.operands[1]), c_u32_31});
                    if (inst.type == IrType::I32) {
                        uint32_t tmp = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, tmp, get_as_i32(inst.operands[0]), masked});
                        EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, res, get_as_u32(inst.operands[0]), masked});
                    }
                    break;
                }
                case IrOp::BfeU32: {
                    uint32_t s0 = get_as_u32(inst.operands[0]);
                    uint32_t off = alloc_id();
                    uint32_t wid = alloc_id();
                    uint32_t shifted = alloc_id();
                    uint32_t p2 = alloc_id();
                    uint32_t mask = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, off, get_as_u32(inst.operands[1]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, wid, get_as_u32(inst.operands[2]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, shifted, s0, off});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, p2, c_u32_1, wid});
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, mask, p2, c_u32_1});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, res, shifted, mask});
                    break;
                }
                case IrOp::BfeI32: {
                    uint32_t s0 = get_as_u32(inst.operands[0]);
                    uint32_t s0_i = get_as_i32(inst.operands[0]);
                    uint32_t off = alloc_id();
                    uint32_t wid = alloc_id();
                    uint32_t sum = alloc_id();
                    uint32_t is_zero = alloc_id();
                    uint32_t lt32 = alloc_id();
                    uint32_t shl_raw = alloc_id();
                    uint32_t shl_safe = alloc_id();
                    uint32_t shr_raw = alloc_id();
                    uint32_t shr_safe = alloc_id();
                    uint32_t tmp_l = alloc_id();
                    uint32_t tmp_li = alloc_id();
                    uint32_t r_lt = alloc_id();
                    uint32_t r_ge = alloc_id();
                    uint32_t r_nz = alloc_id();
                    uint32_t r_fin = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, off, get_as_u32(inst.operands[1]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, wid, get_as_u32(inst.operands[2]), c_u32_31});
                    EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, sum, off, wid});
                    EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, is_zero, wid, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, lt32, sum, c_u32_32});
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, shl_raw, c_u32_32, sum});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, shl_safe, lt32, shl_raw, c_u32_0});
                    EmitSpvInst(sec_fn, SpvOpISub, {t_u32, shr_raw, c_u32_32, wid});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, shr_safe, is_zero, c_u32_0, shr_raw});
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, tmp_l, s0, shl_safe});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_i32, tmp_li, tmp_l});
                    EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, r_lt, tmp_li, shr_safe});
                    EmitSpvInst(sec_fn, SpvOpShiftRightArithmetic, {t_i32, r_ge, s0_i, off});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_i32, r_nz, lt32, r_lt, r_ge});
                    EmitSpvInst(sec_fn, SpvOpSelect, {t_i32, r_fin, is_zero, c_i32_0, r_nz});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, r_fin});
                    break;
                }
                case IrOp::BfiB32: {
                    uint32_t m = get_as_u32(inst.operands[0]);
                    uint32_t ins = get_as_u32(inst.operands[1]);
                    uint32_t base = get_as_u32(inst.operands[2]);
                    uint32_t not_m = alloc_id();
                    uint32_t p1 = alloc_id();
                    uint32_t p2 = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpNot, {t_u32, not_m, m});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, p1, m, ins});
                    EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, p2, not_m, base});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, p1, p2});
                    break;
                }
                case IrOp::PkAddF16:
                case IrOp::PkMulF16:
                case IrOp::PkFmaF16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const uint8_t neg_lo = static_cast<uint8_t>((f >> 8) & 7u);
                    const uint8_t neg_hi = static_cast<uint8_t>((f >> 12) & 7u);
                    const bool clamp = ((f >> 16) & 1u) != 0u;
                    auto unpack_sel_v2 = [&](IrValueId v, int idx) -> uint32_t {
                        uint32_t raw_v2 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpExtInst,
                                    {t_v2f32, raw_v2, glsl_ext_id, GlslUnpackHalf2x16, get_as_u32(v)});
                        const bool lo_hi = ((op_sel >> idx) & 1u) != 0u;
                        const bool hi_hi = ((op_sel_hi >> idx) & 1u) != 0u;
                        const bool n_lo = ((neg_lo >> idx) & 1u) != 0u;
                        const bool n_hi = ((neg_hi >> idx) & 1u) != 0u;
                        if (!lo_hi && hi_hi && !n_lo && !n_hi) {
                            return raw_v2;
                        }
                        uint32_t c0 = alloc_id();
                        uint32_t c1 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, c0, raw_v2, lo_hi ? 1u : 0u});
                        EmitSpvInst(sec_fn, SpvOpCompositeExtract, {t_f32, c1, raw_v2, hi_hi ? 1u : 0u});
                        if (n_lo) {
                            uint32_t neg0 = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFNegate, {t_f32, neg0, c0});
                            c0 = neg0;
                        }
                        if (n_hi) {
                            uint32_t neg1 = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpFNegate, {t_f32, neg1, c1});
                            c1 = neg1;
                        }
                        uint32_t v2 = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, v2, c0, c1});
                        return v2;
                    };
                    uint32_t va = unpack_sel_v2(inst.operands[0], 0);
                    uint32_t vb = unpack_sel_v2(inst.operands[1], 1);
                    uint32_t vr = alloc_id();
                    if (inst.op == IrOp::PkFmaF16) {
                        uint32_t vc = unpack_sel_v2(inst.operands[2], 2);
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vr, glsl_ext_id, GlslFma, va, vb, vc});
                    } else if (inst.op == IrOp::PkAddF16) {
                        EmitSpvInst(sec_fn, SpvOpFAdd, {t_v2f32, vr, va, vb});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpFMul, {t_v2f32, vr, va, vb});
                    }
                    if (clamp) {
                        uint32_t v0 = alloc_id();
                        uint32_t v1 = alloc_id();
                        uint32_t vlo = alloc_id();
                        uint32_t vcl = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, v0, c_f32_0, c_f32_0});
                        EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {t_v2f32, v1, c_f32_1, c_f32_1});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vlo, glsl_ext_id, GlslFMax, vr, v0});
                        EmitSpvInst(sec_fn, SpvOpExtInst, {t_v2f32, vcl, glsl_ext_id, GlslFMin, vlo, v1});
                        vr = vcl;
                    }
                    EmitSpvInst(sec_fn, SpvOpExtInst, {t_u32, res, glsl_ext_id, GlslPackHalf2x16, vr});
                    break;
                }
                case IrOp::PkAddU16:
                case IrOp::PkShl16:
                case IrOp::PkShr16: {
                    const uint32_t f = inst.imm_u32;
                    const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                    const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                    const bool is_shift = (inst.op == IrOp::PkShl16 || inst.op == IrOp::PkShr16);
                    auto extract_u16 = [&](uint32_t uval, int isa_idx, bool hi_out) -> uint32_t {
                        const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> isa_idx) & 1u) != 0u;
                        if (pick_hi) {
                            uint32_t sh = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpShiftRightLogical, {t_u32, sh, uval, c_u32_16});
                            return sh;
                        }
                        uint32_t lo = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, lo, uval, c_u32_65535});
                        return lo;
                    };
                    const uint32_t ua = get_as_u32(inst.operands[0]);
                    const uint32_t ub = get_as_u32(inst.operands[1]);
                    const uint32_t a0 = extract_u16(ua, is_shift ? 1 : 0, false);
                    const uint32_t a1 = extract_u16(ua, is_shift ? 1 : 0, true);
                    const uint32_t b0 = extract_u16(ub, is_shift ? 0 : 1, false);
                    const uint32_t b1 = extract_u16(ub, is_shift ? 0 : 1, true);
                    auto eval_u16 = [&](uint32_t x, uint32_t y) -> uint32_t {
                        uint32_t raw = alloc_id();
                        if (inst.op == IrOp::PkAddU16) {
                            EmitSpvInst(sec_fn, SpvOpIAdd, {t_u32, raw, x, y});
                        } else {
                            uint32_t sh = alloc_id();
                            EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, sh, y, c_u32_15});
                            EmitSpvInst(sec_fn,
                                        (inst.op == IrOp::PkShl16) ? SpvOpShiftLeftLogical : SpvOpShiftRightLogical,
                                        {t_u32, raw, x, sh});
                        }
                        uint32_t m = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, m, raw, c_u32_65535});
                        return m;
                    };
                    const uint32_t r0 = eval_u16(a0, b0);
                    const uint32_t r1 = eval_u16(a1, b1);
                    uint32_t r1_hi = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpShiftLeftLogical, {t_u32, r1_hi, r1, c_u32_16});
                    EmitSpvInst(sec_fn, SpvOpBitwiseOr, {t_u32, res, r0, r1_hi});
                    break;
                }
                case IrOp::I2F:
                    EmitSpvInst(sec_fn, SpvOpConvertSToF, {t_f32, res, get_as_i32(inst.operands[0])});
                    break;
                case IrOp::U2F:
                    EmitSpvInst(sec_fn, SpvOpConvertUToF, {t_f32, res, get_as_u32(inst.operands[0])});
                    break;
                case IrOp::F2I: {
                    uint32_t tmp = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpConvertFToS, {t_i32, tmp, get_as_f32(inst.operands[0])});
                    EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, res, tmp});
                    break;
                }
                case IrOp::F2U:
                    EmitSpvInst(sec_fn, SpvOpConvertFToU, {t_u32, res, get_as_f32(inst.operands[0])});
                    break;
                case IrOp::CmpEq:
                case IrOp::CmpNe:
                case IrOp::CmpLt:
                case IrOp::CmpGt:
                case IrOp::CmpGe: {
                    const bool is_flt = (val_kind[inst.operands[0]] == SpvValKind::F32 ||
                                         val_kind[inst.operands[1]] == SpvValKind::F32);
                    if (is_flt) {
                        uint16_t spv_op = SpvOpFOrdEqual;
                        if (inst.op == IrOp::CmpNe) spv_op = SpvOpFOrdNotEqual;
                        else if (inst.op == IrOp::CmpLt) spv_op = SpvOpFOrdLessThan;
                        else if (inst.op == IrOp::CmpGt) spv_op = SpvOpFOrdGreaterThan;
                        else if (inst.op == IrOp::CmpGe) spv_op = SpvOpFOrdGreaterThanEqual;
                        EmitSpvInst(sec_fn, spv_op, {t_bool, res, get_as_f32(inst.operands[0]), get_as_f32(inst.operands[1])});
                    } else {
                        uint16_t spv_op = SpvOpIEqual;
                        if (inst.op == IrOp::CmpNe) spv_op = SpvOpINotEqual;
                        else if (inst.op == IrOp::CmpLt) spv_op = SpvOpSLessThan;
                        else if (inst.op == IrOp::CmpGt) spv_op = SpvOpSGreaterThan;
                        else if (inst.op == IrOp::CmpGe) spv_op = SpvOpSGreaterThanEqual;
                        EmitSpvInst(sec_fn, spv_op, {t_bool, res, get_as_i32(inst.operands[0]), get_as_i32(inst.operands[1])});
                    }
                    break;
                }
                case IrOp::CmpLtU:
                    EmitSpvInst(sec_fn, SpvOpULessThan, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpEqU:
                    EmitSpvInst(sec_fn, SpvOpIEqual, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpGtU:
                    EmitSpvInst(sec_fn, SpvOpUGreaterThan, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::CmpGeU:
                    EmitSpvInst(sec_fn, SpvOpUGreaterThanEqual, {t_bool, res, get_as_u32(inst.operands[0]), get_as_u32(inst.operands[1])});
                    break;
                case IrOp::Select: {
                    const SpvValKind k = CanonicalSpvKind(inst.type);
                    const uint32_t c = get_as_bool(inst.operands[0]);
                    if (k == SpvValKind::F32) {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_f32, res, c, get_as_f32(inst.operands[1]), get_as_f32(inst.operands[2])});
                    } else if (k == SpvValKind::Bool) {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_bool, res, c, get_as_bool(inst.operands[1]), get_as_bool(inst.operands[2])});
                    } else {
                        EmitSpvInst(sec_fn, SpvOpSelect, {t_u32, res, c, get_as_u32(inst.operands[1]), get_as_u32(inst.operands[2])});
                    }
                    break;
                }
                case IrOp::Barrier:
                    if (ir.stage == ShaderStage::Compute) {
                        emit_wg_barrier();
                    }
                    break;
                case IrOp::Export: {
                    uint32_t vec_id = alloc_id();
                    EmitSpvInst(sec_fn, SpvOpCompositeConstruct, {
                        t_v4f32, vec_id,
                        get_as_f32(inst.operands[0]),
                        get_as_f32(inst.operands[1]),
                        get_as_f32(inst.operands[2]),
                        get_as_f32(inst.operands[3]),
                    });
                    if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && var_pervertex != 0u) {
                        uint32_t ptr_pos = alloc_id();
                        EmitSpvInst(sec_fn, SpvOpAccessChain, {t_ptr_out_v4f32, ptr_pos, var_pervertex, c_i32_0});
                        EmitSpvInst(sec_fn, SpvOpStore, {ptr_pos, vec_id});
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0) && var_param0_out != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_param0_out, vec_id});
                    } else if (inst.export_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0) && var_mrt0 != 0u) {
                        EmitSpvInst(sec_fn, SpvOpStore, {var_mrt0, vec_id});
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Phi edge coercions right before terminator
        for (const auto& fx : block_exit_fixups[bi]) {
            if (fx.expected_kind == SpvValKind::F32) {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpBitcast, {t_f32, fx.coerced_spv_id, u});
            } else if (fx.expected_kind == SpvValKind::Bool) {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpINotEqual, {t_bool, fx.coerced_spv_id, u, c_u32_0});
            } else {
                uint32_t u = get_as_u32(fx.incoming_val);
                EmitSpvInst(sec_fn, SpvOpBitcast, {t_u32, fx.coerced_spv_id, u});
            }
        }

        // Terminator (with mandatory OpSelectionMerge for Vulkan structured CFG)
        if (!bb.instructions.empty()) {
            const auto& term = bb.instructions.back();
            if (term.op == IrOp::Branch) {
                EmitSpvInst(sec_fn, SpvOpBranch, {bb_labels[term.true_block]});
            } else if (term.op == IrOp::CondBranch) {
                if (term.true_block == term.false_block) {
                    EmitSpvInst(sec_fn, SpvOpBranch, {bb_labels[term.true_block]});
                } else {
                    const uint32_t merge_bb = ipdom[bi];
                    const uint32_t merge_lbl = (merge_bb < ir.blocks.size()) ? bb_labels[merge_bb] : epilogue_label;
                    const uint32_t cond = get_as_bool(term.operands[0]);
                    EmitSpvInst(sec_fn, SpvOpSelectionMerge, {merge_lbl, 0u});
                    EmitSpvInst(sec_fn, SpvOpBranchConditional,
                                {cond, bb_labels[term.true_block], bb_labels[term.false_block]});
                }
            } else {
                if (need_epilogue) {
                    EmitSpvInst(sec_fn, SpvOpBranch, {epilogue_label});
                } else {
                    EmitSpvInst(sec_fn, SpvOpReturn, {});
                }
            }
        } else {
            if (need_epilogue) {
                EmitSpvInst(sec_fn, SpvOpBranch, {epilogue_label});
            } else {
                EmitSpvInst(sec_fn, SpvOpReturn, {});
            }
        }
    }

    if (need_epilogue) {
        EmitSpvInst(sec_fn, SpvOpLabel, {epilogue_label});
        EmitSpvInst(sec_fn, SpvOpReturn, {});
    }

    EmitSpvInst(sec_fn, SpvOpFunctionEnd, {});

    // 7. Assemble final SPIR-V 1.3 binary stream
    const size_t total_words = 5u + sec_caps.size() + sec_ext.size() + sec_mem.size() +
                               sec_entry.size() + sec_exec.size() + sec_decor.size() +
                               sec_types.size() + sec_globals.size() + sec_fn.size();
    out_spirv.reserve(total_words);
    out_spirv.push_back(kSpirvMagic);
    out_spirv.push_back(kSpirvVersion13);
    out_spirv.push_back(kSpirvGeneratorOpenScarlett);
    out_spirv.push_back(next_id);
    out_spirv.push_back(0u);

    auto append_sec = [&](const std::vector<uint32_t>& s) {
        out_spirv.insert(out_spirv.end(), s.begin(), s.end());
    };
    append_sec(sec_caps);
    append_sec(sec_ext);
    append_sec(sec_mem);
    append_sec(sec_entry);
    append_sec(sec_exec);
    append_sec(sec_decor);
    append_sec(sec_types);
    append_sec(sec_globals);
    append_sec(sec_fn);

    CallTelemetry::Global().Record("rdna2_lifter", "EmitSpirv", CallOutcome::Ok, 0);
    return Status::Ok();
}

Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv) {
    if (spirv.size() < 5u) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V binary shorter than 5-word header");
    }
    if (spirv[0] != kSpirvMagic) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V magic number");
    }
    if (spirv[1] != kSpirvVersion13) {
        return Status::Error(StatusCode::ShaderError, "Unexpected SPIR-V version (expected 1.3)");
    }
    const uint32_t bound = spirv[3];
    if (bound < 2u) {
        return Status::Error(StatusCode::ShaderError, "Invalid SPIR-V ID bound");
    }
    if (spirv[4] != 0u) {
        return Status::Error(StatusCode::ShaderError, "Non-zero SPIR-V reserved schema word");
    }

    bool saw_capability = false;
    bool saw_memory_model = false;
    bool saw_entry_point = false;
    bool saw_function = false;
    bool saw_return = false;
    bool saw_function_end = false;
    int fn_depth = 0;
    bool allow_phi = false;

    size_t pos = 5u;
    while (pos < spirv.size()) {
        const uint32_t header = spirv[pos];
        const uint16_t word_count = static_cast<uint16_t>((header >> 16) & 0xFFFFu);
        const uint16_t opcode = static_cast<uint16_t>(header & 0xFFFFu);
        if (word_count == 0u || pos + word_count > spirv.size()) {
            return Status::Error(StatusCode::ShaderError, "Truncated or zero-length SPIR-V instruction");
        }

        if (opcode != SpvOpPhi && opcode != SpvOpLabel) {
            allow_phi = false;
        }

        switch (opcode) {
            case SpvOpCapability:
                if (word_count != 2u) return Status::Error(StatusCode::ShaderError, "Malformed OpCapability");
                saw_capability = true;
                break;
            case SpvOpMemoryModel:
                if (word_count != 3u) return Status::Error(StatusCode::ShaderError, "Malformed OpMemoryModel");
                saw_memory_model = true;
                break;
            case SpvOpEntryPoint:
                if (word_count < 4u || spirv[pos + 2] == 0u || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpEntryPoint");
                }
                saw_entry_point = true;
                break;
            case SpvOpDecorate:
                if (word_count < 3u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpDecorate");
                }
                break;
            case SpvOpMemberDecorate:
                if (word_count < 4u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpMemberDecorate");
                }
                break;
            case SpvOpFunction:
                if (word_count != 5u || spirv[pos + 1] >= bound || spirv[pos + 2] >= bound || spirv[pos + 4] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpFunction");
                }
                saw_function = true;
                ++fn_depth;
                break;
            case SpvOpFunctionEnd:
                if (word_count != 1u || fn_depth <= 0) {
                    return Status::Error(StatusCode::ShaderError, "Unbalanced OpFunctionEnd");
                }
                --fn_depth;
                saw_function_end = true;
                break;
            case SpvOpLabel:
                if (word_count != 2u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpLabel");
                }
                allow_phi = true;
                break;
            case SpvOpPhi:
                if (!allow_phi || word_count < 5u || ((word_count - 3u) & 1u) != 0u ||
                    spirv[pos + 1] >= bound || spirv[pos + 2] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed or misplaced OpPhi");
                }
                for (uint16_t k = 3u; k < word_count; ++k) {
                    if (spirv[pos + k] == 0u || spirv[pos + k] >= bound) {
                        return Status::Error(StatusCode::ShaderError, "OpPhi operand ID out of bound");
                    }
                }
                break;
            case SpvOpBranch:
                if (word_count != 2u || spirv[pos + 1] == 0u || spirv[pos + 1] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpBranch");
                }
                break;
            case SpvOpBranchConditional:
                if (word_count < 4u || spirv[pos + 1] >= bound || spirv[pos + 2] >= bound || spirv[pos + 3] >= bound) {
                    return Status::Error(StatusCode::ShaderError, "Malformed OpBranchConditional");
                }
                break;
            case SpvOpReturn:
                if (word_count != 1u) return Status::Error(StatusCode::ShaderError, "Malformed OpReturn");
                saw_return = true;
                break;
            default:
                break;
        }
        pos += word_count;
    }

    if (!saw_capability || !saw_memory_model || !saw_entry_point ||
        !saw_function || !saw_return || !saw_function_end || fn_depth != 0) {
        return Status::Error(StatusCode::ShaderError, "SPIR-V module missing required sections or unbalanced function");
    }
    return Status::Ok();
}

Status ValidateSpirvExternal(
    const std::vector<uint32_t>& spirv,
    std::string& out_diagnostics,
    const std::string& spirv_val_path,
    const std::string& target_env) {
    out_diagnostics.clear();
    if (auto st = ValidateSpirvBinary(spirv); !st.ok()) {
        out_diagnostics = st.message;
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -1);
        return st;
    }
#if defined(_WIN32)
    if (!std::filesystem::exists(spirv_val_path)) {
        out_diagnostics = "spirv-val.exe not found at: " + spirv_val_path;
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -2);
        return Status::Error(StatusCode::NotFound, out_diagnostics);
    }

    const std::string spv_path = MakeUniqueTempPath("spv", ".spv");
    {
        std::ofstream ofs(spv_path, std::ios::binary);
        if (!ofs) {
            out_diagnostics = "Failed to open temp SPIR-V file: " + spv_path;
            CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -3);
            return Status::Error(StatusCode::IoError, out_diagnostics);
        }
        ofs.write(reinterpret_cast<const char*>(spirv.data()),
                  static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));
    }

    const std::string cmd = "\"" + spirv_val_path + "\" --target-env " + target_env + " \"" + spv_path + "\"";
    uint32_t exit_code = 1;
    Status run_st = RunExternalProcess(cmd, exit_code, out_diagnostics);

    std::error_code ec;
    std::filesystem::remove(spv_path, ec);

    if (!run_st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error, -4);
        return run_st;
    }
    if (exit_code != 0u) {
        CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Error,
                                       static_cast<int32_t>(exit_code));
        return Status::Error(StatusCode::ShaderError, "spirv-val failed: " + out_diagnostics);
    }

    CallTelemetry::Global().Record("rdna2_lifter", "ValidateSpirvExternal", CallOutcome::Ok, 0);
    return Status::Ok();
#else
    (void)spirv_val_path;
    (void)target_env;
    return Status::Error(StatusCode::NotImplemented, "ValidateSpirvExternal requires Windows");
#endif
}

// ---------------------------------------------------------------------------
// HLSL Emission & External DXC Compilation
// ---------------------------------------------------------------------------
Status EmitHlsl(const Rdna2IrModule& ir, std::string& out_hlsl, std::string& out_target, bool use_sm65) {
    HlslEmitter emitter(ir, use_sm65);
    Status st = emitter.Emit(out_hlsl, out_target);
    CallTelemetry::Global().Record("rdna2_lifter", "EmitHlsl", st.ok() ? CallOutcome::Ok : CallOutcome::Error,
                                   st.ok() ? 0 : -1);
    return st;
}

Status CompileHlslWithDxc(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path) {
    out_dxil.clear();
    out_diagnostics.clear();
    if (hlsl.empty() || target.empty() || entry_point.empty()) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -1);
        return Status::Error(StatusCode::InvalidArgument, "CompileHlslWithDxc requires non-empty HLSL/target/entry");
    }
#if defined(_WIN32)
    const std::string resolved_dxc = ResolvePortableDxcPath(dxc_path);
    if (!std::filesystem::exists(resolved_dxc)) {
        out_diagnostics = "dxc.exe not found at: " + resolved_dxc;
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -2);
        return Status::Error(StatusCode::NotFound, out_diagnostics);
    }

    const std::string hlsl_path = MakeUniqueTempPath("hlsl", ".hlsl");
    const std::string dxil_path = MakeUniqueTempPath("dxil", ".dxil");

    {
        std::ofstream ofs(hlsl_path, std::ios::binary);
        if (!ofs) {
            out_diagnostics = "Failed to open temp HLSL file: " + hlsl_path;
            CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -3);
            return Status::Error(StatusCode::IoError, out_diagnostics);
        }
        ofs.write(hlsl.data(), static_cast<std::streamsize>(hlsl.size()));
    }

    const std::string cmd = "\"" + resolved_dxc + "\" -T " + target + " -E " + entry_point +
                            " -Fo \"" + dxil_path + "\" \"" + hlsl_path + "\"";
    uint32_t exit_code = 1;
    Status run_st = RunExternalProcess(cmd, exit_code, out_diagnostics);

    std::error_code ec;
    if (run_st.ok() && exit_code == 0u) {
        std::ifstream ifs(dxil_path, std::ios::binary | std::ios::ate);
        if (ifs) {
            const std::streamsize sz = ifs.tellg();
            if (sz > 0) {
                out_dxil.resize(static_cast<size_t>(sz));
                ifs.seekg(0, std::ios::beg);
                ifs.read(reinterpret_cast<char*>(out_dxil.data()), sz);
            }
        }
    }

    std::filesystem::remove(hlsl_path, ec);
    std::filesystem::remove(dxil_path, ec);

    if (!run_st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -4);
        return run_st;
    }
    if (exit_code != 0u) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error,
                                       static_cast<int32_t>(exit_code));
        return Status::Error(StatusCode::ShaderError, "dxc.exe failed: " + out_diagnostics);
    }
    if (out_dxil.size() < 32u || std::memcmp(out_dxil.data(), "DXBC", 4) != 0) {
        CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Error, -5);
        return Status::Error(StatusCode::ShaderError, "dxc.exe produced invalid or empty DXIL container");
    }

    CallTelemetry::Global().Record("rdna2_lifter", "CompileHlslWithDxc", CallOutcome::Ok, 0);
    return Status::Ok();
#else
    (void)dxc_path;
    return Status::Error(StatusCode::NotImplemented, "CompileHlslWithDxc requires Windows");
#endif
}

std::string ResolvePortableDxcPath(const std::string& requested_path) {
#if defined(_WIN32)
    return ResolveDxcExecutable(requested_path);
#else
    return requested_path;
#endif
}

Status LiftRdna2Xs2Shader(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options) {
    out_result = Rdna2LiftResult{};

    XboxDxbcContainer container_info{};
    if (auto st = ParseXboxShaderContainer(xs2_bytes, container_info); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -1);
        return st;
    }
    if (!container_info.has_xs2) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -1);
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Shader container does not carry an Xbox XS2 RDNA2 payload");
    }
    out_result.container = std::move(container_info.xs2);

    if (auto st = DisassembleRdna2(out_result.container.code_words, out_result.instructions); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -2);
        return st;
    }
    if (auto st = BuildRdna2Ir(out_result.container, out_result.instructions, out_result.ir); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -3);
        return st;
    }
    out_result.ir.mesh_has_payload = options.mesh_has_payload;

    if (auto st = OptimizeRdna2Ir(out_result.ir); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -4);
        return st;
    }

    const bool stage_supports_spirv =
        (out_result.ir.stage == ShaderStage::Compute ||
         out_result.ir.stage == ShaderStage::Vertex ||
         out_result.ir.stage == ShaderStage::Pixel);

    if (options.emit_spirv && stage_supports_spirv) {
        if (auto st = EmitSpirv(out_result.ir, out_result.spirv_words); !st.ok()) {
            CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -5);
            return st;
        }
        if (auto st = ValidateSpirvBinary(out_result.spirv_words); !st.ok()) {
            CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -6);
            return st;
        }
        if (options.validate_spirv_external) {
            if (auto st = ValidateSpirvExternal(out_result.spirv_words, out_result.spirv_val_diagnostics,
                                                options.spirv_val_path, options.spirv_target_env);
                !st.ok()) {
                CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -7);
                return st;
            }
        }
    }

    if (options.emit_hlsl) {
        if (auto st = EmitHlsl(out_result.ir, out_result.hlsl_source, out_result.hlsl_target, options.use_sm65);
            !st.ok()) {
            CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -8);
            return st;
        }
        if (!options.target_profile_override.empty()) {
            out_result.hlsl_target = options.target_profile_override;
        }
        if (options.compile_dxil_with_dxc) {
            if (auto st = CompileHlslWithDxc(out_result.hlsl_source, out_result.hlsl_target, "main",
                                             out_result.pc_dxil, out_result.dxc_diagnostics, options.dxc_path);
                !st.ok()) {
                CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -9);
                return st;
            }
            out_result.pc_dxil_hash = HashBytesFnv1a(out_result.pc_dxil.data(), out_result.pc_dxil.size());
        }
    }

    CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Ok, 0);
    return Status::Ok();
}

}  // namespace openscarlett
