# OpenScarlett Wave 2 — Multi-Pass GDK Workloads & External Real-Binary Bring-Up
# Modular per-agent includes keep all 4 parallel Wave 2 worktrees 100% conflict-free.

foreach(_os_wave2_cmake
    cmake/wave2_simple_pbr.cmake
    cmake/wave2_meshlet_cull.cmake
    cmake/wave2_dxr_triangle.cmake
    cmake/wave2_external_gdk.cmake
)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_os_wave2_cmake}")
        include("${CMAKE_CURRENT_SOURCE_DIR}/${_os_wave2_cmake}")
    endif()
endforeach()
