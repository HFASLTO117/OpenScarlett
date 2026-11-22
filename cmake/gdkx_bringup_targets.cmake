# OpenScarlett Real Untouched _GAMING_XBOX_SCARLETT (GDKX) Bring-Up & ABI Suite
# Compiles 100% untouched MIT Microsoft Xbox-GDK-Samples
# (SimpleTriangle + SimpleLighting + SimpleTexture + SimpleInstancing)
# under /D_GAMING_XBOX_SCARLETT linking ONLY OpenScarlett's shims, and verifies
# zero raw-host escape + real rendered PNG pixels via openscarlett.exe APC injection.

# 1. Cross-toolchain GDKX ABI & appnotify.h PLM unit test suite (MSVC, Clang-CL, MinGW-w64)
openscarlett_add_test(test_gdkx_abi tests/test_gdkx_abi.cpp)
target_link_libraries(test_gdkx_abi PRIVATE
    d3d12_xs
    kernelx
    xgameruntime
)
if(WIN32)
    target_link_libraries(test_gdkx_abi PRIVATE dxguid)
endif()

# 2. Untouched Microsoft _GAMING_XBOX_SCARLETT samples (MSVC + dxc.exe)
if(WIN32 AND MSVC)
    set(_os_dxc_hints "")
    if(DEFINED ENV{WindowsSdkVerBinPath} AND NOT "$ENV{WindowsSdkVerBinPath}" STREQUAL "")
        list(APPEND _os_dxc_hints "$ENV{WindowsSdkVerBinPath}/x64")
    endif()
    file(GLOB _os_sdk_dxc_dirs "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
    if(_os_sdk_dxc_dirs)
        list(SORT _os_sdk_dxc_dirs ORDER DESCENDING)
        list(APPEND _os_dxc_hints ${_os_sdk_dxc_dirs})
    endif()
    find_program(OPENSCARLETT_DXC_EXE
        NAMES dxc dxc.exe
        HINTS ${_os_dxc_hints}
    )

    if(OPENSCARLETT_DXC_EXE)
        message(STATUS "OpenScarlett GDKX bring-up: using dxc at ${OPENSCARLETT_DXC_EXE}")

        set(_os_gdkx_headers "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdkx_bringup/gdkx_headers")
        set(_os_gdkx_manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdkx_bringup/utf8.manifest")
        set(_os_gdkx_cso_root "${CMAKE_BINARY_DIR}/gdkx_shaders")

        function(openscarlett_compile_hlsl out_var hlsl_path cso_path profile include_dir)
            get_filename_component(_cso_dir "${cso_path}" DIRECTORY)
            set(_extra_deps ${ARGN})
            add_custom_command(
                OUTPUT "${cso_path}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_cso_dir}"
                COMMAND "${OPENSCARLETT_DXC_EXE}" -nologo -T ${profile} -E main -I "${include_dir}" -Fo "${cso_path}" "${hlsl_path}"
                DEPENDS "${hlsl_path}" ${_extra_deps}
                COMMENT "DXC compiling untouched GDKX shader ${hlsl_path} (${profile})"
                VERBATIM
            )
            set(${out_var} "${cso_path}" PARENT_SCOPE)
        endfunction()

        # Helper to build an untouched MIT Microsoft _GAMING_XBOX_SCARLETT sample
        function(openscarlett_add_untouched_gdkx_sample target_name sample_dir cpp_files)
            set(_sample_abs_srcs "")
            foreach(_f IN LISTS cpp_files)
                list(APPEND _sample_abs_srcs "${sample_dir}/${_f}")
            endforeach()

            add_executable(${target_name} WIN32
                ${_sample_abs_srcs}
                "${_os_gdkx_manifest}"
            )
            # Microsoft's untouched Main.cpp uses C++17 u8"..." literals as LPCSTR
            set_target_properties(${target_name} PROPERTIES
                CXX_STANDARD 17
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF
            )
            # Strictly ONLY clean-room GDKX headers + the untouched sample directory
            # (NO OpenScarlett internal include/ headers!)
            target_include_directories(${target_name} PRIVATE
                "${_os_gdkx_headers}"
                "${sample_dir}"
            )
            target_compile_definitions(${target_name} PRIVATE
                _GAMING_XBOX_SCARLETT=1
                _GAMING_XBOX=1
                UNICODE
                _UNICODE
            )
            # Link ONLY OpenScarlett's GDKX shim libs + standard Win32 OS libs
            # (kernelx precedes xgameruntime so appnotify.h resolves to kernelx.dll, matching GDKX)
            target_link_libraries(${target_name} PRIVATE
                d3d12_xs
                kernelx
                xgameruntime
                pixevt
                user32
                dxguid
            )
            # Suppress third-party /W4 & pch.h off-by-default warning noise ONLY on
            # Microsoft's untouched sample .cpp files (never editing Microsoft's code).
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
                set_source_files_properties(${_sample_abs_srcs} PROPERTIES
                    COMPILE_OPTIONS "-Wno-everything;/arch:AVX2"
                )
            else()
                set_source_files_properties(${_sample_abs_srcs} PROPERTIES
                    COMPILE_OPTIONS "/W3;/wd4061;/wd4062;/wd4100;/wd4191;/wd4244;/wd4263;/wd4264;/wd4265;/wd4266;/wd4365;/wd4746;/wd4826;/wd4986;/wd4987;/wd5029;/wd5038;/wd5042"
                )
            endif()
        endfunction()

        # --- Sample 1: Untouched MIT SimpleTriangle ---
        set(_tri_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdkx_bringup/samples/SimpleTriangle")
        openscarlett_add_untouched_gdkx_sample(SimpleTriangle_GDKX "${_tri_dir}"
            "pch.cpp;DeviceResources.cpp;SimpleTriangle.cpp;Main.cpp"
        )
        openscarlett_compile_hlsl(_tri_vs_cso "${_tri_dir}/VertexShader.hlsl" "${_os_gdkx_cso_root}/SimpleTriangle/VertexShader.cso" vs_6_0 "${_tri_dir}")
        openscarlett_compile_hlsl(_tri_ps_cso "${_tri_dir}/PixelShader.hlsl" "${_os_gdkx_cso_root}/SimpleTriangle/PixelShader.cso" ps_6_0 "${_tri_dir}")
        add_custom_target(SimpleTriangle_GDKX_shaders ALL DEPENDS "${_tri_vs_cso}" "${_tri_ps_cso}")
        add_dependencies(SimpleTriangle_GDKX SimpleTriangle_GDKX_shaders)

        # --- Sample 2: Untouched MIT SimpleLighting ---
        set(_lit_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdkx_bringup/samples/SimpleLighting")
        openscarlett_add_untouched_gdkx_sample(SimpleLighting_GDKX "${_lit_dir}"
            "pch.cpp;DeviceResources.cpp;SimpleLighting.cpp;Main.cpp"
        )
        openscarlett_compile_hlsl(_lit_vs_cso "${_lit_dir}/TriangleVS.hlsl" "${_os_gdkx_cso_root}/SimpleLighting/TriangleVS.cso" vs_6_0 "${_lit_dir}" "${_lit_dir}/SimpleLighting.hlsli")
        openscarlett_compile_hlsl(_lit_lambert_cso "${_lit_dir}/LambertPS.hlsl" "${_os_gdkx_cso_root}/SimpleLighting/LambertPS.cso" ps_6_0 "${_lit_dir}" "${_lit_dir}/SimpleLighting.hlsli")
        openscarlett_compile_hlsl(_lit_solid_cso "${_lit_dir}/SolidColorPS.hlsl" "${_os_gdkx_cso_root}/SimpleLighting/SolidColorPS.cso" ps_6_0 "${_lit_dir}" "${_lit_dir}/SimpleLighting.hlsli")
        add_custom_target(SimpleLighting_GDKX_shaders ALL DEPENDS "${_lit_vs_cso}" "${_lit_lambert_cso}" "${_lit_solid_cso}")
        add_dependencies(SimpleLighting_GDKX SimpleLighting_GDKX_shaders)

        # --- Sample 3: Untouched MIT SimpleTexture ---
        set(_tex_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdkx_bringup/samples/SimpleTexture")
        openscarlett_add_untouched_gdkx_sample(SimpleTexture_GDKX "${_tex_dir}"
            "pch.cpp;DeviceResources.cpp;SimpleTexture.cpp;Main.cpp"
        )
        target_link_libraries(SimpleTexture_GDKX PRIVATE ole32 windowscodecs)
        openscarlett_compile_hlsl(_tex_vs_cso "${_tex_dir}/VertexShader.hlsl" "${_os_gdkx_cso_root}/SimpleTexture/VertexShader.cso" vs_6_0 "${_tex_dir}")
        openscarlett_compile_hlsl(_tex_ps_cso "${_tex_dir}/PixelShader.hlsl" "${_os_gdkx_cso_root}/SimpleTexture/PixelShader.cso" ps_6_0 "${_tex_dir}")
        add_custom_target(SimpleTexture_GDKX_shaders ALL DEPENDS "${_tex_vs_cso}" "${_tex_ps_cso}")
        add_dependencies(SimpleTexture_GDKX SimpleTexture_GDKX_shaders)

        # --- Sample 4: Untouched MIT SimpleInstancing ---
        set(_inst_dir "${CMAKE_CURRENT_SOURCE_DIR}/tests/gdkx_bringup/samples/SimpleInstancing")
        openscarlett_add_untouched_gdkx_sample(SimpleInstancing_GDKX "${_inst_dir}"
            "pch.cpp;DeviceResources.cpp;SimpleInstancing.cpp;Main.cpp"
        )
        openscarlett_compile_hlsl(_inst_vs_cso "${_inst_dir}/VertexShader.hlsl" "${_os_gdkx_cso_root}/SimpleInstancing/VertexShader.cso" vs_6_0 "${_inst_dir}" "${_inst_dir}/SimpleInstancing.hlsli" "${_inst_dir}/Shared.h")
        openscarlett_compile_hlsl(_inst_ps_cso "${_inst_dir}/PixelShader.hlsl" "${_os_gdkx_cso_root}/SimpleInstancing/PixelShader.cso" ps_6_0 "${_inst_dir}" "${_inst_dir}/SimpleInstancing.hlsli" "${_inst_dir}/Shared.h")
        add_custom_target(SimpleInstancing_GDKX_shaders ALL DEPENDS "${_inst_vs_cso}" "${_inst_ps_cso}")
        add_dependencies(SimpleInstancing_GDKX SimpleInstancing_GDKX_shaders)

        # --- End-to-End CTest Harness (Isolated Zero-DLL APC Run + Telemetry + PNG Pixel Check) ---
        openscarlett_add_test(test_gdkx_untouched_samples tests/test_gdkx_untouched_samples.cpp)
        target_link_libraries(test_gdkx_untouched_samples PRIVATE ole32 windowscodecs)
        add_dependencies(test_gdkx_untouched_samples
            openscarlett
            openscarlett_hook
            d3d12_xs
            kernelx
            xgameruntime
            pixevt
            SimpleTriangle_GDKX
            SimpleTriangle_GDKX_shaders
            SimpleLighting_GDKX
            SimpleLighting_GDKX_shaders
            SimpleTexture_GDKX
            SimpleTexture_GDKX_shaders
            SimpleInstancing_GDKX
            SimpleInstancing_GDKX_shaders
        )
        set_tests_properties(test_gdkx_untouched_samples PROPERTIES
            TIMEOUT 480
        )
    else()
        message(STATUS "OpenScarlett GDKX bring-up: dxc.exe not found; skipping untouched HLSL sample builds")
    endif()
endif()
