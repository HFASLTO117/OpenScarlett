# Worker 2 (wt-gpu-d3d12x-vk-xg): D3D12X NVIDIA/Xbox Extensions,
# Vulkan Sparse/Reserved Resources + S#/T# Mip Parity, and xg_xs Console Metadata Parity.

openscarlett_add_test(test_gpu_d3d12x_vk_parity tests/test_gpu_d3d12x_vk_parity.cpp)
target_link_libraries(test_gpu_d3d12x_vk_parity PRIVATE d3d12_xs xg_xs)
if(WIN32)
    target_link_libraries(test_gpu_d3d12x_vk_parity PRIVATE d3d12 dxgi d3dcompiler)
endif()
