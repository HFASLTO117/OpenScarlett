# OpenScarlett Repo-Wide Build & CTest Speedups
# - Automatic sccache/ccache compiler cache integration (cross-worktree .obj reuse)
# - Optional PCH opt-in when compiler cache is not active
# - Parallel CTest critical-path COST scheduling + safe RESOURCE_LOCK / RUN_SERIAL guards

option(OPENSCARLETT_USE_PCH
    "Enable precompiled headers for openscarlett_core (only when compiler cache is disabled)"
    OFF)

if(OPENSCARLETT_USE_PCH AND NOT CMAKE_CXX_COMPILER_LAUNCHER AND TARGET openscarlett_core)
    target_precompile_headers(openscarlett_core PRIVATE
        <algorithm>
        <array>
        <atomic>
        <cstdint>
        <cstring>
        <filesystem>
        <memory>
        <mutex>
        <string>
        <string_view>
        <unordered_map>
        <vector>
    )
    message(STATUS "OpenScarlett build speedup: precompiled headers enabled for openscarlett_core")
endif()

# Helper to assign CTest properties safely only when a test target exists.
function(_openscarlett_set_test_cost name cost_val)
    if(TEST ${name})
        set_tests_properties(${name} PROPERTIES COST ${cost_val})
    endif()
endfunction()

function(_openscarlett_add_test_lock name lock_name)
    if(TEST ${name})
        get_test_property(${name} RESOURCE_LOCK _existing_locks)
        if(_existing_locks AND NOT _existing_locks STREQUAL "NOTFOUND")
            set_tests_properties(${name} PROPERTIES RESOURCE_LOCK "${_existing_locks};${lock_name}")
        else()
            set_tests_properties(${name} PROPERTIES RESOURCE_LOCK "${lock_name}")
        endif()
    endif()
endfunction()

# 1. Critical-path COST weights so `ctest --parallel N` launches the slowest tests first.
_openscarlett_set_test_cost(test_gdkx_untouched_samples 100)
_openscarlett_set_test_cost(test_rdna2_gpu_accuracy 80)
_openscarlett_set_test_cost(test_rdna2_corpus_bench 50)
_openscarlett_set_test_cost(test_rdna2_lifter_live 45)
_openscarlett_set_test_cost(test_rdna2_as_task_spirv 40)
_openscarlett_set_test_cost(test_gpu_d3d12x_vk_parity 40)
_openscarlett_set_test_cost(test_s13_shader_cfg_safety 38)
_openscarlett_set_test_cost(test_d3d12_xs_autolift 36)
_openscarlett_set_test_cost(test_s13_live_xbox_shader_e2e 35)
_openscarlett_set_test_cost(test_scarlett_game_live 35)
_openscarlett_set_test_cost(test_external_gdk_bringup 32)
_openscarlett_set_test_cost(test_rdna2_lockstep_isa 30)
_openscarlett_set_test_cost(test_s12_rayquery_live 30)
_openscarlett_set_test_cost(test_xg_gpu_untile_and_stages 28)
_openscarlett_set_test_cost(test_s12_d3d12_aaa_wiring 26)
_openscarlett_set_test_cost(test_rdna2_isa_scalar_vector 24)
_openscarlett_set_test_cost(test_launcher_updater 24)
_openscarlett_set_test_cost(test_gdk_simple_lighting 22)
_openscarlett_set_test_cost(test_gdk_hdr_cubemap 22)
_openscarlett_set_test_cost(test_rdna2_corpus_parity 22)
_openscarlett_set_test_cost(test_d3d12_vk_routing 20)
_openscarlett_set_test_cost(test_s14_xbox_bvh_builder_abi 20)
_openscarlett_set_test_cost(test_s16_shader_completeness 35)
_openscarlett_set_test_cost(test_s16_graphics_completeness 30)
_openscarlett_set_test_cost(test_s16_media_storage_audio 25)
_openscarlett_set_test_cost(test_s16_os_vfs_gdk_shims 25)

# 2. Strict wall-clock benchmark / timing-sensitive tests run uncontended.
if(TEST test_s14_shader_sm66_779)
    set_tests_properties(test_s14_shader_sm66_779 PROPERTIES
        RUN_SERIAL TRUE
        COST 90
    )
endif()
if(TEST test_s15_rdna2_bench_trap_harness)
    set_tests_properties(test_s15_rdna2_bench_trap_harness PROPERTIES
        RUN_SERIAL TRUE
        COST 85
    )
endif()

# 3. Serialize tests that create desktop HWNDs / Win32 swapchains / focus checks.
foreach(_win_test
    test_gdkx_untouched_samples
    test_presentx_windowing
    test_scarlett_game_live
    test_launcher_updater
    test_gdk_3d_scene_dual_api
    test_hdr10_d3d12
    test_vulkan_graphics_live
    test_vulkan_mesh_showcase
    test_s13_live_xbox_shader_e2e
    test_s16_os_vfs_gdk_shims
)
    _openscarlett_add_test_lock(${_win_test} window)
endforeach()

# 3b. Serialize all-core CPU/WARP/DXC-saturating suites against each other so 4K WARP
# tessellation (SimpleBezier_warp) never starves while light/medium tests still run in parallel.
foreach(_heavy_cpu_test
    test_gdkx_untouched_samples
    test_rdna2_gpu_accuracy
    test_rdna2_corpus_bench
    test_rdna2_as_task_spirv
    test_s16_shader_completeness
)
    _openscarlett_add_test_lock(${_heavy_cpu_test} heavy_cpu)
endforeach()

# 4. Serialize tests touching live host WASAPI audio endpoints.
foreach(_audio_test
    test_audio_opus_wasapi
    test_dstorage_audio_diag_parity
    test_media_video_gameinput
    test_s16_media_storage_audio
    test_retail_audio_engine
)
    _openscarlett_add_test_lock(${_audio_test} audio)
endforeach()

# 5. Serialize tests sharing OPENSCARLETT_SAVE_ROOT / VFS overlay scratch roots.
foreach(_vfs_test
    test_apc_loader
    test_loader_vfs_config
    test_loader_child_hook_parity
    test_s12_os_loader_gameinput
    test_gdkx_untouched_samples
    test_minecraft_perf_hardening
    test_s16_os_vfs_gdk_shims
)
    _openscarlett_add_test_lock(${_vfs_test} vfs_saves)
endforeach()
