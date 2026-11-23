# UE5 Xbox Series X import bringup (Worker 2):
# D3D12 COM interface IID GUID DATA exports & D3D12Xbox* process/device helpers in d3d12_xs.dll.

openscarlett_add_test(test_ue5_d3d12_exports tests/test_ue5_d3d12_exports.cpp)
add_dependencies(test_ue5_d3d12_exports d3d12_xs)
if(WIN32)
    target_link_libraries(test_ue5_d3d12_exports PRIVATE
        d3d12
        dxgi
        ole32
        uuid
    )
endif()

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_ue5_d3d12_exports)
endif()
