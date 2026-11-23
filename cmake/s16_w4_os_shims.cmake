# Sprint 16 Workstream 4: OS, VFS, GDK Runtime & Retired Non-Media Stub DLLs

if(WIN32)
    if(TARGET xgameruntime)
        target_link_libraries(xgameruntime PRIVATE user32)
    endif()
    if(TARGET minuserx)
        target_link_libraries(minuserx PRIVATE gdi32)
    endif()

    add_executable(test_s16_os_vfs_gdk_shims
        tests/test_s16_os_vfs_gdk_shims.cpp
    )
    target_include_directories(test_s16_os_vfs_gdk_shims PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_s16_os_vfs_gdk_shims PRIVATE
        openscarlett_core
        xgameruntime
        kernelx
        user32
    )
    if(MSVC)
        target_compile_options(test_s16_os_vfs_gdk_shims PRIVATE /W4 /WX-)
    endif()
    if(COMMAND _openscarlett_apply_portability_to_target)
        _openscarlett_apply_portability_to_target(test_s16_os_vfs_gdk_shims)
    endif()
    add_dependencies(test_s16_os_vfs_gdk_shims
        openscarlett_hook
        xgameruntime
        kernelx
        hix_xs
        xbtp
        gamecoreclient
        minuserx
        xtfpix
        xtfdebugmonitor
    )
    add_test(NAME test_s16_os_vfs_gdk_shims COMMAND test_s16_os_vfs_gdk_shims)
    set_tests_properties(test_s16_os_vfs_gdk_shims PROPERTIES
        RESOURCE_LOCK "window;vfs_saves"
    )
endif()
