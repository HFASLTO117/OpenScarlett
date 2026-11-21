#include "openscarlett/gpu/rdna2_ir.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace openscarlett {

namespace {

uint32_t FloatToBits(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Clean-room IEEE-754 binary16 <-> float32 conversion helpers (round-to-nearest-even)
float HalfBitsToFloat(uint16_t h) noexcept {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x03FFu;
    uint32_t fbits = 0;
    if (exp == 0u) {
        if (mant == 0u) {
            fbits = sign;
        } else {
            uint32_t m = mant;
            uint32_t e = 113u;  // 127 - 14
            while ((m & 0x0400u) == 0u) {
                m <<= 1;
                --e;
            }
            m &= 0x03FFu;
            fbits = sign | (e << 23) | (m << 13);
        }
    } else if (exp == 31u) {
        fbits = sign | 0x7F800000u | (mant << 13);
    } else {
        fbits = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    return BitsToFloat(fbits);
}

uint16_t FloatToHalfBits(float f) noexcept {
    const uint32_t bits = FloatToBits(f);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t exp = (bits >> 23) & 0xFFu;
    const uint32_t mant = bits & 0x007FFFFFu;
    if (exp == 255u) {
        if (mant != 0u) return static_cast<uint16_t>(sign | 0x7E00u | (mant >> 13));
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    const int32_t new_exp = static_cast<int32_t>(exp) - 112;
    if (new_exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    if (new_exp <= 0) {
        if (new_exp < -10) return static_cast<uint16_t>(sign);
        const uint32_t m = (mant | 0x00800000u) >> static_cast<uint32_t>(1 - new_exp);
        const uint32_t rounded = (m + 0x00001000u + ((m >> 13) & 1u) - 1u) >> 13;
        return static_cast<uint16_t>(sign | rounded);
    }
    const uint32_t half_mant = mant + 0x00000FFFu + ((mant >> 13) & 1u);
    if ((half_mant & 0x00800000u) != 0u) {
        if (new_exp + 1 >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
        return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp + 1) << 10));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp) << 10) | (half_mant >> 13));
}

uint16_t SelectHalf16(uint32_t packed, bool hi) noexcept {
    return static_cast<uint16_t>(hi ? ((packed >> 16) & 0xFFFFu) : (packed & 0xFFFFu));
}

float FlushDenormF32(float x) noexcept {
    const uint32_t bits = FloatToBits(x);
    if ((bits & 0x7F800000u) == 0u) {
        return BitsToFloat(bits & 0x80000000u);
    }
    return x;
}

float EvalMadF32(float a, float b, float c) noexcept {
    const float fa = FlushDenormF32(a);
    const float fb = FlushDenormF32(b);
    const float fc = FlushDenormF32(c);
    const float prod = FlushDenormF32(fa * fb);
    return FlushDenormF32(prod + fc);
}

bool IsBranchOpcode(Rdna2Opcode op) {
    switch (op) {
        case Rdna2Opcode::S_BRANCH:
        case Rdna2Opcode::S_CBRANCH_SCC0:
        case Rdna2Opcode::S_CBRANCH_SCC1:
        case Rdna2Opcode::S_CBRANCH_VCCZ:
        case Rdna2Opcode::S_CBRANCH_VCCNZ:
        case Rdna2Opcode::S_CBRANCH_EXECZ:
        case Rdna2Opcode::S_CBRANCH_EXECNZ:
            return true;
        default:
            return false;
    }
}

bool IsConditionalBranchOpcode(Rdna2Opcode op) {
    return IsBranchOpcode(op) && op != Rdna2Opcode::S_BRANCH;
}

void AddUnique(std::vector<uint32_t>& vec, uint32_t val) {
    if (std::find(vec.begin(), vec.end(), val) == vec.end()) {
        vec.push_back(val);
    }
}

struct RegState {
    std::array<IrValueId, 256> vgpr{};
    std::array<IrValueId, 106> sgpr{};
    IrValueId scc = kInvalidIrValue;
    IrValueId vcc = kInvalidIrValue;
};

}  // namespace

const IrInstruction* Rdna2IrModule::FindDef(IrValueId id) const noexcept {
    if (id == kInvalidIrValue) return nullptr;
    for (const auto& bb : blocks) {
        for (const auto& inst : bb.instructions) {
            if (inst.id == id) return &inst;
        }
    }
    return nullptr;
}

const char* IrOpName(IrOp op) noexcept {
    switch (op) {
        case IrOp::ConstU32:      return "const.u32";
        case IrOp::ConstF32:      return "const.f32";
        case IrOp::ThreadIdX:     return "thread_id.x";
        case IrOp::StageInput:    return "stage_input";
        case IrOp::LoadRootConst: return "load_root_const";
        case IrOp::LoadUav:       return "load_uav";
        case IrOp::StoreUav:      return "store_uav";
        case IrOp::ImageSample:   return "image_sample";
        case IrOp::ImageSampleImplicit: return "image_sample_implicit";
        case IrOp::ImageLoad:     return "image_load";
        case IrOp::ImageLoadMip:  return "image_load_mip";
        case IrOp::ImageStore:    return "image_store";
        case IrOp::ImageAtomicCmpSwap: return "image_atomic_cmpswap";
        case IrOp::BvhIntersectRay:    return "bvh_intersect_ray";
        case IrOp::LdsLoad:       return "lds_load";
        case IrOp::LdsStore:      return "lds_store";
        case IrOp::LdsAtomicAdd:  return "lds_atomic_add";
        case IrOp::ScratchLoad:   return "scratch_load";
        case IrOp::ScratchStore:  return "scratch_store";
        case IrOp::WaveLaneId:            return "wave_lane_id";
        case IrOp::WaveReadFirstLane:     return "wave_read_first_lane";
        case IrOp::WaveReadLaneAt:        return "wave_read_lane_at";
        case IrOp::WaveBallotLo:          return "wave_ballot_lo";
        case IrOp::WaveBallotHi:          return "wave_ballot_hi";
        case IrOp::WaveActiveCountBits64: return "wave_active_count_bits64";
        case IrOp::WavePrefixCount64:     return "wave_prefix_count64";
        case IrOp::WavePermLane16:        return "wave_permlane16";
        case IrOp::SetMeshOutputCounts:   return "set_mesh_output_counts";
        case IrOp::ExportMeshPrim:        return "export_mesh_prim";
        case IrOp::DispatchMeshGroups:    return "dispatch_mesh_groups";
        case IrOp::FAdd:          return "fadd";
        case IrOp::FSub:          return "fsub";
        case IrOp::FMul:          return "fmul";
        case IrOp::FFma:          return "ffma";
        case IrOp::FMad:          return "fmad";
        case IrOp::FFma16:        return "ffma.f16";
        case IrOp::FMin:          return "fmin";
        case IrOp::FMax:          return "fmax";
        case IrOp::FRcp:          return "frcp";
        case IrOp::FRsq:          return "frsq";
        case IrOp::FSqrt:         return "fsqrt";
        case IrOp::FFloor:        return "ffloor";
        case IrOp::FFract:        return "ffract";
        case IrOp::FDot2:         return "fdot2";
        case IrOp::IAdd:          return "iadd";
        case IrOp::ISub:          return "isub";
        case IrOp::IMul:          return "imul";
        case IrOp::IMin:          return "imin";
        case IrOp::IMax:          return "imax";
        case IrOp::BitAnd:        return "and";
        case IrOp::BitOr:         return "or";
        case IrOp::BitXor:        return "xor";
        case IrOp::BitNot:        return "not";
        case IrOp::BitCount32:    return "bcnt32";
        case IrOp::Shl:           return "shl";
        case IrOp::Shr:           return "shr";
        case IrOp::BfeU32:        return "bfe.u32";
        case IrOp::BfeI32:        return "bfe.i32";
        case IrOp::BfiB32:        return "bfi.b32";
        case IrOp::PkAddF16:      return "pk_add.f16";
        case IrOp::PkMulF16:      return "pk_mul.f16";
        case IrOp::PkFmaF16:      return "pk_fma.f16";
        case IrOp::PkAddU16:      return "pk_add.u16";
        case IrOp::PkShl16:       return "pk_shl.b16";
        case IrOp::PkShr16:       return "pk_shr.b16";
        case IrOp::I2F:           return "i2f";
        case IrOp::F2I:           return "f2i";
        case IrOp::U2F:           return "u2f";
        case IrOp::F2U:           return "f2u";
        case IrOp::CmpEq:         return "cmp.eq";
        case IrOp::CmpNe:         return "cmp.ne";
        case IrOp::CmpLt:         return "cmp.lt";
        case IrOp::CmpGt:         return "cmp.gt";
        case IrOp::CmpGe:         return "cmp.ge";
        case IrOp::CmpLtU:        return "cmp.ltu";
        case IrOp::CmpEqU:        return "cmp.equ";
        case IrOp::CmpGtU:        return "cmp.gtu";
        case IrOp::CmpGeU:        return "cmp.geu";
        case IrOp::Select:        return "select";
        case IrOp::Phi:           return "phi";
        case IrOp::Barrier:       return "barrier";
        case IrOp::Branch:        return "br";
        case IrOp::CondBranch:    return "cbr";
        case IrOp::Export:        return "export";
        case IrOp::Return:        return "ret";
        default:                  return "unknown";
    }
}

const char* IrTypeName(IrType type) noexcept {
    switch (type) {
        case IrType::Void: return "void";
        case IrType::U32:  return "u32";
        case IrType::I32:  return "i32";
        case IrType::F32:  return "f32";
        case IrType::Bool: return "bool";
        default:           return "?";
    }
}

bool IrOpHasSideEffects(IrOp op) noexcept {
    switch (op) {
        case IrOp::StoreUav:
        case IrOp::ImageStore:
        case IrOp::ImageAtomicCmpSwap:
        case IrOp::LdsStore:
        case IrOp::LdsAtomicAdd:
        case IrOp::ScratchStore:
        case IrOp::SetMeshOutputCounts:
        case IrOp::ExportMeshPrim:
        case IrOp::DispatchMeshGroups:
        case IrOp::Barrier:
        case IrOp::Branch:
        case IrOp::CondBranch:
        case IrOp::Export:
        case IrOp::Return:
            return true;
        default:
            return false;
    }
}

Status BuildRdna2Ir(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2IrModule& out_module) {
    out_module = Rdna2IrModule{};
    out_module.stage = container.stage;
    out_module.threadgroup_x = container.threadgroup_x;
    out_module.threadgroup_y = container.threadgroup_y;
    out_module.threadgroup_z = container.threadgroup_z;
    out_module.sgpr_count = container.sgpr_count;
    out_module.vgpr_count = container.vgpr_count;
    out_module.wave_size = (container.wave_size == 64u) ? 64u : 32u;
    out_module.uses_wave64_emulation = (out_module.wave_size == 64u);

    if (instructions.empty()) {
        return Status::Error(StatusCode::ParseError, "Cannot build IR from empty instruction stream");
    }

    // 1. Map word offsets -> instruction indices & identify basic block leaders.
    std::unordered_map<uint32_t, size_t> word_to_idx;
    word_to_idx.reserve(instructions.size());
    for (size_t i = 0; i < instructions.size(); ++i) {
        word_to_idx[instructions[i].word_offset] = i;
    }

    std::set<size_t> leader_set;
    leader_set.insert(0);
    std::unordered_map<size_t, size_t> branch_target_idx;

    for (size_t i = 0; i < instructions.size(); ++i) {
        const auto& inst = instructions[i];
        if (IsBranchOpcode(inst.opcode)) {
            const int64_t target_word =
                static_cast<int64_t>(inst.word_offset) + 1 + static_cast<int64_t>(inst.simm16);
            if (target_word < 0) {
                return Status::Error(StatusCode::ParseError, "RDNA2 branch target out of range (< 0)");
            }
            auto it = word_to_idx.find(static_cast<uint32_t>(target_word));
            if (it == word_to_idx.end()) {
                return Status::Error(StatusCode::ParseError, "RDNA2 branch target does not land on instruction boundary");
            }
            branch_target_idx[i] = it->second;
            leader_set.insert(it->second);
            if (i + 1 < instructions.size()) {
                leader_set.insert(i + 1);
            }
        } else if (inst.opcode == Rdna2Opcode::S_ENDPGM) {
            if (i + 1 < instructions.size()) {
                leader_set.insert(i + 1);
            }
        }
    }

    std::vector<size_t> leaders(leader_set.begin(), leader_set.end());
    std::unordered_map<size_t, uint32_t> inst_idx_to_bb;
    out_module.blocks.resize(leaders.size());
    for (size_t b = 0; b < leaders.size(); ++b) {
        out_module.blocks[b].id = static_cast<uint32_t>(b);
        out_module.blocks[b].start_word_offset = instructions[leaders[b]].word_offset;
        inst_idx_to_bb[leaders[b]] = static_cast<uint32_t>(b);
    }

    // Build CFG edges between basic blocks.
    for (size_t b = 0; b < leaders.size(); ++b) {
        const size_t end_i = (b + 1 < leaders.size()) ? leaders[b + 1] : instructions.size();
        const size_t last_i = end_i - 1;
        const auto& last = instructions[last_i];

        if (last.opcode == Rdna2Opcode::S_ENDPGM) {
            // Terminal block
        } else if (last.opcode == Rdna2Opcode::S_BRANCH) {
            const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[last_i]];
            AddUnique(out_module.blocks[b].successors, tgt_bb);
            AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
        } else if (IsConditionalBranchOpcode(last.opcode)) {
            const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[last_i]];
            AddUnique(out_module.blocks[b].successors, tgt_bb);
            AddUnique(out_module.blocks[tgt_bb].predecessors, static_cast<uint32_t>(b));
            if (b + 1 < leaders.size()) {
                const uint32_t fall_bb = static_cast<uint32_t>(b + 1);
                AddUnique(out_module.blocks[b].successors, fall_bb);
                AddUnique(out_module.blocks[fall_bb].predecessors, static_cast<uint32_t>(b));
            }
        } else if (b + 1 < leaders.size()) {
            const uint32_t fall_bb = static_cast<uint32_t>(b + 1);
            AddUnique(out_module.blocks[b].successors, fall_bb);
            AddUnique(out_module.blocks[fall_bb].predecessors, static_cast<uint32_t>(b));
        }
    }

    // 2. Entry-block hoisted input/constant definitions so they dominate all blocks.
    std::vector<IrInstruction> entry_preamble;
    std::unordered_map<IrValueId, IrType> value_types;

    auto register_preamble = [&](IrOp op, IrType type, uint32_t imm_u = 0, float imm_f = 0.0f) -> IrValueId {
        IrInstruction inst{};
        inst.id = out_module.AllocateValueId();
        inst.op = op;
        inst.type = type;
        inst.imm_u32 = imm_u;
        inst.imm_f32 = imm_f;
        value_types[inst.id] = type;
        entry_preamble.push_back(std::move(inst));
        return entry_preamble.back().id;
    };

    IrValueId tid_x_val = kInvalidIrValue;
    IrValueId zero_u32_val = kInvalidIrValue;
    IrValueId zero_f32_val = kInvalidIrValue;
    std::array<IrValueId, 4> ps_inputs{};
    std::unordered_map<uint32_t, IrValueId> root_const_vals;

    auto get_zero_u32 = [&]() -> IrValueId {
        if (zero_u32_val == kInvalidIrValue) {
            zero_u32_val = register_preamble(IrOp::ConstU32, IrType::U32, 0u, 0.0f);
        }
        return zero_u32_val;
    };

    auto get_zero_f32 = [&]() -> IrValueId {
        if (zero_f32_val == kInvalidIrValue) {
            zero_f32_val = register_preamble(IrOp::ConstF32, IrType::F32, 0u, 0.0f);
        }
        return zero_f32_val;
    };

    auto get_tid_x = [&]() -> IrValueId {
        if (tid_x_val == kInvalidIrValue) {
            tid_x_val = register_preamble(IrOp::ThreadIdX, IrType::U32, 0u, 0.0f);
        }
        return tid_x_val;
    };

    auto get_ps_input = [&](uint32_t ch) -> IrValueId {
        if (ch < 4u && ps_inputs[ch] == kInvalidIrValue) {
            ps_inputs[ch] = register_preamble(IrOp::StageInput, IrType::F32, ch, 0.0f);
        }
        return ch < 4u ? ps_inputs[ch] : get_zero_f32();
    };

    auto get_root_const = [&](uint32_t idx) -> IrValueId {
        auto it = root_const_vals.find(idx);
        if (it != root_const_vals.end()) return it->second;
        out_module.uses_root_constants = true;
        if (idx > out_module.max_root_const_index) {
            out_module.max_root_const_index = idx;
        }
        IrValueId v = register_preamble(IrOp::LoadRootConst, IrType::U32, idx, 0.0f);
        root_const_vals[idx] = v;
        return v;
    };

    auto get_type = [&](IrValueId id) -> IrType {
        auto it = value_types.find(id);
        return it != value_types.end() ? it->second : IrType::U32;
    };

    // 3. Lower each basic block into SSA IR.
    std::vector<RegState> exit_states(leaders.size());
    std::vector<bool> block_processed(leaders.size(), false);

    for (size_t b = 0; b < leaders.size(); ++b) {
        auto& bb = out_module.blocks[b];
        RegState state{};

        auto emit_in_bb = [&](IrOp op, IrType type, std::vector<IrValueId> ops = {},
                              uint32_t imm_u = 0, float imm_f = 0.0f) -> IrValueId {
            IrInstruction inst{};
            inst.op = op;
            inst.type = type;
            inst.operands = std::move(ops);
            inst.imm_u32 = imm_u;
            inst.imm_f32 = imm_f;
            if (type != IrType::Void) {
                inst.id = out_module.AllocateValueId();
                value_types[inst.id] = type;
            }
            bb.instructions.push_back(std::move(inst));
            return bb.instructions.back().id;
        };

        // Merge register state from predecessors (emitting Phi nodes at join blocks).
        std::vector<uint32_t> ready_preds;
        for (uint32_t p : bb.predecessors) {
            if (block_processed[p]) ready_preds.push_back(p);
        }

        if (ready_preds.size() == 1) {
            state = exit_states[ready_preds[0]];
        } else if (ready_preds.size() > 1) {
            auto merge_reg = [&](auto getter) -> IrValueId {
                IrValueId first = kInvalidIrValue;
                bool differs = false;
                bool any_set = false;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v != kInvalidIrValue) any_set = true;
                    if (first == kInvalidIrValue) {
                        first = v;
                    } else if (v != first) {
                        differs = true;
                    }
                }
                if (!any_set) return kInvalidIrValue;
                if (!differs) return first;

                IrType phi_type = IrType::U32;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v != kInvalidIrValue) {
                        phi_type = get_type(v);
                        break;
                    }
                }

                IrInstruction phi{};
                phi.id = out_module.AllocateValueId();
                phi.op = IrOp::Phi;
                phi.type = phi_type;
                for (uint32_t p : ready_preds) {
                    IrValueId v = getter(exit_states[p]);
                    if (v == kInvalidIrValue) {
                        v = (phi_type == IrType::F32) ? get_zero_f32() : get_zero_u32();
                    }
                    phi.phi_incoming.emplace_back(p, v);
                }
                value_types[phi.id] = phi_type;
                bb.instructions.push_back(std::move(phi));
                return bb.instructions.back().id;
            };

            for (size_t r = 0; r < 256; ++r) {
                state.vgpr[r] = merge_reg([r](const RegState& s) { return s.vgpr[r]; });
            }
            for (size_t r = 0; r < 106; ++r) {
                state.sgpr[r] = merge_reg([r](const RegState& s) { return s.sgpr[r]; });
            }
            state.scc = merge_reg([](const RegState& s) { return s.scc; });
            state.vcc = merge_reg([](const RegState& s) { return s.vcc; });
        }

        auto read_vgpr = [&](uint8_t idx, bool expect_float) -> IrValueId {
            if (state.vgpr[idx] != kInvalidIrValue) return state.vgpr[idx];
            if (out_module.stage == ShaderStage::Pixel && idx < 4u) {
                state.vgpr[idx] = get_ps_input(idx);
                return state.vgpr[idx];
            }
            if ((out_module.stage == ShaderStage::Compute || out_module.stage == ShaderStage::Vertex ||
                 out_module.stage == ShaderStage::Geometry ||
                 out_module.stage == ShaderStage::Mesh || out_module.stage == ShaderStage::Amplification) &&
                idx == 0u) {
                state.vgpr[idx] = get_tid_x();
                return state.vgpr[idx];
            }
            return expect_float ? get_zero_f32() : get_zero_u32();
        };

        auto read_sgpr = [&](uint8_t idx) -> IrValueId {
            if (state.sgpr[idx] != kInvalidIrValue) return state.sgpr[idx];
            if (idx < 16u) {
                state.sgpr[idx] = get_root_const(idx);
                return state.sgpr[idx];
            }
            return get_zero_u32();
        };

        // Per-block cache for 64-lane ballot masks derived from a predicate value.
        std::unordered_map<IrValueId, std::pair<IrValueId, IrValueId>> ballot_cache;
        auto get_ballot_pair = [&](IrValueId pred_bool) -> std::pair<IrValueId, IrValueId> {
            auto it = ballot_cache.find(pred_bool);
            if (it != ballot_cache.end()) return it->second;
            out_module.uses_wave64_emulation = true;
            IrValueId lo = emit_in_bb(IrOp::WaveBallotLo, IrType::U32, {pred_bool});
            IrValueId hi = emit_in_bb(IrOp::WaveBallotHi, IrType::U32, {pred_bool});
            ballot_cache[pred_bool] = {lo, hi};
            return {lo, hi};
        };

        auto eval_operand = [&](const Rdna2Operand& op, bool expect_float) -> IrValueId {
            IrValueId val = kInvalidIrValue;
            switch (op.kind) {
                case Rdna2OperandKind::Vgpr:
                    val = read_vgpr(static_cast<uint8_t>(op.reg_index), expect_float);
                    break;
                case Rdna2OperandKind::Sgpr:
                    val = read_sgpr(static_cast<uint8_t>(op.reg_index));
                    break;
                case Rdna2OperandKind::InlineInt:
                    if (expect_float && op.int_value == 0) {
                        val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(op.int_value), 0.0f);
                    }
                    break;
                case Rdna2OperandKind::InlineFloat:
                    val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, op.literal_bits, op.float_value);
                    break;
                case Rdna2OperandKind::Literal32:
                    if (expect_float) {
                        val = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, op.literal_bits, BitsToFloat(op.literal_bits));
                    } else {
                        val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, op.literal_bits, 0.0f);
                    }
                    break;
                case Rdna2OperandKind::VccLo:
                case Rdna2OperandKind::VccHi:
                    val = (state.vcc != kInvalidIrValue) ? state.vcc : get_zero_u32();
                    break;
                case Rdna2OperandKind::ExecLo:
                case Rdna2OperandKind::ExecHi:
                    val = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    break;
                case Rdna2OperandKind::Scc:
                    val = (state.scc != kInvalidIrValue) ? state.scc : get_zero_u32();
                    break;
                default:
                    val = expect_float ? get_zero_f32() : get_zero_u32();
                    break;
            }
            if (expect_float) {
                if (op.abs) {
                    IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    IrValueId neg_v = emit_in_bb(IrOp::FSub, IrType::F32, {z, val});
                    val = emit_in_bb(IrOp::FMax, IrType::F32, {val, neg_v});
                }
                if (op.neg) {
                    IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                    val = emit_in_bb(IrOp::FSub, IrType::F32, {z, val});
                }
            }
            return val;
        };

        auto apply_clamp = [&](IrValueId val, bool clamp) -> IrValueId {
            if (!clamp) return val;
            IrValueId z = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
            IrValueId o = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
            IrValueId lo = emit_in_bb(IrOp::FMax, IrType::F32, {val, z});
            return emit_in_bb(IrOp::FMin, IrType::F32, {lo, o});
        };

        auto ensure_bool = [&](IrValueId v) -> IrValueId {
            if (v == kInvalidIrValue) {
                IrValueId z = get_zero_u32();
                return emit_in_bb(IrOp::CmpNe, IrType::Bool, {z, z});
            }
            if (get_type(v) == IrType::Bool) return v;
            IrValueId z = get_zero_u32();
            return emit_in_bb(IrOp::CmpNe, IrType::Bool, {v, z});
        };

        auto write_dst = [&](const Rdna2Operand& dst, IrValueId val) {
            if (dst.kind == Rdna2OperandKind::Vgpr) {
                state.vgpr[dst.reg_index] = val;
            } else if (dst.kind == Rdna2OperandKind::Sgpr) {
                state.sgpr[dst.reg_index] = val;
            } else if (dst.kind == Rdna2OperandKind::VccLo) {
                state.vcc = val;
            } else if (dst.kind == Rdna2OperandKind::Scc) {
                state.scc = val;
            }
        };

        const size_t start_i = leaders[b];
        const size_t end_i = (b + 1 < leaders.size()) ? leaders[b + 1] : instructions.size();
        bool terminated = false;

        for (size_t i = start_i; i < end_i; ++i) {
            const auto& inst = instructions[i];
            switch (inst.opcode) {
                // ----- SOP2 -----
                case Rdna2Opcode::S_ADD_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_SUB_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_ADD_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_SUB_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MIN_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MIN_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MAX_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MAX_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_CSELECT_B32: {
                    IrValueId c = ensure_bool(state.scc);
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Select, get_type(a), {c, a, bval}));
                    break;
                }
                case Rdna2Opcode::S_AND_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_OR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitOr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_XOR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitXor, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_LSHL_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shl, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_LSHR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_ASHR_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::S_MUL_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;

                // ----- SOP1 -----
                case Rdna2Opcode::S_MOV_B32: {
                    const bool is_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    write_dst(inst.dst, eval_operand(inst.src[0], is_flt));
                    break;
                }
                case Rdna2Opcode::S_NOT_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitNot, IrType::U32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::S_BCNT1_I32_B32: {
                    IrValueId word = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccLo) {
                        word = get_ballot_pair(ensure_bool(state.vcc)).first;
                    } else if (inst.src[0].kind == Rdna2OperandKind::VccHi) {
                        word = get_ballot_pair(ensure_bool(state.vcc)).second;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecLo) {
                        word = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecHi) {
                        word = emit_in_bb(IrOp::ConstU32, IrType::U32, {},
                                          (out_module.wave_size == 64u) ? 0xFFFFFFFFu : 0u, 0.0f);
                    } else {
                        word = eval_operand(inst.src[0], false);
                    }
                    IrValueId cnt = emit_in_bb(IrOp::BitCount32, IrType::U32, {word});
                    write_dst(inst.dst, cnt);
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {cnt, get_zero_u32()});
                    break;
                }
                case Rdna2Opcode::S_BCNT1_I32_B64: {
                    IrValueId lo = kInvalidIrValue;
                    IrValueId hi = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccLo || inst.src[0].kind == Rdna2OperandKind::VccHi) {
                        auto pair = get_ballot_pair(ensure_bool(state.vcc));
                        lo = pair.first;
                        hi = pair.second;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecLo || inst.src[0].kind == Rdna2OperandKind::ExecHi) {
                        lo = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                        hi = emit_in_bb(IrOp::ConstU32, IrType::U32, {},
                                        (out_module.wave_size == 64u) ? 0xFFFFFFFFu : 0u, 0.0f);
                    } else if (inst.src[0].kind == Rdna2OperandKind::Sgpr) {
                        const uint8_t idx = static_cast<uint8_t>(inst.src[0].reg_index);
                        lo = read_sgpr(idx);
                        hi = (idx + 1u < 106u) ? read_sgpr(static_cast<uint8_t>(idx + 1u)) : get_zero_u32();
                    } else if (inst.src[0].kind == Rdna2OperandKind::InlineInt && inst.src[0].int_value < 0) {
                        lo = eval_operand(inst.src[0], false);
                        hi = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else {
                        lo = eval_operand(inst.src[0], false);
                        hi = get_zero_u32();
                    }
                    IrValueId cnt = emit_in_bb(IrOp::WaveActiveCountBits64, IrType::U32, {lo, hi});
                    write_dst(inst.dst, cnt);
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {cnt, get_zero_u32()});
                    break;
                }
                case Rdna2Opcode::S_GETPC_B64: {
                    IrValueId pc_lo = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, (inst.word_offset + 1u) * 4u, 0.0f);
                    write_dst(inst.dst, pc_lo);
                    if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index + 1u < 106u) {
                        state.sgpr[inst.dst.reg_index + 1u] = get_zero_u32();
                    }
                    break;
                }

                // ----- SOPC -----
                case Rdna2Opcode::S_CMP_EQ_I32:
                    state.scc = emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LG_I32:
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GT_I32:
                    state.scc = emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GE_I32:
                    state.scc = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LT_I32:
                    state.scc = emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LE_I32:
                    state.scc = emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[1], false), eval_operand(inst.src[0], false)});
                    break;
                case Rdna2Opcode::S_CMP_EQ_U32:
                    state.scc = emit_in_bb(IrOp::CmpEqU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LG_U32:
                    state.scc = emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GT_U32:
                    state.scc = emit_in_bb(IrOp::CmpGtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_GE_U32:
                    state.scc = emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LT_U32:
                    state.scc = emit_in_bb(IrOp::CmpLtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    break;
                case Rdna2Opcode::S_CMP_LE_U32:
                    state.scc = emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[1], false), eval_operand(inst.src[0], false)});
                    break;

                // ----- SOPK -----
                case Rdna2Opcode::S_MOVK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    write_dst(inst.dst, emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f));
                    break;
                }
                case Rdna2Opcode::S_ADDK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    IrValueId cur = read_sgpr(static_cast<uint8_t>(inst.dst.reg_index));
                    IrValueId k = emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f);
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::I32, {cur, k}));
                    break;
                }
                case Rdna2Opcode::S_MULK_I32: {
                    const uint32_t imm = static_cast<uint32_t>(static_cast<int32_t>(inst.simm16));
                    IrValueId cur = read_sgpr(static_cast<uint8_t>(inst.dst.reg_index));
                    IrValueId k = emit_in_bb(IrOp::ConstU32, IrType::I32, {}, imm, 0.0f);
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::I32, {cur, k}));
                    break;
                }

                // ----- SOPP -----
                case Rdna2Opcode::S_NOP:
                case Rdna2Opcode::S_WAITCNT:
                    break;
                case Rdna2Opcode::S_BARRIER:
                    emit_in_bb(IrOp::Barrier, IrType::Void);
                    break;
                case Rdna2Opcode::S_SENDMSG: {
                    const uint16_t msg_id = static_cast<uint16_t>(static_cast<uint16_t>(inst.simm16) & 0xFu);
                    if (out_module.stage == ShaderStage::Mesh || msg_id == kRdna2SendMsgGsAllocReq) {
                        const uint32_t imm_v = (static_cast<uint32_t>(static_cast<uint16_t>(inst.simm16)) >> 4) & 0x3Fu;
                        const uint32_t imm_p = (static_cast<uint32_t>(static_cast<uint16_t>(inst.simm16)) >> 10) & 0x3Fu;
                        IrValueId vcnt = (state.sgpr[0] != kInvalidIrValue)
                                             ? state.sgpr[0]
                                             : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, imm_v ? imm_v : 3u, 0.0f);
                        IrValueId pcnt = (state.sgpr[1] != kInvalidIrValue)
                                             ? state.sgpr[1]
                                             : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, imm_p ? imm_p : 1u, 0.0f);
                        emit_in_bb(IrOp::SetMeshOutputCounts, IrType::Void, {vcnt, pcnt});
                    } else if (out_module.stage == ShaderStage::Amplification || msg_id == kRdna2SendMsgDispatchMesh) {
                        IrValueId gx = (state.sgpr[0] != kInvalidIrValue)
                                           ? state.sgpr[0]
                                           : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        IrValueId gy = (state.sgpr[1] != kInvalidIrValue)
                                           ? state.sgpr[1]
                                           : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        IrValueId gz = (state.sgpr[2] != kInvalidIrValue)
                                           ? state.sgpr[2]
                                           : emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        emit_in_bb(IrOp::DispatchMeshGroups, IrType::Void, {gx, gy, gz});
                    }
                    break;
                }
                case Rdna2Opcode::S_ENDPGM:
                    emit_in_bb(IrOp::Return, IrType::Void);
                    terminated = true;
                    break;
                case Rdna2Opcode::S_BRANCH: {
                    IrInstruction br{};
                    br.op = IrOp::Branch;
                    br.type = IrType::Void;
                    br.true_block = inst_idx_to_bb[branch_target_idx[i]];
                    bb.instructions.push_back(std::move(br));
                    terminated = true;
                    break;
                }
                case Rdna2Opcode::S_CBRANCH_SCC0:
                case Rdna2Opcode::S_CBRANCH_SCC1:
                case Rdna2Opcode::S_CBRANCH_VCCZ:
                case Rdna2Opcode::S_CBRANCH_VCCNZ:
                case Rdna2Opcode::S_CBRANCH_EXECZ:
                case Rdna2Opcode::S_CBRANCH_EXECNZ: {
                    const bool uses_scc = (inst.opcode == Rdna2Opcode::S_CBRANCH_SCC0 || inst.opcode == Rdna2Opcode::S_CBRANCH_SCC1);
                    const bool branch_when_false = (inst.opcode == Rdna2Opcode::S_CBRANCH_SCC0 ||
                                                    inst.opcode == Rdna2Opcode::S_CBRANCH_VCCZ ||
                                                    inst.opcode == Rdna2Opcode::S_CBRANCH_EXECZ);
                    IrValueId cond = ensure_bool(uses_scc ? state.scc : state.vcc);
                    const uint32_t tgt_bb = inst_idx_to_bb[branch_target_idx[i]];
                    const uint32_t fall_bb = (b + 1 < leaders.size()) ? static_cast<uint32_t>(b + 1) : tgt_bb;
                    IrInstruction cbr{};
                    cbr.op = IrOp::CondBranch;
                    cbr.type = IrType::Void;
                    cbr.operands.push_back(cond);
                    if (branch_when_false) {
                        cbr.true_block = fall_bb;
                        cbr.false_block = tgt_bb;
                    } else {
                        cbr.true_block = tgt_bb;
                        cbr.false_block = fall_bb;
                    }
                    bb.instructions.push_back(std::move(cbr));
                    terminated = true;
                    break;
                }

                // ----- VOP2 / promoted VOP3 -----
                case Rdna2Opcode::V_ADD_F32: {
                    IrValueId r = emit_in_bb(IrOp::FAdd, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_SUB_F32: {
                    IrValueId r = emit_in_bb(IrOp::FSub, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_SUBREV_F32: {
                    IrValueId r = emit_in_bb(IrOp::FSub, IrType::F32, {eval_operand(inst.src[1], true), eval_operand(inst.src[0], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MUL_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMul, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MIN_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMin, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MAX_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMax, IrType::F32, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MIN_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAX_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::I32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MIN_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMin, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAX_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMax, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_MAC_F32: {
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId acc = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), true);
                    IrValueId r = emit_in_bb(IrOp::FMad, IrType::F32, {a, bval, acc});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MADMK_F32:
                case Rdna2Opcode::V_MADAK_F32: {
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId cval = eval_operand(inst.src[2], true);
                    IrValueId r = emit_in_bb(IrOp::FMad, IrType::F32, {a, bval, cval});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_FMAC_F32: {
                    IrValueId a = eval_operand(inst.src[0], true);
                    IrValueId bval = eval_operand(inst.src[1], true);
                    IrValueId acc = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), true);
                    IrValueId r = emit_in_bb(IrOp::FFma, IrType::F32, {a, bval, acc});
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_FMAC_F16: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId acc = read_vgpr(static_cast<uint8_t>(inst.dst.reg_index), false);
                    const uint32_t flags = inst.clamp ? (1u << 16) : 0u;
                    write_dst(inst.dst, emit_in_bb(IrOp::FFma16, IrType::U32, {a, bval, acc}, flags));
                    break;
                }
                case Rdna2Opcode::V_FMAMK_F16:
                case Rdna2Opcode::V_FMAAK_F16: {
                    IrValueId a = eval_operand(inst.src[0], false);
                    IrValueId bval = eval_operand(inst.src[1], false);
                    IrValueId cval = eval_operand(inst.src[2], false);
                    const uint32_t flags = inst.clamp ? (1u << 16) : 0u;
                    write_dst(inst.dst, emit_in_bb(IrOp::FFma16, IrType::U32, {a, bval, cval}, flags));
                    break;
                }
                case Rdna2Opcode::V_AND_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_OR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitOr, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_XOR_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BitXor, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_LSHLREV_B32: {
                    // AMD ISA: vdst = vsrc1 << (src0 & 31)
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shl, IrType::U32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_LSHRREV_B32: {
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::U32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_ASHRREV_I32: {
                    IrValueId shift = eval_operand(inst.src[0], false);
                    IrValueId val = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::Shr, IrType::I32, {val, shift}));
                    break;
                }
                case Rdna2Opcode::V_ADD_NC_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_SUB_NC_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::ISub, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CNDMASK_B32: {
                    // AMD ISA: D = cond ? src1 : src0
                    IrValueId cond_raw = (inst.src_count >= 3)
                                             ? eval_operand(inst.src[2], false)
                                             : state.vcc;
                    IrValueId cond = ensure_bool(cond_raw);
                    const bool s0_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    const bool s1_flt = (inst.src[1].kind == Rdna2OperandKind::InlineFloat);
                    IrValueId v0 = eval_operand(inst.src[0], s0_flt);
                    IrValueId v1 = eval_operand(inst.src[1], s1_flt);
                    IrType sel_type = (get_type(v1) == IrType::F32 || get_type(v0) == IrType::F32)
                                          ? IrType::F32
                                          : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::Select, sel_type, {cond, v1, v0}));
                    break;
                }

                // ----- VOP1 -----
                case Rdna2Opcode::V_NOP:
                    break;
                case Rdna2Opcode::V_MOV_B32: {
                    const bool is_flt = (inst.src[0].kind == Rdna2OperandKind::InlineFloat);
                    write_dst(inst.dst, eval_operand(inst.src[0], is_flt));
                    break;
                }
                case Rdna2Opcode::V_READFIRSTLANE_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    const IrType t = (get_type(val) == IrType::F32) ? IrType::F32 : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::WaveReadFirstLane, t, {val}));
                    break;
                }
                case Rdna2Opcode::V_CVT_F32_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::I2F, IrType::F32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::V_CVT_I32_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::F2I, IrType::I32, {eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_CVT_F32_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::U2F, IrType::F32, {eval_operand(inst.src[0], false)}));
                    break;
                case Rdna2Opcode::V_CVT_U32_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::F2U, IrType::U32, {eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_RCP_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FRcp, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_RSQ_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FRsq, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_SQRT_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FSqrt, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_FLOOR_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FFloor, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;
                case Rdna2Opcode::V_FRACT_F32:
                    write_dst(inst.dst, apply_clamp(emit_in_bb(IrOp::FFract, IrType::F32, {eval_operand(inst.src[0], true)}), inst.clamp));
                    break;

                // ----- VOPC -----
                case Rdna2Opcode::V_CMP_LT_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_EQ_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_LE_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[1], true), eval_operand(inst.src[0], true)}));
                    break;
                case Rdna2Opcode::V_CMP_GT_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_LG_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpNe, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_GE_F32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], true), eval_operand(inst.src[1], true)}));
                    break;
                case Rdna2Opcode::V_CMP_LT_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpLt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_EQ_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpEq, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GT_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGt, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GE_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGe, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_LT_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpLtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_EQ_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpEqU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GT_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGtU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;
                case Rdna2Opcode::V_CMP_GE_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::CmpGeU, IrType::Bool, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)}));
                    break;

                // ----- VOP3 (native 3-op & 16-bit VOP3) -----
                case Rdna2Opcode::V_FMA_F32: {
                    IrValueId r = emit_in_bb(IrOp::FFma, IrType::F32, {
                        eval_operand(inst.src[0], true),
                        eval_operand(inst.src[1], true),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_MAD_F32: {
                    IrValueId r = emit_in_bb(IrOp::FMad, IrType::F32, {
                        eval_operand(inst.src[0], true),
                        eval_operand(inst.src[1], true),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_FMA_F16: {
                    const uint32_t neg_mask = (inst.src[0].neg ? 1u : 0u) | (inst.src[1].neg ? 2u : 0u) |
                                              (inst.src[2].neg ? 4u : 0u);
                    const uint32_t abs_mask = (inst.src[0].abs ? 1u : 0u) | (inst.src[1].abs ? 2u : 0u) |
                                              (inst.src[2].abs ? 4u : 0u);
                    const uint32_t flags = static_cast<uint32_t>(inst.vop3_op_sel & 7u) |
                                           (neg_mask << 8) | (abs_mask << 12) |
                                           (inst.clamp ? (1u << 16) : 0u);
                    write_dst(inst.dst, emit_in_bb(IrOp::FFma16, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }, flags));
                    break;
                }
                case Rdna2Opcode::V_ADD_NC_U16:
                case Rdna2Opcode::V_SUB_NC_U16:
                case Rdna2Opcode::V_MIN_U16:
                case Rdna2Opcode::V_MAX_U16:
                case Rdna2Opcode::V_MIN_I16:
                case Rdna2Opcode::V_MAX_I16:
                case Rdna2Opcode::V_MUL_LO_U16:
                case Rdna2Opcode::V_LSHLREV_B16:
                case Rdna2Opcode::V_LSHRREV_B16:
                case Rdna2Opcode::V_MAD_U16: {
                    // Structural 16-bit integer VOP3 lowering (classified by encoding/metadata, not _e64 string)
                    IrValueId c_mask16 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFu, 0.0f);
                    IrValueId c_sh16 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 16u, 0.0f);
                    auto read_u16 = [&](uint8_t src_idx) -> IrValueId {
                        IrValueId raw = eval_operand(inst.src[src_idx], false);
                        if (((inst.vop3_op_sel >> src_idx) & 1u) != 0u) {
                            raw = emit_in_bb(IrOp::Shr, IrType::U32, {raw, c_sh16});
                        }
                        return emit_in_bb(IrOp::BitAnd, IrType::U32, {raw, c_mask16});
                    };
                    IrValueId a16 = read_u16(0u);
                    IrValueId b16 = read_u16(1u);
                    IrValueId r16 = kInvalidIrValue;
                    if (inst.opcode == Rdna2Opcode::V_ADD_NC_U16) {
                        r16 = emit_in_bb(IrOp::IAdd, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_SUB_NC_U16) {
                        r16 = emit_in_bb(IrOp::ISub, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MUL_LO_U16) {
                        r16 = emit_in_bb(IrOp::IMul, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MAD_U16) {
                        IrValueId c16 = read_u16(2u);
                        IrValueId prod = emit_in_bb(IrOp::IMul, IrType::U32, {a16, b16});
                        r16 = emit_in_bb(IrOp::IAdd, IrType::U32, {prod, c16});
                    } else if (inst.opcode == Rdna2Opcode::V_MIN_U16) {
                        r16 = emit_in_bb(IrOp::IMin, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MAX_U16) {
                        r16 = emit_in_bb(IrOp::IMax, IrType::U32, {a16, b16});
                    } else if (inst.opcode == Rdna2Opcode::V_MIN_I16 || inst.opcode == Rdna2Opcode::V_MAX_I16) {
                        IrValueId c_zero = get_zero_u32();
                        IrValueId sa = emit_in_bb(IrOp::BfeI32, IrType::I32, {a16, c_zero, c_sh16});
                        IrValueId sb = emit_in_bb(IrOp::BfeI32, IrType::I32, {b16, c_zero, c_sh16});
                        r16 = emit_in_bb((inst.opcode == Rdna2Opcode::V_MIN_I16) ? IrOp::IMin : IrOp::IMax,
                                         IrType::I32, {sa, sb});
                    } else {
                        IrValueId c_mask4 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 15u, 0.0f);
                        IrValueId sh = emit_in_bb(IrOp::BitAnd, IrType::U32, {a16, c_mask4});
                        r16 = emit_in_bb((inst.opcode == Rdna2Opcode::V_LSHLREV_B16) ? IrOp::Shl : IrOp::Shr,
                                         IrType::U32, {b16, sh});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::BitAnd, IrType::U32, {r16, c_mask16}));
                    break;
                }
                case Rdna2Opcode::V_MUL_LO_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::IMul, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                    }));
                    break;
                case Rdna2Opcode::V_MBCNT_LO_U32_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId mask = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccLo) {
                        mask = get_ballot_pair(ensure_bool(state.vcc)).first;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecLo) {
                        mask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else {
                        mask = eval_operand(inst.src[0], false);
                    }
                    IrValueId init = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::WavePrefixCount64, IrType::U32, {mask, init}, 0u));
                    break;
                }
                case Rdna2Opcode::V_MBCNT_HI_U32_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId mask = kInvalidIrValue;
                    if (inst.src[0].kind == Rdna2OperandKind::VccHi || inst.src[0].kind == Rdna2OperandKind::VccLo) {
                        mask = get_ballot_pair(ensure_bool(state.vcc)).second;
                    } else if (inst.src[0].kind == Rdna2OperandKind::ExecHi || inst.src[0].kind == Rdna2OperandKind::ExecLo) {
                        mask = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0xFFFFFFFFu, 0.0f);
                    } else {
                        mask = eval_operand(inst.src[0], false);
                    }
                    IrValueId init = eval_operand(inst.src[1], false);
                    write_dst(inst.dst, emit_in_bb(IrOp::WavePrefixCount64, IrType::U32, {mask, init}, 1u));
                    break;
                }
                case Rdna2Opcode::V_READLANE_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    IrValueId lane = eval_operand(inst.src[1], false);
                    const IrType t = (get_type(val) == IrType::F32) ? IrType::F32 : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::WaveReadLaneAt, t, {val, lane}));
                    break;
                }
                case Rdna2Opcode::V_ADD3_U32: {
                    IrValueId ab = emit_in_bb(IrOp::IAdd, IrType::U32, {eval_operand(inst.src[0], false), eval_operand(inst.src[1], false)});
                    write_dst(inst.dst, emit_in_bb(IrOp::IAdd, IrType::U32, {ab, eval_operand(inst.src[2], false)}));
                    break;
                }
                case Rdna2Opcode::V_DOT2_F32_F16: {
                    IrValueId r = emit_in_bb(IrOp::FDot2, IrType::F32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], true),
                    });
                    write_dst(inst.dst, apply_clamp(r, inst.clamp));
                    break;
                }
                case Rdna2Opcode::V_BFE_U32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BfeU32, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_BFE_I32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BfeI32, IrType::I32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_BFI_B32:
                    write_dst(inst.dst, emit_in_bb(IrOp::BfiB32, IrType::U32, {
                        eval_operand(inst.src[0], false),
                        eval_operand(inst.src[1], false),
                        eval_operand(inst.src[2], false),
                    }));
                    break;
                case Rdna2Opcode::V_PERMLANE16_B32:
                case Rdna2Opcode::V_PERMLANEX16_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    IrValueId s1 = eval_operand(inst.src[1], false);
                    IrValueId s2 = eval_operand(inst.src[2], false);
                    const uint32_t cross = (inst.opcode == Rdna2Opcode::V_PERMLANEX16_B32) ? 1u : 0u;
                    const IrType t = (get_type(val) == IrType::F32) ? IrType::F32 : IrType::U32;
                    write_dst(inst.dst, emit_in_bb(IrOp::WavePermLane16, t, {val, s1, s2}, cross));
                    break;
                }

                // ----- VOP3P (packed 2x16 f16 / u16 / b16) -----
                case Rdna2Opcode::V_PK_ADD_F16:
                case Rdna2Opcode::V_PK_MUL_F16:
                case Rdna2Opcode::V_PK_FMA_F16:
                case Rdna2Opcode::V_PK_ADD_U16:
                case Rdna2Opcode::V_PK_LSHLREV_B16:
                case Rdna2Opcode::V_PK_LSHRREV_B16: {
                    const uint32_t pk_flags = PackVop3pIrFlags(
                        inst.vop3p_op_sel, inst.vop3p_op_sel_hi,
                        inst.vop3p_neg_lo, inst.vop3p_neg_hi, inst.clamp);
                    IrValueId s0 = eval_operand(inst.src[0], false);
                    IrValueId s1 = eval_operand(inst.src[1], false);
                    if (inst.opcode == Rdna2Opcode::V_PK_FMA_F16) {
                        IrValueId s2 = eval_operand(inst.src[2], false);
                        write_dst(inst.dst, emit_in_bb(IrOp::PkFmaF16, IrType::U32, {s0, s1, s2}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_ADD_F16) {
                        write_dst(inst.dst, emit_in_bb(IrOp::PkAddF16, IrType::U32, {s0, s1}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_MUL_F16) {
                        write_dst(inst.dst, emit_in_bb(IrOp::PkMulF16, IrType::U32, {s0, s1}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_ADD_U16) {
                        write_dst(inst.dst, emit_in_bb(IrOp::PkAddU16, IrType::U32, {s0, s1}, pk_flags));
                    } else if (inst.opcode == Rdna2Opcode::V_PK_LSHLREV_B16) {
                        // REV: src1 is value, src0 is shift amount
                        write_dst(inst.dst, emit_in_bb(IrOp::PkShl16, IrType::U32, {s1, s0}, pk_flags));
                    } else {
                        // REV: src1 is value, src0 is shift amount
                        write_dst(inst.dst, emit_in_bb(IrOp::PkShr16, IrType::U32, {s1, s0}, pk_flags));
                    }
                    break;
                }

                // ----- SMEM -----
                case Rdna2Opcode::S_LOAD_DWORD:
                case Rdna2Opcode::S_BUFFER_LOAD_DWORD: {
                    const uint32_t byte_off = inst.mem_offset >= 0 ? static_cast<uint32_t>(inst.mem_offset) : 0u;
                    const uint32_t slot = byte_off / 4u;
                    if (slot < 16u && (inst.soffset == 0x7Du || inst.soffset == 0x7Cu || inst.soffset == 128u)) {
                        write_dst(inst.dst, get_root_const(slot));
                    } else {
                        out_module.uses_uav = true;
                        out_module.uses_uav0 = true;
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, byte_off, 0.0f);
                        if (inst.soffset < 106u) {
                            off = emit_in_bb(IrOp::IAdd, IrType::U32, {off, read_sgpr(inst.soffset)});
                        }
                        write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {off}, 0u));
                    }
                    break;
                }

                // ----- MUBUF -----
                case Rdna2Opcode::BUFFER_LOAD_DWORD:
                case Rdna2Opcode::BUFFER_STORE_DWORD: {
                    const uint32_t uav_slot = (inst.srsrc == 0u) ? 0u : 1u;
                    out_module.uses_uav = true;
                    if (uav_slot == 0u) {
                        out_module.uses_uav0 = true;
                    } else {
                        out_module.uses_uav1 = true;
                    }
                    IrValueId addr = kInvalidIrValue;
                    if (inst.offen) {
                        addr = eval_operand(inst.src[1], false);
                    } else if (inst.idxen) {
                        IrValueId idx = eval_operand(inst.src[1], false);
                        IrValueId sh = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 2u, 0.0f);
                        addr = emit_in_bb(IrOp::Shl, IrType::U32, {idx, sh});
                    } else {
                        addr = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 0u, 0.0f);
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    if (inst.soffset != 128u && inst.soffset != 125u) {
                        Rdna2Operand sop = DecodeRdna2SourceOperand(inst.soffset);
                        IrValueId soff_v = eval_operand(sop, false);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, soff_v});
                    }
                    if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                        write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {addr}, uav_slot));
                    } else {
                        IrValueId data = eval_operand(inst.src[0], false);
                        emit_in_bb(IrOp::StoreUav, IrType::Void, {addr, data}, uav_slot);
                    }
                    break;
                }

                // ----- MIMG -----
                case Rdna2Opcode::IMAGE_LOAD: {
                    out_module.uses_texture_srv = true;
                    out_module.uses_texture_load = true;
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    uint8_t out_reg = vdata_base;
                    for (uint32_t ch = 0; ch < 4u; ++ch) {
                        if ((dmask & (1u << ch)) != 0u) {
                            state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageLoad, IrType::F32, {x, y}, ch);
                        }
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_LOAD_MIP: {
                    const uint8_t dim = inst.mimg_dim;
                    if (dim == 2u) {
                        out_module.uses_texture3d_srv = true;
                    } else if (dim == 3u) {
                        out_module.uses_texture_cube_srv = true;
                    } else {
                        out_module.uses_texture_srv = true;
                        out_module.uses_texture2d_mip_srv = true;
                    }
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    IrValueId z = get_zero_u32();
                    IrValueId mip = get_zero_u32();
                    if (dim == 2u || dim == 3u) {
                        z = read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), false);
                        mip = read_vgpr(static_cast<uint8_t>(vaddr_base + 3u), false);
                    } else {
                        mip = read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), false);
                    }
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    uint8_t out_reg = vdata_base;
                    for (uint32_t ch = 0; ch < 4u; ++ch) {
                        if ((dmask & (1u << ch)) != 0u) {
                            const uint32_t imm = (ch & 3u) | (static_cast<uint32_t>(dim & 7u) << 4);
                            state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageLoadMip, IrType::F32, {x, y, z, mip}, imm);
                        }
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP: {
                    const uint8_t dim = (inst.mimg_dim == 2u || inst.mimg_dim == 3u) ? inst.mimg_dim : 1u;
                    out_module.uses_texture_u32_uav = true;
                    out_module.texture_u32_uav_dim = dim;
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[1].reg_index);
                    // RDNA2 ISA: vdata[0] = src (new value), vdata[1] = cmp (compare value)
                    IrValueId src_val = read_vgpr(vdata_base, false);
                    IrValueId cmp_val = read_vgpr(static_cast<uint8_t>(vdata_base + 1u), false);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    IrValueId z = (dim == 2u || dim == 3u)
                                      ? read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), false)
                                      : get_zero_u32();
                    IrValueId prev = emit_in_bb(IrOp::ImageAtomicCmpSwap, IrType::U32,
                                                {x, y, z, cmp_val, src_val}, dim);
                    if (inst.glc) {
                        state.vgpr[vdata_base] = prev;
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY: {
                    out_module.uses_uav = true;
                    out_module.uses_uav0 = true;
                    out_module.uses_bvh_ray = true;
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    IrValueId node_ptr = read_vgpr(vaddr_base, false);
                    IrValueId tmax = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), true);
                    IrValueId ox = read_vgpr(static_cast<uint8_t>(vaddr_base + 2u), true);
                    IrValueId oy = read_vgpr(static_cast<uint8_t>(vaddr_base + 3u), true);
                    IrValueId oz = read_vgpr(static_cast<uint8_t>(vaddr_base + 4u), true);
                    IrValueId dx = read_vgpr(static_cast<uint8_t>(vaddr_base + 5u), true);
                    IrValueId dy = read_vgpr(static_cast<uint8_t>(vaddr_base + 6u), true);
                    IrValueId dz = read_vgpr(static_cast<uint8_t>(vaddr_base + 7u), true);
                    std::vector<IrValueId> ops = {node_ptr, tmax, ox, oy, oz, dx, dy, dz};
                    state.vgpr[vdata_base + 0u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::F32, ops, 0u);
                    state.vgpr[vdata_base + 1u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::F32, ops, 1u);
                    state.vgpr[vdata_base + 2u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::F32, ops, 2u);
                    state.vgpr[vdata_base + 3u] = emit_in_bb(IrOp::BvhIntersectRay, IrType::U32, ops, 3u);
                    break;
                }
                case Rdna2Opcode::IMAGE_SAMPLE:
                case Rdna2Opcode::IMAGE_SAMPLE_LZ: {
                    out_module.uses_texture_srv = true;
                    out_module.uses_texture_sample = true;
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.dst.reg_index);
                    IrValueId u = read_vgpr(vaddr_base, true);
                    IrValueId v = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), true);
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    uint8_t out_reg = vdata_base;
                    if (out_module.stage == ShaderStage::Pixel && inst.opcode == Rdna2Opcode::IMAGE_SAMPLE) {
                        // Pixel shader implicit-LOD sample: use helper-lane-aware affine quad derivatives
                        out_module.uses_pixel_quad_derivatives = true;
                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                            if ((dmask & (1u << ch)) != 0u) {
                                state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageSampleImplicit, IrType::F32, {u, v}, ch);
                            }
                        }
                    } else {
                        IrValueId lod = get_zero_f32();
                        if (inst.opcode == Rdna2Opcode::IMAGE_SAMPLE &&
                            vaddr_base + 2u < 256u && state.vgpr[vaddr_base + 2u] != kInvalidIrValue) {
                            lod = state.vgpr[vaddr_base + 2u];
                        }
                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                            if ((dmask & (1u << ch)) != 0u) {
                                state.vgpr[out_reg++] = emit_in_bb(IrOp::ImageSample, IrType::F32, {u, v, lod}, ch);
                            }
                        }
                    }
                    break;
                }
                case Rdna2Opcode::IMAGE_STORE: {
                    out_module.uses_texture_uav = true;
                    const uint8_t vdata_base = static_cast<uint8_t>(inst.src[0].reg_index);
                    const uint8_t vaddr_base = static_cast<uint8_t>(inst.src[1].reg_index);
                    IrValueId x = read_vgpr(vaddr_base, false);
                    IrValueId y = read_vgpr(static_cast<uint8_t>(vaddr_base + 1u), false);
                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                    IrValueId rgba[4]{};
                    uint8_t in_reg = vdata_base;
                    for (uint32_t ch = 0; ch < 4u; ++ch) {
                        if ((dmask & (1u << ch)) != 0u) {
                            rgba[ch] = read_vgpr(in_reg++, true);
                        } else if (ch == 3u) {
                            rgba[ch] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
                        } else {
                            rgba[ch] = get_zero_f32();
                        }
                    }
                    emit_in_bb(IrOp::ImageStore, IrType::Void, {x, y, rgba[0], rgba[1], rgba[2], rgba[3]});
                    break;
                }

                // ----- DS (LDS Shared Memory & Swizzle) -----
                case Rdna2Opcode::DS_WRITE_B32: {
                    out_module.uses_lds = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    emit_in_bb(IrOp::LdsStore, IrType::Void, {addr, data});
                    break;
                }
                case Rdna2Opcode::DS_READ_B32: {
                    out_module.uses_lds = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::LdsLoad, IrType::U32, {addr}));
                    break;
                }
                case Rdna2Opcode::DS_ADD_U32:
                case Rdna2Opcode::DS_ADD_RTN_U32: {
                    out_module.uses_lds = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    IrValueId prev = emit_in_bb(IrOp::LdsAtomicAdd, IrType::U32, {addr, data});
                    if (inst.opcode == Rdna2Opcode::DS_ADD_RTN_U32) {
                        write_dst(inst.dst, prev);
                    }
                    break;
                }
                case Rdna2Opcode::DS_SWIZZLE_B32: {
                    out_module.uses_wave64_emulation = true;
                    IrValueId val = eval_operand(inst.src[0], false);
                    IrValueId lane = emit_in_bb(IrOp::WaveLaneId, IrType::U32);
                    const uint32_t swiz = static_cast<uint32_t>(inst.mem_offset) & 0xFFFFu;
                    IrValueId target_lane = lane;
                    if ((swiz & 0x8000u) != 0u) {
                        const uint32_t and_m = (swiz & 0x1Fu) | 0x20u;
                        const uint32_t or_m = (swiz >> 5) & 0x1Fu;
                        const uint32_t xor_m = (swiz >> 10) & 0x1Fu;
                        IrValueId c_and = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, and_m, 0.0f);
                        IrValueId c_or = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, or_m, 0.0f);
                        IrValueId c_xor = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, xor_m, 0.0f);
                        IrValueId t1 = emit_in_bb(IrOp::BitAnd, IrType::U32, {lane, c_and});
                        IrValueId t2 = emit_in_bb(IrOp::BitOr, IrType::U32, {t1, c_or});
                        target_lane = emit_in_bb(IrOp::BitXor, IrType::U32, {t2, c_xor});
                    } else {
                        IrValueId c_3 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 3u, 0.0f);
                        IrValueId c_inv3 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, ~3u, 0.0f);
                        IrValueId c_1 = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, 1u, 0.0f);
                        IrValueId c_swiz = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, swiz, 0.0f);
                        IrValueId quad_base = emit_in_bb(IrOp::BitAnd, IrType::U32, {lane, c_inv3});
                        IrValueId q_lane = emit_in_bb(IrOp::BitAnd, IrType::U32, {lane, c_3});
                        IrValueId shift = emit_in_bb(IrOp::Shl, IrType::U32, {q_lane, c_1});
                        IrValueId shifted = emit_in_bb(IrOp::Shr, IrType::U32, {c_swiz, shift});
                        IrValueId sel = emit_in_bb(IrOp::BitAnd, IrType::U32, {shifted, c_3});
                        target_lane = emit_in_bb(IrOp::BitOr, IrType::U32, {quad_base, sel});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::WaveReadLaneAt, IrType::U32, {val, target_lane}));
                    break;
                }

                // ----- GLOBAL / FLAT -----
                case Rdna2Opcode::GLOBAL_LOAD_DWORD:
                case Rdna2Opcode::FLAT_LOAD_DWORD: {
                    out_module.uses_uav = true;
                    out_module.uses_uav0 = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::LoadUav, IrType::U32, {addr}, 0u));
                    break;
                }
                case Rdna2Opcode::GLOBAL_STORE_DWORD:
                case Rdna2Opcode::FLAT_STORE_DWORD: {
                    out_module.uses_uav = true;
                    out_module.uses_uav0 = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    emit_in_bb(IrOp::StoreUav, IrType::Void, {addr, data}, 0u);
                    break;
                }
                case Rdna2Opcode::SCRATCH_LOAD_DWORD: {
                    out_module.uses_scratch = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.saddr != 0x7Fu && inst.saddr < 106u) {
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, read_sgpr(inst.saddr)});
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    write_dst(inst.dst, emit_in_bb(IrOp::ScratchLoad, IrType::U32, {addr}));
                    break;
                }
                case Rdna2Opcode::SCRATCH_STORE_DWORD: {
                    out_module.uses_scratch = true;
                    IrValueId addr = eval_operand(inst.src[0], false);
                    if (inst.saddr != 0x7Fu && inst.saddr < 106u) {
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, read_sgpr(inst.saddr)});
                    }
                    if (inst.mem_offset != 0) {
                        IrValueId off = emit_in_bb(IrOp::ConstU32, IrType::U32, {}, static_cast<uint32_t>(inst.mem_offset), 0.0f);
                        addr = emit_in_bb(IrOp::IAdd, IrType::U32, {addr, off});
                    }
                    IrValueId data = eval_operand(inst.src[1], false);
                    emit_in_bb(IrOp::ScratchStore, IrType::Void, {addr, data});
                    break;
                }

                // ----- EXP -----
                case Rdna2Opcode::EXP: {
                    if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Prim)) {
                        out_module.exports_prim = true;
                        if (inst.exp_en == 0x1u) {
                            IrValueId packed = eval_operand(inst.src[0], false);
                            emit_in_bb(IrOp::ExportMeshPrim, IrType::Void, {packed}, 1u);
                        } else {
                            IrValueId i0 = eval_operand(inst.src[0], false);
                            IrValueId i1 = eval_operand(inst.src[1], false);
                            IrValueId i2 = eval_operand(inst.src[2], false);
                            emit_in_bb(IrOp::ExportMeshPrim, IrType::Void, {i0, i1, i2}, 0u);
                        }
                        break;
                    }
                    std::vector<IrValueId> ch(4);
                    for (uint8_t c = 0; c < 4; ++c) {
                        if ((inst.exp_en & (1u << c)) != 0u) {
                            ch[c] = eval_operand(inst.src[c], true);
                        } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && c == 3u) {
                            ch[c] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, FloatToBits(1.0f), 1.0f);
                        } else {
                            ch[c] = emit_in_bb(IrOp::ConstF32, IrType::F32, {}, 0u, 0.0f);
                        }
                    }
                    IrInstruction ex{};
                    ex.op = IrOp::Export;
                    ex.type = IrType::Void;
                    ex.operands = std::move(ch);
                    ex.export_target = inst.exp_target;
                    ex.export_mask = inst.exp_en;
                    bb.instructions.push_back(std::move(ex));

                    if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0)) {
                        out_module.exports_pos0 = true;
                    } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0)) {
                        out_module.exports_mrt0 = true;
                    } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0)) {
                        out_module.exports_param0 = true;
                    }
                    break;
                }

                default:
                    return Status::Error(
                        StatusCode::NotImplemented,
                        std::string("Unhandled RDNA2 opcode in IR builder: ") + Rdna2OpcodeName(inst.opcode));
            }
        }

        if (!terminated) {
            if (b + 1 < leaders.size()) {
                IrInstruction br{};
                br.op = IrOp::Branch;
                br.type = IrType::Void;
                br.true_block = static_cast<uint32_t>(b + 1);
                bb.instructions.push_back(std::move(br));
            } else {
                emit_in_bb(IrOp::Return, IrType::Void);
            }
        }

        exit_states[b] = state;
        block_processed[b] = true;
    }

    // Prepend hoisted entry preamble into block 0.
    if (!entry_preamble.empty()) {
        auto& bb0 = out_module.blocks[0].instructions;
        bb0.insert(bb0.begin(), std::make_move_iterator(entry_preamble.begin()),
                   std::make_move_iterator(entry_preamble.end()));
    }

    uint32_t total_ops = 0;
    for (const auto& bb : out_module.blocks) {
        total_ops += static_cast<uint32_t>(bb.instructions.size());
    }
    out_module.opt_stats.initial_ops = total_ops;
    out_module.opt_stats.final_ops = total_ops;

    return Status::Ok();
}

// ---------------------------------------------------------------------------
// Constant Folding + Dead-Code Elimination Passes
// ---------------------------------------------------------------------------
Status OptimizeRdna2Ir(Rdna2IrModule& module, Rdna2IrOptStats* out_stats) {
    Rdna2IrOptStats stats{};
    for (const auto& bb : module.blocks) {
        stats.initial_ops += static_cast<uint32_t>(bb.instructions.size());
    }

    // Build value definition lookup table.
    std::unordered_map<IrValueId, IrInstruction*> defs;
    auto rebuild_defs = [&]() {
        defs.clear();
        for (auto& bb : module.blocks) {
            for (auto& inst : bb.instructions) {
                if (inst.id != kInvalidIrValue) {
                    defs[inst.id] = &inst;
                }
            }
        }
    };

    auto get_const_u32 = [&](IrValueId id, uint32_t& out_v) -> bool {
        auto it = defs.find(id);
        if (it == defs.end()) return false;
        const auto* d = it->second;
        if (d->op == IrOp::ConstU32) {
            out_v = d->imm_u32;
            return true;
        }
        if (d->op == IrOp::ConstF32) {
            out_v = FloatToBits(d->imm_f32);
            return true;
        }
        return false;
    };

    auto get_const_f32 = [&](IrValueId id, float& out_v) -> bool {
        auto it = defs.find(id);
        if (it == defs.end()) return false;
        const auto* d = it->second;
        if (d->op == IrOp::ConstF32) {
            out_v = d->imm_f32;
            return true;
        }
        if (d->op == IrOp::ConstU32) {
            out_v = BitsToFloat(d->imm_u32);
            return true;
        }
        return false;
    };

    // Pass 1: Iterative Constant Folding
    for (int iter = 0; iter < 16; ++iter) {
        rebuild_defs();
        bool changed = false;

        for (auto& bb : module.blocks) {
            for (auto& inst : bb.instructions) {
                if (inst.id == kInvalidIrValue || IrOpHasSideEffects(inst.op)) continue;
                if (inst.op == IrOp::ConstU32 || inst.op == IrOp::ConstF32 ||
                    inst.op == IrOp::ThreadIdX || inst.op == IrOp::StageInput ||
                    inst.op == IrOp::LoadRootConst || inst.op == IrOp::LoadUav ||
                    inst.op == IrOp::ImageSample || inst.op == IrOp::ImageSampleImplicit ||
                    inst.op == IrOp::ImageLoad ||
                    inst.op == IrOp::ImageLoadMip || inst.op == IrOp::ImageAtomicCmpSwap ||
                    inst.op == IrOp::BvhIntersectRay ||
                    inst.op == IrOp::LdsLoad || inst.op == IrOp::ScratchLoad ||
                    inst.op == IrOp::WaveLaneId ||
                    inst.op == IrOp::WaveReadFirstLane || inst.op == IrOp::WaveReadLaneAt ||
                    inst.op == IrOp::WaveBallotLo || inst.op == IrOp::WaveBallotHi ||
                    inst.op == IrOp::WaveActiveCountBits64 || inst.op == IrOp::WavePrefixCount64 ||
                    inst.op == IrOp::WavePermLane16) {
                    continue;
                }

                auto fold_to_f32 = [&](float v) {
                    inst.op = IrOp::ConstF32;
                    inst.type = IrType::F32;
                    inst.operands.clear();
                    inst.phi_incoming.clear();
                    inst.imm_f32 = v;
                    inst.imm_u32 = FloatToBits(v);
                    ++stats.folded_constants;
                    changed = true;
                };

                auto fold_to_u32 = [&](uint32_t v, IrType t) {
                    inst.op = IrOp::ConstU32;
                    inst.type = t;
                    inst.operands.clear();
                    inst.phi_incoming.clear();
                    inst.imm_u32 = v;
                    inst.imm_f32 = BitsToFloat(v);
                    ++stats.folded_constants;
                    changed = true;
                };

                float fa = 0.0f, fb = 0.0f, fc = 0.0f;
                uint32_t ua = 0, ub = 0;

                switch (inst.op) {
                    case IrOp::FAdd:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(fa + fb);
                        break;
                    case IrOp::FSub:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(fa - fb);
                        break;
                    case IrOp::FMul:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(fa * fb);
                        break;
                    case IrOp::FFma:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb) &&
                            get_const_f32(inst.operands[2], fc)) {
                            fold_to_f32(std::fmaf(fa, fb, fc));
                        }
                        break;
                    case IrOp::FMad:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb) &&
                            get_const_f32(inst.operands[2], fc)) {
                            fold_to_f32(EvalMadF32(fa, fb, fc));
                        }
                        break;
                    case IrOp::FFma16: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            const uint32_t f = inst.imm_u32;
                            auto read_f16 = [&](uint32_t u, int idx) -> float {
                                const bool hi = ((f >> idx) & 1u) != 0u;
                                float v = HalfBitsToFloat(SelectHalf16(u, hi));
                                if (((f >> (12 + idx)) & 1u) != 0u) v = std::fabs(v);
                                if (((f >> (8 + idx)) & 1u) != 0u) v = -v;
                                return v;
                            };
                            float r = std::fmaf(read_f16(ua, 0), read_f16(ub, 1), read_f16(uc, 2));
                            if (((f >> 16) & 1u) != 0u) r = std::fmin(std::fmax(r, 0.0f), 1.0f);
                            fold_to_u32(static_cast<uint32_t>(FloatToHalfBits(r)), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::FMin:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(std::fmin(fa, fb));
                        break;
                    case IrOp::FMax:
                        if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) fold_to_f32(std::fmax(fa, fb));
                        break;
                    case IrOp::FRcp:
                        if (get_const_f32(inst.operands[0], fa) && fa != 0.0f) fold_to_f32(1.0f / fa);
                        break;
                    case IrOp::FSqrt:
                        if (get_const_f32(inst.operands[0], fa) && fa >= 0.0f) fold_to_f32(std::sqrt(fa));
                        break;
                    case IrOp::FRsq:
                        if (get_const_f32(inst.operands[0], fa) && fa > 0.0f) fold_to_f32(1.0f / std::sqrt(fa));
                        break;
                    case IrOp::FFloor:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_f32(std::floor(fa));
                        break;
                    case IrOp::FFract:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_f32(fa - std::floor(fa));
                        break;
                    case IrOp::IAdd:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua + ub, inst.type);
                        break;
                    case IrOp::ISub:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua - ub, inst.type);
                        break;
                    case IrOp::IMul:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua * ub, inst.type);
                        break;
                    case IrOp::IMin:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(std::min(static_cast<int32_t>(ua), static_cast<int32_t>(ub)))
                                                   : std::min(ua, ub);
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::IMax:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(std::max(static_cast<int32_t>(ua), static_cast<int32_t>(ub)))
                                                   : std::max(ua, ub);
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::BitAnd:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua & ub, IrType::U32);
                        break;
                    case IrOp::BitOr:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua | ub, IrType::U32);
                        break;
                    case IrOp::BitXor:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua ^ ub, IrType::U32);
                        break;
                    case IrOp::BitNot:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_u32(~ua, IrType::U32);
                        break;
                    case IrOp::BitCount32:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_u32(static_cast<uint32_t>(std::popcount(ua)), IrType::U32);
                        break;
                    case IrOp::Shl:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) fold_to_u32(ua << (ub & 31u), IrType::U32);
                        break;
                    case IrOp::Shr:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t r = (inst.type == IrType::I32)
                                                   ? static_cast<uint32_t>(static_cast<int32_t>(ua) >> (ub & 31u))
                                                   : (ua >> (ub & 31u));
                            fold_to_u32(r, inst.type);
                        }
                        break;
                    case IrOp::BfeU32: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            const uint32_t off = ub & 31u;
                            const uint32_t wid = uc & 31u;
                            const uint32_t mask = (wid == 0u) ? 0u : ((1u << wid) - 1u);
                            fold_to_u32((ua >> off) & mask, IrType::U32);
                        }
                        break;
                    }
                    case IrOp::BfeI32: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            const uint32_t off = ub & 31u;
                            const uint32_t wid = uc & 31u;
                            int32_t res = 0;
                            if (wid == 0u) {
                                res = 0;
                            } else if (off + wid < 32u) {
                                res = static_cast<int32_t>(ua << (32u - off - wid)) >> (32u - wid);
                            } else {
                                res = static_cast<int32_t>(ua) >> off;
                            }
                            fold_to_u32(static_cast<uint32_t>(res), IrType::I32);
                        }
                        break;
                    }
                    case IrOp::BfiB32: {
                        uint32_t uc = 0;
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            get_const_u32(inst.operands[2], uc)) {
                            fold_to_u32((ua & ub) | (~ua & uc), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::PkAddU16:
                    case IrOp::PkShl16:
                    case IrOp::PkShr16: {
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const uint32_t f = inst.imm_u32;
                            const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                            const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                            // Note: for PkShl16/PkShr16 operands[0] is val (ISA src1), operands[1] is shift (ISA src0)
                            const bool is_shift = (inst.op == IrOp::PkShl16 || inst.op == IrOp::PkShr16);
                            const uint16_t a_lo = SelectHalf16(ua, ((op_sel >> (is_shift ? 1 : 0)) & 1u) != 0u);
                            const uint16_t a_hi = SelectHalf16(ua, ((op_sel_hi >> (is_shift ? 1 : 0)) & 1u) != 0u);
                            const uint16_t b_lo = SelectHalf16(ub, ((op_sel >> (is_shift ? 0 : 1)) & 1u) != 0u);
                            const uint16_t b_hi = SelectHalf16(ub, ((op_sel_hi >> (is_shift ? 0 : 1)) & 1u) != 0u);
                            uint16_t r_lo = 0, r_hi = 0;
                            if (inst.op == IrOp::PkAddU16) {
                                r_lo = static_cast<uint16_t>(a_lo + b_lo);
                                r_hi = static_cast<uint16_t>(a_hi + b_hi);
                            } else if (inst.op == IrOp::PkShl16) {
                                r_lo = static_cast<uint16_t>(a_lo << (b_lo & 15u));
                                r_hi = static_cast<uint16_t>(a_hi << (b_hi & 15u));
                            } else {
                                r_lo = static_cast<uint16_t>(a_lo >> (b_lo & 15u));
                                r_hi = static_cast<uint16_t>(a_hi >> (b_hi & 15u));
                            }
                            fold_to_u32(static_cast<uint32_t>(r_lo) | (static_cast<uint32_t>(r_hi) << 16), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::PkAddF16:
                    case IrOp::PkMulF16:
                    case IrOp::PkFmaF16: {
                        uint32_t uc = 0;
                        const bool has_c = (inst.op == IrOp::PkFmaF16);
                        if (get_const_u32(inst.operands[0], ua) &&
                            get_const_u32(inst.operands[1], ub) &&
                            (!has_c || get_const_u32(inst.operands[2], uc))) {
                            const uint32_t f = inst.imm_u32;
                            const uint8_t op_sel = static_cast<uint8_t>(f & 7u);
                            const uint8_t op_sel_hi = static_cast<uint8_t>((f >> 4) & 7u);
                            const uint8_t neg_lo = static_cast<uint8_t>((f >> 8) & 7u);
                            const uint8_t neg_hi = static_cast<uint8_t>((f >> 12) & 7u);
                            const bool clamp = ((f >> 16) & 1u) != 0u;
                            auto lane_f = [&](uint32_t src, int idx, bool hi_out) -> float {
                                const bool pick_hi = (((hi_out ? op_sel_hi : op_sel) >> idx) & 1u) != 0u;
                                const bool do_neg = (((hi_out ? neg_hi : neg_lo) >> idx) & 1u) != 0u;
                                float v = HalfBitsToFloat(SelectHalf16(src, pick_hi));
                                return do_neg ? -v : v;
                            };
                            auto eval_lane = [&](bool hi_out) -> uint16_t {
                                const float x = lane_f(ua, 0, hi_out);
                                const float y = lane_f(ub, 1, hi_out);
                                float r = 0.0f;
                                if (inst.op == IrOp::PkAddF16) r = x + y;
                                else if (inst.op == IrOp::PkMulF16) r = x * y;
                                else r = std::fma(x, y, lane_f(uc, 2, hi_out));
                                if (clamp) r = std::fmin(std::fmax(r, 0.0f), 1.0f);
                                return FloatToHalfBits(r);
                            };
                            const uint16_t r_lo = eval_lane(false);
                            const uint16_t r_hi = eval_lane(true);
                            fold_to_u32(static_cast<uint32_t>(r_lo) | (static_cast<uint32_t>(r_hi) << 16), IrType::U32);
                        }
                        break;
                    }
                    case IrOp::I2F:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_f32(static_cast<float>(static_cast<int32_t>(ua)));
                        break;
                    case IrOp::U2F:
                        if (get_const_u32(inst.operands[0], ua)) fold_to_f32(static_cast<float>(ua));
                        break;
                    case IrOp::F2I:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_u32(static_cast<uint32_t>(static_cast<int32_t>(fa)), IrType::I32);
                        break;
                    case IrOp::F2U:
                        if (get_const_f32(inst.operands[0], fa)) fold_to_u32(static_cast<uint32_t>(fa), IrType::U32);
                        break;
                    case IrOp::CmpEq:
                    case IrOp::CmpNe:
                    case IrOp::CmpLt:
                    case IrOp::CmpGt:
                    case IrOp::CmpGe: {
                        const auto* d0 = defs.count(inst.operands[0]) ? defs[inst.operands[0]] : nullptr;
                        const auto* d1 = defs.count(inst.operands[1]) ? defs[inst.operands[1]] : nullptr;
                        if (d0 && d1 && (d0->type == IrType::F32 || d1->type == IrType::F32)) {
                            if (get_const_f32(inst.operands[0], fa) && get_const_f32(inst.operands[1], fb)) {
                                bool r = false;
                                if (inst.op == IrOp::CmpEq) r = (fa == fb);
                                else if (inst.op == IrOp::CmpNe) r = (fa != fb);
                                else if (inst.op == IrOp::CmpLt) r = (fa < fb);
                                else if (inst.op == IrOp::CmpGt) r = (fa > fb);
                                else if (inst.op == IrOp::CmpGe) r = (fa >= fb);
                                fold_to_u32(r ? 1u : 0u, IrType::Bool);
                            }
                        } else if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            const int32_t sa = static_cast<int32_t>(ua);
                            const int32_t sb = static_cast<int32_t>(ub);
                            bool r = false;
                            if (inst.op == IrOp::CmpEq) r = (sa == sb);
                            else if (inst.op == IrOp::CmpNe) r = (sa != sb);
                            else if (inst.op == IrOp::CmpLt) r = (sa < sb);
                            else if (inst.op == IrOp::CmpGt) r = (sa > sb);
                            else if (inst.op == IrOp::CmpGe) r = (sa >= sb);
                            fold_to_u32(r ? 1u : 0u, IrType::Bool);
                        }
                        break;
                    }
                    case IrOp::CmpLtU:
                    case IrOp::CmpEqU:
                    case IrOp::CmpGtU:
                    case IrOp::CmpGeU:
                        if (get_const_u32(inst.operands[0], ua) && get_const_u32(inst.operands[1], ub)) {
                            bool r = false;
                            if (inst.op == IrOp::CmpLtU) r = (ua < ub);
                            else if (inst.op == IrOp::CmpEqU) r = (ua == ub);
                            else if (inst.op == IrOp::CmpGtU) r = (ua > ub);
                            else if (inst.op == IrOp::CmpGeU) r = (ua >= ub);
                            fold_to_u32(r ? 1u : 0u, IrType::Bool);
                        }
                        break;
                    case IrOp::Select:
                        if (get_const_u32(inst.operands[0], ua)) {
                            const IrValueId chosen = (ua != 0u) ? inst.operands[1] : inst.operands[2];
                            if (inst.type == IrType::F32 && get_const_f32(chosen, fa)) {
                                fold_to_f32(fa);
                            } else if (inst.type != IrType::F32 && get_const_u32(chosen, ub)) {
                                fold_to_u32(ub, inst.type);
                            }
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        if (!changed) break;
    }

    // Pass 2: Dead-Code Elimination (mark-and-sweep from side-effecting roots)
    rebuild_defs();
    std::unordered_set<IrValueId> live;
    std::vector<IrValueId> worklist;

    auto mark_use = [&](IrValueId id) {
        if (id != kInvalidIrValue && live.insert(id).second) {
            worklist.push_back(id);
        }
    };

    for (const auto& bb : module.blocks) {
        for (const auto& inst : bb.instructions) {
            if (IrOpHasSideEffects(inst.op)) {
                if (inst.id != kInvalidIrValue) {
                    live.insert(inst.id);
                }
                for (IrValueId op_id : inst.operands) {
                    mark_use(op_id);
                }
            }
        }
    }

    while (!worklist.empty()) {
        const IrValueId cur = worklist.back();
        worklist.pop_back();
        auto it = defs.find(cur);
        if (it == defs.end()) continue;
        const auto* d = it->second;
        for (IrValueId op_id : d->operands) {
            mark_use(op_id);
        }
        for (const auto& incoming : d->phi_incoming) {
            mark_use(incoming.second);
        }
    }

    for (auto& bb : module.blocks) {
        std::vector<IrInstruction> kept;
        kept.reserve(bb.instructions.size());
        for (auto& inst : bb.instructions) {
            if (IrOpHasSideEffects(inst.op) || (inst.id != kInvalidIrValue && live.count(inst.id) > 0)) {
                kept.push_back(std::move(inst));
            } else {
                ++stats.eliminated_ops;
            }
        }
        bb.instructions = std::move(kept);
    }

    for (const auto& bb : module.blocks) {
        stats.final_ops += static_cast<uint32_t>(bb.instructions.size());
    }

    module.opt_stats = stats;
    if (out_stats) {
        *out_stats = stats;
    }
    return Status::Ok();
}

std::string DumpRdna2Ir(const Rdna2IrModule& module) {
    std::ostringstream os;
    os << "; OpenScarlett RDNA2 SSA IR (blocks=" << module.blocks.size()
       << ", tg=" << module.threadgroup_x << "x" << module.threadgroup_y << "x" << module.threadgroup_z << ")\n";
    for (const auto& bb : module.blocks) {
        os << "bb" << bb.id << ":";
        if (!bb.predecessors.empty()) {
            os << "  ; preds=";
            for (size_t i = 0; i < bb.predecessors.size(); ++i) {
                if (i) os << ",";
                os << "bb" << bb.predecessors[i];
            }
        }
        os << "\n";
        for (const auto& inst : bb.instructions) {
            os << "  ";
            if (inst.id != kInvalidIrValue) {
                os << "%" << inst.id << " : " << IrTypeName(inst.type) << " = ";
            }
            os << IrOpName(inst.op);
            if (inst.op == IrOp::ConstU32) {
                os << " " << inst.imm_u32;
            } else if (inst.op == IrOp::ConstF32) {
                os << " " << inst.imm_f32;
            } else if (inst.op == IrOp::LoadRootConst || inst.op == IrOp::StageInput) {
                os << " " << inst.imm_u32;
            } else if (inst.op == IrOp::Phi) {
                for (size_t i = 0; i < inst.phi_incoming.size(); ++i) {
                    os << (i ? ", " : " ") << "[bb" << inst.phi_incoming[i].first << ": %" << inst.phi_incoming[i].second << "]";
                }
            } else if (inst.op == IrOp::Branch) {
                os << " bb" << inst.true_block;
            } else if (inst.op == IrOp::CondBranch) {
                os << " %" << inst.operands[0] << ", bb" << inst.true_block << ", bb" << inst.false_block;
            } else {
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    os << (i ? ", " : " ") << "%" << inst.operands[i];
                }
                if (inst.op == IrOp::Export) {
                    os << " (target=" << static_cast<uint32_t>(inst.export_target) << ")";
                }
            }
            os << "\n";
        }
    }
    return os.str();
}

}  // namespace openscarlett
