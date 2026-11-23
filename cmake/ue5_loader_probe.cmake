# OpenScarlett UE5 Loader Probe & Static Stub Synthesis Tests

if(WIN32)
    openscarlett_add_test(test_ue5_probe_static_stub tests/test_ue5_probe_static_stub.cpp)
    target_link_libraries(test_ue5_probe_static_stub PRIVATE
        openscarlett_launcher_core
    )
    add_dependencies(test_ue5_probe_static_stub
        openscarlett
        openscarlett_hook
        OpenScarlettLauncher
    )
    set_tests_properties(test_ue5_probe_static_stub PROPERTIES
        ENVIRONMENT "OPENSCARLETT_NO_UPDATE_CHECK=1;OPENSCARLETT_AUDIO_NULL=1"
    )
endif()
