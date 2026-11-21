# OpenScarlett Hardware DXR Tier 1.1 & SAH Software BVH targets

target_sources(openscarlett_core PRIVATE
    src/gpu/rt_bvh.cpp
    src/gpu/dxr_hardware.cpp
)

openscarlett_add_test(test_dxr_live tests/test_dxr_live.cpp)
