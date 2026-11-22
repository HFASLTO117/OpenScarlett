# OpenScarlett Minecraft Preview / UE5 d3d12_xs GUID & xmem XMemTransferMemory tests

openscarlett_add_test(test_mine_d3d12_xmem tests/test_mine_d3d12_xmem.cpp)
add_dependencies(test_mine_d3d12_xmem d3d12_xs xmem kernelx)
