#pragma once
#include <cstdint>
#include <Windows.h>

// Clean-room MIT pix3.h importing real PIX exports from OpenScarlett's pixevt.dll.

#ifndef PIX_COLOR_DEFAULT
#define PIX_COLOR_DEFAULT 0u
#endif

extern "C" {
__declspec(dllimport) void WINAPI PIXBeginEventOnCpu(uint64_t color, const char* formatString);
__declspec(dllimport) void WINAPI PIXEndEventOnCpu(void);
__declspec(dllimport) void WINAPI PIXBeginEventOnCommandList(
    void* commandList, uint64_t color, const char* formatString);
__declspec(dllimport) void WINAPI PIXEndEventOnCommandList(void* commandList);
}

inline void PIXBeginEvent(uint64_t color, const wchar_t*, ...) {
    PIXBeginEventOnCpu(color, "PIX");
}
inline void PIXEndEvent() {
    PIXEndEventOnCpu();
}
inline void PIXBeginEvent(void* commandList, uint64_t color, const wchar_t*, ...) {
    PIXBeginEventOnCommandList(commandList, color, "PIX_CL");
}
inline void PIXEndEvent(void* commandList) {
    PIXEndEventOnCommandList(commandList);
}

struct PixScopedGuard {
    template <typename... Args>
    PixScopedGuard(uint64_t color, const wchar_t*, Args&&...) {
        PIXBeginEventOnCpu(color, "PIX_SCOPED");
    }
    ~PixScopedGuard() {
        PIXEndEventOnCpu();
    }
};

#define PIXScopedEvent(color, ...) PixScopedGuard _pix_guard_##__LINE__(color, __VA_ARGS__)
