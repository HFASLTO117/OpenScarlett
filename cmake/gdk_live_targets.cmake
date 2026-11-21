# OpenScarlett Sprint 3 — Real Live Xbox-ABI Guest Binaries & Microsoft Xbox-GDK-Samples Bring-Up
# Owned by sprint3-gdk-live worktree.

function(openscarlett_add_gdk_guest_exe name src)
    add_executable(${name} ${src})
    # Pure standalone Xbox-ABI Win32 x64 guest executable:
    # Deliberately does NOT link openscarlett_core!
    # Resolves Xbox DLLs purely via PE IAT / LoadLibrary + openscarlett run APC hook.
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    if(WIN32)
        target_compile_definitions(${name} PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
        target_link_libraries(${name} PRIVATE
            kernel32
            user32
            ole32
            uuid
            dxgi
        )
        if(MSVC)
            target_compile_options(${name} PRIVATE /W4)
        endif()
    endif()
endfunction()

# Sample 1: Microsoft Xbox-GDK-Samples / IntroGraphics / SimpleTriangle (Xbox ABI)
openscarlett_add_gdk_guest_exe(gdk_simple_triangle tests/gdk_samples/gdk_simple_triangle.cpp)
target_link_libraries(gdk_simple_triangle PRIVATE
    d3d12_xs
    xgameruntime
    GameInput
)

# Sample 2: Microsoft Xbox-GDK-Samples / IntroGraphics / SimpleMeshShader (Xbox ABI)
openscarlett_add_gdk_guest_exe(gdk_simple_mesh_shader tests/gdk_samples/gdk_simple_mesh_shader.cpp)
target_link_libraries(gdk_simple_mesh_shader PRIVATE
    d3d12_xs
    xgameruntime
)

# Sample 3: Microsoft Xbox-GDK-Samples / IntroGraphics / SimpleCompute (Xbox ABI)
openscarlett_add_gdk_guest_exe(gdk_simple_compute tests/gdk_samples/gdk_simple_compute.cpp)
target_link_libraries(gdk_simple_compute PRIVATE
    d3d12_xs
    xgameruntime
)

# Sample 4: Unified System Combo (exercises all 10 shimmed Xbox DLLs in one live process)
# Dynamically loads kernelx.dll via hooked LoadLibraryA while statically importing the other 9 Xbox DLLs.
openscarlett_add_gdk_guest_exe(gdk_system_combo tests/gdk_samples/gdk_system_combo.cpp)
target_link_libraries(gdk_system_combo PRIVATE
    d3d12_xs
    xg_xs
    xgameruntime
    GameInput
    dstorage_xs
    XAudio2_9
    AcpHal
    winrt_xs
    Microsoft.Xbox.Services.GDK.C
)

# Live End-to-End Test Harness (runs all 4 standalone Xbox guests via openscarlett.exe run)
openscarlett_add_test(test_gdk_live tests/test_gdk_live.cpp)
add_dependencies(test_gdk_live
    openscarlett
    openscarlett_hook
    d3d12_xs
    xg_xs
    kernelx
    xgameruntime
    GameInput
    XAudio2_9
    AcpHal
    dstorage_xs
    winrt_xs
    Microsoft.Xbox.Services.GDK.C
    gdk_simple_triangle
    gdk_simple_mesh_shader
    gdk_simple_compute
    gdk_system_combo
)
set_tests_properties(test_gdk_live PROPERTIES
    ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
)
