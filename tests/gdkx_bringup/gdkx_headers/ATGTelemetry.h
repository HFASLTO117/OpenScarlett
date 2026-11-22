#pragma once
#include <Windows.h>
// Clean-room MIT ATGTelemetry.h no-op for untouched Xbox-GDK-Samples.
namespace ATG {
inline void SendLaunchTelemetry() noexcept {
    char buf[4]{};
    if (::GetEnvironmentVariableA("OPENSCARLETT_TRACE_ODS", buf, sizeof(buf)) == 1u && buf[0] == '1') {
        ::OutputDebugStringA("TRACE: ATG::SendLaunchTelemetry reached\n");
    }
}
}  // namespace ATG
