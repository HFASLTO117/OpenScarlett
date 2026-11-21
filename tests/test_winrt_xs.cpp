#include <cstdio>

#include "openscarlett/winrt/winrt_abi.h"
#include "os_check.h"

int main() {
    OS_REQUIRE(RoInitialize(SX_RO_INIT_MULTITHREADED) == 0);

    const uint16_t raw[] = {'W', 'i', 'n', 'S', 'c', 'a', 'r', 'l', 'e', 't', 't', 0};
    SX_HSTRING h1 = nullptr;
    SX_HSTRING h2 = nullptr;
    OS_REQUIRE(WindowsCreateString(raw, 11, &h1) == 0);
    OS_REQUIRE(WindowsGetStringLen(h1) == 11);
    OS_REQUIRE(WindowsDuplicateString(h1, &h2) == 0);

    int32_t cmp = -1;
    OS_REQUIRE(WindowsCompareStringOrdinal(h1, h2, &cmp) == 0);
    OS_REQUIRE(cmp == 0);

    OS_REQUIRE(WindowsDeleteString(h2) == 0);
    OS_REQUIRE(WindowsDeleteString(h1) == 0);

    // Activate built-in Windows.System.Profile.AnalyticsInfo WinRT class
    const uint16_t cls[] = {
        'W','i','n','d','o','w','s','.','S','y','s','t','e','m','.',
        'P','r','o','f','i','l','e','.','A','n','a','l','y','t','i','c','s','I','n','f','o', 0
    };
    SX_HSTRING_HEADER hdr{};
    SX_HSTRING hcls = nullptr;
    OS_REQUIRE(WindowsCreateStringReference(cls, 36, &hdr, &hcls) == 0);
    void* instance = nullptr;
    OS_REQUIRE(RoActivateInstance(hcls, &instance) == 0);
    OS_REQUIRE(instance != nullptr);

    RoUninitialize();
    std::printf("[PASS] test_winrt_xs (HSTRING + AnalyticsInfo WinRT activation verified)\n");
    return 0;
}
