#pragma once
// Exact MIT ATG linear clear color matching Microsoft/Xbox-GDK-Samples Kits/ATGTK/ATGColors.h.
// Linear 0.052860655 converts to sRGB ~0.255 -> #414141 (RGB 65, 65, 65) in B8G8R8A8_UNORM_SRGB.
namespace ATG {
namespace ColorsLinear {
inline constexpr float Background[4] = {0.052860655f, 0.052860655f, 0.052860655f, 1.0f};
}  // namespace ColorsLinear
}  // namespace ATG
