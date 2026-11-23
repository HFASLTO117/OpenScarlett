# Sprint 16 Workstream 3: Storage, Compression, Audio & Video Media 100% Test Target

openscarlett_add_test(test_s16_media_storage_audio tests/test_s16_media_storage_audio.cpp)
target_link_libraries(test_s16_media_storage_audio PRIVATE
    openscarlett_core
    dstorage_xs
    XAudio2_9
    AcpHal
    xapu
    Bink2Scarlett
)
add_dependencies(test_s16_media_storage_audio
    mfplat_xs
    mfhevcdec
    mflnmhelp
)
if(WIN32)
    target_link_libraries(test_s16_media_storage_audio PRIVATE d3d12 dxgi ole32 uuid)
endif()
if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s16_media_storage_audio)
endif()
if(MSVC)
    target_compile_options(test_s16_media_storage_audio PRIVATE /W4)
endif()
set_tests_properties(test_s16_media_storage_audio PROPERTIES RESOURCE_LOCK audio)
