# OpenScarlett Worker 5 — Unified DLL Routing, Hardened CoW VFS & Win32 Child-Process Hook Propagation
# Branch: feat/loader-vfs-child-hook

openscarlett_add_test(test_loader_child_hook_parity tests/test_loader_child_hook_parity.cpp)
add_dependencies(test_loader_child_hook_parity
    openscarlett
    openscarlett_hook
    winrt_xs
)
