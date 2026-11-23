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

# 1b. chad.dll (Xbox Scarlett CHAD / XDSP hardware DSP codec & stream API)
add_library(chad SHARED src/shims/chad_shim.cpp)
set_target_properties(chad PROPERTIES
    PREFIX ""
    OUTPUT_NAME "chad"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
target_link_libraries(chad PRIVATE openscarlett_core)
if(MSVC)
    target_compile_options(chad PRIVATE /W4)
endif()

# 2. Unit test dynamically loading xapu.dll and AcpHal.dll via LoadLibraryW / GetProcAddress
openscarlett_add_test(test_mine_xapu_acphal tests/test_mine_xapu_acphal.cpp)
add_dependencies(test_mine_xapu_acphal xapu AcpHal)

# 2b. Retail audio engine suite (xapu.dll, chad.dll, AcpHal.dll, XAudio2_9.dll)
openscarlett_add_test(test_retail_audio_engine tests/test_retail_audio_engine.cpp)
target_link_libraries(test_retail_audio_engine PRIVATE XAudio2_9)
add_dependencies(test_retail_audio_engine xapu chad AcpHal XAudio2_9)

# 3. Clean-room regression test suite for Minecraft Preview perf & hardening
openscarlett_add_test(test_minecraft_perf_hardening tests/test_minecraft_perf_hardening.cpp)
add_dependencies(test_minecraft_perf_hardening openscarlett_hook XAudio2_9 AcpHal xapu dstorage_xs d3d12_xs)

# 4. Real-time system resource & native sampling CPU profiler (Windows)
if(WIN32)
    add_executable(os_live_profiler tools/os_live_profiler.cpp)
    target_link_libraries(os_live_profiler PRIVATE dbghelp pdh psapi)
    if(MSVC)
        target_compile_options(os_live_profiler PRIVATE /W4)
    endif()
endif()
