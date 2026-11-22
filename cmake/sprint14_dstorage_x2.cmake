# Sprint 14 Worker 4: DirectStorage X2 Interfaces, Real Xbox RFC 1950/1951 Zlib &
# Hardware BCPack BC1-BC7 Bitstream Decode, Swizzle/PhysicalPages, Parallel IOCP
# Async File I/O (> 64 GiB Sparse File Safe), and MSAA 2x/4x/8x XG Tiled Layout.

openscarlett_add_test(test_s14_dstorage_x2_iocp tests/test_s14_dstorage_x2_iocp.cpp)
target_link_libraries(test_s14_dstorage_x2_iocp PRIVATE
    openscarlett_core
    dstorage_xs
    xg_xs
    kernelx
)
if(WIN32)
    target_link_libraries(test_s14_dstorage_x2_iocp PRIVATE
        d3d12
        dxgi
        ole32
        uuid
    )
endif()

if(COMMAND _openscarlett_apply_portability_to_target)
    _openscarlett_apply_portability_to_target(test_s14_dstorage_x2_iocp)
endif()
