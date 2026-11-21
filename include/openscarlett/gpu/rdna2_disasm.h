#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/shader_ir.h"

namespace openscarlett {

// ---------------------------------------------------------------------------
// Clean-room Scarlett RDNA2 / GFX10.3 ('XS2\0') Shader Container
// ---------------------------------------------------------------------------
constexpr uint32_t kXs2ContainerVersion = 1u;
constexpr uint32_t kXs2ContainerVersionV2 = 2u;
constexpr uint32_t kXs2HeaderSizeBytes = 40u;
constexpr uint32_t kXs2HeaderSizeBytesV2 = 44u;
constexpr uint32_t kXs2SgprFlagWave64 = 0x80000000u;

// Public RDNA2 / GFX10.3 S_SENDMSG message IDs
constexpr uint16_t kRdna2SendMsgGsAllocReq = 9u;
constexpr uint16_t kRdna2SendMsgDispatchMesh = 10u;

struct Xs2ShaderContainer {
    uint32_t version = kXs2ContainerVersion;
    ShaderStage stage = ShaderStage::Compute;
    uint32_t sgpr_count = 16;
    uint32_t vgpr_count = 16;
    uint32_t threadgroup_x = 1;
    uint32_t threadgroup_y = 1;
    uint32_t threadgroup_z = 1;
    uint32_t entry_offset = kXs2HeaderSizeBytes;
    uint32_t wave_size = 32;  // 32 (native RDNA2 wave32) or 64 (wave64)
    std::vector<uint32_t> code_words;
};

Status ParseXs2Container(const std::vector<uint8_t>& bytes, Xs2ShaderContainer& out_container);
std::vector<uint8_t> BuildXs2Container(const Xs2ShaderContainer& container);

// ---------------------------------------------------------------------------
// Xbox-Wrapped DXBC / DXIL Shader Container Parser & Builder
// ---------------------------------------------------------------------------
enum class XboxShaderBlobFormat : uint8_t {
    Unknown = 0,
    RawXs2,            // Raw "XS2\0" RDNA2 shader blob
    XboxDxbc,          // "DXBC" container carrying Xbox RDNA2 chunks ("XBOX" / "XS2M")
    StandardHostDxbc,  // Standard host PC DXBC / DXIL container (no Xbox RDNA2 chunks)
};

struct XboxDxbcChunk {
    char fourcc[5] = {};  // Null-terminated 4-char chunk ID (e.g. "XBOX", "XS2M", "ISG1")
    std::vector<uint8_t> data;
};

struct XboxSignatureElement {
    std::string semantic_name;
    uint32_t semantic_index = 0;
    uint32_t system_value_type = 0;
    uint32_t component_type = 0;  // 1=uint, 2=sint, 3=float
    uint32_t register_index = 0;
    uint8_t mask = 0xFu;
    uint8_t rw_mask = 0xFu;
    uint32_t stream = 0;
};

struct XboxStaticSamplerInfo {
    uint32_t filter = 0;
    uint32_t address_u = 0;
    uint32_t address_v = 0;
    uint32_t address_w = 0;
    float mip_lod_bias = 0.0f;
    uint32_t max_anisotropy = 1;
    uint32_t comparison_func = 0;
    uint32_t border_color = 0;
    float min_lod = 0.0f;
    float max_lod = 3.402823466e+38f;
    uint32_t shader_register = 0;
    uint32_t register_space = 0;
    uint32_t shader_visibility = 0;
};

struct XboxDxbcContainer {
    XboxShaderBlobFormat format = XboxShaderBlobFormat::Unknown;
    bool has_xs2 = false;
    Xs2ShaderContainer xs2{};
    bool has_dxil_or_shdr = false;
    std::string host_shader_chunk_fourcc;  // "DXIL", "SHEX", or "SHDR" if present
    std::vector<XboxDxbcChunk> chunks;
    std::vector<XboxSignatureElement> input_signature;   // Parsed from ISG1 / ISGN
    std::vector<XboxSignatureElement> output_signature;  // Parsed from OSG1 / OSGN
    std::vector<XboxStaticSamplerInfo> static_samplers;  // Parsed from RTS0 / STSM
    std::vector<uint8_t> raw_root_signature_chunk;       // Raw RTS0 chunk payload if present
};

struct XboxDxbcBuildDesc {
    std::vector<uint8_t> xs2_blob;          // Embedded into "XBOX" or "XS2M" chunk
    std::string xbox_chunk_fourcc = "XBOX"; // "XBOX" or "XS2M"
    std::vector<XboxSignatureElement> input_signature;
    std::vector<XboxSignatureElement> output_signature;
    std::vector<XboxStaticSamplerInfo> static_samplers;
    bool emit_rts0_for_samplers = false;    // Emit RTS0 root signature chunk vs STSM chunk
    std::vector<uint8_t> aux_dxil_or_shdr;  // Optional "DXIL"/"SHEX"/"SHDR" chunk payload
    std::string aux_shader_fourcc = "DXIL";
};

// Fast, zero-allocation check: returns true iff blob is raw "XS2\0" or "DXBC" with "XBOX"/"XS2M" chunk.
bool IsXboxShaderBytecode(const void* data, size_t size) noexcept;

// Parses raw "XS2\0" OR "DXBC" container carrying Xbox chunks ("XBOX", "XS2M", "SHDR"/"SHEX"/"DXIL",
// "ISG1"/"ISGN", "OSG1"/"OSGN", "RTS0"/"STSM").
Status ParseXboxShaderContainer(const void* data, size_t size, XboxDxbcContainer& out_container);
Status ParseXboxShaderContainer(const std::vector<uint8_t>& bytes, XboxDxbcContainer& out_container);

// Clean-room builder for Xbox-wrapped DXBC containers (for testing and tooling).
std::vector<uint8_t> BuildXboxDxbcContainer(const XboxDxbcBuildDesc& desc);

// ---------------------------------------------------------------------------
// AMD RDNA2 / GFX10.3 Binary Instruction Model (Public ISA Reference Guide)
// ---------------------------------------------------------------------------
enum class Rdna2Encoding : uint8_t {
    Unknown = 0,
    SOP2,
    SOP1,
    SOPC,
    SOPK,
    SOPP,
    VOP2,
    VOP1,
    VOPC,
    VOP3,
    VOP3P,
    SMEM,
    MUBUF,
    MIMG,
    DS,
    FLAT,
    EXP,
};

enum class Rdna2Opcode : uint16_t {
    Unknown = 0,

    // SOP2
    S_ADD_U32,
    S_SUB_U32,
    S_ADD_I32,
    S_SUB_I32,
    S_MIN_I32,
    S_MIN_U32,
    S_MAX_I32,
    S_MAX_U32,
    S_CSELECT_B32,
    S_AND_B32,
    S_OR_B32,
    S_XOR_B32,
    S_LSHL_B32,
    S_LSHR_B32,
    S_ASHR_I32,
    S_MUL_I32,

    // SOP1
    S_MOV_B32,
    S_NOT_B32,
    S_BCNT1_I32_B64,
    S_GETPC_B64,

    // SOPC
    S_CMP_EQ_I32,
    S_CMP_LG_I32,
    S_CMP_GT_I32,
    S_CMP_GE_I32,
    S_CMP_LT_I32,
    S_CMP_LE_I32,
    S_CMP_EQ_U32,
    S_CMP_LG_U32,
    S_CMP_GT_U32,
    S_CMP_GE_U32,
    S_CMP_LT_U32,
    S_CMP_LE_U32,

    // SOPK
    S_MOVK_I32,
    S_ADDK_I32,
    S_MULK_I32,

    // SOPP
    S_NOP,
    S_ENDPGM,
    S_BRANCH,
    S_CBRANCH_SCC0,
    S_CBRANCH_SCC1,
    S_CBRANCH_VCCZ,
    S_CBRANCH_VCCNZ,
    S_CBRANCH_EXECZ,
    S_CBRANCH_EXECNZ,
    S_WAITCNT,
    S_BARRIER,
    S_SENDMSG,

    // VOP2
    V_CNDMASK_B32,
    V_ADD_F32,
    V_SUB_F32,
    V_SUBREV_F32,
    V_MUL_F32,
    V_MIN_F32,
    V_MAX_F32,
    V_MIN_I32,
    V_MAX_I32,
    V_MIN_U32,
    V_MAX_U32,
    V_FMAC_F32,
    V_AND_B32,
    V_OR_B32,
    V_XOR_B32,
    V_LSHLREV_B32,
    V_LSHRREV_B32,
    V_ASHRREV_I32,
    V_ADD_NC_U32,
    V_SUB_NC_U32,

    // VOP1
    V_NOP,
    V_MOV_B32,
    V_READFIRSTLANE_B32,
    V_CVT_F32_I32,
    V_CVT_I32_F32,
    V_CVT_F32_U32,
    V_CVT_U32_F32,
    V_RCP_F32,
    V_RSQ_F32,
    V_SQRT_F32,
    V_FLOOR_F32,
    V_FRACT_F32,

    // VOPC
    V_CMP_LT_F32,
    V_CMP_EQ_F32,
    V_CMP_LE_F32,
    V_CMP_GT_F32,
    V_CMP_LG_F32,
    V_CMP_GE_F32,
    V_CMP_LT_I32,
    V_CMP_EQ_I32,
    V_CMP_GT_I32,
    V_CMP_GE_I32,
    V_CMP_LT_U32,
    V_CMP_EQ_U32,
    V_CMP_GT_U32,
    V_CMP_GE_U32,

    // VOP3 (native 3-operand / wave / bitfield ops)
    V_FMA_F32,
    V_MAD_F32,
    V_BFE_U32,
    V_BFE_I32,
    V_BFI_B32,
    V_MUL_LO_U32,
    V_MBCNT_LO_U32_B32,
    V_MBCNT_HI_U32_B32,
    V_READLANE_B32,
    V_ADD3_U32,
    V_DOT2_F32_F16,
    V_PERMLANE16_B32,
    V_PERMLANEX16_B32,

    // VOP3P (packed 16-bit f16 / u16 / b16 ops, encoding 0x33)
    V_PK_LSHLREV_B16,
    V_PK_LSHRREV_B16,
    V_PK_ADD_U16,
    V_PK_FMA_F16,
    V_PK_ADD_F16,
    V_PK_MUL_F16,

    // SMEM
    S_LOAD_DWORD,
    S_BUFFER_LOAD_DWORD,

    // MUBUF
    BUFFER_LOAD_DWORD,
    BUFFER_STORE_DWORD,

    // MIMG
    IMAGE_LOAD,
    IMAGE_LOAD_MIP,
    IMAGE_STORE,
    IMAGE_ATOMIC_CMPSWAP,
    IMAGE_SAMPLE,
    IMAGE_SAMPLE_LZ,
    IMAGE_BVH_INTERSECT_RAY,

    // DS (LDS / Shared Memory & Swizzle)
    DS_ADD_U32,
    DS_WRITE_B32,
    DS_ADD_RTN_U32,
    DS_SWIZZLE_B32,
    DS_READ_B32,

    // FLAT / GLOBAL / SCRATCH
    GLOBAL_LOAD_DWORD,
    GLOBAL_STORE_DWORD,
    FLAT_LOAD_DWORD,
    FLAT_STORE_DWORD,
    SCRATCH_LOAD_DWORD,
    SCRATCH_STORE_DWORD,

    // EXP
    EXP,
};

enum class Rdna2OperandKind : uint8_t {
    None = 0,
    Sgpr,
    Vgpr,
    InlineInt,
    InlineFloat,
    Literal32,
    VccLo,
    VccHi,
    ExecLo,
    ExecHi,
    Scc,
    M0,
    Null,
};

struct Rdna2Operand {
    Rdna2OperandKind kind = Rdna2OperandKind::None;
    uint16_t raw_code = 0;   // 9-bit RDNA2 source code or register index
    uint16_t reg_index = 0;  // SGPR (0..105) or VGPR (0..255)
    int32_t int_value = 0;
    float float_value = 0.0f;
    uint32_t literal_bits = 0;
    bool neg = false;
    bool abs = false;

    static Rdna2Operand Sgpr(uint8_t idx);
    static Rdna2Operand Vgpr(uint8_t idx);
    static Rdna2Operand InlineInt(int32_t val);
    static Rdna2Operand InlineFloat(float val);
    static Rdna2Operand LiteralU32(uint32_t bits);
    static Rdna2Operand LiteralF32(float val);
    static Rdna2Operand VccLo();
    static Rdna2Operand VccHi();
    static Rdna2Operand ExecLo();
    static Rdna2Operand ExecHi();
    static Rdna2Operand Scc();
    static Rdna2Operand Null();
};

enum class Rdna2ExportTarget : uint8_t {
    Mrt0 = 0,
    Mrt1 = 1,
    Mrt2 = 2,
    Mrt3 = 3,
    Mrt4 = 4,
    Mrt5 = 5,
    Mrt6 = 6,
    Mrt7 = 7,
    MrtZ = 8,
    Null = 9,
    Pos0 = 12,
    Pos1 = 13,
    Pos2 = 14,
    Pos3 = 15,
    Prim = 20,  // GFX10.3 NGG / Mesh primitive connectivity export
    Param0 = 32,
    Param1 = 33,
    Param2 = 34,
    Param3 = 35,
};

struct Rdna2Instruction {
    uint32_t word_offset = 0;   // Index of word0 in code_words
    uint32_t word_count = 1;    // 1 (32-bit), 2 (64-bit), or 3 (VOP3 + literal32)
    uint32_t raw_words[3] = {0, 0, 0};

    Rdna2Encoding encoding = Rdna2Encoding::Unknown;
    Rdna2Opcode opcode = Rdna2Opcode::Unknown;
    uint16_t raw_opcode = 0;

    Rdna2Operand dst{};
    Rdna2Operand src[4]{};
    uint8_t src_count = 0;

    // Immediate / modifier / memory / export metadata
    int16_t simm16 = 0;       // SOPK / SOPP signed 16-bit immediate
    int32_t mem_offset = 0;   // SMEM (21-bit signed), MUBUF (12-bit unsigned), FLAT/GLOBAL (12-bit signed), DS (16-bit)
    uint8_t sbase = 0;        // SMEM base SGPR index (s[sbase*2])
    uint8_t srsrc = 0;        // MUBUF/MIMG descriptor SGPR base (s[srsrc*4])
    uint8_t ssamp = 0;        // MIMG sampler descriptor SGPR base (s[ssamp*4])
    uint8_t soffset = 0;      // SMEM / MUBUF scalar offset code
    uint8_t saddr = 0x7Fu;    // GLOBAL/FLAT scalar base SGPR (0x7F = off)
    uint8_t flat_seg = 0;     // 0=FLAT, 1=SCRATCH, 2=GLOBAL
    bool offen = false;
    bool idxen = false;
    bool glc = false;
    bool slc = false;
    bool dlc = false;
    bool clamp = false;
    uint8_t omod = 0;

    // VOP3P metadata (packed 2x16 ops, encoding 0x33)
    uint8_t vop3p_op_sel = 0;     // bits [2:0]: lo-half src0/1/2 select (0=lo16, 1=hi16)
    uint8_t vop3p_op_sel_hi = 7;  // bits [2:0]: hi-half src0/1/2 select (default 0b111 = hi16)
    uint8_t vop3p_neg_lo = 0;     // bits [2:0]: lo-half negate
    uint8_t vop3p_neg_hi = 0;     // bits [2:0]: hi-half negate

    // MIMG metadata
    uint8_t mimg_dmask = 0;
    uint8_t mimg_dim = 0;
    bool mimg_unrm = false;

    // DS metadata
    uint8_t ds_offset0 = 0;
    uint8_t ds_offset1 = 0;
    bool ds_gds = false;

    // EXP metadata
    uint8_t exp_target = 0;
    uint8_t exp_en = 0;
    bool exp_done = false;
    bool exp_compr = false;
    bool exp_vm = false;

    std::string mnemonic;
    std::string disasm;
};

// Decodes a 9-bit RDNA2 source operand code (0..511), optionally consuming a 32-bit literal.
Rdna2Operand DecodeRdna2SourceOperand(uint16_t code, uint32_t literal_bits = 0);

// Encodes an operand into a 9-bit RDNA2 source code; sets out_needs_literal if code == 255.
bool EncodeRdna2SourceOperand(
    const Rdna2Operand& op,
    uint16_t& out_code,
    uint32_t& out_literal,
    bool& out_needs_literal);

const char* Rdna2OpcodeName(Rdna2Opcode op) noexcept;
const char* Rdna2EncodingName(Rdna2Encoding enc) noexcept;
std::string FormatRdna2Operand(const Rdna2Operand& op);
std::string FormatRdna2Instruction(const Rdna2Instruction& inst);

// Disassembles a raw 32-bit RDNA2 / GFX10.3 instruction word stream.
Status DisassembleRdna2(
    const std::vector<uint32_t>& words,
    std::vector<Rdna2Instruction>& out_instructions);

// ---------------------------------------------------------------------------
// Clean-room RDNA2 / GFX10.3 Binary Instruction Assembler
// ---------------------------------------------------------------------------
class Rdna2Assembler {
public:
    void Clear() { words_.clear(); }
    const std::vector<uint32_t>& Words() const noexcept { return words_; }
    uint32_t CurrentWordOffset() const noexcept { return static_cast<uint32_t>(words_.size()); }

    // SOP2
    void EmitSop2(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1);
    // SOP1
    void EmitSop1(Rdna2Opcode op, uint8_t sdst, const Rdna2Operand& ssrc0);
    // SOPC
    void EmitSopc(Rdna2Opcode op, const Rdna2Operand& ssrc0, const Rdna2Operand& ssrc1);
    // SOPK
    void EmitSopk(Rdna2Opcode op, uint8_t sdst, int16_t simm16);
    // SOPP (returns word offset of emitted instruction so branch offsets can be patched)
    uint32_t EmitSopp(Rdna2Opcode op, int16_t simm16 = 0);
    void PatchBranch(uint32_t branch_word_offset, uint32_t target_word_offset);

    // VOP2 (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVop2(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0, uint8_t vsrc1);
    // VOP1 (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVop1(Rdna2Opcode op, uint8_t vdst, const Rdna2Operand& src0);
    // VOPC (automatically emits 64-bit word when src0 is a 32-bit literal)
    void EmitVopc(Rdna2Opcode op, const Rdna2Operand& src0, uint8_t vsrc1);
    // VOP3 (64-bit, or 96-bit if one source is a 32-bit literal)
    void EmitVop3(
        Rdna2Opcode op,
        uint8_t vdst,
        const Rdna2Operand& src0,
        const Rdna2Operand& src1,
        const Rdna2Operand& src2 = Rdna2Operand::InlineInt(0),
        bool clamp = false);

    // VOP3P (64-bit, or 96-bit with literal; encoding 0x33 packed 16-bit ops)
    void EmitVop3p(
        Rdna2Opcode op,
        uint8_t vdst,
        const Rdna2Operand& src0,
        const Rdna2Operand& src1,
        const Rdna2Operand& src2 = Rdna2Operand::InlineInt(0),
        uint8_t op_sel = 0u,
        uint8_t op_sel_hi = 7u,
        uint8_t neg_lo = 0u,
        uint8_t neg_hi = 0u,
        bool clamp = false);

    // SMEM (64-bit)
    void EmitSmem(Rdna2Opcode op, uint8_t sdata, uint8_t sbase_sgpr, int32_t offset, uint8_t soffset = 0x7Du);

    // MUBUF (64-bit)
    void EmitMubuf(
        Rdna2Opcode op,
        uint8_t vdata,
        uint8_t vaddr,
        uint8_t srsrc_sgpr,
        uint16_t offset = 0,
        bool offen = true,
        bool idxen = false,
        uint8_t soffset = 128);

    // MIMG (64-bit: IMAGE_LOAD, IMAGE_LOAD_MIP, IMAGE_STORE, IMAGE_ATOMIC_CMPSWAP,
    // IMAGE_SAMPLE, IMAGE_SAMPLE_LZ, IMAGE_BVH_INTERSECT_RAY)
    void EmitMimg(
        Rdna2Opcode op,
        uint8_t vdata,
        uint8_t vaddr,
        uint8_t srsrc_sgpr,
        uint8_t ssamp_sgpr = 0,
        uint8_t dmask = 0xFu,
        uint8_t dim = 1u,
        bool unrm = false,
        bool glc = false);

    // DS (64-bit: DS_WRITE_B32, DS_READ_B32, DS_ADD_U32, DS_ADD_RTN_U32, DS_SWIZZLE_B32)
    void EmitDs(
        Rdna2Opcode op,
        uint8_t vdst,
        uint8_t addr,
        uint8_t data0 = 0,
        uint8_t data1 = 0,
        uint16_t offset = 0,
        bool gds = false);

    // GLOBAL / FLAT / SCRATCH (64-bit)
    void EmitGlobal(
        Rdna2Opcode op,
        uint8_t vdst_or_data,
        uint8_t vaddr,
        int16_t offset = 0,
        uint8_t saddr = 0x7Fu);

    // SCRATCH (64-bit FLAT encoding with flat_seg == 1)
    void EmitScratch(
        Rdna2Opcode op,
        uint8_t vdst_or_data,
        uint8_t vaddr,
        int16_t offset = 0,
        uint8_t saddr = 0x7Fu);

    // EXP (64-bit)
    void EmitExp(
        uint8_t target,
        uint8_t vsrc0,
        uint8_t vsrc1,
        uint8_t vsrc2,
        uint8_t vsrc3,
        uint8_t en = 0xFu,
        bool done = true,
        bool compr = false,
        bool vm = false);

    // Helper: wrap assembled words into an XS2\0 container
    std::vector<uint8_t> BuildXs2(
        ShaderStage stage,
        uint32_t tg_x = 1,
        uint32_t tg_y = 1,
        uint32_t tg_z = 1,
        uint32_t sgpr_count = 16,
        uint32_t vgpr_count = 16,
        uint32_t wave_size = 32) const;

private:
    std::vector<uint32_t> words_;
};

}  // namespace openscarlett
