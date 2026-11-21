#include <cstdio>
#include <vector>

#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_ir.h"
#include "os_check.h"

int main() {
    using namespace openscarlett;

    // Assemble a real RDNA2 GFX10.3 compute microcode sequence into XS2\0
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

    std::printf("[PASS] test_rdna2_core (%zu insns decoded, %u constants folded, %u dead ops eliminated)\n",
                insns.size(), stats.folded_constants, stats.eliminated_ops);
    return 0;
}
