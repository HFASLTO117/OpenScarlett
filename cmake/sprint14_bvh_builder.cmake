# OpenScarlett Sprint 14 (Worker 3):
# Real MSVC-mangled class XboxBvhBuilder C++ ABI exports, dual-path pre-built BVH4
# rehydration (DXR 1.1 VA translation + DXR=0 in-place 64-bit pointer patching &
# live GPU compute ray-triangle/ray-AABB parity), and LiftDxrStateObjectDesc.

openscarlett_add_test(test_s14_xbox_bvh_builder_abi tests/test_s14_xbox_bvh_builder_abi.cpp)
target_link_libraries(test_s14_xbox_bvh_builder_abi PRIVATE XboxBvhBuilder)

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s14_xbox_bvh_builder_abi)
endif()
