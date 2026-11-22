#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "openscarlett/core/framebuffer.h"
#include "openscarlett/core/status.h"

namespace openscarlett {

inline constexpr uint32_t kSpirvMagicNumber = 0x07230203u;

enum class VulkanDeviceType : uint32_t {
    Other = 0,
    IntegratedGpu = 1,
    DiscreteGpu = 2,
    VirtualGpu = 3,
    Cpu = 4,
};

struct VulkanDeviceInfo {
    std::string device_name;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t api_version = 0;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    uint32_t api_patch = 0;
    uint32_t driver_version = 0;
    VulkanDeviceType device_type = VulkanDeviceType::Other;
    uint32_t compute_queue_family = 0;
    bool supports_compute = false;
    bool supports_graphics = false;
    bool is_hardware = false;
    bool is_fallback = false;

    // Extended Vulkan 1.2/1.3/1.4 graphics, mesh shader & WSI capabilities
    uint32_t graphics_queue_family = 0;
    bool supports_mesh_shader = false;
    bool supports_task_shader = false;
    bool supports_win32_surface = false;
    bool supports_swapchain = false;
    bool supports_hdr10_colorspace = false;

    // Sparse / reserved resource capabilities (VK_IMAGE_CREATE_SPARSE_*)
    uint32_t sparse_queue_family = 0;
    bool supports_sparse_binding = false;
    bool supports_sparse_residency_image_2d = false;
    bool supports_sparse_residency_image_3d = false;
};

struct SpirvValidationInfo {
    uint32_t version_major = 0;
    uint32_t version_minor = 0;
    uint32_t generator = 0;
    uint32_t bound = 0;
    uint32_t instruction_count = 0;
    uint32_t entry_point_count = 0;
    bool has_compute_entry = false;
    std::string entry_point_name;
    uint32_t local_size_x = 1;
    uint32_t local_size_y = 1;
    uint32_t local_size_z = 1;
    bool has_vertex_entry = false;
    bool has_fragment_entry = false;
    bool has_task_entry = false;
    bool has_mesh_entry = false;
};

enum class VulkanCommandOp : uint32_t {
    Nop = 0,
    PipelineBarrier = 1,
    BindComputePipeline = 2,
    BindDescriptorSets = 3,
    Dispatch = 4,
};

struct VulkanCommandItem {
    VulkanCommandOp op = VulkanCommandOp::Nop;
    uint32_t x = 1;
    uint32_t y = 1;
    uint32_t z = 1;
    uint64_t resource_id = 0;
};

struct VulkanCommandListDesc {
    std::vector<VulkanCommandItem> commands;
    bool submit_empty_fence_sync = true;
};

struct VulkanCommandListResult {
    uint32_t recorded_commands = 0;
    uint32_t dispatch_count = 0;
    uint32_t barrier_count = 0;
    bool executed_on_vulkan_icd = false;
};

// Supported Vulkan image formats (values match Khronos VkFormat constants).
enum class VulkanImageFormat : uint32_t {
    Rgba8Unorm = 37,    // VK_FORMAT_R8G8B8A8_UNORM
    Bgra8Unorm = 44,    // VK_FORMAT_B8G8R8A8_UNORM
    Rgba16Sfloat = 97,  // VK_FORMAT_R16G16B16A16_SFLOAT
    D32Sfloat = 126,    // VK_FORMAT_D32_SFLOAT
};

// Supported vertex attribute formats (values match Khronos VkFormat constants).
enum class VulkanVertexFormat : uint32_t {
    R32Uint = 98,
    R32Sfloat = 100,
    R32G32Sfloat = 103,
    R32G32B32Sfloat = 106,
    R32G32B32A32Uint = 107,
    R32G32B32A32Sfloat = 109,
};

struct VulkanVertexAttribute {
    uint32_t location = 0;
    uint32_t binding = 0;
    VulkanVertexFormat format = VulkanVertexFormat::R32G32B32A32Sfloat;
    uint32_t offset = 0;
};

// Supported descriptor kinds (values match Khronos VkDescriptorType constants).
enum class VulkanDescriptorKind : uint32_t {
    Sampler = 0,
    CombinedImageSampler = 1,
    SampledImage = 2,
    UniformBuffer = 6,
    StorageBuffer = 7,
};

// Single mip level of a 2D or 3D texture.
struct VulkanTextureMipLevel {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    std::vector<uint8_t> data;  // Tightly packed texels (4 B/texel RGBA8, 8 B/texel RGBA16F)
};

// Descriptor for a mipmapped 2D texture or 3D volume texture.
struct VulkanTextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    bool is_3d = false;
    VulkanImageFormat format = VulkanImageFormat::Rgba8Unorm;
    std::vector<VulkanTextureMipLevel> mips;
};

struct VulkanTextureRoundtripResult {
    std::vector<VulkanTextureMipLevel> readback_mips;
    bool executed_on_vulkan_icd = false;
};

// Binding in descriptor set 0 for graphics or mesh pipelines.
struct VulkanDescriptorBinding {
    uint32_t binding = 0;
    VulkanDescriptorKind kind = VulkanDescriptorKind::UniformBuffer;
    std::vector<uint8_t> buffer_data;  // UniformBuffer / StorageBuffer payload
    VulkanTextureDesc texture;         // CombinedImageSampler / SampledImage payload
    bool linear_filter = false;        // false = nearest, true = linear

    // Optional T# view-range mip selection (VkImageViewCreateInfo::subresourceRange)
    uint32_t view_base_mip_level = 0;
    uint32_t view_mip_level_count = 0;  // 0 = all remaining mips from view_base_mip_level

    // Optional S# sampler LOD controls (VkSamplerCreateInfo)
    float sampler_min_lod = 0.0f;
    float sampler_max_lod = 16.0f;
    float sampler_mip_lod_bias = 0.0f;
};

// Full VS+PS graphics draw configuration (VB, IB, UBO/SSBO/2D/3D textures, push constants, depth, MSAA 4x).
struct VulkanGraphicsDrawDesc {
    std::vector<uint32_t> vertex_spirv;
    std::vector<uint32_t> fragment_spirv;
    std::string vertex_entry = "main";
    std::string fragment_entry = "main";

    uint32_t width = 128;
    uint32_t height = 128;
    VulkanImageFormat color_format = VulkanImageFormat::Rgba8Unorm;
    bool enable_depth = false;
    bool enable_msaa_4x = false;
    bool depth_write = true;
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth = 1.0f;

    // Optional vertex buffer + attributes (if empty, VS may generate geometry from VertexIndex).
    std::vector<uint8_t> vertex_bytes;
    uint32_t vertex_stride = 0;
    std::vector<VulkanVertexAttribute> vertex_attributes;

    // Optional 32-bit index buffer (if empty, uses vkCmdDraw with vertex_count).
    std::vector<uint32_t> indices;
    uint32_t vertex_count = 3;

    // Optional push constants (bound to VERTEX | FRAGMENT).
    std::vector<uint8_t> push_constants;

    // Optional descriptor set 0 bindings.
    std::vector<VulkanDescriptorBinding> descriptors;
};

struct VulkanGraphicsDrawResult {
    FrameBufferRgba8 framebuffer;         // Always populated (FP16 clamped/quantized to RGBA8)
    std::vector<uint16_t> raw_fp16_rgba;  // Populated when color_format == Rgba16Sfloat
    uint64_t fnv1a_hash = 0;
    bool executed_on_vulkan_icd = false;
    bool used_msaa_4x = false;
    bool used_depth_buffer = false;
};

// VK_EXT_mesh_shader draw configuration (optional Task/AS + Mesh/MS + Fragment/PS).
struct VulkanMeshDrawDesc {
    std::vector<uint32_t> task_spirv;      // Optional TaskEXT (Amplification) SPIR-V 1.4+
    std::vector<uint32_t> mesh_spirv;      // Required MeshEXT SPIR-V 1.4+
    std::vector<uint32_t> fragment_spirv;  // Required Fragment SPIR-V
    std::string task_entry = "main";
    std::string mesh_entry = "main";
    std::string fragment_entry = "main";

    uint32_t group_count_x = 1;
    uint32_t group_count_y = 1;
    uint32_t group_count_z = 1;

    uint32_t width = 128;
    uint32_t height = 128;
    VulkanImageFormat color_format = VulkanImageFormat::Rgba8Unorm;
    bool enable_depth = false;
    float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float clear_depth = 1.0f;

    std::vector<uint8_t> push_constants;
    std::vector<VulkanDescriptorBinding> descriptors;

    // If true, transparently falls back to deterministic clean-room SPIR-V mesh emulation
    // when the host GPU / CI runner does not expose VK_EXT_mesh_shader.
    bool allow_fallback_emulation = true;
};

struct VulkanMeshDrawResult {
    FrameBufferRgba8 framebuffer;
    std::vector<uint16_t> raw_fp16_rgba;
    uint64_t fnv1a_hash = 0;
    bool executed_on_mesh_shader_hw = false;
    bool used_task_shader = false;
    bool used_fallback = false;
};

// Win32 surface / swapchain / headless offscreen presentation descriptor.
inline constexpr uint32_t kVkColorSpaceSrgbNonlinearKhr = 0u;
inline constexpr uint32_t kVkColorSpaceHdr10St2084Ext = 1000104008u;

struct VulkanSwapchainDesc {
    void* hwnd = nullptr;       // Win32 HWND; nullptr or headless_offscreen=true selects offscreen mode
    void* hinstance = nullptr;  // Optional HINSTANCE (defaults to GetModuleHandleA(nullptr))
    uint32_t width = 128;
    uint32_t height = 128;
    bool prefer_hdr10 = false;
    bool headless_offscreen = false;
};

struct VulkanSwapchainInfo {
    uint64_t handle = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t image_count = 0;
    uint32_t vk_format = 0;
    uint32_t vk_color_space = 0;
    bool is_win32_surface = false;
    bool is_headless_offscreen = false;
    bool hdr10_advertised = false;
    bool hdr10_active = false;
    std::vector<uint32_t> supported_color_spaces;
};

struct VulkanPresentResult {
    uint32_t image_index = 0;
    bool presented_to_win32_swapchain = false;
    bool readback_available = false;
    FrameBufferRgba8 readback_framebuffer;
    uint64_t fnv1a_hash = 0;
};

// Sparse / reserved image resource structs (D3D12 CreateReservedResource + GetResourceTiling parity).
struct VulkanReservedResourceDesc {
    uint32_t width = 256;
    uint32_t height = 256;
    uint32_t depth = 1;
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;
    bool is_3d = false;
    VulkanImageFormat format = VulkanImageFormat::Rgba8Unorm;
};

struct VulkanSubresourceTiling {
    uint32_t width_in_tiles = 0;
    uint32_t height_in_tiles = 0;
    uint32_t depth_in_tiles = 0;
    uint32_t start_tile_index_in_overall_resource = 0;
};

struct VulkanResourceTilingInfo {
    uint32_t num_tiles_for_entire_resource = 0;
    uint32_t num_standard_mips = 0;
    uint32_t num_packed_mips = 0;
    uint32_t num_tiles_for_packed_mips = 0;
    uint32_t start_tile_index_in_overall_resource = 0;
    uint32_t tile_width_in_texels = 0;
    uint32_t tile_height_in_texels = 0;
    uint32_t tile_depth_in_texels = 1;
    uint64_t tile_size_bytes = 65536;
    uint64_t mip_tail_offset_bytes = 0;
    uint64_t mip_tail_size_bytes = 0;
    uint64_t mip_tail_stride_bytes = 0;
    uint32_t sparse_format_flags = 0;
    std::vector<VulkanSubresourceTiling> subresource_tilings;
    bool executed_on_vulkan_icd = false;
};

struct VulkanSparseTileMapping {
    uint32_t mip_level = 0;
    uint32_t array_layer = 0;
    uint32_t tile_x = 0;
    uint32_t tile_y = 0;
    uint32_t tile_z = 0;
    uint32_t tile_count_x = 1;
    uint32_t tile_count_y = 1;
    uint32_t tile_count_z = 1;
    uint32_t heap_tile_offset = 0;
    bool map_null = false;
    bool bind_mip_tail = false;
};

struct VulkanReservedResourceHandle {
    uint64_t handle = 0;
    VulkanResourceTilingInfo tiling;
    bool executed_on_vulkan_icd = false;
};

// Clean-room Dynamic Vulkan 1.2/1.3/1.4 backend with zero Vulkan SDK header dependencies.
// Dynamically loads vulkan-1.dll at runtime via LoadLibraryA + vkGetInstanceProcAddr,
// executes real hardware/ICD compute, graphics rasterization, VK_EXT_mesh_shader,
// 2D/3D mipmapped texture copies, and Win32 swapchain presentation when available,
// and provides deterministic clean-room SPIR-V software fallbacks when no ICD is present.
class VulkanBackend {
public:
    VulkanBackend();
    ~VulkanBackend();

    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // Attempts to initialize a real Vulkan 1.2/1.3/1.4 ICD via vulkan-1.dll.
    // If no ICD is available and allow_software_fallback is true, enters deterministic
    // software SPIR-V compute/graphics/mesh fallback mode.
    Status Initialize(bool allow_software_fallback = true);

    // Explicitly initializes the deterministic software SPIR-V fallback mode
    // (useful for testing fallback parity even on hosts with hardware Vulkan GPUs).
    Status InitializeSoftwareFallback();

    void Shutdown();
    bool IsInitialized() const noexcept;
    bool is_fallback_mode() const noexcept;

    const VulkanDeviceInfo& GetDeviceInfo() const noexcept { return device_info_; }

    // Validates SPIR-V binary magic (0x07230203), version, header bounds, instruction
    // word lengths, OpMemoryModel presence, and balanced OpFunction/OpFunctionEnd blocks.
    static Status ValidateSpirvBinary(const std::vector<uint32_t>& spirv,
                                      SpirvValidationInfo* out_info = nullptr);

    // Validates SPIR-V and executes a 1D compute shader with storage buffer binding 0 (input)
    // and binding 1 (output) across group_x workgroups on real Vulkan GPU or software fallback.
    Status ExecuteComputeShader(const std::vector<uint32_t>& spirv,
                                const std::vector<uint32_t>& input_u32,
                                std::vector<uint32_t>& output_u32,
                                uint32_t group_x);

    // Records a command list (real VkCommandBuffer begin/barrier/end/submit on Vulkan ICD,
    // or validated recording in software fallback mode).
    Status RecordCommandList(const VulkanCommandListDesc& desc,
                             VulkanCommandListResult& out_result);

    // Executes a full VS+PS rasterization draw with optional vertex/index buffers, UBO/SSBO,
    // 2D/3D sampled textures, push constants, D32_SFLOAT depth buffer, and 4x MSAA resolve.
    Status ExecuteGraphicsDraw(const VulkanGraphicsDrawDesc& desc,
                               VulkanGraphicsDrawResult& out_result);

    // Executes a VK_EXT_mesh_shader pipeline (optional TaskEXT + MeshEXT + Fragment) via
    // vkCmdDrawMeshTasksEXT with graceful deterministic fallback when unavailable.
    Status ExecuteMeshShaderDraw(const VulkanMeshDrawDesc& desc,
                                 VulkanMeshDrawResult& out_result);

    // Uploads all mip levels of a 2D or 3D volume texture via vkCmdCopyBufferToImage and
    // reads back every mip level via vkCmdCopyImageToBuffer to verify exact GPU layout/copies.
    Status UploadAndReadbackTexture(const VulkanTextureDesc& desc,
                                    VulkanTextureRoundtripResult& out_result);

    // Creates either an interactive Win32 VkSwapchainKHR (with HDR10 colorspace probe)
    // or a headless offscreen render-target swapchain (for CI/headless environments).
    Status CreateWin32Swapchain(const VulkanSwapchainDesc& desc,
                                VulkanSwapchainInfo& out_info);

    // Presents an RGBA8 framebuffer to a Win32 swapchain or offscreen headless target.
    Status PresentRgba8ToSwapchain(uint64_t swapchain_handle,
                                   const FrameBufferRgba8& frame,
                                   VulkanPresentResult& out_result,
                                   bool readback_frame = false);

    // Destroys a previously created swapchain/surface handle.
    Status DestroyWin32Swapchain(uint64_t swapchain_handle);

    // Creates a sparse/reserved Vulkan image (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
    // VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT) and queries vkGetImageSparseMemoryRequirements
    // to populate D3D12 GetResourceTiling-compatible tiling metadata.
    Status CreateReservedImage(const VulkanReservedResourceDesc& desc,
                               VulkanReservedResourceHandle& out_handle);

    // Queries GetResourceTiling-compatible tiling layout for an existing reserved image.
    Status GetReservedImageTiling(uint64_t reserved_handle,
                                  VulkanResourceTilingInfo& out_tiling) const;

    // Binds or unbinds sparse tiles (and optional opaque mip tail) via vkQueueBindSparse.
    Status BindReservedImageTiles(uint64_t reserved_handle,
                                  const std::vector<VulkanSparseTileMapping>& mappings);

    // Uploads texel data to a bound tile of a reserved image and reads it back from the GPU.
    Status UploadAndReadbackReservedImageTile(uint64_t reserved_handle,
                                              uint32_t mip_level,
                                              uint32_t tile_x,
                                              uint32_t tile_y,
                                              const std::vector<uint8_t>& tile_pixels,
                                              std::vector<uint8_t>& out_readback_pixels);

    // Destroys a previously created reserved/sparse image and releases its backing memory.
    Status DestroyReservedImage(uint64_t reserved_handle);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    VulkanDeviceInfo device_info_{};
};

}  // namespace openscarlett
