# OpenScarlett Worker 7: Real GDK Thunks Live Verification + Playable Scarlett Game Stack
# Owned by wt-playable-scarlett-game worktree.
#
# Builds:
#   1. test_real_gdk_thunks_live  - 292/292 xgameruntime + 16/16 XCurl + real commercial thunks test
#   2. scarlett_platform_host     - 100% MIT standalone Scarlett platform library (d3d12_xs, kernelx,
#                                   xgameruntime, GameInput, XAudio2_9)
#   3. ScarlettArena              - 100% MIT playable Scarlett 3D raycaster (always built, CI-safe)
#   4. ScarlettDoom               - Real Freedoom: Phase 1 (doomgeneric + freedoom1.wad) built
#                                   automatically when E:\OpenScarlett\.tmp\games\doomgeneric exists
#   5. test_scarlett_game_live    - End-to-end live verification via `openscarlett.exe run`

if(WIN32)
    # -------------------------------------------------------------------------
    # 1. Real Commercial GDK Thunks & 292/292 Export Coverage Verification
    # -------------------------------------------------------------------------
    openscarlett_add_test(test_real_gdk_thunks_live tests/test_real_gdk_thunks_live.cpp)
    add_dependencies(test_real_gdk_thunks_live
        xgameruntime
        XCurl
        Microsoft.Xbox.Services.GDK.C
        openscarlett
    )

    # -------------------------------------------------------------------------
    # 2. Standalone Xbox Series X|S (Scarlett) Platform Host Static Library
    #    (Does NOT link openscarlett_core — pure guest PE IAT to Xbox DLLs!)
    # -------------------------------------------------------------------------
    add_library(scarlett_platform_host STATIC
        tests/scarlett_game/scarlett_platform.cpp
    )
    target_include_directories(scarlett_platform_host PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/scarlett_game
    )
    target_compile_definitions(scarlett_platform_host PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(scarlett_platform_host PUBLIC
        d3d12_xs
        xgameruntime
        GameInput
        XAudio2_9
        d3d12
        dxgi
        d3dcompiler
        user32
        kernel32
        ole32
        uuid
    )
    if(MSVC)
        target_compile_options(scarlett_platform_host PRIVATE /W4)
    else()
        target_compile_options(scarlett_platform_host PRIVATE -Wall -Wextra)
    endif()

    # -------------------------------------------------------------------------
    # 3. ScarlettArena — 100% MIT Standalone Playable Scarlett 3D Raycaster
    # -------------------------------------------------------------------------
    add_executable(ScarlettArena
        tests/scarlett_game/scarlett_arena.cpp
    )
    target_link_libraries(ScarlettArena PRIVATE scarlett_platform_host)
    target_compile_definitions(ScarlettArena PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(ScarlettArena PRIVATE /W4)
    else()
        target_compile_options(ScarlettArena PRIVATE -Wall -Wextra)
    endif()

    # -------------------------------------------------------------------------
    # 4. ScarlettDoom — Real Playable Freedoom: Phase 1 (optional local .tmp)
    #    Uses unmodified external doomgeneric source from E:\OpenScarlett\.tmp\games\doomgeneric
    #    NEVER committed to git; skipped cleanly on GitHub Actions CI!
    # -------------------------------------------------------------------------
    set(OPENSCARLETT_DOOMGENERIC_DIR "E:/OpenScarlett/.tmp/games/doomgeneric/doomgeneric"
        CACHE PATH "External doomgeneric source directory (never committed to git)")
    set(OPENSCARLETT_FREEDOOM_WAD "E:/OpenScarlett/.tmp/games/freedoom/freedoom1.wad"
        CACHE FILEPATH "External Freedoom Phase 1 WAD file (never committed to git)")

    set(_os_has_scarlett_doom FALSE)
    if(EXISTS "${OPENSCARLETT_DOOMGENERIC_DIR}/doomgeneric.c" AND
       EXISTS "${OPENSCARLETT_DOOMGENERIC_DIR}/d_main.c")
        set(_os_has_scarlett_doom TRUE)
        set(_os_doom_core_srcs
            "${OPENSCARLETT_DOOMGENERIC_DIR}/dummy.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/am_map.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/doomdef.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/doomstat.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/dstrings.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/d_event.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/d_items.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/d_iwad.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/d_loop.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/d_main.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/d_mode.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/d_net.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/f_finale.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/f_wipe.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/g_game.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/hu_lib.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/hu_stuff.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/info.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_cdmus.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_endoom.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_input.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_joystick.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_scale.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_sound.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_system.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_timer.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/i_video.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/memio.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_argv.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_bbox.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_cheat.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_config.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_controls.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_fixed.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_menu.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_misc.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/m_random.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_ceilng.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_doors.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_enemy.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_floor.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_inter.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_lights.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_map.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_maputl.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_mobj.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_plats.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_pspr.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_saveg.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_setup.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_sight.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_spec.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_switch.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_telept.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_tick.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/p_user.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_bsp.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_data.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_draw.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_main.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_plane.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_segs.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_sky.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/r_things.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/sha1.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/sounds.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/statdump.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/st_lib.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/st_stuff.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/s_sound.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/tables.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/v_video.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/wi_stuff.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/w_checksum.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/w_file.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/w_file_stdc.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/w_main.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/w_wad.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/z_zone.c"
            "${OPENSCARLETT_DOOMGENERIC_DIR}/doomgeneric.c"
        )

        # Keep third-party doomgeneric C warnings silenced cleanly without touching third-party source!
        add_library(doomgeneric_ext_core OBJECT ${_os_doom_core_srcs})
        target_include_directories(doomgeneric_ext_core PRIVATE "${OPENSCARLETT_DOOMGENERIC_DIR}")
        target_compile_definitions(doomgeneric_ext_core PRIVATE
            _CRT_SECURE_NO_WARNINGS
            _CRT_NONSTDC_NO_DEPRECATE
        )
        if(MSVC)
            target_compile_options(doomgeneric_ext_core PRIVATE /W0)
        else()
            target_compile_options(doomgeneric_ext_core PRIVATE -w)
        endif()

        add_executable(ScarlettDoom
            tests/scarlett_game/scarlett_doom_bridge.c
            $<TARGET_OBJECTS:doomgeneric_ext_core>
        )
        target_include_directories(ScarlettDoom PRIVATE
            "${OPENSCARLETT_DOOMGENERIC_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/scarlett_game"
        )
        target_compile_definitions(ScarlettDoom PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(MSVC)
            target_compile_options(ScarlettDoom PRIVATE /W4)
        else()
            target_compile_options(ScarlettDoom PRIVATE -Wall -Wextra)
        endif()
        target_link_libraries(ScarlettDoom PRIVATE scarlett_platform_host)
    endif()

    # -------------------------------------------------------------------------
    # 5. Live End-to-End Playable Scarlett Game Verification Test
    # -------------------------------------------------------------------------
    openscarlett_add_test(test_scarlett_game_live tests/scarlett_game/test_scarlett_game_live.cpp)
    add_dependencies(test_scarlett_game_live
        openscarlett
        openscarlett_hook
        d3d12_xs
        kernelx
        xgameruntime
        GameInput
        XAudio2_9
        ScarlettArena
    )
    if(_os_has_scarlett_doom)
        add_dependencies(test_scarlett_game_live ScarlettDoom)
    endif()
    set_tests_properties(test_scarlett_game_live PROPERTIES
        ENVIRONMENT "OPENSCARLETT_AUDIO_NULL=1"
    )
endif()
