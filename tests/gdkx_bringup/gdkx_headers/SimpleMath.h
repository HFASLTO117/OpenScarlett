#pragma once
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <Windows.h>

// Clean-room MIT DirectXTK12 SimpleMath subset for untouched Xbox-GDK-Samples.

namespace DirectX {
namespace SimpleMath {

struct Matrix : public XMFLOAT4X4 {
    Matrix() noexcept
        : XMFLOAT4X4(
              1.f, 0.f, 0.f, 0.f,
              0.f, 1.f, 0.f, 0.f,
              0.f, 0.f, 1.f, 0.f,
              0.f, 0.f, 0.f, 1.f) {}

    Matrix(const Matrix&) = default;
    Matrix& operator=(const Matrix&) = default;

    Matrix(CXMMATRIX M) noexcept { XMStoreFloat4x4(this, M); }

    Matrix& operator=(CXMMATRIX M) noexcept {
        XMStoreFloat4x4(this, M);
        return *this;
    }

    operator XMMATRIX() const noexcept { return XMLoadFloat4x4(this); }
};

struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float minDepth = 0.0f;
    float maxDepth = 1.0f;

    static RECT ComputeTitleSafeArea(UINT backBufferWidth, UINT backBufferHeight) noexcept {
        const LONG safeX = static_cast<LONG>((backBufferWidth + 19u) / 20u);
        const LONG safeY = static_cast<LONG>((backBufferHeight + 19u) / 20u);
        RECT r{};
        r.left = safeX;
        r.top = safeY;
        r.right = static_cast<LONG>(backBufferWidth) - safeX;
        r.bottom = static_cast<LONG>(backBufferHeight) - safeY;
        return r;
    }
};

}  // namespace SimpleMath
}  // namespace DirectX
