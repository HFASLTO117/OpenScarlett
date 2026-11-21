#include <cstdint>
#include <cstdio>
#include <vector>

#include "openscarlett/gpu/vulkan_backend.h"
#include "os_check.h"

namespace {

// Minimal valid SPIR-V 1.3 compute shader: out[gid] = in[gid] * 2 + 7
// Mirrors the validated compute kernel verified bit-for-bit on GTX 1650 Ti Vulkan 1.4.
std::vector<uint32_t> BuildDoublePlusSevenSpirv() {
    std::vector<uint32_t> w;
    auto op = [&](uint16_t opcode, std::initializer_list<uint32_t> args) {
        w.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | opcode);
        for (uint32_t a : args) w.push_back(a);
    };
    // Header: Magic, SPIR-V 1.3, Generator=0, Bound=40, Schema=0
    w.insert(w.end(), {0x07230203u, 0x00010300u, 0u, 40u, 0u});
    op(17, {1});                                       // OpCapability Shader
    op(14, {0, 1});                                    // OpMemoryModel Logical GLSL450
    op(15, {5, 1, 0x6E69616Du, 0u, 10});               // OpEntryPoint GLCompute %1 "main" %10
    op(16, {1, 17, 1, 1, 1});                          // OpExecutionMode %1 LocalSize 1 1 1
    op(71, {10, 11, 28});                              // OpDecorate %10 BuiltIn GlobalInvocationId
    op(71, {14, 6, 4});                                // OpDecorate %14 ArrayStride 4
    op(71, {15, 3});                                   // OpDecorate %15 BufferBlock
    op(72, {15, 0, 35, 0});                            // OpMemberDecorate %15 0 Offset 0
    op(71, {17, 34, 0});                               // OpDecorate %17 DescriptorSet 0
    op(71, {17, 33, 0});                               // OpDecorate %17 Binding 0
    op(71, {18, 34, 0});                               // OpDecorate %18 DescriptorSet 0
    op(71, {18, 33, 1});                               // OpDecorate %18 Binding 1

    op(19, {2});                                       // %2 = OpTypeVoid
    op(33, {3, 2});                                    // %3 = OpTypeFunction %2
    op(21, {4, 32, 0});                                // %4 = OpTypeInt 32 0 (uint)
    op(21, {5, 32, 1});                                // %5 = OpTypeInt 32 1 (int)
    op(23, {6, 4, 3});                                 // %6 = OpTypeVector %4 3
    op(32, {7, 1, 6});                                 // %7 = OpTypePointer Input %6
    op(32, {8, 1, 4});                                 // %8 = OpTypePointer Input %4
    op(59, {7, 10, 1});                                // %10 = OpVariable %7 Input
    op(43, {4, 11, 0});                                // %11 = OpConstant %4 0
    op(43, {5, 12, 0});                                // %12 = OpConstant %5 0
    op(43, {4, 13, 2});                                // %13 = OpConstant %4 2
    op(43, {4, 29, 7});                                // %29 = OpConstant %4 7

    op(29, {14, 4});                                   // %14 = OpTypeRuntimeArray %4
    op(30, {15, 14});                                  // %15 = OpTypeStruct %14
    op(32, {16, 2, 15});                               // %16 = OpTypePointer Uniform %15
    op(59, {16, 17, 2});                               // %17 = OpVariable %16 Uniform (in)
    op(59, {16, 18, 2});                               // %18 = OpVariable %16 Uniform (out)
    op(32, {19, 2, 4});                                // %19 = OpTypePointer Uniform %4

    op(54, {2, 1, 0, 3});                              // OpFunction %2 %1 None %3
    op(248, {20});                                     // %20 = OpLabel
    op(65, {8, 21, 10, 11});                           // %21 = OpAccessChain %8 %10 %11
    op(61, {4, 22, 21});                               // %22 = OpLoad %4 %21 (gid.x)
    op(65, {19, 23, 17, 12, 22});                      // %23 = OpAccessChain %19 %17 %12 %22
    op(61, {4, 24, 23});                               // %24 = OpLoad %4 %23 (in[gid])
    op(132, {4, 25, 24, 13});                          // %25 = OpIMul %4 %24 %13
    op(128, {4, 26, 25, 29});                          // %26 = OpIAdd %4 %25 %29
    op(65, {19, 27, 18, 12, 22});                      // %27 = OpAccessChain %19 %18 %12 %22
    op(62, {27, 26});                                  // OpStore %27 %26
    op(253, {});                                       // OpReturn
    op(56, {});                                        // OpFunctionEnd
    return w;
}

}  // namespace

int main() {
    using namespace openscarlett;

    auto spirv = BuildDoublePlusSevenSpirv();
    SpirvValidationInfo val{};
    OS_REQUIRE(VulkanBackend::ValidateSpirvBinary(spirv, &val).ok());

    VulkanBackend vk;
    OS_REQUIRE(vk.Initialize(true).ok());
    const auto& dev = vk.GetDeviceInfo();

    constexpr uint32_t kCount = 64;
    std::vector<uint32_t> input(kCount);
    for (uint32_t i = 0; i < kCount; ++i) input[i] = i * 3 + 1;
    std::vector<uint32_t> output;
    OS_REQUIRE(vk.ExecuteComputeShader(spirv, input, output, kCount).ok());
    OS_REQUIRE(output.size() == kCount);
    for (uint32_t i = 0; i < kCount; ++i) {
        OS_REQUIRE(output[i] == input[i] * 2u + 7u);
    }

    vk.Shutdown();
    std::printf("[PASS] test_vulkan_live (%s, hw=%s, %u lanes verified)\n",
                dev.device_name.c_str(),
                dev.is_hardware ? "yes" : "fallback",
                kCount);
    return 0;
}
