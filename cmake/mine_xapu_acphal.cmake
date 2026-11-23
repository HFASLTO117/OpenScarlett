# OpenScarlett Xbox Series X Audio/APU Shims (xapu.dll + AcpHal.dll Apu* heap exports)
# Unblocks Minecraft Preview (fmod.dll) and UE5 audio middleware.

# 1. xapu.dll
add_library(xapu SHARED src/shims/xapu_shim.cpp)
set_target_properties(xapu PROPERTIES
    PREFIX ""
    OUTPUT_NAME "xapu"
)
target_compile_definitions(xapu PRIVATE OPENSCARLETT_BUILDING_XAPU=1)
target_link_libraries(xapu PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(xapu PRIVATE /W4)
endif()

# 2. Unit test dynamically loading xapu.dll and AcpHal.dll via LoadLibraryW / GetProcAddress
openscarlett_add_test(test_mine_xapu_acphal tests/test_mine_xapu_acphal.cpp)
add_dependencies(test_mine_xapu_acphal xapu AcpHal)

# 3. Clean-room regression test suite for Minecraft Preview perf & hardening
openscarlett_add_test(test_minecraft_perf_hardening tests/test_minecraft_perf_hardening.cpp)
add_dependencies(test_minecraft_perf_hardening openscarlett_hook XAudio2_9 AcpHal xapu dstorage_xs d3d12_xs)
