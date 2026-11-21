# Wave 3 / Wave 1: Full Zero-SDK Vulkan 1.2/1.4 Graphics, Mesh Shaders, 3D Textures & Win32 Swapchain

openscarlett_add_test(test_vulkan_graphics_live tests/test_vulkan_graphics_live.cpp)
if(WIN32)
    target_link_libraries(test_vulkan_graphics_live PRIVATE user32)
endif()
