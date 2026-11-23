# OpenScarlett UE5 Matrix Awakens 7-DLL End-to-End Integration Test

if(WIN32)
    openscarlett_add_test(test_ue5_matrix_import_bringup tests/test_ue5_matrix_import_bringup.cpp)
    target_link_libraries(test_ue5_matrix_import_bringup PRIVATE
        openscarlett_launcher_core
    )
    add_dependencies(test_ue5_matrix_import_bringup
        openscarlett
        openscarlett_hook
        dxcompiler_xs
        xcrdapi
        dstorage_xs
        xg_xs
        d3d12_xs
        AcpHal
        xmem
        kernelx
        mfplat_xs
    )
    set_tests_properties(test_ue5_matrix_import_bringup PROPERTIES
        ENVIRONMENT "OPENSCARLETT_NO_UPDATE_CHECK=1;OPENSCARLETT_AUDIO_NULL=1"
    )
    if(COMMAND _openscarlett_apply_portability_to_target)
        _openscarlett_apply_portability_to_target(test_ue5_matrix_import_bringup)
    endif()
endif()
