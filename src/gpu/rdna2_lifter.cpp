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
    SpvOpFunction = 54,
    SpvOpFunctionEnd = 56,
    SpvOpVariable = 59,
    SpvOpLoad = 61,
    SpvOpStore = 62,
    SpvOpAccessChain = 65,
    SpvOpDecorate = 71,
    SpvOpMemberDecorate = 72,
    SpvOpCompositeConstruct = 80,
    SpvOpCompositeExtract = 81,
    SpvOpSampledImage = 86,
    SpvOpImageSampleExplicitLod = 88,
    SpvOpImageRead = 98,
    SpvOpImageWrite = 99,
    SpvOpConvertFToU = 109,
    SpvOpConvertFToS = 110,
    SpvOpConvertSToF = 111,
    SpvOpConvertUToF = 112,
    SpvOpBitcast = 124,
    SpvOpIAdd = 128,
    SpvOpFAdd = 129,
    SpvOpISub = 130,
    SpvOpFSub = 131,
    SpvOpIMul = 132,
    SpvOpFMul = 133,
    SpvOpFDiv = 136,
    SpvOpDot = 148,
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
    SpvOpFOrdGreaterThanEqual = 188,
    SpvOpShiftRightLogical = 194,
    SpvOpShiftRightArithmetic = 195,
    SpvOpShiftLeftLogical = 196,
    SpvOpBitwiseOr = 197,
    SpvOpBitwiseXor = 198,
    SpvOpBitwiseAnd = 199,
    SpvOpNot = 200,
    SpvOpBitCount = 205,
    SpvOpControlBarrier = 224,
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
    GlslUnpackHalf2x16 = 58,
};

// SPIR-V Storage Classes
constexpr uint32_t kSpvStorageUniformConstant = 0u;
constexpr uint32_t kSpvStorageInput = 1u;
constexpr uint32_t kSpvStorageUniform = 2u;
constexpr uint32_t kSpvStorageOutput = 3u;
constexpr uint32_t kSpvStorageWorkgroup = 4u;
constexpr uint32_t kSpvStorageStorageBuffer = 12u;

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
    std::filesystem::create_directories(kLifterTempDir, ec);
    uint32_t pid = 0;
#if defined(_WIN32)
    pid = static_cast<uint32_t>(GetCurrentProcessId());
#endif
    const uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << kLifterTempDir << "\\" << prefix << "_" << pid << "_" << seq << ext;
    return oss.str();
}

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
        if (ir_.uses_texture_srv) {
            hdr << "Texture2D<float4> _tex0 : register(t0);\n";
        }
        if (ir_.uses_texture_sample) {
            hdr << "SamplerState _samp0 : register(s0);\n";
        }
        if (ir_.uses_texture_uav) {
            const uint32_t rw_reg = uses_dual_uav ? 2u : 1u;
            hdr << "RWTexture2D<float4> _rwtex0 : register(u" << rw_reg << ");\n";
        }
        if (ir_.uses_lds) {
            const uint32_t lds_n = ir_.lds_size_dwords ? ir_.lds_size_dwords : 1024u;
            hdr << "groupshared uint _lds[" << lds_n << "];\n";
        }
        if (ir_.uses_wave64_emulation) {
            hdr << "groupshared uint _w64_ballot[2];\n";
            hdr << "groupshared uint _w64_lanes[64];\n";
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
                case IrOp::ImageStore:
                    os << ind << "_rwtex0[uint2(" << AsU32(inst.operands[0]) << ", " << AsU32(inst.operands[1])
                       << ")] = float4(" << AsF32(inst.operands[2]) << ", " << AsF32(inst.operands[3])
                       << ", " << AsF32(inst.operands[4]) << ", " << AsF32(inst.operands[5]) << ");\n";
                    break;
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
                    const std::string lane_expr = (inst.op == IrOp::WaveReadFirstLane)
                                                      ? "0u"
                                                      : ("(" + AsU32(inst.operands[1]) + " & 63u)");
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
    const uint32_t t_v3u32 = alloc_id();
    const uint32_t t_v4f32 = alloc_id();
    const uint32_t t_fn_void = alloc_id();

    EmitSpvInst(sec_types, SpvOpTypeVoid, {t_void});
    EmitSpvInst(sec_types, SpvOpTypeBool, {t_bool});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_u32, 32u, 0u});
    EmitSpvInst(sec_types, SpvOpTypeInt, {t_i32, 32u, 1u});
    EmitSpvInst(sec_types, SpvOpTypeFloat, {t_f32, 32u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v2f32, t_f32, 2u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v2i32, t_i32, 2u});
    EmitSpvInst(sec_types, SpvOpTypeVector, {t_v3u32, t_u32, 3u});
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
    const uint32_t c_u32_31 = get_const_u32(31u);
    const uint32_t c_u32_32 = get_const_u32(32u);
    const uint32_t c_u32_63 = get_const_u32(63u);
    const uint32_t c_u32_allones = get_const_u32(0xFFFFFFFFu);
    const uint32_t c_i32_0 = get_const_i32(0);
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

    if (ir.uses_texture_sample) {
        t_img2d = alloc_id();
        t_sampler = alloc_id();
        t_sampled_img2d = alloc_id();
        const uint32_t t_ptr_uc_img = alloc_id();
        const uint32_t t_ptr_uc_samp = alloc_id();

        EmitSpvInst(sec_types, SpvOpTypeImage, {t_img2d, t_f32, 1u, 0u, 0u, 0u, 1u, 0u});  // 2D, Sampled=1, Unknown
        EmitSpvInst(sec_types, SpvOpTypeSampler, {t_sampler});
        EmitSpvInst(sec_types, SpvOpTypeSampledImage, {t_sampled_img2d, t_img2d});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_img, kSpvStorageUniformConstant, t_img2d});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_uc_samp, kSpvStorageUniformConstant, t_sampler});

        var_tex0 = alloc_id();
        var_samp0 = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_img, var_tex0, kSpvStorageUniformConstant});
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_uc_samp, var_samp0, kSpvStorageUniformConstant});

        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex0, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_tex0, kSpvDecBinding, 3u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_samp0, kSpvDecDescriptorSet, 0u});
        EmitSpvInst(sec_decor, SpvOpDecorate, {var_samp0, kSpvDecBinding, 4u});
    }

    if (ir.uses_texture_uav || ir.uses_texture_load) {
        t_rwimg2d = alloc_id();
        const uint32_t t_ptr_uc_rwimg = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeImage, {t_rwimg2d, t_f32, 1u, 0u, 0u, 0u, 2u, 1u});  // 2D, Sampled=2, Rgba32f
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

    // Workgroup shared memory for LDS and/or wave64->wave32 emulation
    uint32_t t_ptr_wg_u32 = 0;
    uint32_t var_lds = 0;
    uint32_t var_w64_ballot = 0;
    uint32_t var_w64_lanes = 0;
    const uint32_t lds_size_dwords = ir.lds_size_dwords ? ir.lds_size_dwords : 1024u;
    const uint32_t c_lds_mask = get_const_u32(lds_size_dwords - 1u);

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
        const uint32_t c_len_64 = get_const_u32(64u);
        const uint32_t t_arr_2 = alloc_id();
        const uint32_t t_arr_64 = alloc_id();
        const uint32_t t_ptr_wg_2 = alloc_id();
        const uint32_t t_ptr_wg_64 = alloc_id();
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_2, t_u32, c_len_2});
        EmitSpvInst(sec_types, SpvOpTypeArray, {t_arr_64, t_u32, c_len_64});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_2, kSpvStorageWorkgroup, t_arr_2});
        EmitSpvInst(sec_types, SpvOpTypePointer, {t_ptr_wg_64, kSpvStorageWorkgroup, t_arr_64});
        var_w64_ballot = alloc_id();
        var_w64_lanes = alloc_id();
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_2, var_w64_ballot, kSpvStorageWorkgroup});
        EmitSpvInst(sec_globals, SpvOpVariable, {t_ptr_wg_64, var_w64_lanes, kSpvStorageWorkgroup});
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
                        EmitSpvInst(sec_fn, SpvOpBitwiseAnd, {t_u32, src_idx, get_as_u32(inst.operands[1]), c_u32_63});
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
    if (!std::filesystem::exists(dxc_path)) {
        out_diagnostics = "dxc.exe not found at: " + dxc_path;
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

    const std::string cmd = "\"" + dxc_path + "\" -T " + target + " -E " + entry_point +
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

Status LiftRdna2Xs2Shader(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options) {
    out_result = Rdna2LiftResult{};

    if (auto st = ParseXs2Container(xs2_bytes, out_result.container); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -1);
        return st;
    }
    if (auto st = DisassembleRdna2(out_result.container.code_words, out_result.instructions); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -2);
        return st;
    }
    if (auto st = BuildRdna2Ir(out_result.container, out_result.instructions, out_result.ir); !st.ok()) {
        CallTelemetry::Global().Record("rdna2_lifter", "LiftRdna2Xs2Shader", CallOutcome::Error, -3);
        return st;
    }
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
