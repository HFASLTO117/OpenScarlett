#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "openscarlett/core/status.h"
#include "openscarlett/gpu/rdna2_disasm.h"
#include "openscarlett/gpu/rdna2_ir.h"

namespace openscarlett {

inline constexpr const char* kDefaultSpirvValPath = "E:\\OpenScarlett\\.tools\\spirv-val.exe";
inline constexpr const char* kDefaultDxcPath =
    "C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\dxc.exe";
inline constexpr const char* kLifterTempDir = "E:\\OpenScarlett\\.tmp\\lifter";

struct Rdna2LiftOptions {
    bool use_sm65 = true;
    bool emit_spirv = true;
    bool emit_hlsl = true;
    bool validate_spirv_external = false;
    bool compile_dxil_with_dxc = false;
    std::string spirv_val_path = kDefaultSpirvValPath;
    std::string spirv_target_env = "vulkan1.2";
    std::string dxc_path = kDefaultDxcPath;
};

struct Rdna2LiftResult {
    Xs2ShaderContainer container;
    std::vector<Rdna2Instruction> instructions;
    Rdna2IrModule ir;
    std::vector<uint32_t> spirv_words;
    std::string hlsl_source;
    std::string hlsl_target;
    std::vector<uint8_t> pc_dxil;
    uint64_t pc_dxil_hash = 0;
    std::string spirv_val_diagnostics;
    std::string dxc_diagnostics;
};

// Emits a strictly valid SPIR-V 1.3 binary module (generator ID 0x4F533200) from an RDNA2 SSA IR module.
Status EmitSpirv(const Rdna2IrModule& ir, std::vector<uint32_t>& out_spirv);

// Structurally validates a SPIR-V 1.3 binary word stream (header, word counts, ID bounds,
// required sections, entry point, function balance).
Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv);

// Validates a SPIR-V binary using Khronos spirv-val.exe (temp files strictly under E:\OpenScarlett\.tmp\).
Status ValidateSpirvExternal(
    const std::vector<uint32_t>& spirv,
    std::string& out_diagnostics,
    const std::string& spirv_val_path = kDefaultSpirvValPath,
    const std::string& target_env = "vulkan1.2");

// Emits clean, compilable HLSL (SM 6.5: cs_6_5/vs_6_5/ps_6_5/ms_6_5/as_6_5, or SM 5.0 fallback).
Status EmitHlsl(
    const Rdna2IrModule& ir,
    std::string& out_hlsl,
    std::string& out_target,
    bool use_sm65 = true);

// Compiles HLSL source to real DXIL bytecode using Microsoft dxc.exe (temp files strictly under E:\OpenScarlett\.tmp\).
Status CompileHlslWithDxc(
    const std::string& hlsl,
    const std::string& target,
    const std::string& entry_point,
    std::vector<uint8_t>& out_dxil,
    std::string& out_diagnostics,
    const std::string& dxc_path = kDefaultDxcPath);

// Full RDNA2 XS2\0 lifting pipeline: Parse -> Disasm -> SSA IR -> Optimize -> SPIR-V 1.3 + HLSL (+ optional spirv-val/DXC).
Status LiftRdna2Xs2Shader(
    const std::vector<uint8_t>& xs2_bytes,
    Rdna2LiftResult& out_result,
    const Rdna2LiftOptions& options = {});

}  // namespace openscarlett
