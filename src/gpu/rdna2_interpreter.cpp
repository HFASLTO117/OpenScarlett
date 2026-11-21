#include "openscarlett/gpu/rdna2_interpreter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace openscarlett {

namespace {

uint32_t FloatToBits(float f) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

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
            uint32_t e = 113u;
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

uint16_t PickHalf16(uint32_t packed, bool hi) noexcept {
    return static_cast<uint16_t>(hi ? ((packed >> 16) & 0xFFFFu) : (packed & 0xFFFFu));
}

uint32_t PopCount32(uint32_t v) noexcept {
    return static_cast<uint32_t>(std::popcount(v));
}

uint32_t PopCount64(uint64_t v) noexcept {
    return static_cast<uint32_t>(std::popcount(v));
}

bool IsBranchOp(Rdna2Opcode op) noexcept {
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

std::vector<size_t> ComputeInstructionIpdom(
    const std::vector<Rdna2Instruction>& instructions,
    const std::unordered_map<size_t, size_t>& branch_targets) {
    const size_t n = instructions.size();
    const size_t exit_node = n;
    std::vector<std::vector<size_t>> succs(n);
    for (size_t i = 0; i < n; ++i) {
        const auto op = instructions[i].opcode;
        if (op == Rdna2Opcode::S_ENDPGM) {
            succs[i].push_back(exit_node);
        } else if (op == Rdna2Opcode::S_BRANCH) {
            auto it = branch_targets.find(i);
            succs[i].push_back(it != branch_targets.end() ? it->second : exit_node);
        } else if (IsBranchOp(op)) {
            auto it = branch_targets.find(i);
            const size_t tgt = (it != branch_targets.end()) ? it->second : exit_node;
            succs[i].push_back(tgt);
            const size_t fall = (i + 1 < n) ? (i + 1) : exit_node;
            if (fall != tgt) {
                succs[i].push_back(fall);
            }
        } else {
            succs[i].push_back((i + 1 < n) ? (i + 1) : exit_node);
        }
    }

    std::vector<std::vector<uint8_t>> pdom(n + 1, std::vector<uint8_t>(n + 1, 1u));
    pdom[exit_node].assign(n + 1, 0u);
    pdom[exit_node][exit_node] = 1u;

    bool changed = true;
    for (size_t iter = 0; changed && iter < (n + 2) * 8; ++iter) {
        changed = false;
        for (size_t i = n; i-- > 0;) {
            std::vector<uint8_t> next_set(n + 1, 1u);
            for (size_t s : succs[i]) {
                for (size_t k = 0; k <= n; ++k) {
                    next_set[k] = static_cast<uint8_t>(next_set[k] & pdom[s][k]);
                }
            }
            next_set[i] = 1u;
            if (next_set != pdom[i]) {
                pdom[i] = std::move(next_set);
                changed = true;
            }
        }
    }

    std::vector<size_t> pdom_size(n + 1, 0);
    for (size_t i = 0; i <= n; ++i) {
        size_t c = 0;
        for (uint8_t b : pdom[i]) c += b;
        pdom_size[i] = c;
    }

    std::vector<size_t> ipdom(n, exit_node);
    for (size_t i = 0; i < n; ++i) {
        size_t best = exit_node;
        size_t best_size = 0;
        for (size_t d = 0; d <= n; ++d) {
            if (d == i || !pdom[i][d]) continue;
            if (pdom_size[d] > best_size) {
                best_size = pdom_size[d];
                best = d;
            }
        }
        ipdom[i] = best;
    }
    return ipdom;
}

uint32_t ReadBufferWord(const std::vector<uint32_t>& buf, uint32_t byte_addr) noexcept {
    const size_t idx = static_cast<size_t>(byte_addr >> 2);
    return (idx < buf.size()) ? buf[idx] : 0u;
}

void WriteBufferWord(std::vector<uint32_t>& buf, uint32_t byte_addr, uint32_t val) noexcept {
    const size_t idx = static_cast<size_t>(byte_addr >> 2);
    if (idx < buf.size()) {
        buf[idx] = val;
    }
}

struct BvhHitResult {
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t hit_id = 0xFFFFFFFFu;
};

BvhHitResult EvalBvhIntersectRayCpu(
    const std::vector<uint32_t>& uav0,
    uint32_t node_ptr,
    float tmax,
    float ox,
    float oy,
    float oz,
    float dx,
    float dy,
    float dz) noexcept {
    BvhHitResult miss{tmax, 0.0f, 0.0f, 0xFFFFFFFFu};
    const uint32_t hdr = ReadBufferWord(uav0, node_ptr);
    const uint32_t hit_id = hdr & 0x7FFFFFFFu;

    if ((hdr & 0x80000000u) == 0u) {
        // Triangle leaf node (Möller-Trumbore)
        const float v0x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 4u));
        const float v0y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 8u));
        const float v0z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 12u));
        const float v1x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 16u));
        const float v1y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 20u));
        const float v1z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 24u));
        const float v2x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 28u));
        const float v2y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 32u));
        const float v2z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 36u));

        const float e1x = v1x - v0x, e1y = v1y - v0y, e1z = v1z - v0z;
        const float e2x = v2x - v0x, e2y = v2y - v0y, e2z = v2z - v0z;
        const float px = dy * e2z - dz * e2y;
        const float py = dz * e2x - dx * e2z;
        const float pz = dx * e2y - dy * e2x;
        const float det = e1x * px + e1y * py + e1z * pz;
        if (std::fabs(det) > 1e-7f) {
            const float inv_det = 1.0f / det;
            const float tvx = ox - v0x, tvy = oy - v0y, tvz = oz - v0z;
            const float u = (tvx * px + tvy * py + tvz * pz) * inv_det;
            const float qx = tvy * e1z - tvz * e1y;
            const float qy = tvz * e1x - tvx * e1z;
            const float qz = tvx * e1y - tvy * e1x;
            const float v = (dx * qx + dy * qy + dz * qz) * inv_det;
            const float t = (e2x * qx + e2y * qy + e2z * qz) * inv_det;
            if (u >= 0.0f && v >= 0.0f && (u + v) <= 1.0f && t >= 0.0f && t <= tmax) {
                return BvhHitResult{t, u, v, hit_id};
            }
        }
        return miss;
    }

    // AABB Box node (Kay-Kajiya Slab)
    const float bmin_x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 4u));
    const float bmin_y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 8u));
    const float bmin_z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 12u));
    const float bmax_x = BitsToFloat(ReadBufferWord(uav0, node_ptr + 16u));
    const float bmax_y = BitsToFloat(ReadBufferWord(uav0, node_ptr + 20u));
    const float bmax_z = BitsToFloat(ReadBufferWord(uav0, node_ptr + 24u));

    float t_enter = 0.0f;
    float t_exit = tmax;
    auto slab = [&](float o, float d, float bmin, float bmax) {
        if (std::fabs(d) > 1e-12f) {
            const float inv_d = 1.0f / d;
            const float t0 = (bmin - o) * inv_d;
            const float t1 = (bmax - o) * inv_d;
            t_enter = std::fmax(t_enter, std::fmin(t0, t1));
            t_exit = std::fmin(t_exit, std::fmax(t0, t1));
        } else if (o < bmin || o > bmax) {
            t_enter = 1e30f;
            t_exit = -1e30f;
        }
    };
    slab(ox, dx, bmin_x, bmax_x);
    slab(oy, dy, bmin_y, bmax_y);
    slab(oz, dz, bmin_z, bmax_z);
    if (t_enter <= t_exit) {
        return BvhHitResult{t_enter, t_exit, 0.0f, hit_id};
    }
    return miss;
}

struct SimtFrame {
    size_t target_pc = 0;
    uint64_t exec_mask = 0;
    size_t reconv_pc = SIZE_MAX;
};

struct CpuWaveState {
    uint32_t lane_count = 32;
    uint32_t base_local_idx = 0;
    std::array<uint32_t, 106> sgpr{};
    std::vector<std::array<uint32_t, 256>> vgpr;
    std::vector<std::vector<uint32_t>> scratch;
    std::vector<uint32_t> global_thread_ids;

    uint64_t exec = 0;
    uint64_t vcc = 0;
    bool scc = false;
    uint32_t m0 = 0;

    size_t pc = 0;
    size_t cur_reconv_pc = SIZE_MAX;
    std::vector<SimtFrame> simt_stack;
    bool halted = false;
    bool at_barrier = false;
    uint32_t steps = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Rdna2CpuTexture Helpers
// ---------------------------------------------------------------------------
void Rdna2CpuTexture::AllocateRgba32f(
    uint8_t dimension,
    uint32_t width,
    uint32_t height,
    uint32_t depth_or_faces,
    uint32_t mip_levels) {
    dim = dimension;
    const uint32_t levels = mip_levels ? mip_levels : 1u;
    mips.clear();
    mips.resize(levels);
    uint32_t w = width ? width : 1u;
    uint32_t h = height ? height : 1u;
    uint32_t d = (dimension == 3u) ? 6u : (depth_or_faces ? depth_or_faces : 1u);
    for (uint32_t m = 0; m < levels; ++m) {
        mips[m].width = w;
        mips[m].height = h;
        mips[m].depth = d;
        mips[m].rgba32f.assign(static_cast<size_t>(w) * h * d * 4u, 0.0f);
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        if (dimension == 2u) {
            d = std::max(1u, d >> 1);
        }
    }
}

void Rdna2CpuTexture::AllocateR32u(
    uint8_t dimension,
    uint32_t width,
    uint32_t height,
    uint32_t depth_or_faces) {
    dim = dimension;
    mips.clear();
    mips.resize(1);
    mips[0].width = width ? width : 1u;
    mips[0].height = height ? height : 1u;
    mips[0].depth = (dimension == 3u) ? 6u : (depth_or_faces ? depth_or_faces : 1u);
    mips[0].r32u.assign(
        static_cast<size_t>(mips[0].width) * mips[0].height * mips[0].depth, 0u);
}

std::array<float, 4> Rdna2CpuTexture::LoadRgba32f(
    uint32_t x, uint32_t y, uint32_t z, uint32_t mip) const noexcept {
    if (mip >= mips.size()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const auto& lv = mips[mip];
    if (x >= lv.width || y >= lv.height || z >= lv.depth) return {0.0f, 0.0f, 0.0f, 0.0f};
    const size_t idx = (static_cast<size_t>(z) * lv.height + y) * lv.width + x;
    if (idx * 4u + 3u >= lv.rgba32f.size()) return {0.0f, 0.0f, 0.0f, 0.0f};
    return {lv.rgba32f[idx * 4u + 0u], lv.rgba32f[idx * 4u + 1u],
            lv.rgba32f[idx * 4u + 2u], lv.rgba32f[idx * 4u + 3u]};
}

std::array<float, 4> Rdna2CpuTexture::SampleNearest(float u, float v, float lod) const noexcept {
    if (mips.empty()) return {0.0f, 0.0f, 0.0f, 0.0f};
    const uint32_t mip = static_cast<uint32_t>(
        std::clamp(static_cast<int32_t>(std::floor(lod + 0.5f)),
                   0, static_cast<int32_t>(mips.size()) - 1));
    const auto& lv = mips[mip];
    if (lv.width == 0u || lv.height == 0u) return {0.0f, 0.0f, 0.0f, 0.0f};
    const float cu = std::clamp(u, 0.0f, 1.0f);
    const float cv = std::clamp(v, 0.0f, 1.0f);
    const int32_t ix = std::clamp(static_cast<int32_t>(std::floor(cu * static_cast<float>(lv.width))),
                                  0, static_cast<int32_t>(lv.width) - 1);
    const int32_t iy = std::clamp(static_cast<int32_t>(std::floor(cv * static_cast<float>(lv.height))),
                                  0, static_cast<int32_t>(lv.height) - 1);
    return LoadRgba32f(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy), 0u, mip);
}

void Rdna2CpuTexture::StoreRgba32f(
    uint32_t x, uint32_t y, uint32_t z, const std::array<float, 4>& rgba) noexcept {
    if (mips.empty()) return;
    auto& lv = mips[0];
    if (x >= lv.width || y >= lv.height || z >= lv.depth) return;
    const size_t idx = (static_cast<size_t>(z) * lv.height + y) * lv.width + x;
    if (idx * 4u + 3u >= lv.rgba32f.size()) return;
    lv.rgba32f[idx * 4u + 0u] = rgba[0];
    lv.rgba32f[idx * 4u + 1u] = rgba[1];
    lv.rgba32f[idx * 4u + 2u] = rgba[2];
    lv.rgba32f[idx * 4u + 3u] = rgba[3];
}

uint32_t Rdna2CpuTexture::AtomicCmpSwapR32u(
    uint32_t x, uint32_t y, uint32_t z, uint32_t cmp_val, uint32_t new_val) noexcept {
    if (mips.empty()) return 0u;
    auto& lv = mips[0];
    if (x >= lv.width || y >= lv.height || z >= lv.depth) return 0u;
    const size_t idx = (static_cast<size_t>(z) * lv.height + y) * lv.width + x;
    if (idx >= lv.r32u.size()) return 0u;
    const uint32_t old = lv.r32u[idx];
    if (old == cmp_val) {
        lv.r32u[idx] = new_val;
    }
    return old;
}

// ---------------------------------------------------------------------------
// Clean-Room CPU RDNA2 Wave32 / Wave64 Interpreter
// ---------------------------------------------------------------------------
Status ExecuteRdna2Cpu(
    const Xs2ShaderContainer& container,
    const std::vector<Rdna2Instruction>& instructions,
    Rdna2CpuExecState& state) {
    if (instructions.empty()) {
        return Status::Error(StatusCode::ParseError, "Cannot execute empty RDNA2 instruction stream");
    }

    const uint32_t wave_size = (container.wave_size == 64u) ? 64u : 32u;
    const uint32_t tg_x = container.threadgroup_x ? container.threadgroup_x : 1u;
    const uint32_t tg_y = container.threadgroup_y ? container.threadgroup_y : 1u;
    const uint32_t tg_z = container.threadgroup_z ? container.threadgroup_z : 1u;
    const uint32_t tg_threads = tg_x * tg_y * tg_z;
    const uint32_t disp_x = state.dispatch_x ? state.dispatch_x : 1u;
    const uint32_t disp_y = state.dispatch_y ? state.dispatch_y : 1u;
    const uint32_t disp_z = state.dispatch_z ? state.dispatch_z : 1u;
    const uint32_t total_groups = disp_x * disp_y * disp_z;

    // Map word_offset -> instruction index and resolve branch targets
    std::unordered_map<uint32_t, size_t> word_to_idx;
    word_to_idx.reserve(instructions.size());
    for (size_t i = 0; i < instructions.size(); ++i) {
        word_to_idx[instructions[i].word_offset] = i;
    }

    std::unordered_map<size_t, size_t> branch_targets;
    for (size_t i = 0; i < instructions.size(); ++i) {
        if (IsBranchOp(instructions[i].opcode)) {
            const int64_t tgt_word = static_cast<int64_t>(instructions[i].word_offset) + 1 +
                                     static_cast<int64_t>(instructions[i].simm16);
            if (tgt_word < 0) {
                return Status::Error(StatusCode::ParseError, "Interpreter branch target < 0");
            }
            auto it = word_to_idx.find(static_cast<uint32_t>(tgt_word));
            if (it == word_to_idx.end()) {
                return Status::Error(StatusCode::ParseError, "Interpreter branch target misaligned");
            }
            branch_targets[i] = it->second;
        }
    }
    const std::vector<size_t> ipdom = ComputeInstructionIpdom(instructions, branch_targets);

    const uint32_t lds_dwords = state.lds_size_dwords ? state.lds_size_dwords : 16384u;
    const uint32_t lds_mask = lds_dwords - 1u;
    const uint32_t scr_dwords = state.scratch_size_dwords ? state.scratch_size_dwords : 256u;
    const uint32_t scr_mask = scr_dwords - 1u;

    state.exports.assign(static_cast<size_t>(total_groups) * tg_threads, Rdna2CpuThreadExport{});
    state.total_instructions_executed = 0;
    state.barriers_synchronized = 0;
    state.waves_executed = 0;

    for (uint32_t gz = 0; gz < disp_z; ++gz) {
        for (uint32_t gy = 0; gy < disp_y; ++gy) {
            for (uint32_t gx = 0; gx < disp_x; ++gx) {
                const uint32_t group_linear = (gz * disp_y + gy) * disp_x + gx;
                std::vector<uint32_t> lds(lds_dwords, 0u);

                const uint32_t num_waves = (tg_threads + wave_size - 1u) / wave_size;
                std::vector<CpuWaveState> waves(num_waves);
                for (uint32_t w = 0; w < num_waves; ++w) {
                    auto& wv = waves[w];
                    wv.base_local_idx = w * wave_size;
                    wv.lane_count = std::min(wave_size, tg_threads - wv.base_local_idx);
                    wv.exec = (wv.lane_count == 64u)
                                  ? ~0ull
                                  : ((1ull << wv.lane_count) - 1ull);
                    wv.vgpr.assign(wv.lane_count, std::array<uint32_t, 256>{});
                    wv.scratch.assign(wv.lane_count, std::vector<uint32_t>(scr_dwords, 0u));
                    wv.global_thread_ids.resize(wv.lane_count, 0u);

                    for (size_t rc = 0; rc < std::min<size_t>(16u, state.root_constants.size()); ++rc) {
                        wv.sgpr[rc] = state.root_constants[rc];
                    }

                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                        const uint32_t local_idx = wv.base_local_idx + l;
                        const uint32_t lx = local_idx % tg_x;
                        const uint32_t global_linear = group_linear * tg_threads + local_idx;
                        wv.global_thread_ids[l] = global_linear;

                        if (container.stage == ShaderStage::Pixel) {
                            if (global_linear < state.stage_inputs.size()) {
                                const auto& in_p = state.stage_inputs[global_linear];
                                wv.vgpr[l][0] = FloatToBits(in_p[0]);
                                wv.vgpr[l][1] = FloatToBits(in_p[1]);
                                wv.vgpr[l][2] = FloatToBits(in_p[2]);
                                wv.vgpr[l][3] = FloatToBits(in_p[3]);
                            }
                        } else if (container.stage == ShaderStage::Mesh ||
                                   container.stage == ShaderStage::Amplification) {
                            wv.vgpr[l][0] = local_idx;
                        } else {
                            // 1D / Compute / Vertex: SV_DispatchThreadID.x = gx * tg_x + lx
                            wv.vgpr[l][0] = gx * tg_x + lx;
                        }
                    }
                    ++state.waves_executed;
                }

                // Step all waves in the workgroup in barrier-synchronized lockstep
                for (;;) {
                    bool any_running = false;
                    for (auto& wv : waves) {
                        while (!wv.halted && !wv.at_barrier) {
                            any_running = true;

                            // Reconverge SIMT stack if we reached cur_reconv_pc
                            while (wv.pc == wv.cur_reconv_pc && !wv.simt_stack.empty()) {
                                const SimtFrame fr = wv.simt_stack.back();
                                wv.simt_stack.pop_back();
                                wv.pc = fr.target_pc;
                                wv.exec = fr.exec_mask;
                                wv.cur_reconv_pc = fr.reconv_pc;
                            }

                            if (wv.pc >= instructions.size()) {
                                if (!wv.simt_stack.empty()) {
                                    const SimtFrame fr = wv.simt_stack.back();
                                    wv.simt_stack.pop_back();
                                    wv.pc = fr.target_pc;
                                    wv.exec = fr.exec_mask;
                                    wv.cur_reconv_pc = fr.reconv_pc;
                                    continue;
                                }
                                wv.halted = true;
                                break;
                            }

                            if (++wv.steps > state.max_steps_per_wave) {
                                return Status::Error(StatusCode::ShaderError,
                                                     "RDNA2 CPU interpreter exceeded max_steps_per_wave");
                            }
                            ++state.total_instructions_executed;

                            const auto& inst = instructions[wv.pc];
                            const size_t cur_pc = wv.pc;
                            size_t next_pc = cur_pc + 1;

                            auto read_op_u32 = [&](const Rdna2Operand& op, uint32_t lane) -> uint32_t {
                                switch (op.kind) {
                                    case Rdna2OperandKind::Sgpr:
                                        return (op.reg_index < 106u) ? wv.sgpr[op.reg_index] : 0u;
                                    case Rdna2OperandKind::Vgpr:
                                        return (op.reg_index < 256u) ? wv.vgpr[lane][op.reg_index] : 0u;
                                    case Rdna2OperandKind::InlineInt:
                                        return static_cast<uint32_t>(op.int_value);
                                    case Rdna2OperandKind::InlineFloat:
                                    case Rdna2OperandKind::Literal32:
                                        return op.literal_bits;
                                    case Rdna2OperandKind::VccLo:
                                        return static_cast<uint32_t>(wv.vcc & 0xFFFFFFFFull);
                                    case Rdna2OperandKind::VccHi:
                                        return static_cast<uint32_t>(wv.vcc >> 32);
                                    case Rdna2OperandKind::ExecLo:
                                        return static_cast<uint32_t>(wv.exec & 0xFFFFFFFFull);
                                    case Rdna2OperandKind::ExecHi:
                                        return static_cast<uint32_t>(wv.exec >> 32);
                                    case Rdna2OperandKind::Scc:
                                        return wv.scc ? 1u : 0u;
                                    case Rdna2OperandKind::M0:
                                        return wv.m0;
                                    default:
                                        return 0u;
                                }
                            };

                            auto read_op_f32 = [&](const Rdna2Operand& op, uint32_t lane) -> float {
                                float v = BitsToFloat(read_op_u32(op, lane));
                                if (op.abs) v = std::fabs(v);
                                if (op.neg) v = -v;
                                return v;
                            };

                            auto clamp_f32 = [&](float v) -> float {
                                return inst.clamp ? std::fmin(std::fmax(v, 0.0f), 1.0f) : v;
                            };

                            auto write_sgpr = [&](const Rdna2Operand& dst, uint32_t val) {
                                if (dst.kind == Rdna2OperandKind::Sgpr && dst.reg_index < 106u) {
                                    wv.sgpr[dst.reg_index] = val;
                                } else if (dst.kind == Rdna2OperandKind::VccLo) {
                                    wv.vcc = (wv.vcc & 0xFFFFFFFF00000000ull) | val;
                                } else if (dst.kind == Rdna2OperandKind::VccHi) {
                                    wv.vcc = (wv.vcc & 0x00000000FFFFFFFFull) | (static_cast<uint64_t>(val) << 32);
                                } else if (dst.kind == Rdna2OperandKind::M0) {
                                    wv.m0 = val;
                                }
                            };

                            auto write_mask64 = [&](const Rdna2Operand& dst, uint64_t mask) {
                                if (dst.kind == Rdna2OperandKind::VccLo) {
                                    wv.vcc = mask;
                                } else if (dst.kind == Rdna2OperandKind::Sgpr && dst.reg_index < 106u) {
                                    wv.sgpr[dst.reg_index] = static_cast<uint32_t>(mask & 0xFFFFFFFFull);
                                    if (wave_size == 64u && dst.reg_index + 1u < 106u) {
                                        wv.sgpr[dst.reg_index + 1u] = static_cast<uint32_t>(mask >> 32);
                                    }
                                }
                            };

                            auto read_mask64 = [&](const Rdna2Operand& op) -> uint64_t {
                                if (op.kind == Rdna2OperandKind::VccLo || op.kind == Rdna2OperandKind::VccHi) {
                                    return wv.vcc;
                                }
                                if (op.kind == Rdna2OperandKind::ExecLo || op.kind == Rdna2OperandKind::ExecHi) {
                                    return wv.exec;
                                }
                                if (op.kind == Rdna2OperandKind::Sgpr && op.reg_index < 106u) {
                                    uint64_t lo = wv.sgpr[op.reg_index];
                                    uint64_t hi = (op.reg_index + 1u < 106u) ? wv.sgpr[op.reg_index + 1u] : 0u;
                                    return lo | (hi << 32);
                                }
                                return read_op_u32(op, 0u);
                            };

                            auto lane_active = [&](uint32_t l) -> bool {
                                return ((wv.exec >> l) & 1ull) != 0ull;
                            };

                            auto do_divergent_branch = [&](uint64_t taken_mask, size_t tgt_pc) {
                                const uint64_t fall_mask = wv.exec & ~taken_mask;
                                if (taken_mask == 0ull) {
                                    next_pc = cur_pc + 1;
                                } else if (fall_mask == 0ull) {
                                    next_pc = tgt_pc;
                                } else {
                                    const size_t reconv = ipdom[cur_pc];
                                    wv.simt_stack.push_back({reconv, wv.exec, wv.cur_reconv_pc});
                                    if (tgt_pc != reconv) {
                                        wv.simt_stack.push_back({tgt_pc, taken_mask, reconv});
                                    }
                                    wv.exec = fall_mask;
                                    wv.cur_reconv_pc = reconv;
                                    next_pc = cur_pc + 1;
                                }
                            };

                            switch (inst.opcode) {
                                // ----- SOP2 -----
                                case Rdna2Opcode::S_ADD_U32: {
                                    const uint64_t sum = static_cast<uint64_t>(read_op_u32(inst.src[0], 0)) +
                                                         static_cast<uint64_t>(read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, static_cast<uint32_t>(sum));
                                    wv.scc = (sum >> 32) != 0ull;
                                    break;
                                }
                                case Rdna2Opcode::S_SUB_U32: {
                                    const uint32_t a = read_op_u32(inst.src[0], 0);
                                    const uint32_t b = read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, a - b);
                                    wv.scc = (b > a);
                                    break;
                                }
                                case Rdna2Opcode::S_ADD_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    const int64_t r = static_cast<int64_t>(a) + static_cast<int64_t>(b);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(static_cast<int32_t>(r)));
                                    wv.scc = (r != static_cast<int64_t>(static_cast<int32_t>(r)));
                                    break;
                                }
                                case Rdna2Opcode::S_SUB_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    const int64_t r = static_cast<int64_t>(a) - static_cast<int64_t>(b);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(static_cast<int32_t>(r)));
                                    wv.scc = (r != static_cast<int64_t>(static_cast<int32_t>(r)));
                                    break;
                                }
                                case Rdna2Opcode::S_MIN_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, static_cast<uint32_t>(std::min(a, b)));
                                    wv.scc = (a < b);
                                    break;
                                }
                                case Rdna2Opcode::S_MIN_U32: {
                                    const uint32_t a = read_op_u32(inst.src[0], 0);
                                    const uint32_t b = read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, std::min(a, b));
                                    wv.scc = (a < b);
                                    break;
                                }
                                case Rdna2Opcode::S_MAX_I32: {
                                    const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], 0));
                                    const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], 0));
                                    write_sgpr(inst.dst, static_cast<uint32_t>(std::max(a, b)));
                                    wv.scc = (a > b);
                                    break;
                                }
                                case Rdna2Opcode::S_MAX_U32: {
                                    const uint32_t a = read_op_u32(inst.src[0], 0);
                                    const uint32_t b = read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, std::max(a, b));
                                    wv.scc = (a > b);
                                    break;
                                }
                                case Rdna2Opcode::S_CSELECT_B32:
                                    write_sgpr(inst.dst, wv.scc ? read_op_u32(inst.src[0], 0) : read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_AND_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) & read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_OR_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) | read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_XOR_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) ^ read_op_u32(inst.src[1], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_LSHL_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) << (read_op_u32(inst.src[1], 0) & 31u);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_LSHR_B32: {
                                    const uint32_t r = read_op_u32(inst.src[0], 0) >> (read_op_u32(inst.src[1], 0) & 31u);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_ASHR_I32: {
                                    const int32_t r = static_cast<int32_t>(read_op_u32(inst.src[0], 0)) >>
                                                      (read_op_u32(inst.src[1], 0) & 31u);
                                    write_sgpr(inst.dst, static_cast<uint32_t>(r));
                                    wv.scc = (r != 0);
                                    break;
                                }
                                case Rdna2Opcode::S_MUL_I32:
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], 0) * read_op_u32(inst.src[1], 0));
                                    break;

                                // ----- SOP1 -----
                                case Rdna2Opcode::S_MOV_B32:
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], 0));
                                    break;
                                case Rdna2Opcode::S_NOT_B32: {
                                    const uint32_t r = ~read_op_u32(inst.src[0], 0);
                                    write_sgpr(inst.dst, r);
                                    wv.scc = (r != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_BCNT1_I32_B64: {
                                    const uint32_t cnt = PopCount64(read_mask64(inst.src[0]));
                                    write_sgpr(inst.dst, cnt);
                                    wv.scc = (cnt != 0u);
                                    break;
                                }
                                case Rdna2Opcode::S_GETPC_B64: {
                                    write_sgpr(inst.dst, (inst.word_offset + 1u) * 4u);
                                    if (inst.dst.kind == Rdna2OperandKind::Sgpr && inst.dst.reg_index + 1u < 106u) {
                                        wv.sgpr[inst.dst.reg_index + 1u] = 0u;
                                    }
                                    break;
                                }

                                // ----- SOPC -----
                                case Rdna2Opcode::S_CMP_EQ_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) == static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_LG_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) != static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_GT_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) > static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_GE_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) >= static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_LT_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) < static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_LE_I32:
                                    wv.scc = (static_cast<int32_t>(read_op_u32(inst.src[0], 0)) <= static_cast<int32_t>(read_op_u32(inst.src[1], 0)));
                                    break;
                                case Rdna2Opcode::S_CMP_EQ_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) == read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_LG_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) != read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_GT_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) > read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_GE_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) >= read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_LT_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) < read_op_u32(inst.src[1], 0));
                                    break;
                                case Rdna2Opcode::S_CMP_LE_U32:
                                    wv.scc = (read_op_u32(inst.src[0], 0) <= read_op_u32(inst.src[1], 0));
                                    break;

                                // ----- SOPK -----
                                case Rdna2Opcode::S_MOVK_I32:
                                    write_sgpr(inst.dst, static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
                                    break;
                                case Rdna2Opcode::S_ADDK_I32: {
                                    const uint32_t cur = read_op_u32(inst.dst, 0);
                                    write_sgpr(inst.dst, cur + static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
                                    break;
                                }
                                case Rdna2Opcode::S_MULK_I32: {
                                    const uint32_t cur = read_op_u32(inst.dst, 0);
                                    write_sgpr(inst.dst, cur * static_cast<uint32_t>(static_cast<int32_t>(inst.simm16)));
                                    break;
                                }

                                // ----- SOPP -----
                                case Rdna2Opcode::S_NOP:
                                case Rdna2Opcode::S_WAITCNT:
                                    break;
                                case Rdna2Opcode::S_BARRIER:
                                    wv.pc = next_pc;
                                    wv.at_barrier = true;
                                    continue;
                                case Rdna2Opcode::S_SENDMSG: {
                                    const uint16_t msg_id = static_cast<uint16_t>(static_cast<uint16_t>(inst.simm16) & 0xFu);
                                    if (container.stage == ShaderStage::Mesh || msg_id == kRdna2SendMsgGsAllocReq) {
                                        state.mesh_vertex_count = wv.sgpr[0];
                                        state.mesh_primitive_count = wv.sgpr[1];
                                    } else if (container.stage == ShaderStage::Amplification || msg_id == kRdna2SendMsgDispatchMesh) {
                                        state.dispatch_mesh_groups = {wv.sgpr[0], wv.sgpr[1], wv.sgpr[2]};
                                    }
                                    break;
                                }
                                case Rdna2Opcode::S_ENDPGM: {
                                    if (!wv.simt_stack.empty()) {
                                        // Mask out finished lanes from pending reconvergence frames
                                        for (auto& fr : wv.simt_stack) {
                                            fr.exec_mask &= ~wv.exec;
                                        }
                                        const SimtFrame fr = wv.simt_stack.back();
                                        wv.simt_stack.pop_back();
                                        wv.pc = fr.target_pc;
                                        wv.exec = fr.exec_mask;
                                        wv.cur_reconv_pc = fr.reconv_pc;
                                        continue;
                                    }
                                    wv.halted = true;
                                    continue;
                                }
                                case Rdna2Opcode::S_BRANCH:
                                    next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_SCC0:
                                    if (!wv.scc) next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_SCC1:
                                    if (wv.scc) next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_VCCZ:
                                    do_divergent_branch(wv.exec & ~wv.vcc, branch_targets[cur_pc]);
                                    break;
                                case Rdna2Opcode::S_CBRANCH_VCCNZ:
                                    do_divergent_branch(wv.exec & wv.vcc, branch_targets[cur_pc]);
                                    break;
                                case Rdna2Opcode::S_CBRANCH_EXECZ:
                                    if (wv.exec == 0ull) next_pc = branch_targets[cur_pc];
                                    break;
                                case Rdna2Opcode::S_CBRANCH_EXECNZ:
                                    if (wv.exec != 0ull) next_pc = branch_targets[cur_pc];
                                    break;

                                // ----- VOP2 / VOP1 / VOP3 per-lane vector ops -----
                                case Rdna2Opcode::V_NOP:
                                    break;
                                case Rdna2Opcode::V_MOV_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_CNDMASK_B32: {
                                    const uint64_t mask = (inst.src_count >= 3) ? read_mask64(inst.src[2]) : wv.vcc;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const bool sel1 = ((mask >> l) & 1ull) != 0ull;
                                        wv.vgpr[l][inst.dst.reg_index] = sel1 ? read_op_u32(inst.src[1], l)
                                                                              : read_op_u32(inst.src[0], l);
                                    }
                                    break;
                                }
                                case Rdna2Opcode::V_ADD_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[0], l) + read_op_f32(inst.src[1], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_SUB_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[0], l) - read_op_f32(inst.src[1], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_SUBREV_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[1], l) - read_op_f32(inst.src[0], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_MUL_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(read_op_f32(inst.src[0], l) * read_op_f32(inst.src[1], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_MIN_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(std::fmin(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAX_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(std::fmax(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_MIN_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], l));
                                        const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], l));
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(std::min(a, b));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAX_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const int32_t a = static_cast<int32_t>(read_op_u32(inst.src[0], l));
                                        const int32_t b = static_cast<int32_t>(read_op_u32(inst.src[1], l));
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(std::max(a, b));
                                    }
                                    break;
                                case Rdna2Opcode::V_MIN_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            std::min(read_op_u32(inst.src[0], l), read_op_u32(inst.src[1], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_MAX_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            std::max(read_op_u32(inst.src[0], l), read_op_u32(inst.src[1], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_FMAC_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float acc = BitsToFloat(wv.vgpr[l][inst.dst.reg_index]);
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(
                                            clamp_f32(std::fma(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l), acc)));
                                    }
                                    break;
                                case Rdna2Opcode::V_AND_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) & read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_OR_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) | read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_XOR_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) ^ read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_LSHLREV_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            read_op_u32(inst.src[1], l) << (read_op_u32(inst.src[0], l) & 31u);
                                    }
                                    break;
                                case Rdna2Opcode::V_LSHRREV_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            read_op_u32(inst.src[1], l) >> (read_op_u32(inst.src[0], l) & 31u);
                                    }
                                    break;
                                case Rdna2Opcode::V_ASHRREV_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const int32_t val = static_cast<int32_t>(read_op_u32(inst.src[1], l));
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(val >> (read_op_u32(inst.src[0], l) & 31u));
                                    }
                                    break;
                                case Rdna2Opcode::V_ADD_NC_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) + read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_SUB_NC_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) - read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_READFIRSTLANE_B32: {
                                    uint32_t first = 0u;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (lane_active(l)) {
                                            first = l;
                                            break;
                                        }
                                    }
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], first));
                                    break;
                                }
                                case Rdna2Opcode::V_CVT_F32_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            FloatToBits(static_cast<float>(static_cast<int32_t>(read_op_u32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_CVT_I32_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(static_cast<int32_t>(read_op_f32(inst.src[0], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_CVT_F32_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            FloatToBits(static_cast<float>(read_op_u32(inst.src[0], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_CVT_U32_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(read_op_f32(inst.src[0], l));
                                    }
                                    break;
                                case Rdna2Opcode::V_RCP_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(1.0f / read_op_f32(inst.src[0], l)));
                                    }
                                    break;
                                case Rdna2Opcode::V_RSQ_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(1.0f / std::sqrt(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_SQRT_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(std::sqrt(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_FLOOR_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(std::floor(read_op_f32(inst.src[0], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_FRACT_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float x = read_op_f32(inst.src[0], l);
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(x - std::floor(x)));
                                    }
                                    break;

                                // ----- VOPC -----
                                case Rdna2Opcode::V_CMP_LT_F32:
                                case Rdna2Opcode::V_CMP_EQ_F32:
                                case Rdna2Opcode::V_CMP_LE_F32:
                                case Rdna2Opcode::V_CMP_GT_F32:
                                case Rdna2Opcode::V_CMP_LG_F32:
                                case Rdna2Opcode::V_CMP_GE_F32:
                                case Rdna2Opcode::V_CMP_LT_I32:
                                case Rdna2Opcode::V_CMP_EQ_I32:
                                case Rdna2Opcode::V_CMP_GT_I32:
                                case Rdna2Opcode::V_CMP_GE_I32:
                                case Rdna2Opcode::V_CMP_LT_U32:
                                case Rdna2Opcode::V_CMP_EQ_U32:
                                case Rdna2Opcode::V_CMP_GT_U32:
                                case Rdna2Opcode::V_CMP_GE_U32: {
                                    uint64_t mask = 0ull;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        bool r = false;
                                        switch (inst.opcode) {
                                            case Rdna2Opcode::V_CMP_LT_F32: r = read_op_f32(inst.src[0], l) < read_op_f32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_EQ_F32: r = read_op_f32(inst.src[0], l) == read_op_f32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_LE_F32: r = read_op_f32(inst.src[0], l) <= read_op_f32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_GT_F32: r = read_op_f32(inst.src[0], l) > read_op_f32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_LG_F32: r = read_op_f32(inst.src[0], l) != read_op_f32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_GE_F32: r = read_op_f32(inst.src[0], l) >= read_op_f32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_LT_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) < static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                            case Rdna2Opcode::V_CMP_EQ_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) == static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                            case Rdna2Opcode::V_CMP_GT_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) > static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                            case Rdna2Opcode::V_CMP_GE_I32: r = static_cast<int32_t>(read_op_u32(inst.src[0], l)) >= static_cast<int32_t>(read_op_u32(inst.src[1], l)); break;
                                            case Rdna2Opcode::V_CMP_LT_U32: r = read_op_u32(inst.src[0], l) < read_op_u32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_EQ_U32: r = read_op_u32(inst.src[0], l) == read_op_u32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_GT_U32: r = read_op_u32(inst.src[0], l) > read_op_u32(inst.src[1], l); break;
                                            case Rdna2Opcode::V_CMP_GE_U32: r = read_op_u32(inst.src[0], l) >= read_op_u32(inst.src[1], l); break;
                                            default: break;
                                        }
                                        if (r) mask |= (1ull << l);
                                    }
                                    write_mask64(inst.dst, mask);
                                    break;
                                }

                                // ----- VOP3 (3-op / wave / bitfield) -----
                                case Rdna2Opcode::V_FMA_F32:
                                case Rdna2Opcode::V_MAD_F32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(
                                            std::fma(read_op_f32(inst.src[0], l), read_op_f32(inst.src[1], l), read_op_f32(inst.src[2], l))));
                                    }
                                    break;
                                case Rdna2Opcode::V_BFE_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t s0 = read_op_u32(inst.src[0], l);
                                        const uint32_t off = read_op_u32(inst.src[1], l) & 31u;
                                        const uint32_t wid = read_op_u32(inst.src[2], l) & 31u;
                                        const uint32_t mask = (wid == 0u) ? 0u : ((1u << wid) - 1u);
                                        wv.vgpr[l][inst.dst.reg_index] = (s0 >> off) & mask;
                                    }
                                    break;
                                case Rdna2Opcode::V_BFE_I32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t s0 = read_op_u32(inst.src[0], l);
                                        const uint32_t off = read_op_u32(inst.src[1], l) & 31u;
                                        const uint32_t wid = read_op_u32(inst.src[2], l) & 31u;
                                        int32_t r = 0;
                                        if (wid == 0u) {
                                            r = 0;
                                        } else if (off + wid < 32u) {
                                            r = static_cast<int32_t>(s0 << (32u - off - wid)) >> (32u - wid);
                                        } else {
                                            r = static_cast<int32_t>(s0) >> off;
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] = static_cast<uint32_t>(r);
                                    }
                                    break;
                                case Rdna2Opcode::V_BFI_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t m = read_op_u32(inst.src[0], l);
                                        const uint32_t ins = read_op_u32(inst.src[1], l);
                                        const uint32_t base = read_op_u32(inst.src[2], l);
                                        wv.vgpr[l][inst.dst.reg_index] = (m & ins) | (~m & base);
                                    }
                                    break;
                                case Rdna2Opcode::V_MUL_LO_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] = read_op_u32(inst.src[0], l) * read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_MBCNT_LO_U32_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t m = read_op_u32(inst.src[0], l);
                                        const uint32_t init = read_op_u32(inst.src[1], l);
                                        const uint32_t lane_m = (l < 32u) ? ((1u << l) - 1u) : 0xFFFFFFFFu;
                                        wv.vgpr[l][inst.dst.reg_index] = PopCount32(m & lane_m) + init;
                                    }
                                    break;
                                case Rdna2Opcode::V_MBCNT_HI_U32_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t m = read_op_u32(inst.src[0], l);
                                        const uint32_t init = read_op_u32(inst.src[1], l);
                                        const uint32_t lane_m = (l >= 32u) ? ((1u << (l - 32u)) - 1u) : 0u;
                                        wv.vgpr[l][inst.dst.reg_index] = PopCount32(m & lane_m) + init;
                                    }
                                    break;
                                case Rdna2Opcode::V_READLANE_B32: {
                                    const uint32_t src_lane = read_op_u32(inst.src[1], 0) % wv.lane_count;
                                    write_sgpr(inst.dst, read_op_u32(inst.src[0], src_lane));
                                    break;
                                }
                                case Rdna2Opcode::V_ADD3_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            read_op_u32(inst.src[0], l) + read_op_u32(inst.src[1], l) + read_op_u32(inst.src[2], l);
                                    }
                                    break;
                                case Rdna2Opcode::V_DOT2_F32_F16:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t a = read_op_u32(inst.src[0], l);
                                        const uint32_t b = read_op_u32(inst.src[1], l);
                                        const float c = read_op_f32(inst.src[2], l);
                                        const float r = HalfBitsToFloat(PickHalf16(a, false)) * HalfBitsToFloat(PickHalf16(b, false)) +
                                                        HalfBitsToFloat(PickHalf16(a, true)) * HalfBitsToFloat(PickHalf16(b, true)) + c;
                                        wv.vgpr[l][inst.dst.reg_index] = FloatToBits(clamp_f32(r));
                                    }
                                    break;
                                case Rdna2Opcode::V_PERMLANE16_B32:
                                case Rdna2Opcode::V_PERMLANEX16_B32: {
                                    std::array<uint32_t, 64> snap{};
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        snap[l] = read_op_u32(inst.src[0], l);
                                    }
                                    const uint32_t x16 = (inst.opcode == Rdna2Opcode::V_PERMLANEX16_B32) ? 16u : 0u;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t row_lane = l & 15u;
                                        const uint32_t row_base = (l & ~15u) ^ x16;
                                        const uint32_t s1 = read_op_u32(inst.src[1], l);
                                        const uint32_t s2 = read_op_u32(inst.src[2], l);
                                        const uint32_t word = (row_lane < 8u) ? s1 : s2;
                                        const uint32_t sel = (word >> ((row_lane & 7u) * 4u)) & 15u;
                                        const uint32_t tgt = (row_base | sel) % wv.lane_count;
                                        wv.vgpr[l][inst.dst.reg_index] = snap[tgt];
                                    }
                                    break;
                                }

                                // ----- VOP3P (packed 2x16 f16 / u16 / b16) -----
                                case Rdna2Opcode::V_PK_ADD_F16:
                                case Rdna2Opcode::V_PK_MUL_F16:
                                case Rdna2Opcode::V_PK_FMA_F16: {
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t u0 = read_op_u32(inst.src[0], l);
                                        const uint32_t u1 = read_op_u32(inst.src[1], l);
                                        const uint32_t u2 = (inst.src_count >= 3) ? read_op_u32(inst.src[2], l) : 0u;
                                        auto lane_f = [&](uint32_t src, int idx, bool hi_out) -> float {
                                            const bool pick_hi = (((hi_out ? inst.vop3p_op_sel_hi : inst.vop3p_op_sel) >> idx) & 1u) != 0u;
                                            const bool do_neg = (((hi_out ? inst.vop3p_neg_hi : inst.vop3p_neg_lo) >> idx) & 1u) != 0u;
                                            float v = HalfBitsToFloat(PickHalf16(src, pick_hi));
                                            return do_neg ? -v : v;
                                        };
                                        auto eval_h = [&](bool hi_out) -> uint16_t {
                                            const float a = lane_f(u0, 0, hi_out);
                                            const float b = lane_f(u1, 1, hi_out);
                                            float r = 0.0f;
                                            if (inst.opcode == Rdna2Opcode::V_PK_ADD_F16) r = a + b;
                                            else if (inst.opcode == Rdna2Opcode::V_PK_MUL_F16) r = a * b;
                                            else r = std::fma(a, b, lane_f(u2, 2, hi_out));
                                            return FloatToHalfBits(clamp_f32(r));
                                        };
                                        const uint16_t lo = eval_h(false);
                                        const uint16_t hi = eval_h(true);
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
                                    }
                                    break;
                                }
                                case Rdna2Opcode::V_PK_ADD_U16:
                                case Rdna2Opcode::V_PK_LSHLREV_B16:
                                case Rdna2Opcode::V_PK_LSHRREV_B16: {
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t u0 = read_op_u32(inst.src[0], l);
                                        const uint32_t u1 = read_op_u32(inst.src[1], l);
                                        const uint16_t s0_lo = PickHalf16(u0, (inst.vop3p_op_sel & 1u) != 0u);
                                        const uint16_t s0_hi = PickHalf16(u0, (inst.vop3p_op_sel_hi & 1u) != 0u);
                                        const uint16_t s1_lo = PickHalf16(u1, ((inst.vop3p_op_sel >> 1) & 1u) != 0u);
                                        const uint16_t s1_hi = PickHalf16(u1, ((inst.vop3p_op_sel_hi >> 1) & 1u) != 0u);
                                        uint16_t r_lo = 0, r_hi = 0;
                                        if (inst.opcode == Rdna2Opcode::V_PK_ADD_U16) {
                                            r_lo = static_cast<uint16_t>(s0_lo + s1_lo);
                                            r_hi = static_cast<uint16_t>(s0_hi + s1_hi);
                                        } else if (inst.opcode == Rdna2Opcode::V_PK_LSHLREV_B16) {
                                            r_lo = static_cast<uint16_t>(s1_lo << (s0_lo & 15u));
                                            r_hi = static_cast<uint16_t>(s1_hi << (s0_hi & 15u));
                                        } else {
                                            r_lo = static_cast<uint16_t>(s1_lo >> (s0_lo & 15u));
                                            r_hi = static_cast<uint16_t>(s1_hi >> (s0_hi & 15u));
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] =
                                            static_cast<uint32_t>(r_lo) | (static_cast<uint32_t>(r_hi) << 16);
                                    }
                                    break;
                                }

                                // ----- SMEM -----
                                case Rdna2Opcode::S_LOAD_DWORD:
                                case Rdna2Opcode::S_BUFFER_LOAD_DWORD: {
                                    const uint32_t byte_off = (inst.mem_offset >= 0) ? static_cast<uint32_t>(inst.mem_offset) : 0u;
                                    const uint32_t slot = byte_off / 4u;
                                    if (slot < 16u && (inst.soffset == 0x7Du || inst.soffset == 0x7Cu || inst.soffset == 128u)) {
                                        const uint32_t val = (slot < state.root_constants.size()) ? state.root_constants[slot] : 0u;
                                        write_sgpr(inst.dst, val);
                                    } else {
                                        uint32_t addr = byte_off;
                                        if (inst.soffset < 106u) addr += wv.sgpr[inst.soffset];
                                        write_sgpr(inst.dst, ReadBufferWord(state.uav0, addr));
                                    }
                                    break;
                                }

                                // ----- MUBUF -----
                                case Rdna2Opcode::BUFFER_LOAD_DWORD:
                                case Rdna2Opcode::BUFFER_STORE_DWORD: {
                                    auto& buf = (inst.srsrc == 0u) ? state.uav0 : state.uav1;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t addr = 0u;
                                        if (inst.offen) {
                                            addr = read_op_u32(inst.src[1], l);
                                        } else if (inst.idxen) {
                                            addr = read_op_u32(inst.src[1], l) << 2;
                                        }
                                        addr += static_cast<uint32_t>(inst.mem_offset);
                                        if (inst.soffset != 128u && inst.soffset != 125u) {
                                            addr += read_op_u32(DecodeRdna2SourceOperand(inst.soffset), l);
                                        }
                                        if (inst.opcode == Rdna2Opcode::BUFFER_LOAD_DWORD) {
                                            wv.vgpr[l][inst.dst.reg_index] = ReadBufferWord(buf, addr);
                                        } else {
                                            WriteBufferWord(buf, addr, read_op_u32(inst.src[0], l));
                                        }
                                    }
                                    break;
                                }

                                // ----- MIMG -----
                                case Rdna2Opcode::IMAGE_LOAD: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const auto px = state.tex2d_srv.LoadRgba32f(
                                            wv.vgpr[l][vaddr_b], wv.vgpr[l][vaddr_b + 1u], 0u, 0u);
                                        uint8_t out_r = vdata_b;
                                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                                            if ((dmask & (1u << ch)) != 0u) {
                                                wv.vgpr[l][out_r++] = FloatToBits(px[ch]);
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_LOAD_MIP: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    const uint8_t dim = inst.mimg_dim;
                                    const auto& tex = (dim == 2u) ? state.tex3d_srv
                                                                  : ((dim == 3u) ? state.texcube_srv : state.tex2d_srv);
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t x = wv.vgpr[l][vaddr_b];
                                        const uint32_t y = wv.vgpr[l][vaddr_b + 1u];
                                        const uint32_t z = (dim == 2u || dim == 3u) ? wv.vgpr[l][vaddr_b + 2u] : 0u;
                                        const uint32_t mip = (dim == 2u || dim == 3u) ? wv.vgpr[l][vaddr_b + 3u] : wv.vgpr[l][vaddr_b + 2u];
                                        const auto px = tex.LoadRgba32f(x, y, z, mip);
                                        uint8_t out_r = vdata_b;
                                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                                            if ((dmask & (1u << ch)) != 0u) {
                                                wv.vgpr[l][out_r++] = FloatToBits(px[ch]);
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_SAMPLE:
                                case Rdna2Opcode::IMAGE_SAMPLE_LZ: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const float u = BitsToFloat(wv.vgpr[l][vaddr_b]);
                                        const float v = BitsToFloat(wv.vgpr[l][vaddr_b + 1u]);
                                        const float lod = (inst.opcode == Rdna2Opcode::IMAGE_SAMPLE)
                                                              ? BitsToFloat(wv.vgpr[l][vaddr_b + 2u])
                                                              : 0.0f;
                                        const auto px = state.tex2d_srv.SampleNearest(u, v, lod);
                                        uint8_t out_r = vdata_b;
                                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                                            if ((dmask & (1u << ch)) != 0u) {
                                                wv.vgpr[l][out_r++] = FloatToBits(px[ch]);
                                            }
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_STORE: {
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[1].reg_index);
                                    const uint8_t dmask = inst.mimg_dmask ? inst.mimg_dmask : 0xFu;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        std::array<float, 4> rgba = {0.0f, 0.0f, 0.0f, 1.0f};
                                        uint8_t in_r = vdata_b;
                                        for (uint32_t ch = 0; ch < 4u; ++ch) {
                                            if ((dmask & (1u << ch)) != 0u) {
                                                rgba[ch] = BitsToFloat(wv.vgpr[l][in_r++]);
                                            }
                                        }
                                        state.tex2d_uav.StoreRgba32f(
                                            wv.vgpr[l][vaddr_b], wv.vgpr[l][vaddr_b + 1u], 0u, rgba);
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP: {
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[1].reg_index);
                                    const uint8_t dim = inst.mimg_dim;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t src_val = wv.vgpr[l][vdata_b];
                                        const uint32_t cmp_val = wv.vgpr[l][vdata_b + 1u];
                                        const uint32_t x = wv.vgpr[l][vaddr_b];
                                        const uint32_t y = wv.vgpr[l][vaddr_b + 1u];
                                        const uint32_t z = (dim == 2u || dim == 3u) ? wv.vgpr[l][vaddr_b + 2u] : 0u;
                                        const uint32_t old = state.tex_u32_uav.AtomicCmpSwapR32u(x, y, z, cmp_val, src_val);
                                        if (inst.glc) {
                                            wv.vgpr[l][vdata_b] = old;
                                        }
                                    }
                                    break;
                                }
                                case Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY: {
                                    const uint8_t vaddr_b = static_cast<uint8_t>(inst.src[0].reg_index);
                                    const uint8_t vdata_b = static_cast<uint8_t>(inst.dst.reg_index);
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const BvhHitResult hit = EvalBvhIntersectRayCpu(
                                            state.uav0,
                                            wv.vgpr[l][vaddr_b + 0u],
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 1u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 2u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 3u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 4u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 5u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 6u]),
                                            BitsToFloat(wv.vgpr[l][vaddr_b + 7u]));
                                        wv.vgpr[l][vdata_b + 0u] = FloatToBits(hit.t);
                                        wv.vgpr[l][vdata_b + 1u] = FloatToBits(hit.u);
                                        wv.vgpr[l][vdata_b + 2u] = FloatToBits(hit.v);
                                        wv.vgpr[l][vdata_b + 3u] = hit.hit_id;
                                    }
                                    break;
                                }

                                // ----- DS (LDS / Swizzle) -----
                                case Rdna2Opcode::DS_WRITE_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        lds[(addr >> 2) & lds_mask] = read_op_u32(inst.src[1], l);
                                    }
                                    break;
                                case Rdna2Opcode::DS_READ_B32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        wv.vgpr[l][inst.dst.reg_index] = lds[(addr >> 2) & lds_mask];
                                    }
                                    break;
                                case Rdna2Opcode::DS_ADD_U32:
                                case Rdna2Opcode::DS_ADD_RTN_U32:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        const uint32_t idx = (addr >> 2) & lds_mask;
                                        const uint32_t prev = lds[idx];
                                        lds[idx] = prev + read_op_u32(inst.src[1], l);
                                        if (inst.opcode == Rdna2Opcode::DS_ADD_RTN_U32) {
                                            wv.vgpr[l][inst.dst.reg_index] = prev;
                                        }
                                    }
                                    break;
                                case Rdna2Opcode::DS_SWIZZLE_B32: {
                                    std::array<uint32_t, 64> snap{};
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        snap[l] = read_op_u32(inst.src[0], l);
                                    }
                                    const uint32_t swiz = static_cast<uint32_t>(inst.mem_offset) & 0xFFFFu;
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t tgt = l;
                                        if ((swiz & 0x8000u) != 0u) {
                                            const uint32_t and_m = (swiz & 0x1Fu) | 0x20u;
                                            const uint32_t or_m = (swiz >> 5) & 0x1Fu;
                                            const uint32_t xor_m = (swiz >> 10) & 0x1Fu;
                                            tgt = ((l & and_m) | or_m) ^ xor_m;
                                        } else {
                                            const uint32_t quad_base = l & ~3u;
                                            const uint32_t sel = (swiz >> ((l & 3u) * 2u)) & 3u;
                                            tgt = quad_base | sel;
                                        }
                                        wv.vgpr[l][inst.dst.reg_index] = snap[tgt % wv.lane_count];
                                    }
                                    break;
                                }

                                // ----- FLAT / GLOBAL / SCRATCH -----
                                case Rdna2Opcode::GLOBAL_LOAD_DWORD:
                                case Rdna2Opcode::FLAT_LOAD_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        wv.vgpr[l][inst.dst.reg_index] = ReadBufferWord(state.uav0, addr);
                                    }
                                    break;
                                case Rdna2Opcode::GLOBAL_STORE_DWORD:
                                case Rdna2Opcode::FLAT_STORE_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        const uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        WriteBufferWord(state.uav0, addr, read_op_u32(inst.src[1], l));
                                    }
                                    break;
                                case Rdna2Opcode::SCRATCH_LOAD_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        if (inst.saddr != 0x7Fu && inst.saddr < 106u) addr += wv.sgpr[inst.saddr];
                                        wv.vgpr[l][inst.dst.reg_index] = wv.scratch[l][(addr >> 2) & scr_mask];
                                    }
                                    break;
                                case Rdna2Opcode::SCRATCH_STORE_DWORD:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        uint32_t addr = read_op_u32(inst.src[0], l) + static_cast<uint32_t>(inst.mem_offset);
                                        if (inst.saddr != 0x7Fu && inst.saddr < 106u) addr += wv.sgpr[inst.saddr];
                                        wv.scratch[l][(addr >> 2) & scr_mask] = read_op_u32(inst.src[1], l);
                                    }
                                    break;

                                // ----- EXP -----
                                case Rdna2Opcode::EXP:
                                    for (uint32_t l = 0; l < wv.lane_count; ++l) {
                                        if (!lane_active(l)) continue;
                                        auto& ex = state.exports[wv.global_thread_ids[l]];
                                        if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Prim)) {
                                            ex.wrote_prim = true;
                                            if (inst.exp_en == 0x1u) {
                                                const uint32_t p = read_op_u32(inst.src[0], l);
                                                ex.prim = {p & 0x3FFu, (p >> 10) & 0x3FFu, (p >> 20) & 0x3FFu};
                                            } else {
                                                ex.prim = {read_op_u32(inst.src[0], l),
                                                           read_op_u32(inst.src[1], l),
                                                           read_op_u32(inst.src[2], l)};
                                            }
                                        } else {
                                            std::array<float, 4> vec = {0.0f, 0.0f, 0.0f, 0.0f};
                                            for (uint8_t c = 0; c < 4u; ++c) {
                                                if ((inst.exp_en & (1u << c)) != 0u) {
                                                    vec[c] = read_op_f32(inst.src[c], l);
                                                } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0) && c == 3u) {
                                                    vec[c] = 1.0f;
                                                }
                                            }
                                            if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Pos0)) {
                                                ex.pos0 = vec;
                                                ex.wrote_pos0 = true;
                                            } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Param0)) {
                                                ex.param0 = vec;
                                                ex.wrote_param0 = true;
                                            } else if (inst.exp_target == static_cast<uint8_t>(Rdna2ExportTarget::Mrt0)) {
                                                ex.mrt0 = vec;
                                                ex.wrote_mrt0 = true;
                                            }
                                        }
                                    }
                                    break;

                                default:
                                    return Status::Error(
                                        StatusCode::NotImplemented,
                                        std::string("Unhandled RDNA2 opcode in CPU interpreter: ") + Rdna2OpcodeName(inst.opcode));
                            }

                            wv.pc = next_pc;
                        }
                    }

                    // Check if any non-halted wave is waiting at a barrier
                    bool any_alive = false;
                    bool all_alive_at_barrier = true;
                    for (const auto& wv : waves) {
                        if (!wv.halted) {
                            any_alive = true;
                            if (!wv.at_barrier) all_alive_at_barrier = false;
                        }
                    }
                    if (!any_alive) break;
                    if (all_alive_at_barrier) {
                        ++state.barriers_synchronized;
                        for (auto& wv : waves) {
                            if (!wv.halted) wv.at_barrier = false;
                        }
                    } else if (!any_running) {
                        break;
                    }
                }
            }
        }
    }

    return Status::Ok();
}

Status ExecuteRdna2BlobCpu(const std::vector<uint8_t>& blob, Rdna2CpuExecState& state) {
    XboxDxbcContainer container{};
    if (auto st = ParseXboxShaderContainer(blob, container); !st.ok()) {
        return st;
    }
    if (!container.has_xs2) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Blob does not contain an Xbox XS2 RDNA2 shader payload");
    }
    std::vector<Rdna2Instruction> insns;
    if (auto st = DisassembleRdna2(container.xs2.code_words, insns); !st.ok()) {
        return st;
    }
    return ExecuteRdna2Cpu(container.xs2, insns, state);
}

}  // namespace openscarlett
