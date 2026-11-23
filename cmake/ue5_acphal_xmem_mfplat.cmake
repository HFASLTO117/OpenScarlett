# OpenScarlett UE5 Xbox Series X Import Bringup — AcpHal, XMem, MFPlat Test Suite
# Owned by Worker 3 (ue5-w3-acphal-xmem-mfplat).

openscarlett_add_test(test_ue5_acphal_xmem_mfplat tests/test_ue5_acphal_xmem_mfplat.cpp)
add_dependencies(test_ue5_acphal_xmem_mfplat AcpHal xmem kernelx mfplat_xs)
