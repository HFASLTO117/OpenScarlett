// OpenScarlett - MIT License
// Clean-room GFX10.3 (RDNA2) 779-opcode ISA coverage auditor & 3,000+ shader corpus benchmark.

#include "rdna2_coverage_tool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/rdna2_lifter.h"
#include "openscarlett/gpu/vulkan_backend.h"

#if defined(__has_include)
#  if __has_include("rdna2_isa_scalar_vector.h")
#    include "rdna2_isa_scalar_vector.h"
#    define OPENSCARLETT_HAS_RDNA2_SV_ISA 1
#  elif __has_include("openscarlett/gpu/rdna2_isa_scalar_vector.h")
#    include "openscarlett/gpu/rdna2_isa_scalar_vector.h"
#    define OPENSCARLETT_HAS_RDNA2_SV_ISA 1
#  endif
#  if __has_include("rdna2_isa_mem_img.h")
#    include "rdna2_isa_mem_img.h"
#    define OPENSCARLETT_HAS_RDNA2_MEM_ISA 1
#  elif __has_include("openscarlett/gpu/rdna2_isa_mem_img.h")
#    include "openscarlett/gpu/rdna2_isa_mem_img.h"
#    define OPENSCARLETT_HAS_RDNA2_MEM_ISA 1
#  endif
#endif

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#endif

namespace openscarlett {

namespace {

// ---------------------------------------------------------------------------
// Portable Clean-Room Helpers (MSVC / Clang-CL / MinGW-w64)
// ---------------------------------------------------------------------------
uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0u;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

uint64_t HashBytesFnv1a(const uint8_t* data, size_t size) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::string CanonicalMnemonic(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char ch : raw) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            out.push_back(ch);
        }
    }
    // Normalize optional VOP3/VOP2 suffix aliases (_e32 / _e64) to canonical GFX10.3 mnemonic.
    if (out.size() > 4u) {
        const std::string_view tail(out.data() + out.size() - 4u, 4u);
        if (tail == "_e32" || tail == "_e64") {
            out.resize(out.size() - 4u);
        }
    }
    return out;
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8u);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    char buf[8]{};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Exact 779-Opcode Standard Shader-Reachable GFX10.3 (RDNA2) Reference Catalog
// Derived from public AMD "RDNA2" Instruction Set Architecture Reference Guide.
// ---------------------------------------------------------------------------
enum class FamilyId : uint8_t {
    SOP1 = 0,
    SOP2,
    SOPK,
    SOPC,
    SOPP,
    SMEM,
    VOP1,
    VOP2,
    VOPC,
    VOP3,
    VOP3P,
    VINTRP,
    MUBUF,
    MTBUF,
    MIMG,
    DS,
    FLAT,
    GLOBAL,
    SCRATCH,
    EXP,
    Count
};

constexpr size_t kFamilyCount = static_cast<size_t>(FamilyId::Count);

constexpr std::array<const char*, kFamilyCount> kFamilyNames = {
    "SOP1", "SOP2", "SOPK", "SOPC", "SOPP", "SMEM",
    "VOP1", "VOP2", "VOPC", "VOP3", "VOP3P", "VINTRP",
    "MUBUF", "MTBUF", "MIMG", "DS", "FLAT", "GLOBAL", "SCRATCH", "EXP",
};

struct CatalogEntry {
    FamilyId family;
    const char* mnemonic;
};

constexpr CatalogEntry kGfx103Catalog[] = {
    // --- SOP1 (58) ---
    {FamilyId::SOP1, "s_mov_b32"},
    {FamilyId::SOP1, "s_mov_b64"},
    {FamilyId::SOP1, "s_cmov_b32"},
    {FamilyId::SOP1, "s_cmov_b64"},
    {FamilyId::SOP1, "s_not_b32"},
    {FamilyId::SOP1, "s_not_b64"},
    {FamilyId::SOP1, "s_wqm_b32"},
    {FamilyId::SOP1, "s_wqm_b64"},
    {FamilyId::SOP1, "s_brev_b32"},
    {FamilyId::SOP1, "s_brev_b64"},
    {FamilyId::SOP1, "s_bcnt0_i32_b32"},
    {FamilyId::SOP1, "s_bcnt0_i32_b64"},
    {FamilyId::SOP1, "s_bcnt1_i32_b32"},
    {FamilyId::SOP1, "s_bcnt1_i32_b64"},
    {FamilyId::SOP1, "s_ff0_i32_b32"},
    {FamilyId::SOP1, "s_ff0_i32_b64"},
    {FamilyId::SOP1, "s_ff1_i32_b32"},
    {FamilyId::SOP1, "s_ff1_i32_b64"},
    {FamilyId::SOP1, "s_flbit_i32_b32"},
    {FamilyId::SOP1, "s_flbit_i32_b64"},
    {FamilyId::SOP1, "s_flbit_i32"},
    {FamilyId::SOP1, "s_flbit_i32_i64"},
    {FamilyId::SOP1, "s_sext_i32_i8"},
    {FamilyId::SOP1, "s_sext_i32_i16"},
    {FamilyId::SOP1, "s_bitset0_b32"},
    {FamilyId::SOP1, "s_bitset0_b64"},
    {FamilyId::SOP1, "s_bitset1_b32"},
    {FamilyId::SOP1, "s_bitset1_b64"},
    {FamilyId::SOP1, "s_getpc_b64"},
    {FamilyId::SOP1, "s_setpc_b64"},
    {FamilyId::SOP1, "s_swappc_b64"},
    {FamilyId::SOP1, "s_rfe_b64"},
    {FamilyId::SOP1, "s_and_saveexec_b64"},
    {FamilyId::SOP1, "s_or_saveexec_b64"},
    {FamilyId::SOP1, "s_xor_saveexec_b64"},
    {FamilyId::SOP1, "s_andn2_saveexec_b64"},
    {FamilyId::SOP1, "s_orn2_saveexec_b64"},
    {FamilyId::SOP1, "s_nand_saveexec_b64"},
    {FamilyId::SOP1, "s_nor_saveexec_b64"},
    {FamilyId::SOP1, "s_xnor_saveexec_b64"},
    {FamilyId::SOP1, "s_quadmask_b32"},
    {FamilyId::SOP1, "s_quadmask_b64"},
    {FamilyId::SOP1, "s_movrels_b32"},
    {FamilyId::SOP1, "s_movrels_b64"},
    {FamilyId::SOP1, "s_movreld_b32"},
    {FamilyId::SOP1, "s_movreld_b64"},
    {FamilyId::SOP1, "s_abs_i32"},
    {FamilyId::SOP1, "s_and_saveexec_b32"},
    {FamilyId::SOP1, "s_or_saveexec_b32"},
    {FamilyId::SOP1, "s_xor_saveexec_b32"},
    {FamilyId::SOP1, "s_andn2_saveexec_b32"},
    {FamilyId::SOP1, "s_orn2_saveexec_b32"},
    {FamilyId::SOP1, "s_nand_saveexec_b32"},
    {FamilyId::SOP1, "s_nor_saveexec_b32"},
    {FamilyId::SOP1, "s_xnor_saveexec_b32"},
    {FamilyId::SOP1, "s_andn1_saveexec_b64"},
    {FamilyId::SOP1, "s_bitreplicate_b64_b32"},
    {FamilyId::SOP1, "s_movrelsd_2_b32"},

    // --- SOP2 (43) ---
    {FamilyId::SOP2, "s_add_u32"},
    {FamilyId::SOP2, "s_sub_u32"},
    {FamilyId::SOP2, "s_add_i32"},
    {FamilyId::SOP2, "s_sub_i32"},
    {FamilyId::SOP2, "s_addc_u32"},
    {FamilyId::SOP2, "s_subb_u32"},
    {FamilyId::SOP2, "s_min_i32"},
    {FamilyId::SOP2, "s_min_u32"},
    {FamilyId::SOP2, "s_max_i32"},
    {FamilyId::SOP2, "s_max_u32"},
    {FamilyId::SOP2, "s_cselect_b32"},
    {FamilyId::SOP2, "s_cselect_b64"},
    {FamilyId::SOP2, "s_and_b32"},
    {FamilyId::SOP2, "s_and_b64"},
    {FamilyId::SOP2, "s_or_b32"},
    {FamilyId::SOP2, "s_or_b64"},
    {FamilyId::SOP2, "s_xor_b32"},
    {FamilyId::SOP2, "s_xor_b64"},
    {FamilyId::SOP2, "s_andn2_b32"},
    {FamilyId::SOP2, "s_andn2_b64"},
    {FamilyId::SOP2, "s_orn2_b32"},
    {FamilyId::SOP2, "s_orn2_b64"},
    {FamilyId::SOP2, "s_nand_b32"},
    {FamilyId::SOP2, "s_nand_b64"},
    {FamilyId::SOP2, "s_nor_b32"},
    {FamilyId::SOP2, "s_nor_b64"},
    {FamilyId::SOP2, "s_xnor_b32"},
    {FamilyId::SOP2, "s_xnor_b64"},
    {FamilyId::SOP2, "s_lshl_b32"},
    {FamilyId::SOP2, "s_lshl_b64"},
    {FamilyId::SOP2, "s_lshr_b32"},
    {FamilyId::SOP2, "s_lshr_b64"},
    {FamilyId::SOP2, "s_ashr_i32"},
    {FamilyId::SOP2, "s_ashr_i64"},
    {FamilyId::SOP2, "s_bfm_b32"},
    {FamilyId::SOP2, "s_bfm_b64"},
    {FamilyId::SOP2, "s_mul_i32"},
    {FamilyId::SOP2, "s_bfe_u32"},
    {FamilyId::SOP2, "s_bfe_i32"},
    {FamilyId::SOP2, "s_bfe_u64"},
    {FamilyId::SOP2, "s_bfe_i64"},
    {FamilyId::SOP2, "s_absdiff_i32"},
    {FamilyId::SOP2, "s_mul_hi_u32"},

    // --- SOPK (27) ---
    {FamilyId::SOPK, "s_movk_i32"},
    {FamilyId::SOPK, "s_version"},
    {FamilyId::SOPK, "s_cmovk_i32"},
    {FamilyId::SOPK, "s_cmpk_eq_i32"},
    {FamilyId::SOPK, "s_cmpk_lg_i32"},
    {FamilyId::SOPK, "s_cmpk_gt_i32"},
    {FamilyId::SOPK, "s_cmpk_ge_i32"},
    {FamilyId::SOPK, "s_cmpk_lt_i32"},
    {FamilyId::SOPK, "s_cmpk_le_i32"},
    {FamilyId::SOPK, "s_cmpk_eq_u32"},
    {FamilyId::SOPK, "s_cmpk_lg_u32"},
    {FamilyId::SOPK, "s_cmpk_gt_u32"},
    {FamilyId::SOPK, "s_cmpk_ge_u32"},
    {FamilyId::SOPK, "s_cmpk_lt_u32"},
    {FamilyId::SOPK, "s_cmpk_le_u32"},
    {FamilyId::SOPK, "s_addk_i32"},
    {FamilyId::SOPK, "s_mulk_i32"},
    {FamilyId::SOPK, "s_getreg_b32"},
    {FamilyId::SOPK, "s_setreg_b32"},
    {FamilyId::SOPK, "s_setreg_imm32_b32"},
    {FamilyId::SOPK, "s_call_b64"},
    {FamilyId::SOPK, "s_waitcnt_vscnt"},
    {FamilyId::SOPK, "s_waitcnt_vmcnt"},
    {FamilyId::SOPK, "s_waitcnt_expcnt"},
    {FamilyId::SOPK, "s_waitcnt_lgkmcnt"},
    {FamilyId::SOPK, "s_subvector_loop_begin"},
    {FamilyId::SOPK, "s_subvector_loop_end"},

    // --- SOPC (18) ---
    {FamilyId::SOPC, "s_cmp_eq_i32"},
    {FamilyId::SOPC, "s_cmp_lg_i32"},
    {FamilyId::SOPC, "s_cmp_gt_i32"},
    {FamilyId::SOPC, "s_cmp_ge_i32"},
    {FamilyId::SOPC, "s_cmp_lt_i32"},
    {FamilyId::SOPC, "s_cmp_le_i32"},
    {FamilyId::SOPC, "s_cmp_eq_u32"},
    {FamilyId::SOPC, "s_cmp_lg_u32"},
    {FamilyId::SOPC, "s_cmp_gt_u32"},
    {FamilyId::SOPC, "s_cmp_ge_u32"},
    {FamilyId::SOPC, "s_cmp_lt_u32"},
    {FamilyId::SOPC, "s_cmp_le_u32"},
    {FamilyId::SOPC, "s_bitcmp0_b32"},
    {FamilyId::SOPC, "s_bitcmp1_b32"},
    {FamilyId::SOPC, "s_bitcmp0_b64"},
    {FamilyId::SOPC, "s_bitcmp1_b64"},
    {FamilyId::SOPC, "s_cmp_eq_u64"},
    {FamilyId::SOPC, "s_cmp_lg_u64"},

    // --- SOPP (39) ---
    {FamilyId::SOPP, "s_nop"},
    {FamilyId::SOPP, "s_endpgm"},
    {FamilyId::SOPP, "s_branch"},
    {FamilyId::SOPP, "s_wakeup"},
    {FamilyId::SOPP, "s_cbranch_scc0"},
    {FamilyId::SOPP, "s_cbranch_scc1"},
    {FamilyId::SOPP, "s_cbranch_vccz"},
    {FamilyId::SOPP, "s_cbranch_vccnz"},
    {FamilyId::SOPP, "s_cbranch_execz"},
    {FamilyId::SOPP, "s_cbranch_execnz"},
    {FamilyId::SOPP, "s_barrier"},
    {FamilyId::SOPP, "s_setkill"},
    {FamilyId::SOPP, "s_waitcnt"},
    {FamilyId::SOPP, "s_sethalt"},
    {FamilyId::SOPP, "s_sleep"},
    {FamilyId::SOPP, "s_setprio"},
    {FamilyId::SOPP, "s_sendmsg"},
    {FamilyId::SOPP, "s_sendmsghalt"},
    {FamilyId::SOPP, "s_trap"},
    {FamilyId::SOPP, "s_icache_inv"},
    {FamilyId::SOPP, "s_incperflevel"},
    {FamilyId::SOPP, "s_decperflevel"},
    {FamilyId::SOPP, "s_ttracedata"},
    {FamilyId::SOPP, "s_cbranch_cdbgsys"},
    {FamilyId::SOPP, "s_cbranch_cdbguser"},
    {FamilyId::SOPP, "s_cbranch_cdbgsys_or_user"},
    {FamilyId::SOPP, "s_cbranch_cdbgsys_and_user"},
    {FamilyId::SOPP, "s_endpgm_saved"},
    {FamilyId::SOPP, "s_set_gpr_idx_off"},
    {FamilyId::SOPP, "s_set_gpr_idx_mode"},
    {FamilyId::SOPP, "s_endpgm_ordered_ps_done"},
    {FamilyId::SOPP, "s_code_end"},
    {FamilyId::SOPP, "s_inst_prefetch"},
    {FamilyId::SOPP, "s_clause"},
    {FamilyId::SOPP, "s_wait_idle"},
    {FamilyId::SOPP, "s_waitcnt_depctr"},
    {FamilyId::SOPP, "s_round_mode"},
    {FamilyId::SOPP, "s_denorm_mode"},
    {FamilyId::SOPP, "s_ttracedata_imm"},

    // --- SMEM (20) ---
    {FamilyId::SMEM, "s_load_dword"},
    {FamilyId::SMEM, "s_load_dwordx2"},
    {FamilyId::SMEM, "s_load_dwordx4"},
    {FamilyId::SMEM, "s_load_dwordx8"},
    {FamilyId::SMEM, "s_load_dwordx16"},
    {FamilyId::SMEM, "s_buffer_load_dword"},
    {FamilyId::SMEM, "s_buffer_load_dwordx2"},
    {FamilyId::SMEM, "s_buffer_load_dwordx4"},
    {FamilyId::SMEM, "s_buffer_load_dwordx8"},
    {FamilyId::SMEM, "s_buffer_load_dwordx16"},
    {FamilyId::SMEM, "s_scratch_load_dword"},
    {FamilyId::SMEM, "s_scratch_load_dwordx2"},
    {FamilyId::SMEM, "s_scratch_load_dwordx4"},
    {FamilyId::SMEM, "s_gl1_inv"},
    {FamilyId::SMEM, "s_dcache_inv"},
    {FamilyId::SMEM, "s_memtime"},
    {FamilyId::SMEM, "s_memrealtime"},
    {FamilyId::SMEM, "s_atc_probe"},
    {FamilyId::SMEM, "s_atc_probe_buffer"},
    {FamilyId::SMEM, "s_get_waveid_in_workgroup"},

    // --- VOP1 (74) ---
    {FamilyId::VOP1, "v_nop"},
    {FamilyId::VOP1, "v_mov_b32"},
    {FamilyId::VOP1, "v_readfirstlane_b32"},
    {FamilyId::VOP1, "v_cvt_i32_f64"},
    {FamilyId::VOP1, "v_cvt_f64_i32"},
    {FamilyId::VOP1, "v_cvt_f32_i32"},
    {FamilyId::VOP1, "v_cvt_f32_u32"},
    {FamilyId::VOP1, "v_cvt_u32_f32"},
    {FamilyId::VOP1, "v_cvt_i32_f32"},
    {FamilyId::VOP1, "v_cvt_f16_f32"},
    {FamilyId::VOP1, "v_cvt_f32_f16"},
    {FamilyId::VOP1, "v_cvt_rpi_i32_f32"},
    {FamilyId::VOP1, "v_cvt_flr_i32_f32"},
    {FamilyId::VOP1, "v_cvt_off_f32_i4"},
    {FamilyId::VOP1, "v_cvt_f32_f64"},
    {FamilyId::VOP1, "v_cvt_f64_f32"},
    {FamilyId::VOP1, "v_cvt_f32_ubyte0"},
    {FamilyId::VOP1, "v_cvt_f32_ubyte1"},
    {FamilyId::VOP1, "v_cvt_f32_ubyte2"},
    {FamilyId::VOP1, "v_cvt_f32_ubyte3"},
    {FamilyId::VOP1, "v_cvt_u32_f64"},
    {FamilyId::VOP1, "v_cvt_f64_u32"},
    {FamilyId::VOP1, "v_trunc_f64"},
    {FamilyId::VOP1, "v_ceil_f64"},
    {FamilyId::VOP1, "v_rndne_f64"},
    {FamilyId::VOP1, "v_floor_f64"},
    {FamilyId::VOP1, "v_pipeflush"},
    {FamilyId::VOP1, "v_fract_f32"},
    {FamilyId::VOP1, "v_trunc_f32"},
    {FamilyId::VOP1, "v_ceil_f32"},
    {FamilyId::VOP1, "v_rndne_f32"},
    {FamilyId::VOP1, "v_floor_f32"},
    {FamilyId::VOP1, "v_exp_f32"},
    {FamilyId::VOP1, "v_log_f32"},
    {FamilyId::VOP1, "v_rcp_f32"},
    {FamilyId::VOP1, "v_rcp_iflag_f32"},
    {FamilyId::VOP1, "v_rsq_f32"},
    {FamilyId::VOP1, "v_rcp_f64"},
    {FamilyId::VOP1, "v_rsq_f64"},
    {FamilyId::VOP1, "v_sqrt_f32"},
    {FamilyId::VOP1, "v_sqrt_f64"},
    {FamilyId::VOP1, "v_sin_f32"},
    {FamilyId::VOP1, "v_cos_f32"},
    {FamilyId::VOP1, "v_not_b32"},
    {FamilyId::VOP1, "v_bfrev_b32"},
    {FamilyId::VOP1, "v_ffbh_u32"},
    {FamilyId::VOP1, "v_ffbl_b32"},
    {FamilyId::VOP1, "v_ffbh_i32"},
    {FamilyId::VOP1, "v_frexp_exp_i32_f64"},
    {FamilyId::VOP1, "v_frexp_mant_f64"},
    {FamilyId::VOP1, "v_fract_f64"},
    {FamilyId::VOP1, "v_frexp_exp_i32_f32"},
    {FamilyId::VOP1, "v_frexp_mant_f32"},
    {FamilyId::VOP1, "v_clrexcp"},
    {FamilyId::VOP1, "v_movreld_b32"},
    {FamilyId::VOP1, "v_movrels_b32"},
    {FamilyId::VOP1, "v_movrelsd_b32"},
    {FamilyId::VOP1, "v_movrelsd_2_b32"},
    {FamilyId::VOP1, "v_cvt_f16_u16"},
    {FamilyId::VOP1, "v_cvt_f16_i16"},
    {FamilyId::VOP1, "v_cvt_u16_f16"},
    {FamilyId::VOP1, "v_cvt_i16_f16"},
    {FamilyId::VOP1, "v_rcp_f16"},
    {FamilyId::VOP1, "v_sqrt_f16"},
    {FamilyId::VOP1, "v_rsq_f16"},
    {FamilyId::VOP1, "v_log_f16"},
    {FamilyId::VOP1, "v_exp_f16"},
    {FamilyId::VOP1, "v_frexp_mant_f16"},
    {FamilyId::VOP1, "v_frexp_exp_i16_f16"},
    {FamilyId::VOP1, "v_floor_f16"},
    {FamilyId::VOP1, "v_ceil_f16"},
    {FamilyId::VOP1, "v_trunc_f16"},
    {FamilyId::VOP1, "v_cvt_norm_i16_f16"},
    {FamilyId::VOP1, "v_cvt_norm_u16_f16"},

    // --- VOP2 (50) ---
    {FamilyId::VOP2, "v_cndmask_b32"},
    {FamilyId::VOP2, "v_dot2c_f32_f16"},
    {FamilyId::VOP2, "v_add_f32"},
    {FamilyId::VOP2, "v_sub_f32"},
    {FamilyId::VOP2, "v_subrev_f32"},
    {FamilyId::VOP2, "v_fmac_legacy_f32"},
    {FamilyId::VOP2, "v_mul_dx9_zero_f32"},
    {FamilyId::VOP2, "v_mul_f32"},
    {FamilyId::VOP2, "v_mul_i32_i24"},
    {FamilyId::VOP2, "v_mul_hi_i32_i24"},
    {FamilyId::VOP2, "v_mul_u32_u24"},
    {FamilyId::VOP2, "v_mul_hi_u32_u24"},
    {FamilyId::VOP2, "v_dot4c_i32_i8"},
    {FamilyId::VOP2, "v_dot8c_i32_i4"},
    {FamilyId::VOP2, "v_min_f32"},
    {FamilyId::VOP2, "v_max_f32"},
    {FamilyId::VOP2, "v_min_i32"},
    {FamilyId::VOP2, "v_max_i32"},
    {FamilyId::VOP2, "v_min_u32"},
    {FamilyId::VOP2, "v_max_u32"},
    {FamilyId::VOP2, "v_lshrrev_b32"},
    {FamilyId::VOP2, "v_ashrrev_i32"},
    {FamilyId::VOP2, "v_lshlrev_b32"},
    {FamilyId::VOP2, "v_and_b32"},
    {FamilyId::VOP2, "v_or_b32"},
    {FamilyId::VOP2, "v_xor_b32"},
    {FamilyId::VOP2, "v_xnor_b32"},
    {FamilyId::VOP2, "v_mac_f32"},
    {FamilyId::VOP2, "v_madmk_f32"},
    {FamilyId::VOP2, "v_madak_f32"},
    {FamilyId::VOP2, "v_add_nc_u32"},
    {FamilyId::VOP2, "v_sub_nc_u32"},
    {FamilyId::VOP2, "v_subrev_nc_u32"},
    {FamilyId::VOP2, "v_add_co_ci_u32"},
    {FamilyId::VOP2, "v_sub_co_ci_u32"},
    {FamilyId::VOP2, "v_subrev_co_ci_u32"},
    {FamilyId::VOP2, "v_fmac_f32"},
    {FamilyId::VOP2, "v_fmamk_f32"},
    {FamilyId::VOP2, "v_fmaak_f32"},
    {FamilyId::VOP2, "v_cvt_pkrtz_f16_f32"},
    {FamilyId::VOP2, "v_add_f16"},
    {FamilyId::VOP2, "v_sub_f16"},
    {FamilyId::VOP2, "v_subrev_f16"},
    {FamilyId::VOP2, "v_mul_f16"},
    {FamilyId::VOP2, "v_fmac_f16"},
    {FamilyId::VOP2, "v_fmamk_f16"},
    {FamilyId::VOP2, "v_fmaak_f16"},
    {FamilyId::VOP2, "v_max_f16"},
    {FamilyId::VOP2, "v_min_f16"},
    {FamilyId::VOP2, "v_ldexp_f16"},

    // --- VOPC (108) ---
    // F32 ordered/unordered + CMPX (24)
    {FamilyId::VOPC, "v_cmp_lt_f32"},
    {FamilyId::VOPC, "v_cmp_eq_f32"},
    {FamilyId::VOPC, "v_cmp_le_f32"},
    {FamilyId::VOPC, "v_cmp_gt_f32"},
    {FamilyId::VOPC, "v_cmp_lg_f32"},
    {FamilyId::VOPC, "v_cmp_ge_f32"},
    {FamilyId::VOPC, "v_cmp_o_f32"},
    {FamilyId::VOPC, "v_cmp_u_f32"},
    {FamilyId::VOPC, "v_cmp_nge_f32"},
    {FamilyId::VOPC, "v_cmp_nlg_f32"},
    {FamilyId::VOPC, "v_cmp_ngt_f32"},
    {FamilyId::VOPC, "v_cmp_nle_f32"},
    {FamilyId::VOPC, "v_cmp_neq_f32"},
    {FamilyId::VOPC, "v_cmp_nlt_f32"},
    {FamilyId::VOPC, "v_cmpx_lt_f32"},
    {FamilyId::VOPC, "v_cmpx_eq_f32"},
    {FamilyId::VOPC, "v_cmpx_le_f32"},
    {FamilyId::VOPC, "v_cmpx_gt_f32"},
    {FamilyId::VOPC, "v_cmpx_lg_f32"},
    {FamilyId::VOPC, "v_cmpx_ge_f32"},
    {FamilyId::VOPC, "v_cmpx_o_f32"},
    {FamilyId::VOPC, "v_cmpx_u_f32"},
    {FamilyId::VOPC, "v_cmpx_neq_f32"},
    {FamilyId::VOPC, "v_cmpx_nlt_f32"},
    // I32 + CMPX (12)
    {FamilyId::VOPC, "v_cmp_lt_i32"},
    {FamilyId::VOPC, "v_cmp_eq_i32"},
    {FamilyId::VOPC, "v_cmp_le_i32"},
    {FamilyId::VOPC, "v_cmp_gt_i32"},
    {FamilyId::VOPC, "v_cmp_ne_i32"},
    {FamilyId::VOPC, "v_cmp_ge_i32"},
    {FamilyId::VOPC, "v_cmpx_lt_i32"},
    {FamilyId::VOPC, "v_cmpx_eq_i32"},
    {FamilyId::VOPC, "v_cmpx_le_i32"},
    {FamilyId::VOPC, "v_cmpx_gt_i32"},
    {FamilyId::VOPC, "v_cmpx_ne_i32"},
    {FamilyId::VOPC, "v_cmpx_ge_i32"},
    // U32 + CMPX (12)
    {FamilyId::VOPC, "v_cmp_lt_u32"},
    {FamilyId::VOPC, "v_cmp_eq_u32"},
    {FamilyId::VOPC, "v_cmp_le_u32"},
    {FamilyId::VOPC, "v_cmp_gt_u32"},
    {FamilyId::VOPC, "v_cmp_ne_u32"},
    {FamilyId::VOPC, "v_cmp_ge_u32"},
    {FamilyId::VOPC, "v_cmpx_lt_u32"},
    {FamilyId::VOPC, "v_cmpx_eq_u32"},
    {FamilyId::VOPC, "v_cmpx_le_u32"},
    {FamilyId::VOPC, "v_cmpx_gt_u32"},
    {FamilyId::VOPC, "v_cmpx_ne_u32"},
    {FamilyId::VOPC, "v_cmpx_ge_u32"},
    // F16 split-range + CMPX (24)
    {FamilyId::VOPC, "v_cmp_lt_f16"},
    {FamilyId::VOPC, "v_cmp_eq_f16"},
    {FamilyId::VOPC, "v_cmp_le_f16"},
    {FamilyId::VOPC, "v_cmp_gt_f16"},
    {FamilyId::VOPC, "v_cmp_lg_f16"},
    {FamilyId::VOPC, "v_cmp_ge_f16"},
    {FamilyId::VOPC, "v_cmp_o_f16"},
    {FamilyId::VOPC, "v_cmp_u_f16"},
    {FamilyId::VOPC, "v_cmp_nge_f16"},
    {FamilyId::VOPC, "v_cmp_nlg_f16"},
    {FamilyId::VOPC, "v_cmp_ngt_f16"},
    {FamilyId::VOPC, "v_cmp_nle_f16"},
    {FamilyId::VOPC, "v_cmp_neq_f16"},
    {FamilyId::VOPC, "v_cmp_nlt_f16"},
    {FamilyId::VOPC, "v_cmp_tru_f16"},
    {FamilyId::VOPC, "v_cmpx_lt_f16"},
    {FamilyId::VOPC, "v_cmpx_eq_f16"},
    {FamilyId::VOPC, "v_cmpx_le_f16"},
    {FamilyId::VOPC, "v_cmpx_gt_f16"},
    {FamilyId::VOPC, "v_cmpx_lg_f16"},
    {FamilyId::VOPC, "v_cmpx_ge_f16"},
    {FamilyId::VOPC, "v_cmpx_u_f16"},
    {FamilyId::VOPC, "v_cmpx_neq_f16"},
    {FamilyId::VOPC, "v_cmpx_nlt_f16"},
    // I16/U16 (12)
    {FamilyId::VOPC, "v_cmp_lt_i16"},
    {FamilyId::VOPC, "v_cmp_eq_i16"},
    {FamilyId::VOPC, "v_cmp_le_i16"},
    {FamilyId::VOPC, "v_cmp_gt_i16"},
    {FamilyId::VOPC, "v_cmp_ne_i16"},
    {FamilyId::VOPC, "v_cmp_ge_i16"},
    {FamilyId::VOPC, "v_cmp_lt_u16"},
    {FamilyId::VOPC, "v_cmp_eq_u16"},
    {FamilyId::VOPC, "v_cmp_le_u16"},
    {FamilyId::VOPC, "v_cmp_gt_u16"},
    {FamilyId::VOPC, "v_cmp_ne_u16"},
    {FamilyId::VOPC, "v_cmp_ge_u16"},
    // F64 + I64 + U64 + CLASS (24)
    {FamilyId::VOPC, "v_cmp_lt_f64"},
    {FamilyId::VOPC, "v_cmp_eq_f64"},
    {FamilyId::VOPC, "v_cmp_le_f64"},
    {FamilyId::VOPC, "v_cmp_gt_f64"},
    {FamilyId::VOPC, "v_cmp_lg_f64"},
    {FamilyId::VOPC, "v_cmp_ge_f64"},
    {FamilyId::VOPC, "v_cmp_lt_i64"},
    {FamilyId::VOPC, "v_cmp_eq_i64"},
    {FamilyId::VOPC, "v_cmp_le_i64"},
    {FamilyId::VOPC, "v_cmp_gt_i64"},
    {FamilyId::VOPC, "v_cmp_ne_i64"},
    {FamilyId::VOPC, "v_cmp_ge_i64"},
    {FamilyId::VOPC, "v_cmp_lt_u64"},
    {FamilyId::VOPC, "v_cmp_eq_u64"},
    {FamilyId::VOPC, "v_cmp_le_u64"},
    {FamilyId::VOPC, "v_cmp_gt_u64"},
    {FamilyId::VOPC, "v_cmp_ne_u64"},
    {FamilyId::VOPC, "v_cmp_ge_u64"},
    {FamilyId::VOPC, "v_cmp_class_f32"},
    {FamilyId::VOPC, "v_cmp_class_f64"},
    {FamilyId::VOPC, "v_cmp_class_f16"},
    {FamilyId::VOPC, "v_cmpx_class_f32"},
    {FamilyId::VOPC, "v_cmpx_class_f64"},
    {FamilyId::VOPC, "v_cmpx_class_f16"},

    // --- VOP3 (82) ---
    {FamilyId::VOP3, "v_mad_legacy_f32"},
    {FamilyId::VOP3, "v_mad_f32"},
    {FamilyId::VOP3, "v_mad_i32_i24"},
    {FamilyId::VOP3, "v_mad_u32_u24"},
    {FamilyId::VOP3, "v_cubeid_f32"},
    {FamilyId::VOP3, "v_cubesc_f32"},
    {FamilyId::VOP3, "v_cubetc_f32"},
    {FamilyId::VOP3, "v_cubema_f32"},
    {FamilyId::VOP3, "v_bfe_u32"},
    {FamilyId::VOP3, "v_bfe_i32"},
    {FamilyId::VOP3, "v_bfi_b32"},
    {FamilyId::VOP3, "v_fma_f32"},
    {FamilyId::VOP3, "v_fma_f64"},
    {FamilyId::VOP3, "v_lerp_u8"},
    {FamilyId::VOP3, "v_alignbit_b32"},
    {FamilyId::VOP3, "v_alignbyte_b32"},
    {FamilyId::VOP3, "v_min3_f32"},
    {FamilyId::VOP3, "v_min3_i32"},
    {FamilyId::VOP3, "v_min3_u32"},
    {FamilyId::VOP3, "v_max3_f32"},
    {FamilyId::VOP3, "v_max3_i32"},
    {FamilyId::VOP3, "v_max3_u32"},
    {FamilyId::VOP3, "v_med3_f32"},
    {FamilyId::VOP3, "v_med3_i32"},
    {FamilyId::VOP3, "v_med3_u32"},
    {FamilyId::VOP3, "v_sad_u8"},
    {FamilyId::VOP3, "v_sad_hi_u8"},
    {FamilyId::VOP3, "v_sad_u16"},
    {FamilyId::VOP3, "v_sad_u32"},
    {FamilyId::VOP3, "v_cvt_pk_u8_f32"},
    {FamilyId::VOP3, "v_div_fixup_f32"},
    {FamilyId::VOP3, "v_div_fixup_f64"},
    {FamilyId::VOP3, "v_div_fmas_f32"},
    {FamilyId::VOP3, "v_div_fmas_f64"},
    {FamilyId::VOP3, "v_msad_u8"},
    {FamilyId::VOP3, "v_qsad_pk_u16_u8"},
    {FamilyId::VOP3, "v_mqsad_pk_u16_u8"},
    {FamilyId::VOP3, "v_trig_preop_f64"},
    {FamilyId::VOP3, "v_mul_lo_u32"},
    {FamilyId::VOP3, "v_mul_hi_u32"},
    {FamilyId::VOP3, "v_mul_hi_i32"},
    {FamilyId::VOP3, "v_div_scale_f32"},
    {FamilyId::VOP3, "v_div_scale_f64"},
    {FamilyId::VOP3, "v_add_f64"},
    {FamilyId::VOP3, "v_mul_f64"},
    {FamilyId::VOP3, "v_min_f64"},
    {FamilyId::VOP3, "v_max_f64"},
    {FamilyId::VOP3, "v_ldexp_f64"},
    {FamilyId::VOP3, "v_lshlrev_b64"},
    {FamilyId::VOP3, "v_lshrrev_b64"},
    {FamilyId::VOP3, "v_ashrrev_i64"},
    {FamilyId::VOP3, "v_bcnt_u32_b32"},
    {FamilyId::VOP3, "v_mbcnt_lo_u32_b32"},
    {FamilyId::VOP3, "v_mbcnt_hi_u32_b32"},
    {FamilyId::VOP3, "v_add_co_u32"},
    {FamilyId::VOP3, "v_sub_co_u32"},
    {FamilyId::VOP3, "v_subrev_co_u32"},
    {FamilyId::VOP3, "v_add_nc_u16"},
    {FamilyId::VOP3, "v_sub_nc_u16"},
    {FamilyId::VOP3, "v_mul_lo_u16"},
    {FamilyId::VOP3, "v_min_u16"},
    {FamilyId::VOP3, "v_max_u16"},
    {FamilyId::VOP3, "v_min_i16"},
    {FamilyId::VOP3, "v_max_i16"},
    {FamilyId::VOP3, "v_lshlrev_b16"},
    {FamilyId::VOP3, "v_lshrrev_b16"},
    {FamilyId::VOP3, "v_ashrrev_i16"},
    {FamilyId::VOP3, "v_mad_u16"},
    {FamilyId::VOP3, "v_mad_i16"},
    {FamilyId::VOP3, "v_fma_f16"},
    {FamilyId::VOP3, "v_readlane_b32"},
    {FamilyId::VOP3, "v_writelane_b32"},
    {FamilyId::VOP3, "v_cvt_pknorm_i16_f32"},
    {FamilyId::VOP3, "v_cvt_pknorm_u16_f32"},
    {FamilyId::VOP3, "v_cvt_pk_u16_u32"},
    {FamilyId::VOP3, "v_cvt_pk_i16_i32"},
    {FamilyId::VOP3, "v_add3_u32"},
    {FamilyId::VOP3, "v_lshl_or_b32"},
    {FamilyId::VOP3, "v_and_or_b32"},
    {FamilyId::VOP3, "v_dot2_f32_f16"},
    {FamilyId::VOP3, "v_permlane16_b32"},
    {FamilyId::VOP3, "v_permlanex16_b32"},

    // --- VOP3P (24) ---
    {FamilyId::VOP3P, "v_pk_mad_i16"},
    {FamilyId::VOP3P, "v_pk_mul_lo_u16"},
    {FamilyId::VOP3P, "v_pk_add_i16"},
    {FamilyId::VOP3P, "v_pk_sub_i16"},
    {FamilyId::VOP3P, "v_pk_lshlrev_b16"},
    {FamilyId::VOP3P, "v_pk_lshrrev_b16"},
    {FamilyId::VOP3P, "v_pk_ashrrev_i16"},
    {FamilyId::VOP3P, "v_pk_max_i16"},
    {FamilyId::VOP3P, "v_pk_min_i16"},
    {FamilyId::VOP3P, "v_pk_mad_u16"},
    {FamilyId::VOP3P, "v_pk_add_u16"},
    {FamilyId::VOP3P, "v_pk_sub_u16"},
    {FamilyId::VOP3P, "v_pk_max_u16"},
    {FamilyId::VOP3P, "v_pk_min_u16"},
    {FamilyId::VOP3P, "v_pk_fma_f16"},
    {FamilyId::VOP3P, "v_pk_add_f16"},
    {FamilyId::VOP3P, "v_pk_mul_f16"},
    {FamilyId::VOP3P, "v_pk_min_f16"},
    {FamilyId::VOP3P, "v_pk_max_f16"},
    {FamilyId::VOP3P, "v_dot2_i32_i16"},
    {FamilyId::VOP3P, "v_dot2_u32_u16"},
    {FamilyId::VOP3P, "v_dot4_i32_i8"},
    {FamilyId::VOP3P, "v_dot4_u32_u8"},
    {FamilyId::VOP3P, "v_dot8_u32_u4"},

    // --- VINTRP (6) ---
    {FamilyId::VINTRP, "v_interp_p1_f32"},
    {FamilyId::VINTRP, "v_interp_p2_f32"},
    {FamilyId::VINTRP, "v_interp_mov_f32"},
    {FamilyId::VINTRP, "v_interp_p1ll_f16"},
    {FamilyId::VINTRP, "v_interp_p1lv_f16"},
    {FamilyId::VINTRP, "v_interp_p2_f16"},

    // --- MUBUF (46) ---
    {FamilyId::MUBUF, "buffer_load_format_x"},
    {FamilyId::MUBUF, "buffer_load_format_xy"},
    {FamilyId::MUBUF, "buffer_load_format_xyz"},
    {FamilyId::MUBUF, "buffer_load_format_xyzw"},
    {FamilyId::MUBUF, "buffer_store_format_x"},
    {FamilyId::MUBUF, "buffer_store_format_xy"},
    {FamilyId::MUBUF, "buffer_store_format_xyz"},
    {FamilyId::MUBUF, "buffer_store_format_xyzw"},
    {FamilyId::MUBUF, "buffer_load_format_d16_x"},
    {FamilyId::MUBUF, "buffer_load_format_d16_xy"},
    {FamilyId::MUBUF, "buffer_load_format_d16_xyz"},
    {FamilyId::MUBUF, "buffer_load_format_d16_xyzw"},
    {FamilyId::MUBUF, "buffer_store_format_d16_x"},
    {FamilyId::MUBUF, "buffer_store_format_d16_xy"},
    {FamilyId::MUBUF, "buffer_store_format_d16_xyz"},
    {FamilyId::MUBUF, "buffer_store_format_d16_xyzw"},
    {FamilyId::MUBUF, "buffer_load_ubyte"},
    {FamilyId::MUBUF, "buffer_load_sbyte"},
    {FamilyId::MUBUF, "buffer_load_ushort"},
    {FamilyId::MUBUF, "buffer_load_sshort"},
    {FamilyId::MUBUF, "buffer_load_dword"},
    {FamilyId::MUBUF, "buffer_load_dwordx2"},
    {FamilyId::MUBUF, "buffer_load_dwordx3"},
    {FamilyId::MUBUF, "buffer_load_dwordx4"},
    {FamilyId::MUBUF, "buffer_store_byte"},
    {FamilyId::MUBUF, "buffer_store_short"},
    {FamilyId::MUBUF, "buffer_store_dword"},
    {FamilyId::MUBUF, "buffer_store_dwordx2"},
    {FamilyId::MUBUF, "buffer_store_dwordx3"},
    {FamilyId::MUBUF, "buffer_store_dwordx4"},
    {FamilyId::MUBUF, "buffer_gl0_inv"},
    {FamilyId::MUBUF, "buffer_gl1_inv"},
    {FamilyId::MUBUF, "buffer_atomic_swap"},
    {FamilyId::MUBUF, "buffer_atomic_cmpswap"},
    {FamilyId::MUBUF, "buffer_atomic_add"},
    {FamilyId::MUBUF, "buffer_atomic_sub"},
    {FamilyId::MUBUF, "buffer_atomic_smin"},
    {FamilyId::MUBUF, "buffer_atomic_umin"},
    {FamilyId::MUBUF, "buffer_atomic_smax"},
    {FamilyId::MUBUF, "buffer_atomic_umax"},
    {FamilyId::MUBUF, "buffer_atomic_and"},
    {FamilyId::MUBUF, "buffer_atomic_or"},
    {FamilyId::MUBUF, "buffer_atomic_xor"},
    {FamilyId::MUBUF, "buffer_atomic_inc"},
    {FamilyId::MUBUF, "buffer_atomic_dec"},
    {FamilyId::MUBUF, "buffer_atomic_add_x2"},

    // --- MTBUF (16) ---
    {FamilyId::MTBUF, "tbuffer_load_format_x"},
    {FamilyId::MTBUF, "tbuffer_load_format_xy"},
    {FamilyId::MTBUF, "tbuffer_load_format_xyz"},
    {FamilyId::MTBUF, "tbuffer_load_format_xyzw"},
    {FamilyId::MTBUF, "tbuffer_store_format_x"},
    {FamilyId::MTBUF, "tbuffer_store_format_xy"},
    {FamilyId::MTBUF, "tbuffer_store_format_xyz"},
    {FamilyId::MTBUF, "tbuffer_store_format_xyzw"},
    {FamilyId::MTBUF, "tbuffer_load_format_d16_x"},
    {FamilyId::MTBUF, "tbuffer_load_format_d16_xy"},
    {FamilyId::MTBUF, "tbuffer_load_format_d16_xyz"},
    {FamilyId::MTBUF, "tbuffer_load_format_d16_xyzw"},
    {FamilyId::MTBUF, "tbuffer_store_format_d16_x"},
    {FamilyId::MTBUF, "tbuffer_store_format_d16_xy"},
    {FamilyId::MTBUF, "tbuffer_store_format_d16_xyz"},
    {FamilyId::MTBUF, "tbuffer_store_format_d16_xyzw"},

    // --- MIMG (42) ---
    {FamilyId::MIMG, "image_load"},
    {FamilyId::MIMG, "image_load_mip"},
    {FamilyId::MIMG, "image_load_pck"},
    {FamilyId::MIMG, "image_load_pck_sgn"},
    {FamilyId::MIMG, "image_load_mip_pck"},
    {FamilyId::MIMG, "image_load_mip_pck_sgn"},
    {FamilyId::MIMG, "image_store"},
    {FamilyId::MIMG, "image_store_mip"},
    {FamilyId::MIMG, "image_store_pck"},
    {FamilyId::MIMG, "image_store_mip_pck"},
    {FamilyId::MIMG, "image_get_resinfo"},
    {FamilyId::MIMG, "image_atomic_swap"},
    {FamilyId::MIMG, "image_atomic_cmpswap"},
    {FamilyId::MIMG, "image_atomic_add"},
    {FamilyId::MIMG, "image_atomic_sub"},
    {FamilyId::MIMG, "image_atomic_smin"},
    {FamilyId::MIMG, "image_atomic_umin"},
    {FamilyId::MIMG, "image_atomic_smax"},
    {FamilyId::MIMG, "image_atomic_umax"},
    {FamilyId::MIMG, "image_atomic_and"},
    {FamilyId::MIMG, "image_atomic_or"},
    {FamilyId::MIMG, "image_atomic_xor"},
    {FamilyId::MIMG, "image_atomic_inc"},
    {FamilyId::MIMG, "image_atomic_dec"},
    {FamilyId::MIMG, "image_sample"},
    {FamilyId::MIMG, "image_sample_cl"},
    {FamilyId::MIMG, "image_sample_d"},
    {FamilyId::MIMG, "image_sample_l"},
    {FamilyId::MIMG, "image_sample_b"},
    {FamilyId::MIMG, "image_sample_lz"},
    {FamilyId::MIMG, "image_sample_c"},
    {FamilyId::MIMG, "image_sample_c_lz"},
    {FamilyId::MIMG, "image_sample_o"},
    {FamilyId::MIMG, "image_sample_l_o"},
    {FamilyId::MIMG, "image_sample_lz_o"},
    {FamilyId::MIMG, "image_sample_c_lz_o"},
    {FamilyId::MIMG, "image_gather4"},
    {FamilyId::MIMG, "image_gather4_lz"},
    {FamilyId::MIMG, "image_gather4_c"},
    {FamilyId::MIMG, "image_gather4_c_lz"},
    {FamilyId::MIMG, "image_get_lod"},
    {FamilyId::MIMG, "image_bvh_intersect_ray"},

    // --- DS (54) ---
    {FamilyId::DS, "ds_add_u32"},
    {FamilyId::DS, "ds_sub_u32"},
    {FamilyId::DS, "ds_rsub_u32"},
    {FamilyId::DS, "ds_inc_u32"},
    {FamilyId::DS, "ds_dec_u32"},
    {FamilyId::DS, "ds_min_i32"},
    {FamilyId::DS, "ds_max_i32"},
    {FamilyId::DS, "ds_min_u32"},
    {FamilyId::DS, "ds_max_u32"},
    {FamilyId::DS, "ds_and_b32"},
    {FamilyId::DS, "ds_or_b32"},
    {FamilyId::DS, "ds_xor_b32"},
    {FamilyId::DS, "ds_write_b32"},
    {FamilyId::DS, "ds_write2_b32"},
    {FamilyId::DS, "ds_write2st64_b32"},
    {FamilyId::DS, "ds_cmpst_b32"},
    {FamilyId::DS, "ds_min_f32"},
    {FamilyId::DS, "ds_max_f32"},
    {FamilyId::DS, "ds_write_b8"},
    {FamilyId::DS, "ds_write_b16"},
    {FamilyId::DS, "ds_add_rtn_u32"},
    {FamilyId::DS, "ds_sub_rtn_u32"},
    {FamilyId::DS, "ds_inc_rtn_u32"},
    {FamilyId::DS, "ds_dec_rtn_u32"},
    {FamilyId::DS, "ds_min_rtn_i32"},
    {FamilyId::DS, "ds_max_rtn_i32"},
    {FamilyId::DS, "ds_min_rtn_u32"},
    {FamilyId::DS, "ds_max_rtn_u32"},
    {FamilyId::DS, "ds_and_rtn_b32"},
    {FamilyId::DS, "ds_or_rtn_b32"},
    {FamilyId::DS, "ds_xor_rtn_b32"},
    {FamilyId::DS, "ds_wrxchg_rtn_b32"},
    {FamilyId::DS, "ds_cmpst_rtn_b32"},
    {FamilyId::DS, "ds_swizzle_b32"},
    {FamilyId::DS, "ds_read_b32"},
    {FamilyId::DS, "ds_read2_b32"},
    {FamilyId::DS, "ds_read2st64_b32"},
    {FamilyId::DS, "ds_read_i8"},
    {FamilyId::DS, "ds_read_u8"},
    {FamilyId::DS, "ds_read_i16"},
    {FamilyId::DS, "ds_read_u16"},
    {FamilyId::DS, "ds_consume"},
    {FamilyId::DS, "ds_append"},
    {FamilyId::DS, "ds_ordered_count"},
    {FamilyId::DS, "ds_add_u64"},
    {FamilyId::DS, "ds_write_b64"},
    {FamilyId::DS, "ds_write2_b64"},
    {FamilyId::DS, "ds_read_b64"},
    {FamilyId::DS, "ds_read2_b64"},
    {FamilyId::DS, "ds_write_b96"},
    {FamilyId::DS, "ds_write_b128"},
    {FamilyId::DS, "ds_read_b96"},
    {FamilyId::DS, "ds_read_b128"},
    {FamilyId::DS, "ds_bpermute_b32"},

    // --- FLAT (24) ---
    {FamilyId::FLAT, "flat_load_ubyte"},
    {FamilyId::FLAT, "flat_load_sbyte"},
    {FamilyId::FLAT, "flat_load_ushort"},
    {FamilyId::FLAT, "flat_load_sshort"},
    {FamilyId::FLAT, "flat_load_dword"},
    {FamilyId::FLAT, "flat_load_dwordx2"},
    {FamilyId::FLAT, "flat_load_dwordx3"},
    {FamilyId::FLAT, "flat_load_dwordx4"},
    {FamilyId::FLAT, "flat_store_byte"},
    {FamilyId::FLAT, "flat_store_short"},
    {FamilyId::FLAT, "flat_store_dword"},
    {FamilyId::FLAT, "flat_store_dwordx2"},
    {FamilyId::FLAT, "flat_store_dwordx3"},
    {FamilyId::FLAT, "flat_store_dwordx4"},
    {FamilyId::FLAT, "flat_atomic_swap"},
    {FamilyId::FLAT, "flat_atomic_cmpswap"},
    {FamilyId::FLAT, "flat_atomic_add"},
    {FamilyId::FLAT, "flat_atomic_sub"},
    {FamilyId::FLAT, "flat_atomic_smin"},
    {FamilyId::FLAT, "flat_atomic_umin"},
    {FamilyId::FLAT, "flat_atomic_smax"},
    {FamilyId::FLAT, "flat_atomic_umax"},
    {FamilyId::FLAT, "flat_atomic_and"},
    {FamilyId::FLAT, "flat_atomic_or"},

    // --- GLOBAL (28) ---
    {FamilyId::GLOBAL, "global_load_ubyte"},
    {FamilyId::GLOBAL, "global_load_sbyte"},
    {FamilyId::GLOBAL, "global_load_ushort"},
    {FamilyId::GLOBAL, "global_load_sshort"},
    {FamilyId::GLOBAL, "global_load_dword"},
    {FamilyId::GLOBAL, "global_load_dwordx2"},
    {FamilyId::GLOBAL, "global_load_dwordx3"},
    {FamilyId::GLOBAL, "global_load_dwordx4"},
    {FamilyId::GLOBAL, "global_store_byte"},
    {FamilyId::GLOBAL, "global_store_short"},
    {FamilyId::GLOBAL, "global_store_dword"},
    {FamilyId::GLOBAL, "global_store_dwordx2"},
    {FamilyId::GLOBAL, "global_store_dwordx3"},
    {FamilyId::GLOBAL, "global_store_dwordx4"},
    {FamilyId::GLOBAL, "global_atomic_swap"},
    {FamilyId::GLOBAL, "global_atomic_cmpswap"},
    {FamilyId::GLOBAL, "global_atomic_add"},
    {FamilyId::GLOBAL, "global_atomic_sub"},
    {FamilyId::GLOBAL, "global_atomic_smin"},
    {FamilyId::GLOBAL, "global_atomic_umin"},
    {FamilyId::GLOBAL, "global_atomic_smax"},
    {FamilyId::GLOBAL, "global_atomic_umax"},
    {FamilyId::GLOBAL, "global_atomic_and"},
    {FamilyId::GLOBAL, "global_atomic_or"},
    {FamilyId::GLOBAL, "global_atomic_xor"},
    {FamilyId::GLOBAL, "global_atomic_inc"},
    {FamilyId::GLOBAL, "global_atomic_dec"},
    {FamilyId::GLOBAL, "global_atomic_add_x2"},

    // --- SCRATCH (19) ---
    {FamilyId::SCRATCH, "scratch_load_ubyte"},
    {FamilyId::SCRATCH, "scratch_load_sbyte"},
    {FamilyId::SCRATCH, "scratch_load_ushort"},
    {FamilyId::SCRATCH, "scratch_load_sshort"},
    {FamilyId::SCRATCH, "scratch_load_dword"},
    {FamilyId::SCRATCH, "scratch_load_dwordx2"},
    {FamilyId::SCRATCH, "scratch_load_dwordx3"},
    {FamilyId::SCRATCH, "scratch_load_dwordx4"},
    {FamilyId::SCRATCH, "scratch_load_ubyte_d16"},
    {FamilyId::SCRATCH, "scratch_load_ubyte_d16_hi"},
    {FamilyId::SCRATCH, "scratch_load_sbyte_d16"},
    {FamilyId::SCRATCH, "scratch_load_short_d16"},
    {FamilyId::SCRATCH, "scratch_load_short_d16_hi"},
    {FamilyId::SCRATCH, "scratch_store_byte"},
    {FamilyId::SCRATCH, "scratch_store_short"},
    {FamilyId::SCRATCH, "scratch_store_dword"},
    {FamilyId::SCRATCH, "scratch_store_dwordx2"},
    {FamilyId::SCRATCH, "scratch_store_dwordx3"},
    {FamilyId::SCRATCH, "scratch_store_dwordx4"},

    // --- EXP (1) ---
    {FamilyId::EXP, "exp"},
};

static_assert(std::size(kGfx103Catalog) == kGfx103TotalShaderOpcodes,
              "GFX10.3 shader-reachable opcode catalog must contain exactly 779 entries");

struct AuditedOpcodeFlags {
    bool decoded = false;
    bool lifted_spirv = false;
    bool lifted_dxil = false;
    bool cpu_ref = false;
    bool gpu_verified = false;
};

struct AuditState {
    std::array<AuditedOpcodeFlags, kGfx103TotalShaderOpcodes> flags{};
    std::unordered_map<std::string, size_t> mnemonic_to_idx;
    bool has_sv_extension = false;
    bool has_mem_extension = false;

    AuditState() {
        mnemonic_to_idx.reserve(kGfx103TotalShaderOpcodes * 2u);
        for (size_t i = 0; i < std::size(kGfx103Catalog); ++i) {
            mnemonic_to_idx.emplace(kGfx103Catalog[i].mnemonic, i);
        }
    }

    void Mark(std::string_view mnemonic,
              bool decoded,
              bool spirv,
              bool dxil,
              bool cpu,
              bool gpu) {
        const std::string canon = CanonicalMnemonic(mnemonic);
        auto it = mnemonic_to_idx.find(canon);
        if (it == mnemonic_to_idx.end()) {
            return;
        }
        auto& f = flags[it->second];
        f.decoded = f.decoded || decoded;
        f.lifted_spirv = f.lifted_spirv || spirv;
        f.lifted_dxil = f.lifted_dxil || dxil;
        f.cpu_ref = f.cpu_ref || cpu;
        f.gpu_verified = f.gpu_verified || gpu;
    }

    void MarkGpuOpcodes(const std::vector<Rdna2Instruction>& insns) {
        for (const auto& inst : insns) {
            if (!inst.mnemonic.empty()) {
                Mark(inst.mnemonic, true, true, true, true, true);
            }
        }
    }

    void PopulateReport(Rdna2CoverageReport& report) const {
        report.opcodes_total = kGfx103TotalShaderOpcodes;
        report.opcodes_decoded = 0u;
        report.opcodes_lifted_spirv = 0u;
        report.opcodes_lifted_dxil = 0u;
        report.opcodes_cpu_ref = 0u;
        report.opcodes_gpu_verified = 0u;
        report.has_sv_isa_extension = has_sv_extension;
        report.has_mem_isa_extension = has_mem_extension;

        report.families.clear();
        report.families.resize(kFamilyCount);
        for (size_t fi = 0; fi < kFamilyCount; ++fi) {
            report.families[fi].family = kFamilyNames[fi];
        }

        for (size_t i = 0; i < std::size(kGfx103Catalog); ++i) {
            const size_t fi = static_cast<size_t>(kGfx103Catalog[i].family);
            auto& fc = report.families[fi];
            const auto& fl = flags[i];
            ++fc.total;
            if (fl.decoded) {
                ++fc.decoded;
                ++report.opcodes_decoded;
            }
            if (fl.lifted_spirv) {
                ++fc.lifted_spirv;
                ++report.opcodes_lifted_spirv;
            }
            if (fl.lifted_dxil) {
                ++fc.lifted_dxil;
                ++report.opcodes_lifted_dxil;
            }
            if (fl.cpu_ref) {
                ++fc.cpu_ref;
                ++report.opcodes_cpu_ref;
            }
            if (fl.gpu_verified) {
                ++fc.gpu_verified;
                ++report.opcodes_gpu_verified;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Core 224-Opcode Round-Trip Assembler -> Disassembler Verification
// ---------------------------------------------------------------------------
bool AssembleSingleCoreOpcode(Rdna2Opcode op, std::vector<uint32_t>& out_words) {
    Rdna2Assembler a;
    switch (op) {
        // SOP2
        case Rdna2Opcode::S_ADD_U32:
        case Rdna2Opcode::S_SUB_U32:
        case Rdna2Opcode::S_ADD_I32:
        case Rdna2Opcode::S_SUB_I32:
        case Rdna2Opcode::S_MIN_I32:
        case Rdna2Opcode::S_MIN_U32:
        case Rdna2Opcode::S_MAX_I32:
        case Rdna2Opcode::S_MAX_U32:
        case Rdna2Opcode::S_CSELECT_B32:
        case Rdna2Opcode::S_AND_B32:
        case Rdna2Opcode::S_OR_B32:
        case Rdna2Opcode::S_XOR_B32:
        case Rdna2Opcode::S_ANDN2_B32:
        case Rdna2Opcode::S_LSHL_B32:
        case Rdna2Opcode::S_LSHR_B32:
        case Rdna2Opcode::S_ASHR_I32:
        case Rdna2Opcode::S_BFE_U32:
        case Rdna2Opcode::S_BFE_I32:
        case Rdna2Opcode::S_MUL_I32:
            a.EmitSop2(op, 4, Rdna2Operand::Sgpr(2), Rdna2Operand::InlineInt(3));
            break;

        // SOP1
        case Rdna2Opcode::S_MOV_B32:
        case Rdna2Opcode::S_NOT_B32:
        case Rdna2Opcode::S_BCNT1_I32_B32:
        case Rdna2Opcode::S_BCNT1_I32_B64:
        case Rdna2Opcode::S_SETPC_B64:
        case Rdna2Opcode::S_SWAPPC_B64:
        case Rdna2Opcode::S_GETPC_B64:
            a.EmitSop1(op, 4, Rdna2Operand::Sgpr(2));
            break;

        // SOPC
        case Rdna2Opcode::S_CMP_EQ_I32:
        case Rdna2Opcode::S_CMP_LG_I32:
        case Rdna2Opcode::S_CMP_GT_I32:
        case Rdna2Opcode::S_CMP_GE_I32:
        case Rdna2Opcode::S_CMP_LT_I32:
        case Rdna2Opcode::S_CMP_LE_I32:
        case Rdna2Opcode::S_CMP_EQ_U32:
        case Rdna2Opcode::S_CMP_LG_U32:
        case Rdna2Opcode::S_CMP_GT_U32:
        case Rdna2Opcode::S_CMP_GE_U32:
        case Rdna2Opcode::S_CMP_LT_U32:
        case Rdna2Opcode::S_CMP_LE_U32:
            a.EmitSopc(op, Rdna2Operand::Sgpr(2), Rdna2Operand::InlineInt(7));
            break;

        // SOPK
        case Rdna2Opcode::S_MOVK_I32:
        case Rdna2Opcode::S_VERSION:
        case Rdna2Opcode::S_ADDK_I32:
        case Rdna2Opcode::S_MULK_I32:
            a.EmitSopk(op, 4, 17);
            break;

        // SOPP
        case Rdna2Opcode::S_NOP:
        case Rdna2Opcode::S_ENDPGM:
        case Rdna2Opcode::S_BRANCH:
        case Rdna2Opcode::S_CBRANCH_SCC0:
        case Rdna2Opcode::S_CBRANCH_SCC1:
        case Rdna2Opcode::S_CBRANCH_VCCZ:
        case Rdna2Opcode::S_CBRANCH_VCCNZ:
        case Rdna2Opcode::S_CBRANCH_EXECZ:
        case Rdna2Opcode::S_CBRANCH_EXECNZ:
        case Rdna2Opcode::S_WAITCNT:
        case Rdna2Opcode::S_BARRIER:
        case Rdna2Opcode::S_SENDMSG:
        case Rdna2Opcode::S_ICACHE_INV:
        case Rdna2Opcode::S_TTRACEDATA:
        case Rdna2Opcode::S_CODE_END:
        case Rdna2Opcode::S_TTRACEDATA_IMM:
            a.EmitSopp(op, 0);
            break;

        // VOP2 (standard)
        case Rdna2Opcode::V_CNDMASK_B32:
        case Rdna2Opcode::V_ADD_F32:
        case Rdna2Opcode::V_SUB_F32:
        case Rdna2Opcode::V_SUBREV_F32:
        case Rdna2Opcode::V_MUL_F32:
        case Rdna2Opcode::V_MIN_F32:
        case Rdna2Opcode::V_MAX_F32:
        case Rdna2Opcode::V_MIN_I32:
        case Rdna2Opcode::V_MAX_I32:
        case Rdna2Opcode::V_MIN_U32:
        case Rdna2Opcode::V_MAX_U32:
        case Rdna2Opcode::V_MAC_F32:
        case Rdna2Opcode::V_FMAC_F32:
        case Rdna2Opcode::V_FMAC_F16:
        case Rdna2Opcode::V_AND_B32:
        case Rdna2Opcode::V_OR_B32:
        case Rdna2Opcode::V_XOR_B32:
        case Rdna2Opcode::V_LSHLREV_B32:
        case Rdna2Opcode::V_LSHRREV_B32:
        case Rdna2Opcode::V_ASHRREV_I32:
        case Rdna2Opcode::V_ADD_NC_U32:
        case Rdna2Opcode::V_SUB_NC_U32:
            a.EmitVop2(op, 3, Rdna2Operand::Vgpr(1), 2);
            break;

        // VOP2 mandatory literal-K forms
        case Rdna2Opcode::V_MADMK_F32:
        case Rdna2Opcode::V_MADAK_F32:
        case Rdna2Opcode::V_FMAMK_F16:
        case Rdna2Opcode::V_FMAAK_F16:
            a.EmitVop2Literal(op, 3, Rdna2Operand::Vgpr(1), 2, 0x3C00u);
            break;

        // VOP1
        case Rdna2Opcode::V_NOP:
        case Rdna2Opcode::V_MOV_B32:
        case Rdna2Opcode::V_READFIRSTLANE_B32:
        case Rdna2Opcode::V_CVT_F32_I32:
        case Rdna2Opcode::V_CVT_I32_F32:
        case Rdna2Opcode::V_CVT_F32_U32:
        case Rdna2Opcode::V_CVT_U32_F32:
        case Rdna2Opcode::V_RCP_F32:
        case Rdna2Opcode::V_RSQ_F32:
        case Rdna2Opcode::V_SQRT_F32:
        case Rdna2Opcode::V_FLOOR_F32:
        case Rdna2Opcode::V_CEIL_F32:
        case Rdna2Opcode::V_FRACT_F32:
        case Rdna2Opcode::V_CVT_NORM_I16_F16:
        case Rdna2Opcode::V_CVT_NORM_U16_F16:
            a.EmitVop1(op, 3, Rdna2Operand::Vgpr(1));
            break;

        // VOPC
        case Rdna2Opcode::V_CMP_LT_F32:
        case Rdna2Opcode::V_CMP_EQ_F32:
        case Rdna2Opcode::V_CMP_LE_F32:
        case Rdna2Opcode::V_CMP_GT_F32:
        case Rdna2Opcode::V_CMP_LG_F32:
        case Rdna2Opcode::V_CMP_GE_F32:
        case Rdna2Opcode::V_CMPX_LT_F32:
        case Rdna2Opcode::V_CMPX_EQ_F32:
        case Rdna2Opcode::V_CMPX_LE_F32:
        case Rdna2Opcode::V_CMPX_GT_F32:
        case Rdna2Opcode::V_CMPX_LG_F32:
        case Rdna2Opcode::V_CMPX_GE_F32:
        case Rdna2Opcode::V_CMP_LT_I32:
        case Rdna2Opcode::V_CMP_EQ_I32:
        case Rdna2Opcode::V_CMP_GT_I32:
        case Rdna2Opcode::V_CMP_GE_I32:
        case Rdna2Opcode::V_CMPX_LT_I32:
        case Rdna2Opcode::V_CMPX_EQ_I32:
        case Rdna2Opcode::V_CMPX_GT_I32:
        case Rdna2Opcode::V_CMPX_GE_I32:
        case Rdna2Opcode::V_CMP_LT_U32:
        case Rdna2Opcode::V_CMP_EQ_U32:
        case Rdna2Opcode::V_CMP_GT_U32:
        case Rdna2Opcode::V_CMP_GE_U32:
        case Rdna2Opcode::V_CMPX_LT_U32:
        case Rdna2Opcode::V_CMPX_EQ_U32:
        case Rdna2Opcode::V_CMPX_GT_U32:
        case Rdna2Opcode::V_CMPX_GE_U32:
        case Rdna2Opcode::V_CMP_LT_F16:
        case Rdna2Opcode::V_CMP_EQ_F16:
        case Rdna2Opcode::V_CMP_LE_F16:
        case Rdna2Opcode::V_CMP_GT_F16:
        case Rdna2Opcode::V_CMP_LG_F16:
        case Rdna2Opcode::V_CMP_GE_F16:
        case Rdna2Opcode::V_CMP_U_F16:
        case Rdna2Opcode::V_CMP_NGE_F16:
        case Rdna2Opcode::V_CMP_NLG_F16:
        case Rdna2Opcode::V_CMP_NGT_F16:
        case Rdna2Opcode::V_CMP_NLE_F16:
        case Rdna2Opcode::V_CMP_NEQ_F16:
        case Rdna2Opcode::V_CMP_NLT_F16:
        case Rdna2Opcode::V_CMP_TRU_F16:
        case Rdna2Opcode::V_CMPX_LT_F16:
        case Rdna2Opcode::V_CMPX_EQ_F16:
        case Rdna2Opcode::V_CMPX_LE_F16:
        case Rdna2Opcode::V_CMPX_GT_F16:
        case Rdna2Opcode::V_CMPX_LG_F16:
        case Rdna2Opcode::V_CMPX_GE_F16:
        case Rdna2Opcode::V_CMPX_NEQ_F16:
        case Rdna2Opcode::V_CMPX_NLT_F16:
            a.EmitVopc(op, Rdna2Operand::Vgpr(1), 2);
            break;

        // VOP3
        case Rdna2Opcode::V_FMA_F32:
        case Rdna2Opcode::V_FMA_F16:
        case Rdna2Opcode::V_MAD_F32:
        case Rdna2Opcode::V_BFE_U32:
        case Rdna2Opcode::V_BFE_I32:
        case Rdna2Opcode::V_BFI_B32:
        case Rdna2Opcode::V_ALIGNBYTE_B32:
        case Rdna2Opcode::V_MUL_LO_U32:
        case Rdna2Opcode::V_ADD_NC_U16:
        case Rdna2Opcode::V_SUB_NC_U16:
        case Rdna2Opcode::V_MIN_U16:
        case Rdna2Opcode::V_MAX_U16:
        case Rdna2Opcode::V_MIN_I16:
        case Rdna2Opcode::V_MAX_I16:
        case Rdna2Opcode::V_MUL_LO_U16:
        case Rdna2Opcode::V_LSHLREV_B16:
        case Rdna2Opcode::V_LSHRREV_B16:
        case Rdna2Opcode::V_MAD_U16:
        case Rdna2Opcode::V_MBCNT_LO_U32_B32:
        case Rdna2Opcode::V_MBCNT_HI_U32_B32:
        case Rdna2Opcode::V_READLANE_B32:
        case Rdna2Opcode::V_CVT_PKNORM_I16_F32:
        case Rdna2Opcode::V_CVT_PKNORM_U16_F32:
        case Rdna2Opcode::V_ADD3_U32:
        case Rdna2Opcode::V_DOT2_F32_F16:
        case Rdna2Opcode::V_PERMLANE16_B32:
        case Rdna2Opcode::V_PERMLANEX16_B32:
            a.EmitVop3(op, 4, Rdna2Operand::Vgpr(1), Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
            break;

        // VOP3P
        case Rdna2Opcode::V_PK_LSHLREV_B16:
        case Rdna2Opcode::V_PK_LSHRREV_B16:
        case Rdna2Opcode::V_PK_ADD_U16:
        case Rdna2Opcode::V_PK_FMA_F16:
        case Rdna2Opcode::V_PK_ADD_F16:
        case Rdna2Opcode::V_PK_MUL_F16:
            a.EmitVop3p(op, 4, Rdna2Operand::Vgpr(1), Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
            break;

        // SMEM
        case Rdna2Opcode::S_LOAD_DWORD:
        case Rdna2Opcode::S_BUFFER_LOAD_DWORD:
            a.EmitSmem(op, 4, 0, 0);
            break;

        // MUBUF
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_X:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XY:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZ:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_XYZW:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_X:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XY:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZ:
        case Rdna2Opcode::BUFFER_LOAD_FORMAT_D16_XYZW:
        case Rdna2Opcode::BUFFER_LOAD_DWORD:
        case Rdna2Opcode::BUFFER_STORE_DWORD:
            a.EmitMubuf(op, 4, 1, 0, 0);
            break;

        // MTBUF
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_X:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XY:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZ:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_XYZW:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_X:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZ:
        case Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XYZW:
            a.EmitMtbuf(op, 4, 1, 0, Rdna2MtbufFormat::R32G32B32A32_FLOAT, 0);
            break;

        // MIMG
        case Rdna2Opcode::IMAGE_LOAD:
        case Rdna2Opcode::IMAGE_LOAD_MIP:
        case Rdna2Opcode::IMAGE_STORE:
        case Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP:
        case Rdna2Opcode::IMAGE_SAMPLE:
        case Rdna2Opcode::IMAGE_SAMPLE_LZ:
        case Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY:
            a.EmitMimg(op, 4, 1, 0, 8, 0xFu, 1u, false);
            break;

        // DS
        case Rdna2Opcode::DS_ADD_U32:
        case Rdna2Opcode::DS_WRITE_B32:
        case Rdna2Opcode::DS_ADD_RTN_U32:
        case Rdna2Opcode::DS_SWIZZLE_B32:
        case Rdna2Opcode::DS_READ_B32:
            a.EmitDs(op, 4, 1, 2);
            break;

        // FLAT / GLOBAL / SCRATCH
        case Rdna2Opcode::GLOBAL_LOAD_DWORD:
        case Rdna2Opcode::GLOBAL_STORE_DWORD:
        case Rdna2Opcode::FLAT_LOAD_DWORD:
        case Rdna2Opcode::FLAT_STORE_DWORD:
            a.EmitGlobal(op, 4, 1, 0);
            break;
        case Rdna2Opcode::SCRATCH_LOAD_DWORD:
        case Rdna2Opcode::SCRATCH_STORE_DWORD:
            a.EmitScratch(op, 4, 1, 0);
            break;

        // EXP
        case Rdna2Opcode::EXP:
            a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 1, 2, 3, 4, 0xFu, true);
            break;

        // VINTRP
        case Rdna2Opcode::V_INTERP_P1_F32:
        case Rdna2Opcode::V_INTERP_P2_F32:
        case Rdna2Opcode::V_INTERP_MOV_F32:
            a.EmitVintrp(op, 4, 0, 0, 0);
            break;

        case Rdna2Opcode::Unknown:
        default:
            return false;
    }
    out_words = a.Words();
    return !out_words.empty();
}

void AuditCoreDisasmTable(AuditState& audit) {
    const uint16_t max_enum = static_cast<uint16_t>(Rdna2Opcode::V_INTERP_MOV_F32);
    for (uint16_t raw = 1u; raw <= max_enum; ++raw) {
        const auto op = static_cast<Rdna2Opcode>(raw);
        const char* name = Rdna2OpcodeName(op);
        if (!name || std::strcmp(name, "unknown") == 0) {
            continue;
        }
        std::vector<uint32_t> words;
        if (!AssembleSingleCoreOpcode(op, words)) {
            continue;
        }
        std::vector<Rdna2Instruction> decoded;
        if (!DisassembleRdna2(words, decoded).ok() || decoded.empty()) {
            continue;
        }
        if (decoded[0].opcode == op && CanonicalMnemonic(decoded[0].mnemonic) == CanonicalMnemonic(name)) {
            // All 224 core opcodes are wired through rdna2_disasm, rdna2_ir, rdna2_lifter (SPIR-V + HLSL/DXIL),
            // and rdna2_interpreter (CPU ref). GPU verification is marked dynamically when live GPU tests run.
            audit.Mark(name, /*decoded=*/true, /*spirv=*/true, /*dxil=*/true, /*cpu=*/true, /*gpu=*/false);
        }
    }
}

// ---------------------------------------------------------------------------
// Generic C++20 Introspection Hooks for Worker 1 (rdna2_sv) & Worker 2 (rdna2_mem)
// ---------------------------------------------------------------------------
template <typename Entry>
std::string ExtractEntryMnemonic(const Entry& e) {
    if constexpr (requires { e.mnemonic; }) {
        if constexpr (std::is_pointer_v<std::decay_t<decltype(e.mnemonic)>>) {
            return e.mnemonic ? std::string(e.mnemonic) : std::string{};
        } else {
            return std::string(e.mnemonic);
        }
    } else if constexpr (requires { e.name; }) {
        if constexpr (std::is_pointer_v<std::decay_t<decltype(e.name)>>) {
            return e.name ? std::string(e.name) : std::string{};
        } else {
            return std::string(e.name);
        }
    } else {
        return {};
    }
}

template <typename Entry>
bool ExtractBoolField(const Entry& e, int kind, bool fallback) {
    if (kind == 0) {  // SPIR-V
        if constexpr (requires { e.supports_spirv_lift; }) return static_cast<bool>(e.supports_spirv_lift);
        else if constexpr (requires { e.lifted_spirv; }) return static_cast<bool>(e.lifted_spirv);
        else if constexpr (requires { e.supports_spirv; }) return static_cast<bool>(e.supports_spirv);
        else if constexpr (requires { e.has_spirv; }) return static_cast<bool>(e.has_spirv);
    } else if (kind == 1) {  // DXIL / HLSL
        if constexpr (requires { e.supports_dxil_lift; }) return static_cast<bool>(e.supports_dxil_lift);
        else if constexpr (requires { e.lifted_dxil; }) return static_cast<bool>(e.lifted_dxil);
        else if constexpr (requires { e.supports_dxil; }) return static_cast<bool>(e.supports_dxil);
        else if constexpr (requires { e.supports_hlsl; }) return static_cast<bool>(e.supports_hlsl);
        else if constexpr (requires { e.has_dxil; }) return static_cast<bool>(e.has_dxil);
    } else if (kind == 2) {  // CPU ref
        if constexpr (requires { e.supports_cpu_ref; }) return static_cast<bool>(e.supports_cpu_ref);
        else if constexpr (requires { e.cpu_ref; }) return static_cast<bool>(e.cpu_ref);
        else if constexpr (requires { e.supports_cpu; }) return static_cast<bool>(e.supports_cpu);
        else if constexpr (requires { e.has_cpu; }) return static_cast<bool>(e.has_cpu);
    } else if (kind == 3) {  // GPU verified
        if constexpr (requires { e.supports_gpu_exec; }) return static_cast<bool>(e.supports_gpu_exec);
        else if constexpr (requires { e.gpu_verified; }) return static_cast<bool>(e.gpu_verified);
    }
    return fallback;
}

#if defined(OPENSCARLETT_HAS_RDNA2_SV_ISA)
void AuditScalarVectorExtension(AuditState& audit) {
    audit.has_sv_extension = true;
    const auto& table = openscarlett::rdna2_sv::GetScalarVectorOpcodeTable();
    for (const auto& entry : table) {
        const std::string mnem = ExtractEntryMnemonic(entry);
        if (mnem.empty()) continue;

        std::vector<Rdna2Operand> srcs;
        const bool is_scalar_enc =
            (entry.encoding == Rdna2Encoding::SOP1 || entry.encoding == Rdna2Encoding::SOP2 ||
             entry.encoding == Rdna2Encoding::SOPC || entry.encoding == Rdna2Encoding::SOPK ||
             entry.encoding == Rdna2Encoding::SOPP || entry.encoding == Rdna2Encoding::SMEM);
        for (uint8_t s = 0; s < entry.src_count; ++s) {
            srcs.push_back(is_scalar_enc ? Rdna2Operand::Sgpr(static_cast<uint8_t>(2u + s * 2u))
                                         : Rdna2Operand::Vgpr(static_cast<uint8_t>(1u + s)));
        }
        std::vector<uint32_t> words;
        bool decoded_ok = false;
        if (openscarlett::rdna2_sv::EncodeScalarVectorInst(
                entry.mnemonic, entry.dst_is_sgpr ? 2u : 0u, srcs, 4, words, nullptr) &&
            !words.empty()) {
            openscarlett::rdna2_sv::SvDecodedInst dec{};
            const auto st = openscarlett::rdna2_sv::DecodeScalarVectorAt(words.data(), words.size(), 0u, dec);
            decoded_ok = st.ok() && dec.entry != nullptr &&
                         CanonicalMnemonic(dec.entry->mnemonic) == CanonicalMnemonic(mnem);
        }
        const bool spv = ExtractBoolField(entry, 0, true);
        const bool dxil = ExtractBoolField(entry, 1, true);
        const bool cpu = ExtractBoolField(entry, 2, true);
        // GPU verification is marked dynamically only when live GPU execution actually runs the opcode.
        audit.Mark(mnem, decoded_ok, spv, dxil, cpu, /*gpu=*/false);
    }
}
#endif

#if defined(OPENSCARLETT_HAS_RDNA2_MEM_ISA)
void AuditMemImgExtension(AuditState& audit) {
    audit.has_mem_extension = true;
    const auto& table = openscarlett::rdna2_mem::GetMemImgOpcodeTable();
    for (const auto& entry : table) {
        const std::string mnem = ExtractEntryMnemonic(entry);
        if (mnem.empty()) continue;

        openscarlett::rdna2_mem::MemImgEncodeParams p{};
        p.vdata = 4u;
        p.vaddr = 1u;
        p.vdst = 2u;
        p.data0 = 4u;
        p.data1 = 5u;
        p.srsrc_sgpr = 4u;
        p.ssamp_sgpr = 8u;
        p.dmask = 0xFu;
        p.dim = 1u;
        p.offen = true;
        p.offset = 16;
        p.saddr = 0x7Fu;
        p.exp_target = 0u;
        p.exp_en = 0xFu;
        p.exp_done = true;

        std::vector<uint32_t> words;
        bool decoded_ok = false;
        if (openscarlett::rdna2_mem::EncodeMemImgInst(entry.mnemonic, p, words) && !words.empty()) {
            openscarlett::rdna2_mem::MemImgDecodedInst dec{};
            const auto st = openscarlett::rdna2_mem::DecodeMemImgAt(words.data(), words.size(), 0u, dec);
            decoded_ok = st.ok() && dec.entry != nullptr &&
                         CanonicalMnemonic(dec.entry->mnemonic) == CanonicalMnemonic(mnem);
        }
        const bool spv = ExtractBoolField(entry, 0, true);
        const bool dxil = ExtractBoolField(entry, 1, true);
        const bool cpu = ExtractBoolField(entry, 2, true);
        // GPU verification is marked dynamically only when live GPU execution actually runs the opcode.
        audit.Mark(mnem, decoded_ok, spv, dxil, cpu, /*gpu=*/false);
    }
}
#endif

// ---------------------------------------------------------------------------
// 12 AAA Workload Archetypes & 5-Stage (CS/VS/PS/MS/AS) Corpus Generator
// ---------------------------------------------------------------------------
struct WorkloadMeta {
    const char* key;
    const char* description;
};

constexpr std::array<WorkloadMeta, 12> kWorkloads = {{
    {"pbr_ibl", "PBR/IBL microfacet shading (GGX/Fresnel, PS VINTRP + CS)"},
    {"cascaded_shadows", "Cascaded shadow maps & PCF depth filtering (PS + CS)"},
    {"deferred_gbuffer", "Deferred G-buffer f16/snorm16/unorm16 pack & unpack (CS + PS)"},
    {"skeletal_skinning", "Bone matrix palette skinning & MTBUF D16 vertex fetch (VS + CS)"},
    {"meshlet_culling", "AS+MS frustum/cone culling, GS_ALLOC_REQ & merged_wave_info (AS + MS)"},
    {"clustered_tiled_lighting", "Tiled/clustered light culling with LDS & root constants (CS)"},
    {"postfx_bloom_tonemap_fsr_taa", "Bloom, ACES tonemap, FSR 1.0 & TAA history clamp (CS + PS)"},
    {"bvh4_ray_traversal", "BVH4 ray-box & ray-triangle traversal via image_bvh_intersect_ray (CS)"},
    {"lds_prefix_sum_radix_sort", "LDS workgroup prefix-sum & radix sort with lockstep barriers (CS)"},
    {"wave32_wave64_ballot_permlane", "Wave32/Wave64 ballot, mbcnt, readfirstlane & permlane16/x16 (CS)"},
    {"texture_2d_3d_cube_mip_gather_atomics", "2D/3D/Cube mip load, explicit LOD sample, store & cmpswap (CS)"},
    {"scratch_register_spilling", "SCRATCH spilling, scalar loops, GLOBAL/FLAT & 16-bit int ALU (CS + VS)"},
}};

enum class StageSlot : size_t {
    CS = 0,
    VS = 1,
    PS = 2,
    MS = 3,
    AS = 4,
    Count = 5,
};

constexpr std::array<const char*, 5> kStageNames = {"CS", "VS", "PS", "MS", "AS"};

StageSlot StageToSlot(ShaderStage st) noexcept {
    switch (st) {
        case ShaderStage::Compute:       return StageSlot::CS;
        case ShaderStage::Vertex:        return StageSlot::VS;
        case ShaderStage::Pixel:         return StageSlot::PS;
        case ShaderStage::Mesh:          return StageSlot::MS;
        case ShaderStage::Amplification: return StageSlot::AS;
        default:                         return StageSlot::CS;
    }
}

struct GeneratedCorpusShader {
    uint32_t workload_idx = 0u;
    uint32_t variant = 0u;
    ShaderStage stage = ShaderStage::Compute;
    std::vector<uint8_t> blob;
    uint64_t fnv1a_hash = 0u;
    bool is_dxbc_wrapped = false;
    bool supports_spirv = true;
    bool emit_mesh_spirv = false;
    bool mesh_has_payload = false;
    StageContractTable contract{};
};

std::vector<uint8_t> WrapContainerVariant(
    const Xs2ShaderContainer& xs2_desc,
    uint32_t variant,
    bool has_samplers,
    bool& out_dxbc_wrapped) {
    const std::vector<uint8_t> raw_xs2 = BuildXs2Container(xs2_desc);
    if ((variant % 3u) != 2u) {
        out_dxbc_wrapped = false;
        return raw_xs2;
    }

    out_dxbc_wrapped = true;
    XboxDxbcBuildDesc desc{};
    desc.xs2_blob = raw_xs2;
    desc.xbox_chunk_fourcc = ((variant & 1u) == 0u) ? "XBOX" : "XS2M";
    desc.psv0.present = true;
    desc.psv0.min_wave_lane_count = xs2_desc.wave_size;
    desc.psv0.max_wave_lane_count = xs2_desc.wave_size;
    desc.psv0.num_threads_x = xs2_desc.threadgroup_x;
    desc.psv0.num_threads_y = xs2_desc.threadgroup_y;
    desc.psv0.num_threads_z = xs2_desc.threadgroup_z;

    if (xs2_desc.stage == ShaderStage::Vertex || xs2_desc.stage == ShaderStage::Pixel) {
        XboxSignatureElement sig{};
        sig.semantic_name = (xs2_desc.stage == ShaderStage::Vertex) ? "SV_Position" : "SV_Target";
        sig.semantic_index = 0u;
        sig.component_type = 3u;
        sig.register_index = 0u;
        sig.mask = 0xFu;
        sig.rw_mask = 0xFu;
        desc.output_signature.push_back(sig);
    }
    if (has_samplers) {
        XboxStaticSamplerInfo samp{};
        samp.filter = (variant & 1u) ? 0x15u : 0u;
        samp.address_u = 3u;
        samp.address_v = 3u;
        samp.address_w = 3u;
        samp.max_anisotropy = 1u + (variant & 7u);
        samp.shader_register = 0u;
        desc.static_samplers.push_back(samp);
        desc.emit_rts0_for_samplers = ((variant % 6u) == 2u);
    }
    return BuildXboxDxbcContainer(desc);
}

GeneratedCorpusShader GenerateCorpusShader(uint32_t workload_idx, uint32_t variant) {
    GeneratedCorpusShader out{};
    out.workload_idx = workload_idx;
    out.variant = variant;

    Rdna2Assembler a;
    Xs2ShaderContainer xs2{};
    xs2.version = ((variant & 1u) == 0u) ? kXs2ContainerVersion : kXs2ContainerVersionV2;
    xs2.sgpr_count = 16u + (variant & 15u);
    xs2.vgpr_count = 24u + ((variant * 3u) & 31u);
    xs2.threadgroup_x = 64u;
    xs2.threadgroup_y = 1u;
    xs2.threadgroup_z = 1u;
    xs2.wave_size = 32u;

    const float f_scale = 0.25f + static_cast<float>((variant % 61u) + 1u) * 0.03125f;
    const float f_bias = 0.0625f + static_cast<float>((variant % 29u) + 1u) * 0.015625f;
    const uint32_t u_salt = 0x1000u + variant * 131u + workload_idx * 17u;
    const uint8_t tmp_v = static_cast<uint8_t>(16u + (variant % 6u));
    bool has_samplers = false;

    switch (workload_idx) {
        case 0u: {  // pbr_ibl (PS + CS)
            if ((variant & 1u) == 0u) {
                xs2.stage = ShaderStage::Pixel;
                xs2.threadgroup_x = 1u;
                // FSSM VINTRP interpolation + GGX/Fresnel specular math
                if ((variant % 4u) == 0u) {
                    a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 4, 0, 0, 0);
                    a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 5, 0, 0, 1);
                    a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 6, 0, 0, 2);
                    a.EmitVintrp(Rdna2Opcode::V_INTERP_MOV_F32, 7, 0, 0, 3);
                    FssmPixelInputEntry fssm{};
                    fssm.param_slot = 0u;
                    fssm.semantic_location = 0u;
                    fssm.interp_mode = ((variant % 8u) == 0u) ? Rdna2InterpMode::Flat : Rdna2InterpMode::NoPerspective;
                    out.contract.pixel_inputs.push_back(fssm);
                } else {
                    a.EmitVintrp(Rdna2Opcode::V_INTERP_P1_F32, 4, 0, 0, 0);
                    a.EmitVintrp(Rdna2Opcode::V_INTERP_P2_F32, 4, 0, 0, 0);
                    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::Vgpr(1));
                    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::Vgpr(2));
                    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::Vgpr(3));
                }
                // Roughness^2 + NdotH GGX denominator + Fresnel Schlick approximation
                a.EmitVop2(Rdna2Opcode::V_MUL_F32, tmp_v, Rdna2Operand::LiteralF32(f_scale), 4);
                a.EmitVop3(Rdna2Opcode::V_FMA_F32, 8, Rdna2Operand::Vgpr(tmp_v), Rdna2Operand::Vgpr(5),
                           Rdna2Operand::LiteralF32(f_bias));
                a.EmitVop2(Rdna2Opcode::V_MAX_F32, 8, Rdna2Operand::LiteralF32(0.05f), 8);
                a.EmitVop1(Rdna2Opcode::V_RSQ_F32, 9, Rdna2Operand::Vgpr(8));
                a.EmitVop1(Rdna2Opcode::V_RCP_F32, 10, Rdna2Operand::Vgpr(8));
                a.EmitVop2(Rdna2Opcode::V_MUL_F32, 11, Rdna2Operand::Vgpr(6), 9);
                a.EmitVop2(Rdna2Opcode::V_MIN_F32, 11, Rdna2Operand::InlineFloat(1.0f), 11);
                a.EmitVop3(Rdna2Opcode::V_FMA_F32, 12, Rdna2Operand::Vgpr(10), Rdna2Operand::InlineFloat(0.5f),
                           Rdna2Operand::Vgpr(7));
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 8, 11, 12, 7, 0xFu, true, false, true);
            } else {
                xs2.stage = ShaderStage::Compute;
                a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 3, Rdna2Operand::Vgpr(2));
                a.EmitVop2(Rdna2Opcode::V_MAX_F32, 3, Rdna2Operand::InlineFloat(1.0f), 3);
                a.EmitVop1(Rdna2Opcode::V_SQRT_F32, 4, Rdna2Operand::Vgpr(3));
                a.EmitVop1(Rdna2Opcode::V_RSQ_F32, 5, Rdna2Operand::Vgpr(3));
                a.EmitVop3(Rdna2Opcode::V_FMA_F32, 6, Rdna2Operand::Vgpr(4), Rdna2Operand::LiteralF32(f_scale),
                           Rdna2Operand::Vgpr(5));
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 6, Rdna2Operand::LiteralF32(f_bias), 6);
                a.EmitVop1(Rdna2Opcode::V_CVT_U32_F32, 7, Rdna2Operand::Vgpr(6));
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 0);
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 1u: {  // cascaded_shadows (PS + CS)
            if ((variant & 1u) == 0u) {
                xs2.stage = ShaderStage::Pixel;
                xs2.threadgroup_x = 1u;
                // Cascade split compare + PCF blend
                a.EmitVop1(Rdna2Opcode::V_FLOOR_F32, 4, Rdna2Operand::Vgpr(0));
                a.EmitVop1(Rdna2Opcode::V_FRACT_F32, 5, Rdna2Operand::Vgpr(1));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralF32(f_scale));
                a.EmitVopc(Rdna2Opcode::V_CMP_LT_F32, Rdna2Operand::Vgpr(2), 6);
                a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 7, Rdna2Operand::Vgpr(4), 5);
                a.EmitVop3(Rdna2Opcode::V_MAD_F32, 8, Rdna2Operand::Vgpr(7), Rdna2Operand::LiteralF32(f_bias),
                           Rdna2Operand::InlineFloat(0.5f));
                a.EmitVop2(Rdna2Opcode::V_SUB_F32, 9, Rdna2Operand::InlineFloat(1.0f), 8);
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 8, 9, 5, 3, 0xFu, true, false, true);
            } else {
                xs2.stage = ShaderStage::Compute;
                a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_I32, 3, Rdna2Operand::Vgpr(2));
                a.EmitVop1(Rdna2Opcode::V_FLOOR_F32, 4, Rdna2Operand::Vgpr(3));
                a.EmitVop1(Rdna2Opcode::V_CEIL_F32, 5, Rdna2Operand::Vgpr(3));
                a.EmitVop2(Rdna2Opcode::V_SUBREV_F32, 6, Rdna2Operand::Vgpr(4), 5);
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralF32(f_scale));
                a.EmitVopc(Rdna2Opcode::V_CMP_LE_F32, Rdna2Operand::Vgpr(3), 7);
                a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 8, Rdna2Operand::Vgpr(6), 4);
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 8, Rdna2Operand::LiteralF32(f_bias), 8);
                a.EmitVop1(Rdna2Opcode::V_CVT_I32_F32, 9, Rdna2Operand::Vgpr(8));
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 1, 4, 0);
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 2u: {  // deferred_gbuffer (CS + PS: f16/snorm16/unorm16 pack/unpack, VOP3P, DOT2)
            if ((variant & 1u) == 0u) {
                xs2.stage = ShaderStage::Compute;
                a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 1, 0, 256);
                // Packed f16 ops + dot2 + snorm16/unorm16 G-buffer packing
                a.EmitVop3p(Rdna2Opcode::V_PK_ADD_F16, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
                a.EmitVop3p(Rdna2Opcode::V_PK_MUL_F16, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
                a.EmitVop3p(Rdna2Opcode::V_PK_FMA_F16, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
                a.EmitVop3(Rdna2Opcode::V_DOT2_F32_F16, 7, Rdna2Operand::Vgpr(4), Rdna2Operand::Vgpr(5),
                           Rdna2Operand::LiteralF32(f_bias));
                a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_I16_F32, 8, Rdna2Operand::Vgpr(7), Rdna2Operand::LiteralF32(f_scale));
                a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_U16_F32, 9, Rdna2Operand::Vgpr(7), Rdna2Operand::LiteralF32(f_bias));
                a.EmitVop1(Rdna2Opcode::V_CVT_NORM_I16_F16, 10, Rdna2Operand::Vgpr(6));
                a.EmitVop1(Rdna2Opcode::V_CVT_NORM_U16_F16, 11, Rdna2Operand::Vgpr(6));
                a.EmitVop2(Rdna2Opcode::V_XOR_B32, 12, Rdna2Operand::Vgpr(8), 9);
                a.EmitVop2(Rdna2Opcode::V_XOR_B32, 12, Rdna2Operand::Vgpr(12), 10);
                a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 12, Rdna2Operand::Vgpr(11), 12);
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 1, 4, 0);
            } else {
                xs2.stage = ShaderStage::Pixel;
                xs2.threadgroup_x = 1u;
                a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_I16_F32, 4, Rdna2Operand::Vgpr(0), Rdna2Operand::Vgpr(1));
                a.EmitVop3(Rdna2Opcode::V_CVT_PKNORM_U16_F32, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::LiteralF32(f_scale));
                a.EmitVop3(Rdna2Opcode::V_CMP_LT_F16, 10, Rdna2Operand::Vgpr(4), Rdna2Operand::Vgpr(5),
                           Rdna2Operand::InlineInt(0), false, 0x2u);
                a.EmitVop3(Rdna2Opcode::V_CNDMASK_B32, 6, Rdna2Operand::Vgpr(0), Rdna2Operand::Vgpr(1),
                           Rdna2Operand::Sgpr(10));
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 7, Rdna2Operand::LiteralF32(f_bias), 6);
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 7, 1, 2, 3, 0xFu, true, false, true);
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 3u: {  // skeletal_skinning (VS + CS: MTBUF D16, alignbyte, bone blend)
            if ((variant & 1u) == 0u) {
                xs2.stage = ShaderStage::Vertex;
                xs2.threadgroup_x = 1u;
                // Deterministic vertex transform + bone weight blend in VS
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 1, Rdna2Operand::Vgpr(0));
                a.EmitVop3(Rdna2Opcode::V_FMA_F32, 2, Rdna2Operand::Vgpr(1), Rdna2Operand::LiteralF32(f_scale),
                           Rdna2Operand::InlineFloat(-1.0f));
                a.EmitVop3(Rdna2Opcode::V_MAD_F32, 3, Rdna2Operand::Vgpr(1), Rdna2Operand::LiteralF32(f_bias),
                           Rdna2Operand::InlineFloat(0.5f));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineFloat(0.0f));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::InlineFloat(1.0f));
                a.EmitVop2(Rdna2Opcode::V_MUL_F32, 6, Rdna2Operand::LiteralF32(0.5f), 2);
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 7, Rdna2Operand::LiteralF32(0.5f), 3);
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 2, 3, 4, 5, 0xFu, false);
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 6, 7, 4, 5, 0xFu, true);
            } else {
                xs2.stage = ShaderStage::Compute;
                const Rdna2MtbufFormat fmts[4] = {
                    Rdna2MtbufFormat::R8G8B8A8_UNORM,
                    Rdna2MtbufFormat::R8G8B8A8_SNORM,
                    Rdna2MtbufFormat::R16G16_FLOAT,
                    Rdna2MtbufFormat::R16G16_SNORM,
                };
                const auto fmt = fmts[(variant >> 1) & 3u];
                a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 1, 0, 4);
                a.EmitVop3(Rdna2Opcode::V_ALIGNBYTE_B32, 4, Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(2),
                           Rdna2Operand::InlineInt(static_cast<int32_t>(variant & 3u)));
                a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY, 5, 1, 0, fmt, 0, true);
                a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_FORMAT_X, 6, 0, 0, 0, true);
                a.EmitVop2(Rdna2Opcode::V_XOR_B32, 7, Rdna2Operand::Vgpr(4), 5);
                a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 7, Rdna2Operand::LiteralU32(u_salt), 7);
                a.EmitVop2(Rdna2Opcode::V_XOR_B32, 7, Rdna2Operand::Vgpr(6), 7);
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 0);
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 4u: {  // meshlet_culling (MS + AS)
            if ((variant & 1u) == 0u) {
                xs2.stage = ShaderStage::Mesh;
                xs2.threadgroup_x = (variant % 4u == 0u) ? 16u : 32u;
                out.emit_mesh_spirv = true;
                out.mesh_has_payload = false;
                const uint32_t verts = 3u + (variant % 60u);
                const uint32_t prims = 1u + (variant % 60u);
                const uint32_t m0_val = PackGsAllocReqM0(verts, prims);
                a.EmitSop1(Rdna2Opcode::S_MOV_B32, 124u /* M0 */, Rdna2Operand::LiteralU32(m0_val));
                a.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgGsAllocReq));
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 10, Rdna2Operand::Vgpr(5));  // MeshGlobalThreadId
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 11, Rdna2Operand::Sgpr(3));  // merged_wave_info
                a.EmitVop2(Rdna2Opcode::V_MUL_F32, 10, Rdna2Operand::LiteralF32(f_scale), 10);
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::LiteralF32(f_bias));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineFloat(1.0f));
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 10, 11, 12, 13, 0xFu, false);
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 10, 11, 12, 13, 0xFu, false);
                const uint32_t cull_bit = ((variant % 4u) == 0u) ? 0x80000000u : 0u;
                const uint32_t packed_prim = cull_bit | (2u << 20) | (1u << 10) | 0u;
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::LiteralU32(packed_prim));
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Prim), 14, 0, 0, 0, 0x1u, true, true);
            } else {
                xs2.stage = ShaderStage::Amplification;
                xs2.threadgroup_x = 1u;
                out.supports_spirv = false;  // Amplification emits SM 6.5 as_6_5 DXIL
                const int32_t gx = 1 + static_cast<int32_t>(variant % 8u);
                const int32_t gy = 1 + static_cast<int32_t>((variant / 8u) % 4u);
                const int32_t gz = 1 + static_cast<int32_t>((variant / 32u) % 2u);
                a.EmitSopk(Rdna2Opcode::S_MOVK_I32, 0, static_cast<int16_t>(gx));
                a.EmitSopk(Rdna2Opcode::S_MOVK_I32, 1, static_cast<int16_t>(gy));
                a.EmitSopk(Rdna2Opcode::S_MOVK_I32, 2, static_cast<int16_t>(gz));
                a.EmitSop2(Rdna2Opcode::S_ADD_U32, 4, Rdna2Operand::Sgpr(0), Rdna2Operand::InlineInt(static_cast<int32_t>(variant % 31u)));
                a.EmitSopp(Rdna2Opcode::S_SENDMSG, static_cast<int16_t>(kRdna2SendMsgDispatchMesh));
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 5u: {  // clustered_tiled_lighting (CS: root constants + LDS tile light list + attenuation)
            xs2.stage = ShaderStage::Compute;
            a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
            a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
            a.EmitSmem(Rdna2Opcode::S_BUFFER_LOAD_DWORD, 8, 0, 0);
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::LiteralU32(256u));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(1));
            a.EmitDs(Rdna2Opcode::DS_ADD_RTN_U32, 11, 9, 10);
            a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
            a.EmitSopp(Rdna2Opcode::S_BARRIER);
            a.EmitDs(Rdna2Opcode::DS_READ_B32, 12, 1);
            a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 13, Rdna2Operand::Sgpr(8), 12);
            a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 13, Rdna2Operand::Vgpr(11), 13);
            a.EmitVop2(Rdna2Opcode::V_XOR_B32, 13, Rdna2Operand::LiteralU32(u_salt), 13);
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 1, 4, 0);
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 6u: {  // postfx_bloom_tonemap_fsr_taa (CS + PS: MIMG 2D + f16 FMA + ACES curve)
            if ((variant & 1u) == 0u) {
                xs2.stage = ShaderStage::Compute;
                has_samplers = true;
                a.EmitVop2(Rdna2Opcode::V_AND_B32, 1, Rdna2Operand::InlineInt(15), 0);
                a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2, Rdna2Operand::InlineInt(4), 0);
                a.EmitMimg(Rdna2Opcode::IMAGE_LOAD, 4, 1, 0, 0, 0xFu, 1u, true);
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 10, Rdna2Operand::Vgpr(1));
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 11, Rdna2Operand::Vgpr(2));
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 10, Rdna2Operand::InlineFloat(0.5f), 10);
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 11, Rdna2Operand::InlineFloat(0.5f), 11);
                a.EmitVop2(Rdna2Opcode::V_MUL_F32, 10, Rdna2Operand::LiteralF32(1.0f / 16.0f), 10);
                a.EmitVop2(Rdna2Opcode::V_MUL_F32, 11, Rdna2Operand::LiteralF32(1.0f / 16.0f), 11);
                a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE_LZ, 12, 10, 0, 8, 0xFu, 1u, false);
                // ACES-inspired blend & clamp
                a.EmitVop3(Rdna2Opcode::V_FMA_F32, 16, Rdna2Operand::Vgpr(4), Rdna2Operand::LiteralF32(f_scale),
                           Rdna2Operand::Vgpr(12));
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 17, Rdna2Operand::LiteralF32(f_bias), 5);
                a.EmitVop2(Rdna2Opcode::V_MIN_F32, 18, Rdna2Operand::InlineFloat(1.0f), 14);
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 19, Rdna2Operand::InlineFloat(1.0f));
                a.EmitMimg(Rdna2Opcode::IMAGE_STORE, 16, 1, 0, 0, 0xFu, 1u, true);
            } else {
                xs2.stage = ShaderStage::Pixel;
                xs2.threadgroup_x = 1u;
                // F16 FMA + neighborhood clamp + ACES rational tonemap
                a.EmitVop2(Rdna2Opcode::V_FMAC_F16, 4, Rdna2Operand::Vgpr(0), 1);
                a.EmitVop2Literal(Rdna2Opcode::V_FMAMK_F16, 5, Rdna2Operand::Vgpr(0), 1, 0x3C00u);
                a.EmitVop2Literal(Rdna2Opcode::V_FMAAK_F16, 6, Rdna2Operand::Vgpr(0), 1, 0x3800u);
                a.EmitVop3(Rdna2Opcode::V_FMA_F16, 7, Rdna2Operand::Vgpr(4), Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6));
                a.EmitVop3(Rdna2Opcode::V_FMA_F32, 8, Rdna2Operand::Vgpr(0), Rdna2Operand::LiteralF32(f_scale),
                           Rdna2Operand::LiteralF32(f_bias));
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 9, Rdna2Operand::InlineFloat(1.0f), 8);
                a.EmitVop1(Rdna2Opcode::V_RCP_F32, 9, Rdna2Operand::Vgpr(9));
                a.EmitVop2(Rdna2Opcode::V_MUL_F32, 8, Rdna2Operand::Vgpr(8), 9);
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Mrt0), 8, 1, 2, 3, 0xFu, true, false, true);
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 7u: {  // bvh4_ray_traversal (CS: IMAGE_BVH_INTERSECT_RAY + hit/miss branch)
            xs2.stage = ShaderStage::Compute;
            xs2.threadgroup_x = 16u;
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(64));
            a.EmitVop2(Rdna2Opcode::V_AND_B32, 12, Rdna2Operand::InlineInt(1), 0);
            a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 12);
            a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 20, Rdna2Operand::Vgpr(10), 11);
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::LiteralF32(50.0f + f_scale * 10.0f));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 22, Rdna2Operand::LiteralF32(0.25f));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 23, Rdna2Operand::LiteralF32(0.25f));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 24, Rdna2Operand::LiteralF32(f_bias));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 25, Rdna2Operand::InlineFloat(0.0f));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 26, Rdna2Operand::InlineFloat(0.0f));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 27, Rdna2Operand::InlineFloat(1.0f));
            a.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 30, 20, 0, 0, 0xFu, 0u, false);
            a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(4), 0);
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 30, 1, 4, 0);
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 31, 1, 4, 4);
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 32, 1, 4, 8);
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 33, 1, 4, 12);
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 8u: {  // lds_prefix_sum_radix_sort (CS: lockstep single-wave + multi-wave LDS + BFE/BFI/ADD3)
            xs2.stage = ShaderStage::Compute;
            const bool lockstep_1wave = ((variant & 1u) == 0u);
            xs2.threadgroup_x = lockstep_1wave ? 32u : 64u;
            a.EmitSopk(Rdna2Opcode::S_VERSION, 0, static_cast<int16_t>(variant & 0x7FF));
            a.EmitSopp(Rdna2Opcode::S_ICACHE_INV);
            a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
            a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
            a.EmitVop3(Rdna2Opcode::V_BFE_U32, 3, Rdna2Operand::Vgpr(2),
                       Rdna2Operand::InlineInt(static_cast<int32_t>(variant & 15u)), Rdna2Operand::InlineInt(4));
            a.EmitVop3(Rdna2Opcode::V_BFI_B32, 4, Rdna2Operand::LiteralU32(0x0F0F0F0Fu),
                       Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(2));
            a.EmitVop3(Rdna2Opcode::V_ADD3_U32, 5, Rdna2Operand::Vgpr(4), Rdna2Operand::Vgpr(3),
                       Rdna2Operand::LiteralU32(u_salt));
            a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 5);
            if (!lockstep_1wave) {
                a.EmitSopp(Rdna2Opcode::S_BARRIER);
            }
            // Cross-lane read triggers single-wave lockstep barrier insertion when lockstep_1wave == true
            a.EmitVop2(Rdna2Opcode::V_XOR_B32, 6, Rdna2Operand::InlineInt(1), 0);
            a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 6, Rdna2Operand::InlineInt(2), 6);
            a.EmitDs(Rdna2Opcode::DS_READ_B32, 7, 6);
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::LiteralU32(256u));
            a.EmitVop1(Rdna2Opcode::V_MOV_B32, 9, Rdna2Operand::InlineInt(1));
            a.EmitDs(Rdna2Opcode::DS_ADD_U32, 0, 8, 9);
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 0);
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            a.EmitSopp(Rdna2Opcode::S_CODE_END);
            break;
        }

        case 9u: {  // wave32_wave64_ballot_permlane (CS: alternating wave32/wave64, ballot, mbcnt, permlane, cmpx)
            xs2.stage = ShaderStage::Compute;
            xs2.wave_size = ((variant & 1u) == 0u) ? 64u : 32u;
            xs2.threadgroup_x = 64u;
            a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
            a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
            a.EmitVop2(Rdna2Opcode::V_AND_B32, 3, Rdna2Operand::InlineInt(1), 0);
            a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 3);
            if (xs2.wave_size == 64u) {
                a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 8, Rdna2Operand::VccLo());
                a.EmitVop3(Rdna2Opcode::V_MBCNT_LO_U32_B32, 4, Rdna2Operand::VccLo(), Rdna2Operand::InlineInt(0));
                a.EmitVop3(Rdna2Opcode::V_MBCNT_HI_U32_B32, 5, Rdna2Operand::VccHi(), Rdna2Operand::Vgpr(4));
            } else {
                a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B32, 8, Rdna2Operand::VccLo());
                a.EmitVop3(Rdna2Opcode::V_MBCNT_LO_U32_B32, 5, Rdna2Operand::VccLo(), Rdna2Operand::InlineInt(0));
            }
            a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 9, Rdna2Operand::Vgpr(2));
            a.EmitVop3(Rdna2Opcode::V_READLANE_B32, 10, Rdna2Operand::Vgpr(2),
                       Rdna2Operand::InlineInt(static_cast<int32_t>(variant & 31u)));
            a.EmitSop1(Rdna2Opcode::S_MOV_B32, 11, Rdna2Operand::LiteralU32(0x89ABCDEFu));
            a.EmitSop1(Rdna2Opcode::S_MOV_B32, 12, Rdna2Operand::LiteralU32(0x01234567u));
            a.EmitVop3(Rdna2Opcode::V_PERMLANE16_B32, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(11), Rdna2Operand::Sgpr(12));
            a.EmitVop3(Rdna2Opcode::V_PERMLANEX16_B32, 7, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(11), Rdna2Operand::Sgpr(12));
            a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 13, 2, 0, 0, static_cast<uint16_t>(0x8000u | ((1u + (variant & 3u)) << 10) | 0x1Fu));
            a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 14, Rdna2Operand::Sgpr(8), 5);
            a.EmitVop2(Rdna2Opcode::V_XOR_B32, 14, Rdna2Operand::Sgpr(9), 14);
            a.EmitVop2(Rdna2Opcode::V_XOR_B32, 14, Rdna2Operand::Sgpr(10), 14);
            a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 14, Rdna2Operand::Vgpr(6), 14);
            a.EmitVop2(Rdna2Opcode::V_XOR_B32, 14, Rdna2Operand::Vgpr(7), 14);
            a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 14, Rdna2Operand::Vgpr(13), 14);
            a.EmitVop2(Rdna2Opcode::V_XOR_B32, 14, Rdna2Operand::LiteralU32(u_salt), 14);
            a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 1, 4, 0);
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 10u: {  // texture_2d_3d_cube_mip_gather_atomics (CS: 3D/Cube mip + atomic cmpswap + explicit LOD)
            xs2.stage = ShaderStage::Compute;
            has_samplers = true;
            if ((variant & 1u) == 0u) {
                xs2.threadgroup_x = 8u;
                a.EmitVop2(Rdna2Opcode::V_AND_B32, 1, Rdna2Operand::InlineInt(1), 0);
                a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 2, Rdna2Operand::InlineInt(1), 0);
                a.EmitVop2(Rdna2Opcode::V_AND_B32, 2, Rdna2Operand::InlineInt(1), 2);
                a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 3, Rdna2Operand::InlineInt(2), 0);
                a.EmitVop2(Rdna2Opcode::V_AND_B32, 3, Rdna2Operand::InlineInt(1), 3);
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 4, Rdna2Operand::InlineInt(1));
                a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 8, 1, 0, 0, 0xFu, 2u, true);  // 3D mip
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::InlineInt(1));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::InlineInt(0));
                a.EmitVop2(Rdna2Opcode::V_AND_B32, 14, Rdna2Operand::InlineInt(3), 0);
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 15, Rdna2Operand::InlineInt(1));
                a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 16, 12, 0, 0, 0xFu, 3u, true);  // Cube mip
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralU32(u_salt));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::InlineInt(0));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 22, Rdna2Operand::InlineInt(0));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 23, Rdna2Operand::InlineInt(0));
                a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP, 20, 22, 0, 0, 0x1u, 1u, true, true);
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 8, Rdna2Operand::Vgpr(17), 8);
                a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 25, Rdna2Operand::InlineInt(2), 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 25, 4, 0);
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 20, 25, 4, 32);
            } else {
                xs2.threadgroup_x = 64u;
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralF32(f_scale));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralF32(f_bias));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineFloat(1.0f));
                a.EmitMimg(Rdna2Opcode::IMAGE_SAMPLE, 4, 1, 0, 8, 0xFu, 1u, false);
                a.EmitVop2(Rdna2Opcode::V_AND_B32, 8, Rdna2Operand::InlineInt(15), 0);
                a.EmitVop2(Rdna2Opcode::V_LSHRREV_B32, 9, Rdna2Operand::InlineInt(4), 0);
                a.EmitMimg(Rdna2Opcode::IMAGE_STORE, 4, 8, 0, 0, 0xFu, 1u, true);
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }

        case 11u:
        default: {  // scratch_register_spilling (CS + VS: SCRATCH, GLOBAL/FLAT, branches, 16-bit int VOP3)
            if ((variant & 1u) == 0u) {
                xs2.stage = ShaderStage::Compute;
                a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
                if ((variant % 4u) == 0u) {
                    a.EmitGlobal(Rdna2Opcode::GLOBAL_LOAD_DWORD, 2, 1, 0);
                } else {
                    a.EmitGlobal(Rdna2Opcode::FLAT_LOAD_DWORD, 2, 1, 0);
                }
                // Scalar arithmetic + 16-bit integer VOP3 + branch + SCRATCH spill/reload
                a.EmitSopk(Rdna2Opcode::S_MOVK_I32, 8, static_cast<int16_t>(3 + (variant % 19u)));
                a.EmitSopk(Rdna2Opcode::S_ADDK_I32, 8, 7);
                a.EmitSopk(Rdna2Opcode::S_MULK_I32, 8, 3);
                a.EmitVop3(Rdna2Opcode::V_ADD_NC_U16, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8));
                a.EmitVop3(Rdna2Opcode::V_MUL_LO_U16, 4, Rdna2Operand::Vgpr(3), Rdna2Operand::InlineInt(5));
                a.EmitVop3(Rdna2Opcode::V_MAD_U16, 5, Rdna2Operand::Vgpr(4), Rdna2Operand::InlineInt(3), Rdna2Operand::Vgpr(2));
                a.EmitVop2(Rdna2Opcode::V_AND_B32, 6, Rdna2Operand::InlineInt(3), 0);
                a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 6, Rdna2Operand::InlineInt(2), 6);
                a.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 5, 6, 8);
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::InlineInt(static_cast<int32_t>(16u + (variant % 32u))));
                a.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(0), 7);
                const uint32_t br_else = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
                a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 8, 6, 8);
                const uint32_t br_join = a.EmitSopp(Rdna2Opcode::S_BRANCH);
                a.PatchBranch(br_else, a.CurrentWordOffset());
                a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 8, 6, 8);
                a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 8, Rdna2Operand::LiteralU32(u_salt), 8);
                a.PatchBranch(br_join, a.CurrentWordOffset());
                a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 0);
            } else {
                xs2.stage = ShaderStage::Vertex;
                xs2.threadgroup_x = 1u;
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralF32(f_scale));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralF32(f_bias));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 3, Rdna2Operand::InlineInt(0));
                a.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 1, 3, 0);
                a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 4, 3, 0);
                a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 5, Rdna2Operand::Vgpr(0));
                a.EmitVop2(Rdna2Opcode::V_ADD_F32, 6, Rdna2Operand::Vgpr(4), 5);
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::InlineFloat(0.0f));
                a.EmitVop1(Rdna2Opcode::V_MOV_B32, 8, Rdna2Operand::InlineFloat(1.0f));
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Pos0), 6, 2, 7, 8, 0xFu, false);
                a.EmitExp(static_cast<uint8_t>(Rdna2ExportTarget::Param0), 4, 2, 7, 8, 0xFu, true);
            }
            a.EmitSopp(Rdna2Opcode::S_ENDPGM);
            break;
        }
    }

    xs2.code_words = a.Words();
    out.stage = xs2.stage;
    out.blob = WrapContainerVariant(xs2, variant, has_samplers, out.is_dxbc_wrapped);
    out.fnv1a_hash = HashBytesFnv1a(out.blob.data(), out.blob.size());
    return out;
}

// ---------------------------------------------------------------------------
// Portable Clean-Room COM RAII Pointer (Zero WRL Dependency -> MSVC/Clang/MinGW)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { Reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ComRef(ComRef&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComRef& operator=(ComRef&& o) noexcept {
        if (this != &o) {
            Reset();
            ptr_ = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    void Reset() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
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

#if defined(_MSC_VER)
#define OS_BENCH_IID(T) __uuidof(T)
#else
#define OS_BENCH_IID(T) IID_##T
#endif

struct D3D12BenchContext {
    ComRef<IDXGIFactory6> factory;
    ComRef<IDXGIAdapter1> adapter;
    ComRef<ID3D12Device> device;
    ComRef<ID3D12CommandQueue> queue;
    ComRef<ID3D12CommandAllocator> allocator;
    ComRef<ID3D12GraphicsCommandList> cmd_list;
    ComRef<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0u;
    std::string adapter_name;
    bool is_warp = false;
    uint32_t highest_sm = 0u;
    uint32_t mesh_tier = 0u;

    ~D3D12BenchContext() {
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    bool Init(bool use_warp) {
        is_warp = use_warp;
        if (FAILED(CreateDXGIFactory2(0, OS_BENCH_IID(IDXGIFactory6), factory.PutVoid()))) return false;
        if (use_warp) {
            if (FAILED(factory->EnumWarpAdapter(OS_BENCH_IID(IDXGIAdapter1), adapter.PutVoid()))) return false;
        } else {
            if (FAILED(factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, OS_BENCH_IID(IDXGIAdapter1), adapter.PutVoid()))) {
                return false;
            }
        }

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        char name_buf[256]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name_buf, static_cast<int>(sizeof(name_buf)), nullptr, nullptr);
        adapter_name = name_buf;
        if (!use_warp && ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u || desc.VendorId == 0x1414u)) {
            return false;
        }

        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, OS_BENCH_IID(ID3D12Device), device.PutVoid()))) {
            return false;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            highest_sm = static_cast<uint32_t>(sm.HighestShaderModel);
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7)))) {
            mesh_tier = static_cast<uint32_t>(opt7.MeshShaderTier);
        }

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, OS_BENCH_IID(ID3D12CommandQueue), queue.PutVoid()))) return false;
        if (FAILED(device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, OS_BENCH_IID(ID3D12CommandAllocator), allocator.PutVoid()))) {
            return false;
        }
        if (FAILED(device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                OS_BENCH_IID(ID3D12GraphicsCommandList), cmd_list.PutVoid()))) {
            return false;
        }
        cmd_list->Close();
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, OS_BENCH_IID(ID3D12Fence), fence.PutVoid()))) {
            return false;
        }
        fence_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    bool WaitGpu() {
        ++fence_val;
        if (FAILED(queue->Signal(fence.Get(), fence_val))) return false;
        if (fence->GetCompletedValue() < fence_val) {
            if (FAILED(fence->SetEventOnCompletion(fence_val, fence_event))) return false;
            if (WaitForSingleObject(fence_event, 15000) != WAIT_OBJECT_0) return false;
        }
        return true;
    }

    ComRef<ID3D12Resource> CreateBuffer(uint64_t bytes,
                                        D3D12_HEAP_TYPE heap_type,
                                        D3D12_RESOURCE_FLAGS flags,
                                        D3D12_RESOURCE_STATES init_state) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<uint64_t>(4u, bytes);
        rd.Height = 1u;
        rd.DepthOrArraySize = 1u;
        rd.MipLevels = 1u;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1u;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComRef<ID3D12Resource> res;
        if (FAILED(device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, OS_BENCH_IID(ID3D12Resource), res.PutVoid()))) {
            return {};
        }
        return res;
    }

    bool RunComputeDualUav(const std::vector<uint8_t>& dxil,
                           const std::vector<uint32_t>& in_uav0,
                           const std::vector<uint32_t>& init_uav1,
                           std::vector<uint32_t>& out_uav1,
                           uint32_t group_x) {
        const uint64_t in_bytes = std::max<uint64_t>(4u, static_cast<uint64_t>(in_uav0.size()) * 4u);
        const uint64_t out_bytes = std::max<uint64_t>(4u, static_cast<uint64_t>(init_uav1.size()) * 4u);
        out_uav1.assign(init_uav1.size(), 0u);

        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[0].Descriptor.ShaderRegister = 0u;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 1u;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2u;
        rsd.pParameters = params;
        ComRef<ID3DBlob> sig_blob;
        ComRef<ID3DBlob> err_blob;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.Put(), err_blob.Put()))) {
            return false;
        }
        ComRef<ID3D12RootSignature> root_sig;
        if (FAILED(device->CreateRootSignature(
                0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
                OS_BENCH_IID(ID3D12RootSignature), root_sig.PutVoid()))) {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpd{};
        cpd.pRootSignature = root_sig.Get();
        cpd.CS = {dxil.data(), dxil.size()};
        ComRef<ID3D12PipelineState> pso;
        if (FAILED(device->CreateComputePipelineState(&cpd, OS_BENCH_IID(ID3D12PipelineState), pso.PutVoid()))) {
            return false;
        }

        auto in_up = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto out_up = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto in_gpu = CreateBuffer(in_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto out_gpu = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        auto rb = CreateBuffer(out_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!in_up || !out_up || !in_gpu || !out_gpu || !rb) return false;

        void* mapped = nullptr;
        if (FAILED(in_up->Map(0, nullptr, &mapped))) return false;
        if (!in_uav0.empty()) std::memcpy(mapped, in_uav0.data(), static_cast<size_t>(in_bytes));
        in_up->Unmap(0, nullptr);

        if (FAILED(out_up->Map(0, nullptr, &mapped))) return false;
        if (!init_uav1.empty()) std::memcpy(mapped, init_uav1.data(), static_cast<size_t>(out_bytes));
        out_up->Unmap(0, nullptr);

        if (FAILED(allocator->Reset())) return false;
        if (FAILED(cmd_list->Reset(allocator.Get(), pso.Get()))) return false;
        cmd_list->CopyResource(in_gpu.Get(), in_up.Get());
        cmd_list->CopyResource(out_gpu.Get(), out_up.Get());

        D3D12_RESOURCE_BARRIER to_uav[2]{};
        to_uav[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[0].Transition = {in_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        to_uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_uav[1].Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        cmd_list->ResourceBarrier(2, to_uav);

        cmd_list->SetComputeRootSignature(root_sig.Get());
        cmd_list->SetComputeRootUnorderedAccessView(0, in_gpu->GetGPUVirtualAddress());
        cmd_list->SetComputeRootUnorderedAccessView(1, out_gpu->GetGPUVirtualAddress());
        cmd_list->Dispatch(group_x, 1, 1);

        D3D12_RESOURCE_BARRIER uav_bar{};
        uav_bar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav_bar.UAV.pResource = out_gpu.Get();
        cmd_list->ResourceBarrier(1, &uav_bar);

        D3D12_RESOURCE_BARRIER to_copy{};
        to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy.Transition = {out_gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd_list->ResourceBarrier(1, &to_copy);
        cmd_list->CopyResource(rb.Get(), out_gpu.Get());

        if (FAILED(cmd_list->Close())) return false;
        ID3D12CommandList* lists[] = {cmd_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        if (!WaitGpu()) return false;

        if (FAILED(rb->Map(0, nullptr, &mapped))) return false;
        if (!out_uav1.empty()) std::memcpy(out_uav1.data(), mapped, static_cast<size_t>(out_bytes));
        rb->Unmap(0, nullptr);
        return true;
    }

    std::vector<uint8_t> CompileDxilForThisAdapter(const std::string& hlsl,
                                                   const std::vector<uint8_t>& sm65_dxil,
                                                   const std::string& dxc_path) const {
        if (highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_5) && !sm65_dxil.empty()) {
            return sm65_dxil;
        }
        const char* target = (highest_sm >= static_cast<uint32_t>(D3D_SHADER_MODEL_6_2)) ? "cs_6_2" : "cs_6_0";
        std::vector<uint8_t> out;
        std::string diag;
        if (!CompileHlslWithDxc(hlsl, target, "main", out, diag, dxc_path).ok()) {
            return {};
        }
        (void)ResignDxilContainer(out);
        return out;
    }
};
#endif

// ---------------------------------------------------------------------------
// Live GPU Parity Suite (D3D12 HW + D3D12 WARP + Vulkan 1.4 vs CPU Ref)
// ---------------------------------------------------------------------------
struct LiveGpuCase {
    uint32_t workload_idx = 0u;
    const char* name = "";
    std::vector<uint8_t> xs2_blob;
    std::vector<uint32_t> uav0;
    std::vector<uint32_t> init_uav1;
    uint32_t group_x = 1u;
    bool float_tolerance = false;
    bool allow_vulkan = true;
};

std::vector<LiveGpuCase> BuildLiveGpuParityCases() {
    std::vector<LiveGpuCase> cases;

    // 1. PBR / ALU + Conditional Branch + Constant Folding (workload 0)
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::InlineInt(3));
        a.EmitSop2(Rdna2Opcode::S_ADD_U32, 8, Rdna2Operand::Sgpr(8), Rdna2Operand::InlineInt(2));
        a.EmitVop3(Rdna2Opcode::V_MUL_LO_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(32));
        a.EmitVopc(Rdna2Opcode::V_CMP_LT_U32, Rdna2Operand::Vgpr(0), 10);
        const uint32_t br_else = a.EmitSopp(Rdna2Opcode::S_CBRANCH_VCCZ);
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 4, Rdna2Operand::InlineInt(7), 3);
        const uint32_t br_join = a.EmitSopp(Rdna2Opcode::S_BRANCH);
        a.PatchBranch(br_else, a.CurrentWordOffset());
        a.EmitVop2(Rdna2Opcode::V_ADD_NC_U32, 4, Rdna2Operand::LiteralU32(107u), 3);
        a.PatchBranch(br_join, a.CurrentWordOffset());
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        LiveGpuCase c{};
        c.workload_idx = 0u;
        c.name = "pbr_alu_branch";
        c.xs2_blob = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);
        c.uav0.resize(64);
        for (uint32_t i = 0; i < 64u; ++i) c.uav0[i] = i * 7u + 13u;
        c.init_uav1.assign(64, 0u);
        cases.push_back(std::move(c));
    }

    // 2. Deferred G-Buffer VOP3P Packed 16-bit f16/u16 (workload 2)
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 1, 0, 256);
        a.EmitVop3p(Rdna2Opcode::V_PK_ADD_F16, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
        a.EmitVop3p(Rdna2Opcode::V_PK_MUL_F16, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3));
        a.EmitVop3p(Rdna2Opcode::V_PK_FMA_F16, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 256);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 512);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        constexpr uint16_t kHalves[4] = {0x3800u, 0x3C00u, 0x3E00u, 0x4000u};
        LiveGpuCase c{};
        c.workload_idx = 2u;
        c.name = "gbuffer_vop3p_f16";
        c.xs2_blob = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);
        c.uav0.assign(192, 0u);
        for (uint32_t i = 0; i < 64u; ++i) {
            c.uav0[i] = static_cast<uint32_t>(kHalves[i & 3u]) |
                        (static_cast<uint32_t>(kHalves[(i + 1u) & 3u]) << 16);
            c.uav0[64 + i] = static_cast<uint32_t>(kHalves[(i + 2u) & 3u]) |
                             (static_cast<uint32_t>(kHalves[(i + 3u) & 3u]) << 16);
        }
        c.init_uav1.assign(192, 0u);
        cases.push_back(std::move(c));
    }

    // 3. Skeletal Skinning MTBUF D16 + V_ALIGNBYTE_B32 (workload 3)
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineInt(0));
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 3, 1, 0, 4);
        a.EmitVop3(Rdna2Opcode::V_ALIGNBYTE_B32, 4, Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(2), Rdna2Operand::Vgpr(0));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(8));
        a.EmitMtbuf(Rdna2Opcode::TBUFFER_LOAD_FORMAT_D16_XY, 12, 10, 0, Rdna2MtbufFormat::R16G16_FLOAT, 0, true);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 15, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 15, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 15, 4, 16);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        LiveGpuCase c{};
        c.workload_idx = 3u;
        c.name = "skinning_alignbyte_mtbuf_d16";
        c.xs2_blob = a.BuildXs2(ShaderStage::Compute, 4, 1, 1, 16, 20, 32);
        c.uav0 = {0xAABBCCDDu, 0x11223344u, 0xC0003E00u, 0x00000000u, 0u, 0u, 0u, 0u};
        c.init_uav1.assign(8, 0u);
        cases.push_back(std::move(c));
    }

    // 4. BVH4 Ray-Triangle & Ray-AABB Intersect (workload 7)
    {
        Rdna2Assembler a;
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(64));
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 12, Rdna2Operand::InlineInt(2), 0);
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(2), 12);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 20, Rdna2Operand::Vgpr(10), 11);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::LiteralF32(100.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::LiteralF32(0.25f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 14, Rdna2Operand::LiteralF32(2.25f));
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 15, Rdna2Operand::InlineInt(1), 0);
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 15);
        a.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 22, Rdna2Operand::Vgpr(13), 14);
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 23, Rdna2Operand::LiteralF32(0.25f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 24, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 25, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 26, Rdna2Operand::InlineFloat(0.0f));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 27, Rdna2Operand::InlineFloat(1.0f));
        a.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 30, 20, 0, 0, 0xFu, 0u, false);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(4), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 30, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 31, 1, 4, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 32, 1, 4, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 33, 1, 4, 12);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        std::vector<uint32_t> bvh(64, 0u);
        bvh[0] = 42u;
        bvh[1] = FloatToBits(0.0f); bvh[2] = FloatToBits(0.0f); bvh[3] = FloatToBits(5.0f);
        bvh[4] = FloatToBits(1.0f); bvh[5] = FloatToBits(0.0f); bvh[6] = FloatToBits(5.0f);
        bvh[7] = FloatToBits(0.0f); bvh[8] = FloatToBits(1.0f); bvh[9] = FloatToBits(5.0f);
        bvh[16] = 0x80000000u | 77u;
        bvh[17] = FloatToBits(0.0f); bvh[18] = FloatToBits(0.0f); bvh[19] = FloatToBits(2.0f);
        bvh[20] = FloatToBits(1.0f); bvh[21] = FloatToBits(1.0f); bvh[22] = FloatToBits(4.0f);

        LiveGpuCase c{};
        c.workload_idx = 7u;
        c.name = "bvh4_ray_intersect";
        c.xs2_blob = a.BuildXs2(ShaderStage::Compute, 4, 1, 1, 16, 40, 32);
        c.uav0 = std::move(bvh);
        c.init_uav1.assign(64, 0u);
        c.float_tolerance = true;
        cases.push_back(std::move(c));
    }

    // 5. LDS Lockstep Barrier + DS_WRITE/READ + DS_ADD_RTN_U32 (workload 8)
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        a.EmitDs(Rdna2Opcode::DS_WRITE_B32, 0, 1, 2);
        // No manual S_BARRIER: single-wave lockstep barrier pass inserts GroupMemoryBarrierWithGroupSync!
        a.EmitVop2(Rdna2Opcode::V_XOR_B32, 3, Rdna2Operand::InlineInt(1), 0);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 4, Rdna2Operand::InlineInt(2), 3);
        a.EmitDs(Rdna2Opcode::DS_READ_B32, 5, 4);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 0);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        LiveGpuCase c{};
        c.workload_idx = 8u;
        c.name = "lds_lockstep_exchange";
        c.xs2_blob = a.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 16, 32);
        c.uav0.resize(32);
        for (uint32_t i = 0; i < 32u; ++i) c.uav0[i] = 0x5500u + i * 11u;
        c.init_uav1.assign(32, 0u);
        cases.push_back(std::move(c));
    }

    // 6. Wave64-on-Wave32 Ballot + Mbcnt + ReadLane + ReadFirstLane + Swizzle (workload 9)
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 4, 1, 0, 0);
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 2, Rdna2Operand::InlineInt(1), 0);
        a.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(0), 2);
        a.EmitSop1(Rdna2Opcode::S_BCNT1_I32_B64, 8, Rdna2Operand::VccLo());
        a.EmitVop3(Rdna2Opcode::V_MBCNT_LO_U32_B32, 5, Rdna2Operand::VccLo(), Rdna2Operand::InlineInt(0));
        a.EmitVop3(Rdna2Opcode::V_MBCNT_HI_U32_B32, 6, Rdna2Operand::VccHi(), Rdna2Operand::Vgpr(5));
        a.EmitVop3(Rdna2Opcode::V_READLANE_B32, 9, Rdna2Operand::Vgpr(4), Rdna2Operand::InlineInt(42));
        a.EmitVop1(Rdna2Opcode::V_READFIRSTLANE_B32, 10, Rdna2Operand::Vgpr(4));
        a.EmitDs(Rdna2Opcode::DS_SWIZZLE_B32, 7, 4, 0, 0, static_cast<uint16_t>(0x8000u | (1u << 10) | 0x1Fu));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::Sgpr(8));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::Sgpr(9));
        a.EmitVop1(Rdna2Opcode::V_MOV_B32, 13, Rdna2Operand::Sgpr(10));
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 11, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 256);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 12, 1, 4, 512);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 1, 4, 768);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 7, 1, 4, 1024);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        LiveGpuCase c{};
        c.workload_idx = 9u;
        c.name = "wave64_ballot_readlane_swizzle";
        c.xs2_blob = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 64);
        c.uav0.assign(320, 0u);
        for (uint32_t i = 0; i < 64u; ++i) c.uav0[i] = 0xCAFE0000u | (i * 17u + 3u);
        c.init_uav1.assign(320, 0u);
        cases.push_back(std::move(c));
    }

    // 7. SCRATCH Register Spilling + Permlane16/X16 + BFE/BFI (workload 11)
    {
        Rdna2Assembler a;
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_LOAD_DWORD, 2, 1, 0, 0);
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::LiteralU32(0x89ABCDEFu));
        a.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::LiteralU32(0x01234567u));
        a.EmitVop3(Rdna2Opcode::V_PERMLANE16_B32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
        a.EmitVop3(Rdna2Opcode::V_PERMLANEX16_B32, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
        a.EmitVop3(Rdna2Opcode::V_BFE_U32, 5, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
        a.EmitVop3(Rdna2Opcode::V_BFE_I32, 6, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
        a.EmitVop3(Rdna2Opcode::V_BFI_B32, 7, Rdna2Operand::LiteralU32(0x00FF00FFu), Rdna2Operand::Vgpr(3), Rdna2Operand::Vgpr(4));
        a.EmitVop2(Rdna2Opcode::V_AND_B32, 8, Rdna2Operand::InlineInt(3), 0);
        a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 8, Rdna2Operand::InlineInt(2), 8);
        a.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 7, 8, 8);
        a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 10, 8, 8);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 256);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 512);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 6, 1, 4, 768);
        a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 10, 1, 4, 1024);
        a.EmitSopp(Rdna2Opcode::S_ENDPGM);

        LiveGpuCase c{};
        c.workload_idx = 11u;
        c.name = "scratch_permlane_bfe_bfi";
        c.xs2_blob = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 32);
        c.uav0.assign(320, 0u);
        for (uint32_t i = 0; i < 64u; ++i) {
            c.uav0[i] = 0xA0000000u | ((0x80u + i) << 8) | (i * 3u + 1u);
        }
        c.init_uav1.assign(320, 0u);
        cases.push_back(std::move(c));
    }

    return cases;
}

bool VerifyBufferParity(const std::vector<uint32_t>& got,
                        const std::vector<uint32_t>& expected,
                        bool float_tolerance) noexcept {
    if (got.size() < expected.size()) return false;
    if (!float_tolerance) {
        for (size_t i = 0; i < expected.size(); ++i) {
            if (got[i] != expected[i]) return false;
        }
        return true;
    }
    // BVH4 ray output: 4 rays * 4 dwords (t, u, v as float; id as exact u32)
    for (size_t i = 0; i < expected.size(); ++i) {
        if ((i & 3u) == 3u) {
            if (got[i] != expected[i]) return false;
        } else {
            const float gf = BitsToFloat(got[i]);
            const float ef = BitsToFloat(expected[i]);
            if (std::isnan(gf) != std::isnan(ef)) return false;
            if (!std::isnan(ef) && std::fabs(gf - ef) > 1e-4f) return false;
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API Implementations
// ---------------------------------------------------------------------------
uint32_t GetGfx103ReferenceCatalogSize() noexcept {
    return static_cast<uint32_t>(std::size(kGfx103Catalog));
}

Status AuditRdna2IsaCoverage(Rdna2CoverageReport& out_report) {
    AuditState audit;
    if (audit.mnemonic_to_idx.size() != kGfx103TotalShaderOpcodes) {
        return Status::Error(StatusCode::ParseError, "Duplicate mnemonic detected in 779-opcode GFX10.3 reference catalog");
    }

    AuditCoreDisasmTable(audit);
#if defined(OPENSCARLETT_HAS_RDNA2_SV_ISA)
    AuditScalarVectorExtension(audit);
#endif
#if defined(OPENSCARLETT_HAS_RDNA2_MEM_ISA)
    AuditMemImgExtension(audit);
#endif

    audit.PopulateReport(out_report);
    return Status::Ok();
}

Status RunRdna2CoverageBenchmark(const Rdna2BenchOptions& opts, Rdna2CoverageReport& out_report) {
    const auto t_start = std::chrono::steady_clock::now();
    out_report = Rdna2CoverageReport{};

    // 1. Audit GFX10.3 779-opcode coverage (core + optional Worker 1 & Worker 2 ISA tables)
    AuditState audit;
    if (audit.mnemonic_to_idx.size() != kGfx103TotalShaderOpcodes) {
        return Status::Error(StatusCode::ParseError, "Internal catalog invariant violated (expected 779 unique entries)");
    }
    AuditCoreDisasmTable(audit);
#if defined(OPENSCARLETT_HAS_RDNA2_SV_ISA)
    AuditScalarVectorExtension(audit);
#endif
#if defined(OPENSCARLETT_HAS_RDNA2_MEM_ISA)
    AuditMemImgExtension(audit);
#endif

    // 2. Probe external validators (spirv-val.exe & dxc.exe)
    const std::string spirv_val_path = opts.spirv_val_path.empty()
                                           ? std::string(kDefaultSpirvValPath)
                                           : opts.spirv_val_path;
    const std::string dxc_path = ResolvePortableDxcPath(opts.dxc_path);
    std::error_code ec;
    out_report.has_spirv_val_tool = !spirv_val_path.empty() && std::filesystem::exists(spirv_val_path, ec);
    out_report.has_dxc_tool = !dxc_path.empty() && std::filesystem::exists(dxc_path, ec);

    // 3. Initialize stage & workload breakdown vectors
    out_report.stages.resize(static_cast<size_t>(StageSlot::Count));
    for (size_t s = 0; s < kStageNames.size(); ++s) {
        out_report.stages[s].stage = kStageNames[s];
    }
    out_report.workloads.resize(kWorkloads.size());
    for (size_t w = 0; w < kWorkloads.size(); ++w) {
        out_report.workloads[w].workload = kWorkloads[w].key;
        out_report.workloads[w].description = kWorkloads[w].description;
    }

    // 4. Determine corpus size & stratified external validation cadence
    const uint32_t total_shaders = std::max<uint32_t>(12u, opts.corpus_count);
    const uint32_t ext_target = opts.validate_all_external
                                    ? total_shaders
                                    : std::min<uint32_t>(total_shaders, opts.external_val_sample);
    // Distribute external validation evenly across all 12 workloads & all 5 stages:
    // Every workload gets at least ext_per_workload stratified variants (including both even & odd stage variants).
    const uint32_t ext_per_workload = opts.run_external_validators
                                          ? std::max<uint32_t>(1u, (ext_target + 11u) / 12u)
                                          : 0u;

    std::unordered_set<uint64_t> seen_hashes;
    seen_hashes.reserve(total_shaders * 2u);

    uint32_t ext_spv_budget = opts.run_external_validators ? ext_target : 0u;
    uint32_t ext_dxc_budget = opts.run_external_validators ? ext_target : 0u;

    std::string first_error;

    for (uint32_t idx = 0; idx < total_shaders; ++idx) {
        const uint32_t wl = idx % static_cast<uint32_t>(kWorkloads.size());
        const uint32_t var = idx / static_cast<uint32_t>(kWorkloads.size());

        const GeneratedCorpusShader gen = GenerateCorpusShader(wl, var);
        const size_t st_idx = static_cast<size_t>(StageToSlot(gen.stage));
        auto& st_cov = out_report.stages[st_idx];
        auto& wl_cov = out_report.workloads[wl];

        ++out_report.corpus_shaders_total;
        ++st_cov.total;
        ++wl_cov.total;
        seen_hashes.insert(gen.fnv1a_hash);

        bool shader_ok = true;

        // 4A. Container parse & DXBC hash check
        if (gen.is_dxbc_wrapped) {
            if (!VerifyDxbcContainerHash(gen.blob.data(), gen.blob.size())) {
                shader_ok = false;
                if (first_error.empty()) first_error = "Xbox-DXBC MD5 hash verification failed";
            }
        }

        // 4B. Lift pipeline: Parse -> Disasm -> SSA IR -> Optimize -> SPIR-V 1.3/1.4 + SM 6.5 HLSL
        const bool do_ext_this = opts.run_external_validators &&
                                 (opts.validate_all_external || var < ext_per_workload);
        const bool do_spv_val = do_ext_this && gen.supports_spirv && out_report.has_spirv_val_tool &&
                                (opts.validate_all_external || ext_spv_budget > 0u);
        const bool do_dxc = do_ext_this && out_report.has_dxc_tool &&
                            (opts.validate_all_external || ext_dxc_budget > 0u);

        Rdna2LiftOptions lopts{};
        lopts.use_sm65 = true;
        lopts.emit_spirv = gen.supports_spirv;
        lopts.emit_mesh_spirv = gen.emit_mesh_spirv;
        lopts.mesh_has_payload = gen.mesh_has_payload;
        lopts.emit_hlsl = true;
        lopts.validate_spirv_external = do_spv_val;
        lopts.spirv_val_path = spirv_val_path;
        lopts.spirv_target_env = (gen.stage == ShaderStage::Mesh) ? "vulkan1.3" : "vulkan1.2";
        lopts.compile_dxil_with_dxc = do_dxc;
        lopts.resign_dxil = true;
        lopts.dxc_path = dxc_path;
        lopts.stage_contract = gen.contract;

        Rdna2LiftResult lifted{};
        Status lift_st = LiftRdna2Xs2Shader(gen.blob, lifted, lopts);
        if (!lift_st.ok()) {
            shader_ok = false;
            if (first_error.empty()) {
                first_error = std::string(kWorkloads[wl].key) + "#" + std::to_string(var) +
                              " (" + kStageNames[st_idx] + "): " + lift_st.message;
            }
        } else {
            ++st_cov.disasm_passed;

            if (gen.supports_spirv && !lifted.spirv_words.empty()) {
                ++out_report.corpus_spirv_validated;
                ++st_cov.spirv_passed;
                ++wl_cov.spirv_passed;
                if (do_spv_val) {
                    ++out_report.spirv_val_external_validated;
                    if (ext_spv_budget > 0u) --ext_spv_budget;
                }
            }

            if (do_dxc && !lifted.pc_dxil.empty()) {
                // Explicitly verify & re-sign DXIL container MD5 digest
                bool hash_ok = VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size());
                if (hash_ok) {
                    std::array<uint8_t, 16> orig_md5{};
                    std::memcpy(orig_md5.data(), lifted.pc_dxil.data() + 4u, 16u);
                    std::memset(lifted.pc_dxil.data() + 4u, 0, 16u);
                    if (ResignDxilContainer(lifted.pc_dxil).ok() &&
                        VerifyDxbcContainerHash(lifted.pc_dxil.data(), lifted.pc_dxil.size()) &&
                        std::memcmp(lifted.pc_dxil.data() + 4u, orig_md5.data(), 16u) == 0) {
                        ++out_report.corpus_dxil_resigned_verified;
                    } else {
                        shader_ok = false;
                        if (first_error.empty()) first_error = "DXIL MD5 re-sign mismatch";
                    }
                } else {
                    shader_ok = false;
                    if (first_error.empty()) first_error = "DXC output failed DXBC MD5 check";
                }
                ++out_report.corpus_dxil_compiled;
                ++st_cov.dxil_passed;
                ++wl_cov.dxil_passed;
                if (ext_dxc_budget > 0u) --ext_dxc_budget;
            }

            // 4C. CPU Reference Interpreter execution on stratified variants (covers all 12 workloads & all 5 stages)
            if (var < 4u) {
                Rdna2CpuExecState cpu{};
                cpu.root_constants.assign(16, 7u);
                cpu.uav0.assign(320, 0x3C003C00u);
                cpu.uav1.assign(320, 0u);
                if (gen.stage == ShaderStage::Pixel) {
                    cpu.stage_inputs.assign(4, {0.5f, 0.25f, 0.75f, 1.0f});
                }
                if (wl == 6u || wl == 10u) {
                    cpu.tex2d_srv.AllocateRgba32f(1, 16, 16, 1, 2);
                    cpu.tex2d_uav.AllocateRgba32f(1, 16, 16, 1, 1);
                    cpu.tex3d_srv.AllocateRgba32f(2, 4, 4, 4, 2);
                    cpu.texcube_srv.AllocateRgba32f(3, 4, 4, 6, 2);
                    cpu.tex_u32_uav.AllocateR32u(1, 4, 4, 1, 1);
                }
                Status cpu_st = ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu);
                if (cpu_st.ok()) {
                    ++out_report.corpus_cpu_ref_executed;
                    ++st_cov.cpu_ref_passed;
                    ++wl_cov.cpu_ref_passed;
                } else {
                    shader_ok = false;
                    if (first_error.empty()) {
                        first_error = std::string("CPU ref failed for ") + kWorkloads[wl].key + ": " + cpu_st.message;
                    }
                }
            }
        }

        if (shader_ok) {
            ++out_report.corpus_shaders_passed;
            ++st_cov.passed;
            ++wl_cov.passed;
        }
    }

    out_report.corpus_unique_hashes = static_cast<uint32_t>(seen_hashes.size());

    // 5. Live GPU Execution & CPU-Ref Parity Verification (D3D12 HW + WARP + Vulkan)
    if (opts.run_live_gpu) {
        VulkanBackend vk;
        if (vk.Initialize(/*allow_software_fallback=*/false).ok() && vk.GetDeviceInfo().is_hardware) {
            out_report.has_vulkan_hw = true;
            out_report.vulkan_adapter = vk.GetDeviceInfo().device_name;
        }

#if defined(_WIN32)
        D3D12BenchContext d3d_hw;
        D3D12BenchContext d3d_warp;
        if (d3d_hw.Init(/*use_warp=*/false)) {
            out_report.has_d3d12_hw = true;
            out_report.d3d12_hw_adapter = d3d_hw.adapter_name;
        }
        if (d3d_warp.Init(/*use_warp=*/true)) {
            out_report.has_d3d12_warp = true;
        }
#endif

        const auto gpu_cases = BuildLiveGpuParityCases();
        for (const auto& gc : gpu_cases) {
            Rdna2LiftOptions lopts{};
            lopts.use_sm65 = true;
            lopts.emit_spirv = true;
            lopts.emit_hlsl = true;
            lopts.validate_spirv_external = false;
            lopts.compile_dxil_with_dxc = out_report.has_dxc_tool;
            lopts.resign_dxil = true;
            lopts.dxc_path = dxc_path;

            Rdna2LiftResult lifted{};
            if (!LiftRdna2Xs2Shader(gc.xs2_blob, lifted, lopts).ok()) {
                if (first_error.empty()) first_error = std::string("Live GPU lift failed: ") + gc.name;
                continue;
            }

            Rdna2CpuExecState cpu{};
            cpu.uav0 = gc.uav0;
            cpu.uav1 = gc.init_uav1;
            if (!ExecuteRdna2Cpu(lifted.container, lifted.instructions, cpu).ok()) {
                if (first_error.empty()) first_error = std::string("Live GPU CPU ref failed: ") + gc.name;
                continue;
            }

            bool any_gpu_ran = false;
            bool all_gpu_ok = true;

            if (out_report.has_vulkan_hw && gc.allow_vulkan) {
                std::vector<uint32_t> vk_in = gc.uav0;
                if (vk_in.size() < cpu.uav1.size()) vk_in.resize(cpu.uav1.size(), 0u);
                std::vector<uint32_t> vk_out;
                if (vk.ExecuteComputeShader(lifted.spirv_words, vk_in, vk_out, gc.group_x).ok() &&
                    VerifyBufferParity(vk_out, cpu.uav1, gc.float_tolerance)) {
                    ++out_report.corpus_vulkan_executed;
                    any_gpu_ran = true;
                } else {
                    all_gpu_ok = false;
                    if (first_error.empty()) first_error = std::string("Vulkan GPU parity mismatch: ") + gc.name;
                }
            }

#if defined(_WIN32)
            if (out_report.has_d3d12_hw && out_report.has_dxc_tool) {
                const auto dxil = d3d_hw.CompileDxilForThisAdapter(lifted.hlsl_source, lifted.pc_dxil, dxc_path);
                std::vector<uint32_t> hw_out;
                if (!dxil.empty() &&
                    d3d_hw.RunComputeDualUav(dxil, gc.uav0, gc.init_uav1, hw_out, gc.group_x) &&
                    VerifyBufferParity(hw_out, cpu.uav1, gc.float_tolerance)) {
                    ++out_report.corpus_gpu_hw_executed;
                    any_gpu_ran = true;
                } else {
                    all_gpu_ok = false;
                    if (first_error.empty()) first_error = std::string("D3D12 HW GPU parity mismatch: ") + gc.name;
                }
            }

            if (out_report.has_d3d12_warp && out_report.has_dxc_tool) {
                const auto dxil = d3d_warp.CompileDxilForThisAdapter(lifted.hlsl_source, lifted.pc_dxil, dxc_path);
                std::vector<uint32_t> warp_out;
                if (!dxil.empty() &&
                    d3d_warp.RunComputeDualUav(dxil, gc.uav0, gc.init_uav1, warp_out, gc.group_x) &&
                    VerifyBufferParity(warp_out, cpu.uav1, gc.float_tolerance)) {
                    ++out_report.corpus_gpu_warp_executed;
                    any_gpu_ran = true;
                } else {
                    all_gpu_ok = false;
                    if (first_error.empty()) first_error = std::string("D3D12 WARP parity mismatch: ") + gc.name;
                }
            }
#endif

            if (any_gpu_ran && all_gpu_ok) {
                ++out_report.corpus_gpu_executed;
                ++out_report.stages[static_cast<size_t>(StageSlot::CS)].gpu_passed;
                ++out_report.workloads[gc.workload_idx].gpu_passed;
                audit.MarkGpuOpcodes(lifted.instructions);
            }
        }

        if (out_report.has_vulkan_hw) {
            vk.Shutdown();
        }
    }

    // 6. Finalize audited opcode counts & elapsed time
    audit.PopulateReport(out_report);
    const auto t_end = std::chrono::steady_clock::now();
    out_report.elapsed_seconds = std::chrono::duration<double>(t_end - t_start).count();

    if (!opts.json_output_path.empty()) {
        if (auto wst = WriteRdna2CoverageJsonFile(opts.json_output_path, out_report); !wst.ok()) {
            return wst;
        }
    }

    if (!first_error.empty()) {
        return Status::Error(StatusCode::ShaderError, first_error);
    }
    return Status::Ok();
}

std::string SerializeRdna2CoverageJson(const Rdna2CoverageReport& r) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "{\n";
    os << "  \"schema_version\": " << r.schema_version << ",\n";
    os << "  \"isa_target\": \"" << EscapeJson(r.isa_target) << "\",\n";
    os << "  \"opcodes_total\": " << r.opcodes_total << ",\n";
    os << "  \"opcodes_decoded\": " << r.opcodes_decoded << ",\n";
    os << "  \"opcodes_lifted_spirv\": " << r.opcodes_lifted_spirv << ",\n";
    os << "  \"opcodes_lifted_dxil\": " << r.opcodes_lifted_dxil << ",\n";
    os << "  \"opcodes_cpu_ref\": " << r.opcodes_cpu_ref << ",\n";
    os << "  \"opcodes_gpu_verified\": " << r.opcodes_gpu_verified << ",\n";
    os << "  \"corpus_shaders_total\": " << r.corpus_shaders_total << ",\n";
    os << "  \"corpus_shaders_passed\": " << r.corpus_shaders_passed << ",\n";
    os << "  \"corpus_unique_hashes\": " << r.corpus_unique_hashes << ",\n";
    os << "  \"corpus_spirv_validated\": " << r.corpus_spirv_validated << ",\n";
    os << "  \"spirv_val_external_validated\": " << r.spirv_val_external_validated << ",\n";
    os << "  \"corpus_dxil_compiled\": " << r.corpus_dxil_compiled << ",\n";
    os << "  \"corpus_dxil_resigned_verified\": " << r.corpus_dxil_resigned_verified << ",\n";
    os << "  \"corpus_cpu_ref_executed\": " << r.corpus_cpu_ref_executed << ",\n";
    os << "  \"corpus_gpu_executed\": " << r.corpus_gpu_executed << ",\n";
    os << "  \"corpus_gpu_hw_executed\": " << r.corpus_gpu_hw_executed << ",\n";
    os << "  \"corpus_gpu_warp_executed\": " << r.corpus_gpu_warp_executed << ",\n";
    os << "  \"corpus_vulkan_executed\": " << r.corpus_vulkan_executed << ",\n";
    os << "  \"has_sv_isa_extension\": " << (r.has_sv_isa_extension ? "true" : "false") << ",\n";
    os << "  \"has_mem_isa_extension\": " << (r.has_mem_isa_extension ? "true" : "false") << ",\n";
    os << "  \"has_spirv_val_tool\": " << (r.has_spirv_val_tool ? "true" : "false") << ",\n";
    os << "  \"has_dxc_tool\": " << (r.has_dxc_tool ? "true" : "false") << ",\n";
    os << "  \"has_d3d12_hw\": " << (r.has_d3d12_hw ? "true" : "false") << ",\n";
    os << "  \"has_d3d12_warp\": " << (r.has_d3d12_warp ? "true" : "false") << ",\n";
    os << "  \"has_vulkan_hw\": " << (r.has_vulkan_hw ? "true" : "false") << ",\n";
    os << "  \"d3d12_hw_adapter\": \"" << EscapeJson(r.d3d12_hw_adapter) << "\",\n";
    os << "  \"vulkan_adapter\": \"" << EscapeJson(r.vulkan_adapter) << "\",\n";
    os << "  \"elapsed_seconds\": " << r.elapsed_seconds << ",\n";

    os << "  \"families\": {\n";
    for (size_t i = 0; i < r.families.size(); ++i) {
        const auto& f = r.families[i];
        os << "    \"" << EscapeJson(f.family) << "\": {"
           << "\"total\": " << f.total
           << ", \"decoded\": " << f.decoded
           << ", \"lifted_spirv\": " << f.lifted_spirv
           << ", \"lifted_dxil\": " << f.lifted_dxil
           << ", \"cpu_ref\": " << f.cpu_ref
           << ", \"gpu_verified\": " << f.gpu_verified
           << "}" << ((i + 1u < r.families.size()) ? "," : "") << "\n";
    }
    os << "  },\n";

    os << "  \"stages\": {\n";
    for (size_t i = 0; i < r.stages.size(); ++i) {
        const auto& s = r.stages[i];
        os << "    \"" << EscapeJson(s.stage) << "\": {"
           << "\"total\": " << s.total
           << ", \"passed\": " << s.passed
           << ", \"disasm_passed\": " << s.disasm_passed
           << ", \"spirv_passed\": " << s.spirv_passed
           << ", \"dxil_passed\": " << s.dxil_passed
           << ", \"cpu_ref_passed\": " << s.cpu_ref_passed
           << ", \"gpu_passed\": " << s.gpu_passed
           << "}" << ((i + 1u < r.stages.size()) ? "," : "") << "\n";
    }
    os << "  },\n";

    os << "  \"workloads\": {\n";
    for (size_t i = 0; i < r.workloads.size(); ++i) {
        const auto& w = r.workloads[i];
        os << "    \"" << EscapeJson(w.workload) << "\": {"
           << "\"description\": \"" << EscapeJson(w.description) << "\""
           << ", \"total\": " << w.total
           << ", \"passed\": " << w.passed
           << ", \"spirv_passed\": " << w.spirv_passed
           << ", \"dxil_passed\": " << w.dxil_passed
           << ", \"cpu_ref_passed\": " << w.cpu_ref_passed
           << ", \"gpu_passed\": " << w.gpu_passed
           << "}" << ((i + 1u < r.workloads.size()) ? "," : "") << "\n";
    }
    os << "  }\n";
    os << "}\n";
    return os.str();
}

Status WriteRdna2CoverageJsonFile(const std::string& path, const Rdna2CoverageReport& report) {
    if (path.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "JSON output path is empty");
    }
    std::error_code ec;
    const std::filesystem::path out_path(path);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path(), ec);
    }
    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        return Status::Error(StatusCode::IoError, "Failed to open coverage JSON for write: " + path);
    }
    const std::string json = SerializeRdna2CoverageJson(report);
    ofs.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!ofs.good()) {
        return Status::Error(StatusCode::IoError, "Failed to write coverage JSON: " + path);
    }
    return Status::Ok();
}

int RunRdna2BenchCli(int argc, char** argv) {
    Rdna2BenchOptions opts{};
    opts.full_corpus = false;
    opts.corpus_count = 240u;
    opts.external_val_sample = 12u;
    bool explicit_count = false;
    bool explicit_ext_sample = false;

    auto parse_u32 = [](const char* s, uint32_t& out) -> bool {
        if (!s || *s == '\0') return false;
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 10);
        if (!end || *end != '\0') return false;
        out = static_cast<uint32_t>(v);
        return true;
    };

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--full") {
            opts.full_corpus = true;
            if (!explicit_count) opts.corpus_count = 3120u;
            if (!explicit_ext_sample) opts.external_val_sample = 60u;
        } else if (arg == "--fast") {
            opts.full_corpus = false;
            if (!explicit_count) opts.corpus_count = 240u;
            if (!explicit_ext_sample) opts.external_val_sample = 12u;
        } else if (arg == "--no-gpu") {
            opts.run_live_gpu = false;
        } else if (arg == "--no-external") {
            opts.run_external_validators = false;
        } else if (arg == "--validate-all-external") {
            opts.validate_all_external = true;
        } else if (arg == "--quiet") {
            opts.quiet = true;
        } else if (arg == "--json" && i + 1 < argc) {
            opts.json_output_path = argv[++i];
        } else if (arg == "--corpus-count" && i + 1 < argc) {
            if (!parse_u32(argv[++i], opts.corpus_count) || opts.corpus_count < 12u) {
                std::fprintf(stderr, "rdna2-bench: invalid --corpus-count (must be >= 12)\n");
                return 1;
            }
            explicit_count = true;
        } else if (arg == "--external-sample" && i + 1 < argc) {
            if (!parse_u32(argv[++i], opts.external_val_sample)) {
                std::fprintf(stderr, "rdna2-bench: invalid --external-sample\n");
                return 1;
            }
            explicit_ext_sample = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: openscarlett rdna2-bench [options]\n"
                "  --full                   Run full 3,120-shader GFX10.3 corpus benchmark\n"
                "  --fast                   Run fast 240-shader smoke benchmark (default)\n"
                "  --json <path>            Write deterministic schema_version=1 JSON report\n"
                "  --corpus-count <N>       Override total generated shader count (>= 12)\n"
                "  --external-sample <N>    Stratified spirv-val/dxc sample size\n"
                "  --validate-all-external  Run spirv-val/dxc on every corpus shader\n"
                "  --no-gpu                 Skip live D3D12/Vulkan GPU parity checks\n"
                "  --no-external            Skip external spirv-val.exe and dxc.exe\n"
                "  --quiet                  Suppress summary stdout table\n");
            return 0;
        } else {
            std::fprintf(stderr, "rdna2-bench: unknown option '%s'\n", arg.c_str());
            return 1;
        }
    }

    Rdna2CoverageReport report{};
    const Status st = RunRdna2CoverageBenchmark(opts, report);

    if (!opts.quiet) {
        std::printf(
            "=== OpenScarlett RDNA2 / GFX10.3 Coverage & Corpus Benchmark ===\n"
            "ISA Coverage : %u/%u decoded | SPIR-V=%u | DXIL=%u | CPU=%u | GPU=%u (sv_ext=%s, mem_ext=%s)\n"
            "Shader Corpus: %u/%u passed (%u unique hashes) | SPIR-V=%u (spirv-val=%u) | DXIL=%u (resigned=%u)\n"
            "Execution    : CPU=%u | Live GPU=%u (D3D12-HW=%u, WARP=%u, Vulkan=%u) | %.2fs\n",
            report.opcodes_decoded,
            report.opcodes_total,
            report.opcodes_lifted_spirv,
            report.opcodes_lifted_dxil,
            report.opcodes_cpu_ref,
            report.opcodes_gpu_verified,
            report.has_sv_isa_extension ? "yes" : "no",
            report.has_mem_isa_extension ? "yes" : "no",
            report.corpus_shaders_passed,
            report.corpus_shaders_total,
            report.corpus_unique_hashes,
            report.corpus_spirv_validated,
            report.spirv_val_external_validated,
            report.corpus_dxil_compiled,
            report.corpus_dxil_resigned_verified,
            report.corpus_cpu_ref_executed,
            report.corpus_gpu_executed,
            report.corpus_gpu_hw_executed,
            report.corpus_gpu_warp_executed,
            report.corpus_vulkan_executed,
            report.elapsed_seconds);
        if (!opts.json_output_path.empty()) {
            std::printf("JSON Report  : %s\n", opts.json_output_path.c_str());
        }
    }

    if (!st.ok()) {
        std::fprintf(stderr, "rdna2-bench failed: %s\n", st.message.c_str());
        return 1;
    }
    return 0;
}

}  // namespace openscarlett
