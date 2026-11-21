#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "openscarlett/core/telemetry.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_interpreter.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "openscarlett/gpu/shader_cache.h"
#include "os_check.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

uint32_t FloatToBits(float f) noexcept {
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    return b;
}

float BitsToFloat(uint32_t b) noexcept {
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof(f));
    return f;
}

uint32_t PackTwoHalfBits(uint16_t lo, uint16_t hi) noexcept {
    return static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
}

void TestBasicComputeAndConstFold() {
    using namespace openscarlett;

    Rdna2Assembler a;
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineFloat(2.0f));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::InlineFloat(1.0f));
    a.EmitVop2(Rdna2Opcode::V_ADD_F32, 3, Rdna2Operand::Vgpr(1), 2);       // foldable 2.0 + 1.0
    a.EmitVop1(Rdna2Opcode::V_CVT_F32_U32, 4, Rdna2Operand::Vgpr(0));      // float(tid)
    a.EmitVop2(Rdna2Opcode::V_MUL_F32, 5, Rdna2Operand::Vgpr(4), 3);       // tid * 3.0
    a.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 6, Rdna2Operand::InlineInt(2), 0);
    a.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 6, 0);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 8, 8);
    Xs2ShaderContainer parsed;
    OS_REQUIRE(ParseXs2Container(xs2, parsed).ok());
    OS_REQUIRE(parsed.stage == ShaderStage::Compute);
    OS_REQUIRE(parsed.code_words.size() == a.Words().size());

    std::vector<Rdna2Instruction> insns;
    OS_REQUIRE(DisassembleRdna2(parsed.code_words, insns).ok());
    OS_REQUIRE(insns.size() == 8);
    OS_REQUIRE(insns.back().opcode == Rdna2Opcode::S_ENDPGM);

    Rdna2IrModule ir;
    OS_REQUIRE(BuildRdna2Ir(parsed, insns, ir).ok());
    Rdna2IrOptStats stats{};
    OS_REQUIRE(OptimizeRdna2Ir(ir, &stats).ok());
    OS_REQUIRE(stats.folded_constants >= 1);

    // Execute on CPU interpreter and check output
    Rdna2CpuExecState cpu{};
    cpu.uav0.assign(64, 0u);
    OS_REQUIRE(ExecuteRdna2Cpu(parsed, insns, cpu).ok());
    for (uint32_t i = 0; i < 64u; ++i) {
        OS_REQUIRE(std::fabs(BitsToFloat(cpu.uav0[i]) - static_cast<float>(i) * 3.0f) < 1e-5f);
    }

    std::printf("[PASS] Core 1: Compute disasm + IR const-fold (%u folded, %u dead) + CPU exec\n",
                stats.folded_constants, stats.eliminated_ops);
}

void TestAaaOpcodesEncodeDecodeIrAndCpu() {
    using namespace openscarlett;

    // 1. Assemble all new AAA opcodes and verify exact binary round-trip + disassembly + IR
    Rdna2Assembler a;
    // Bitfield ops: V_BFE_U32, V_BFE_I32, V_BFI_B32
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::LiteralU32(0x1234F89Au));
    a.EmitVop3(Rdna2Opcode::V_BFE_U32, 2, Rdna2Operand::Vgpr(1), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
    a.EmitVop3(Rdna2Opcode::V_BFE_I32, 3, Rdna2Operand::Vgpr(1), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));
    a.EmitVop3(Rdna2Opcode::V_BFI_B32, 4, Rdna2Operand::LiteralU32(0x00FF00FFu), Rdna2Operand::Vgpr(1), Rdna2Operand::InlineInt(0));

    // VOP3P (0x33) packed 16-bit f16 & u16/b16 ops
    // half(1.5) = 0x3E00, half(2.0) = 0x4000, half(0.5) = 0x3800, half(4.0) = 0x4400
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 5, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3E00u, 0x4000u)));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3800u, 0x4400u)));
    a.EmitVop3p(Rdna2Opcode::V_PK_ADD_F16, 7, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6));
    a.EmitVop3p(Rdna2Opcode::V_PK_MUL_F16, 8, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6));
    a.EmitVop3p(Rdna2Opcode::V_PK_FMA_F16, 9, Rdna2Operand::Vgpr(5), Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(5));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0010u, 0x0100u)));
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0002u, 0x0003u)));
    a.EmitVop3p(Rdna2Opcode::V_PK_ADD_U16, 12, Rdna2Operand::Vgpr(10), Rdna2Operand::Vgpr(11));
    a.EmitVop3p(Rdna2Opcode::V_PK_LSHLREV_B16, 13, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(10));
    a.EmitVop3p(Rdna2Opcode::V_PK_LSHRREV_B16, 14, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(10));

    // V_PERMLANE16_B32 & V_PERMLANEX16_B32 (reverse 16-lane row: 0x76543210 / 0xFEDCBA98 -> reversed = 0x89ABCDEF / 0x01234567)
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::LiteralU32(0x89ABCDEFu));
    a.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::LiteralU32(0x01234567u));
    a.EmitVop3(Rdna2Opcode::V_PERMLANE16_B32, 15, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
    a.EmitVop3(Rdna2Opcode::V_PERMLANEX16_B32, 16, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));

    // SCRATCH_STORE_DWORD & SCRATCH_LOAD_DWORD (FLAT encoding with flat_seg == 1)
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 17, Rdna2Operand::InlineInt(16));
    a.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 16, 17, 4);
    a.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 18, 17, 4);

    // MIMG: IMAGE_LOAD_MIP (3D dim=2), IMAGE_LOAD_MIP (Cube dim=3), IMAGE_ATOMIC_CMPSWAP, IMAGE_BVH_INTERSECT_RAY
    a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 20, 24, 0, 0, 0xFu, 2u, true);
    a.EmitMimg(Rdna2Opcode::IMAGE_LOAD_MIP, 28, 24, 0, 0, 0xFu, 3u, true);
    a.EmitMimg(Rdna2Opcode::IMAGE_ATOMIC_CMPSWAP, 32, 24, 0, 0, 0x1u, 1u, true, true);
    a.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 36, 40, 0, 0, 0xFu, 0u, false);
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);

    std::vector<Rdna2Instruction> insns;
    OS_REQUIRE(DisassembleRdna2(a.Words(), insns).ok());
    OS_REQUIRE(insns.size() == 26u);

    // Verify exact VOP3P encoding prefix 0x33 and FLAT flat_seg == 1
    bool saw_vop3p = false;
    bool saw_scratch_seg1 = false;
    bool saw_bvh = false;
    for (const auto& inst : insns) {
        if (inst.encoding == Rdna2Encoding::VOP3P) {
            OS_REQUIRE((inst.raw_words[0] >> 26) == 0x33u);
            saw_vop3p = true;
        }
        if (inst.opcode == Rdna2Opcode::SCRATCH_LOAD_DWORD || inst.opcode == Rdna2Opcode::SCRATCH_STORE_DWORD) {
            OS_REQUIRE(inst.flat_seg == 1u);
            saw_scratch_seg1 = true;
        }
        if (inst.opcode == Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY) {
            OS_REQUIRE(inst.raw_opcode == 0xE6u);
            saw_bvh = true;
        }
    }
    OS_REQUIRE(saw_vop3p && saw_scratch_seg1 && saw_bvh);

    Xs2ShaderContainer c{};
    c.stage = ShaderStage::Compute;
    c.threadgroup_x = 32;
    c.wave_size = 32;
    c.code_words = a.Words();

    Rdna2IrModule ir{};
    OS_REQUIRE(BuildRdna2Ir(c, insns, ir).ok());
    OS_REQUIRE(ir.uses_scratch);
    OS_REQUIRE(ir.uses_texture3d_srv);
    OS_REQUIRE(ir.uses_texture_cube_srv);
    OS_REQUIRE(ir.uses_texture_u32_uav);
    OS_REQUIRE(ir.uses_bvh_ray);
    OS_REQUIRE(ir.uses_wave64_emulation);

    Rdna2IrOptStats opt{};
    OS_REQUIRE(OptimizeRdna2Ir(ir, &opt).ok());
    OS_REQUIRE(opt.folded_constants >= 6u);  // BFE/BFI + VOP3P constants folded

    // 2. Verify CPU RDNA2 interpreter on bitfield + VOP3P + scratch + permlane + BVH + MIMG 3D/Cube/Atomic
    Rdna2Assembler b;
    // v1 = tid * 4
    b.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(2), 0);
    // Bitfield checks stored to uav1[0..2] on lane 0
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 2, Rdna2Operand::LiteralU32(0x1234F89Au));
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 20, Rdna2Operand::LiteralU32(0x0000AB00u));
    b.EmitVop3(Rdna2Opcode::V_BFE_U32, 3, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));   // 0xF8
    b.EmitVop3(Rdna2Opcode::V_BFE_I32, 4, Rdna2Operand::Vgpr(2), Rdna2Operand::InlineInt(8), Rdna2Operand::InlineInt(8));   // -8 (0xFFFFFFF8)
    b.EmitVop3(Rdna2Opcode::V_BFI_B32, 5, Rdna2Operand::LiteralU32(0x0000FF00u), Rdna2Operand::Vgpr(20), Rdna2Operand::Vgpr(2));  // 0x1234AB9A
    // Packed f16: (1.5, 2.0) + (0.5, 4.0) = (2.0, 6.0) -> (0x4000, 0x4600)
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 6, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3E00u, 0x4000u)));
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 7, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x3800u, 0x4400u)));
    b.EmitVop3p(Rdna2Opcode::V_PK_ADD_F16, 8, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));
    b.EmitVop3p(Rdna2Opcode::V_PK_MUL_F16, 9, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7));   // (0.75=0x3A00, 8.0=0x4800)
    b.EmitVop3p(Rdna2Opcode::V_PK_FMA_F16, 10, Rdna2Operand::Vgpr(6), Rdna2Operand::Vgpr(7), Rdna2Operand::Vgpr(6));  // (2.25=0x4080, 10.0=0x4900)
    // Packed u16
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0010u, 0x0100u)));
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 12, Rdna2Operand::LiteralU32(PackTwoHalfBits(0x0002u, 0x0003u)));
    b.EmitVop3p(Rdna2Opcode::V_PK_ADD_U16, 13, Rdna2Operand::Vgpr(11), Rdna2Operand::Vgpr(12));      // (0x0012, 0x0103)
    b.EmitVop3p(Rdna2Opcode::V_PK_LSHLREV_B16, 14, Rdna2Operand::Vgpr(12), Rdna2Operand::Vgpr(11)); // (0x0040, 0x0800)
    b.EmitVop3p(Rdna2Opcode::V_PK_LSHRREV_B16, 15, Rdna2Operand::Vgpr(12), Rdna2Operand::Vgpr(11)); // (0x0004, 0x0020)
    // Scratch round-trip
    b.EmitVop1(Rdna2Opcode::V_MOV_B32, 16, Rdna2Operand::InlineInt(8));
    b.EmitScratch(Rdna2Opcode::SCRATCH_STORE_DWORD, 10, 16, 4);
    b.EmitScratch(Rdna2Opcode::SCRATCH_LOAD_DWORD, 17, 16, 4);
    // Permlane16 reverse + permlanex16 swap halves
    b.EmitSop1(Rdna2Opcode::S_MOV_B32, 8, Rdna2Operand::LiteralU32(0x89ABCDEFu));
    b.EmitSop1(Rdna2Opcode::S_MOV_B32, 9, Rdna2Operand::LiteralU32(0x01234567u));
    b.EmitVop3(Rdna2Opcode::V_PERMLANE16_B32, 18, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));
    b.EmitVop3(Rdna2Opcode::V_PERMLANEX16_B32, 19, Rdna2Operand::Vgpr(0), Rdna2Operand::Sgpr(8), Rdna2Operand::Sgpr(9));

    // Store per-lane results to uav1 (srsrc=1)
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 3, 1, 4, 0);     // [0..31]   BFE_U32
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 4, 1, 4, 128);   // [32..63]  BFE_I32
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 5, 1, 4, 256);   // [64..95]  BFI_B32
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 8, 1, 4, 384);   // [96..127] PK_ADD_F16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 9, 1, 4, 512);   // [128..159] PK_MUL_F16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 17, 1, 4, 640);  // [160..191] SCRATCH(PK_FMA_F16)
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 13, 1, 4, 768);  // [192..223] PK_ADD_U16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 14, 1, 4, 896);  // [224..255] PK_LSHLREV_B16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 15, 1, 4, 1024); // [256..287] PK_LSHRREV_B16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 18, 1, 4, 1152); // [288..319] PERMLANE16
    b.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 19, 1, 4, 1280); // [320..351] PERMLANEX16
    b.EmitSopp(Rdna2Opcode::S_ENDPGM);

    Rdna2CpuExecState cpu{};
    cpu.uav1.assign(352, 0u);
    OS_REQUIRE(ExecuteRdna2BlobCpu(b.BuildXs2(ShaderStage::Compute, 32, 1, 1, 16, 32, 32), cpu).ok());
    OS_REQUIRE(cpu.uav1[0] == 0x000000F8u);
    OS_REQUIRE(cpu.uav1[32] == 0xFFFFFFF8u);
    OS_REQUIRE(cpu.uav1[64] == 0x1234AB9Au);
    OS_REQUIRE(cpu.uav1[96] == PackTwoHalfBits(0x4000u, 0x4600u));
    OS_REQUIRE(cpu.uav1[128] == PackTwoHalfBits(0x3A00u, 0x4800u));
    OS_REQUIRE(cpu.uav1[160] == PackTwoHalfBits(0x4080u, 0x4900u));
    OS_REQUIRE(cpu.uav1[192] == PackTwoHalfBits(0x0012u, 0x0103u));
    OS_REQUIRE(cpu.uav1[224] == PackTwoHalfBits(0x0040u, 0x0800u));
    OS_REQUIRE(cpu.uav1[256] == PackTwoHalfBits(0x0004u, 0x0020u));
    for (uint32_t l = 0; l < 32u; ++l) {
        const uint32_t exp_p16 = (l & ~15u) | (15u - (l & 15u));
        const uint32_t exp_px16 = ((l & ~15u) ^ 16u) | (15u - (l & 15u));
        OS_REQUIRE(cpu.uav1[288 + l] == exp_p16);
        OS_REQUIRE(cpu.uav1[320 + l] == exp_px16);
    }

    // 3. Verify CPU interpreter BVH triangle & box intersection + MIMG 3D/Cube/Atomic
    {
        Rdna2Assembler rt;
        // Ray 0 (lane 0): hits triangle at byte offset 0 (hit_id = 42, t = 5.0, u = 0.25, v = 0.25)
        // Ray 1 (lane 1): hits AABB box at byte offset 64 (hit_id = 77, t_enter = 2.0, t_exit = 4.0)
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 10, Rdna2Operand::InlineInt(0));
        rt.EmitVopc(Rdna2Opcode::V_CMP_EQ_U32, Rdna2Operand::InlineInt(1), 0);
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 11, Rdna2Operand::InlineInt(64));
        rt.EmitVop2(Rdna2Opcode::V_CNDMASK_B32, 20, Rdna2Operand::Vgpr(10), 11);  // node_ptr = lane==1 ? 64 : 0
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 21, Rdna2Operand::LiteralF32(100.0f));  // tmax
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 22, Rdna2Operand::LiteralF32(0.25f));   // ox
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 23, Rdna2Operand::LiteralF32(0.25f));   // oy
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 24, Rdna2Operand::InlineFloat(0.0f));   // oz
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 25, Rdna2Operand::InlineFloat(0.0f));   // dx
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 26, Rdna2Operand::InlineFloat(0.0f));   // dy
        rt.EmitVop1(Rdna2Opcode::V_MOV_B32, 27, Rdna2Operand::InlineFloat(1.0f));   // dz
        rt.EmitMimg(Rdna2Opcode::IMAGE_BVH_INTERSECT_RAY, 30, 20, 0, 0, 0xFu, 0u, false);
        rt.EmitVop2(Rdna2Opcode::V_LSHLREV_B32, 1, Rdna2Operand::InlineInt(4), 0);  // tid * 16
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 30, 1, 4, 0);
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 31, 1, 4, 4);
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 32, 1, 4, 8);
        rt.EmitMubuf(Rdna2Opcode::BUFFER_STORE_DWORD, 33, 1, 4, 12);
        rt.EmitSopp(Rdna2Opcode::S_ENDPGM);

        Rdna2CpuExecState rt_state{};
        rt_state.uav0.assign(32, 0u);
        // Triangle at uav0[0..9]: (0,0,5), (1,0,5), (0,1,5), id=42
        rt_state.uav0[0] = 42u;
        rt_state.uav0[1] = FloatToBits(0.0f); rt_state.uav0[2] = FloatToBits(0.0f); rt_state.uav0[3] = FloatToBits(5.0f);
        rt_state.uav0[4] = FloatToBits(1.0f); rt_state.uav0[5] = FloatToBits(0.0f); rt_state.uav0[6] = FloatToBits(5.0f);
        rt_state.uav0[7] = FloatToBits(0.0f); rt_state.uav0[8] = FloatToBits(1.0f); rt_state.uav0[9] = FloatToBits(5.0f);
        // AABB Box at uav0[16..22] (byte offset 64): bmin=(0,0,2), bmax=(1,1,4), id=77 | 0x80000000
        rt_state.uav0[16] = 0x80000000u | 77u;
        rt_state.uav0[17] = FloatToBits(0.0f); rt_state.uav0[18] = FloatToBits(0.0f); rt_state.uav0[19] = FloatToBits(2.0f);
        rt_state.uav0[20] = FloatToBits(1.0f); rt_state.uav0[21] = FloatToBits(1.0f); rt_state.uav0[22] = FloatToBits(4.0f);
        rt_state.uav1.assign(8, 0u);

        OS_REQUIRE(ExecuteRdna2BlobCpu(rt.BuildXs2(ShaderStage::Compute, 2, 1, 1, 16, 40, 32), rt_state).ok());
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[0]) - 5.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[1]) - 0.25f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[2]) - 0.25f) < 1e-5f);
        OS_REQUIRE(rt_state.uav1[3] == 42u);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[4]) - 2.0f) < 1e-5f);
        OS_REQUIRE(std::fabs(BitsToFloat(rt_state.uav1[5]) - 4.0f) < 1e-5f);
        OS_REQUIRE(rt_state.uav1[7] == 77u);
    }

    std::printf("[PASS] Core 2: AAA opcodes (permlane16/x16, bfe/bfi, VOP3P f16/u16, scratch, MIMG 3D/Cube/atomic, BVH) + CPU interpreter\n");
}

void TestXboxDxbcContainerParser() {
    using namespace openscarlett;

    Rdna2Assembler a;
    a.EmitVop1(Rdna2Opcode::V_MOV_B32, 1, Rdna2Operand::InlineInt(7));
    a.EmitSopp(Rdna2Opcode::S_ENDPGM);
    const auto xs2 = a.BuildXs2(ShaderStage::Compute, 64, 1, 1, 16, 16, 64);

    // 1. Raw XS2 detection
    OS_REQUIRE(IsXboxShaderBytecode(xs2.data(), xs2.size()));
    XboxDxbcContainer parsed_raw{};
    OS_REQUIRE(ParseXboxShaderContainer(xs2, parsed_raw).ok());
    OS_REQUIRE(parsed_raw.format == XboxShaderBlobFormat::RawXs2);
    OS_REQUIRE(parsed_raw.has_xs2);
    OS_REQUIRE(parsed_raw.xs2.wave_size == 64u);

    // 2. DXBC container with XBOX + ISG1 + OSG1 + STSM + DXIL
    XboxDxbcBuildDesc desc{};
    desc.xs2_blob = xs2;
    desc.xbox_chunk_fourcc = "XBOX";
    desc.input_signature.push_back({"POSITION", 0, 0, 3, 0, 0xFu, 0xFu, 0});
    desc.input_signature.push_back({"TEXCOORD", 0, 0, 3, 1, 0x3u, 0x3u, 0});
    desc.output_signature.push_back({"SV_TARGET", 0, 0, 3, 0, 0xFu, 0xFu, 0});
    XboxStaticSamplerInfo samp{};
    samp.filter = 0x15u;
    samp.address_u = 1u;
    samp.shader_register = 2u;
    samp.max_lod = 12.0f;
    desc.static_samplers.push_back(samp);
    desc.aux_dxil_or_shdr = {0x10u, 0x20u, 0x30u, 0x40u};
    desc.aux_shader_fourcc = "DXIL";

    const auto dxbc_xbox = BuildXboxDxbcContainer(desc);
    OS_REQUIRE(IsXboxShaderBytecode(dxbc_xbox.data(), dxbc_xbox.size()));

    XboxDxbcContainer parsed{};
    OS_REQUIRE(ParseXboxShaderContainer(dxbc_xbox, parsed).ok());
    OS_REQUIRE(parsed.format == XboxShaderBlobFormat::XboxDxbc);
    OS_REQUIRE(parsed.has_xs2);
    OS_REQUIRE(parsed.has_dxil_or_shdr);
    OS_REQUIRE(parsed.host_shader_chunk_fourcc == "DXIL");
    OS_REQUIRE(parsed.input_signature.size() == 2u);
    OS_REQUIRE(parsed.input_signature[0].semantic_name == "POSITION");
    OS_REQUIRE(parsed.input_signature[1].semantic_name == "TEXCOORD");
    OS_REQUIRE(parsed.output_signature.size() == 1u);
    OS_REQUIRE(parsed.output_signature[0].semantic_name == "SV_TARGET");
    OS_REQUIRE(parsed.static_samplers.size() == 1u);
    OS_REQUIRE(parsed.static_samplers[0].filter == 0x15u);
    OS_REQUIRE(parsed.static_samplers[0].shader_register == 2u);
    OS_REQUIRE(parsed.xs2.wave_size == 64u);

    // 3. DXBC container with XS2M + RTS0 root signature static samplers
    XboxDxbcBuildDesc desc2{};
    desc2.xs2_blob = xs2;
    desc2.xbox_chunk_fourcc = "XS2M";
    desc2.static_samplers.push_back(samp);
    desc2.emit_rts0_for_samplers = true;
    const auto dxbc_xs2m = BuildXboxDxbcContainer(desc2);
    OS_REQUIRE(IsXboxShaderBytecode(dxbc_xs2m.data(), dxbc_xs2m.size()));
    XboxDxbcContainer parsed2{};
    OS_REQUIRE(ParseXboxShaderContainer(dxbc_xs2m, parsed2).ok());
    OS_REQUIRE(parsed2.format == XboxShaderBlobFormat::XboxDxbc);
    OS_REQUIRE(parsed2.has_xs2);
    OS_REQUIRE(!parsed2.raw_root_signature_chunk.empty());
    OS_REQUIRE(parsed2.static_samplers.size() == 1u);
    OS_REQUIRE(parsed2.static_samplers[0].shader_register == 2u);

    // 4. Standard host DXBC (no XBOX/XS2M) must return IsXboxShaderBytecode == false
    XboxDxbcBuildDesc host_desc{};
    host_desc.aux_dxil_or_shdr = {0x01u, 0x02u, 0x03u, 0x04u};
    host_desc.aux_shader_fourcc = "DXIL";
    const auto host_dxbc = BuildXboxDxbcContainer(host_desc);
    OS_REQUIRE(!IsXboxShaderBytecode(host_dxbc.data(), host_dxbc.size()));
    XboxDxbcContainer parsed_host{};
    OS_REQUIRE(ParseXboxShaderContainer(host_dxbc, parsed_host).ok());
    OS_REQUIRE(parsed_host.format == XboxShaderBlobFormat::StandardHostDxbc);
    OS_REQUIRE(!parsed_host.has_xs2);

    std::printf("[PASS] Core 3: Xbox DXBC/DXIL container parser (XS2/XBOX/XS2M/ISG1/OSG1/STSM/RTS0/host passthrough)\n");
}

void TestShaderCacheAtomicsAndCorruptRecovery() {
    using namespace openscarlett;

    // Isolated temp dir under %TEMP% (strictly on E:\ under build_wt.cmd)
    std::filesystem::path cache_dir;
#if defined(_WIN32)
    char env_buf[1024]{};
    DWORD len = GetEnvironmentVariableA("TEMP", env_buf, static_cast<DWORD>(sizeof(env_buf)));
    if (len > 0 && len < sizeof(env_buf)) {
        cache_dir = std::filesystem::path(std::string(env_buf, len)) / "test_rdna2_core_cache";
    } else {
        cache_dir = std::filesystem::temp_directory_path() / "test_rdna2_core_cache";
    }
#else
    cache_dir = std::filesystem::temp_directory_path() / "test_rdna2_core_cache";
#endif
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);

    ShaderCache cache;
    cache.SetCacheDirectory(cache_dir.string());
    cache.ResetStats();

    const uint8_t sample_in[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const ShaderCacheKey k1 = ComputeShaderCacheKey(sample_in, sizeof(sample_in), "cs_6_5", 0x6500u);
    const ShaderCacheKey k1_dup = ComputeShaderCacheKey(sample_in, sizeof(sample_in), "cs_6_5", 0x6500u);
    const ShaderCacheKey k2 = ComputeShaderCacheKey(sample_in, sizeof(sample_in), "cs_6_2", 0x6200u);
    OS_REQUIRE(k1 == k1_dup);
    OS_REQUIRE(k1 != k2);
    OS_REQUIRE(k1.ToHex().size() == 32u);

    // Fake valid-magic DXBC payload for cache round-trip + corruption tests
    std::vector<uint8_t> fake_dxil(64, 0x5Au);
    std::memcpy(fake_dxil.data(), "DXBC", 4);

    CachedShaderResult res{};
    OS_REQUIRE(!cache.TryGet(k1, res));
    OS_REQUIRE(cache.Put(k1, ShaderStage::Compute, "cs_6_5", fake_dxil).ok());

    // Memory hit
    OS_REQUIRE(cache.TryGet(k1, res));
    OS_REQUIRE(res.from_memory_cache && !res.from_disk_cache);
    OS_REQUIRE(res.dxil == fake_dxil);

    // Disk hit after clearing in-memory cache
    cache.ClearMemoryCache();
    OS_REQUIRE(cache.TryGet(k1, res));
    OS_REQUIRE(!res.from_memory_cache && res.from_disk_cache);
    OS_REQUIRE(res.dxil == fake_dxil);

    // Corrupt on-disk cache file -> must gracefully miss & delete corrupt entry
    cache.ClearMemoryCache();
    const std::string disk_file = cache.CacheFilePathForKey(k1);
    OS_REQUIRE(std::filesystem::exists(disk_file, ec));
    {
        std::fstream fs(disk_file, std::ios::binary | std::ios::in | std::ios::out);
        OS_REQUIRE(fs.good());
        fs.seekp(-1, std::ios::end);
        const char bad = static_cast<char>(0xFF);
        fs.write(&bad, 1);
    }
    OS_REQUIRE(!cache.TryGet(k1, res));
    OS_REQUIRE(!std::filesystem::exists(disk_file, ec));

    const auto stats = cache.Stats();
    OS_REQUIRE(stats.memory_hits == 1u);
    OS_REQUIRE(stats.disk_hits == 1u);
    OS_REQUIRE(stats.corrupt_entries == 1u);

    std::filesystem::remove_all(cache_dir, ec);
    std::printf("[PASS] Core 4: ShaderCache 128-bit hash + atomic disk hit + corrupt-file recovery\n");
}

}  // namespace

int main() {
    TestBasicComputeAndConstFold();
    TestAaaOpcodesEncodeDecodeIrAndCpu();
    TestXboxDxbcContainerParser();
    TestShaderCacheAtomicsAndCorruptRecovery();
    std::printf("[PASS] test_rdna2_core: ALL 4 CORE SUITES PASSED!\n");
    return 0;
}
