# OpenScarlett Sprint 7 Wave 2 Targets
# All-in-One Native Windows GUI (OpenScarlettLauncher.exe),
# Integrated 60s Hardware Self-Test Bridge & Opt-In WinHTTP/BCrypt SHA-256 Updater.

if(WIN32)
    enable_language(RC)

    function(_openscarlett_configure_launcher_target tgt)
        if(MSVC)
            target_compile_options(${tgt} PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W4>)
        elseif(MINGW)
            target_compile_definitions(${tgt} PRIVATE
                OPENSCARLETT_MINGW=1
                _WIN32_WINNT=0x0A00
                WINVER=0x0A00
            )
            target_compile_options(${tgt} PRIVATE
                $<$<COMPILE_LANGUAGE:C,CXX>:-Wall>
                $<$<COMPILE_LANGUAGE:C,CXX>:-Wextra>
                $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unknown-pragmas>
                $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>
                $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
            )
        else()
            target_compile_options(${tgt} PRIVATE
                $<$<COMPILE_LANGUAGE:C,CXX>:-Wall>
                $<$<COMPILE_LANGUAGE:C,CXX>:-Wextra>
            )
        endif()
    endfunction()

    add_library(openscarlett_launcher_core STATIC
        src/launcher/updater.cpp
        src/launcher/hwtest_runner.cpp
    )
    target_include_directories(openscarlett_launcher_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(openscarlett_launcher_core PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(openscarlett_launcher_core PUBLIC
        openscarlett_core
        winhttp
        bcrypt
        advapi32
        version
        user32
        shell32
    )
    _openscarlett_configure_launcher_target(openscarlett_launcher_core)

    add_executable(OpenScarlettLauncher WIN32
        src/launcher/launcher_main.cpp
        src/launcher/launcher.rc
    )
    set_source_files_properties(src/launcher/launcher.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/icon.ico"
    )
    target_include_directories(OpenScarlettLauncher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/launcher
        ${CMAKE_CURRENT_SOURCE_DIR}/docs
    )
    target_compile_definitions(OpenScarlettLauncher PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    target_link_libraries(OpenScarlettLauncher PRIVATE
        openscarlett_launcher_core
        openscarlett_core
        d2d1
        dwrite
        dwmapi
        comctl32
        comdlg32
        shell32
        ole32
        uuid
        gdi32
        user32
        winhttp
        bcrypt
        advapi32
        version
    )
    if(MINGW)
        target_link_options(OpenScarlettLauncher PRIVATE -municode)
    endif()
    add_dependencies(OpenScarlettLauncher openscarlett)
    _openscarlett_configure_launcher_target(OpenScarlettLauncher)

    openscarlett_add_test(test_launcher_updater tests/test_launcher_updater.cpp)
    target_include_directories(test_launcher_updater PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/launcher
    )
    target_link_libraries(test_launcher_updater PRIVATE
        openscarlett_launcher_core
        d2d1
        dwrite
        ole32
        uuid
        user32
        shell32
        winhttp
        bcrypt
        advapi32
        version
    )
    add_dependencies(test_launcher_updater
        OpenScarlettLauncher
        openscarlett
        gdk_3d_scene
    )
    set_tests_properties(test_launcher_updater PROPERTIES
        ENVIRONMENT "OPENSCARLETT_NO_UPDATE_CHECK=1;OPENSCARLETT_AUDIO_NULL=1"
    )
    _openscarlett_configure_launcher_target(test_launcher_updater)
endif()
