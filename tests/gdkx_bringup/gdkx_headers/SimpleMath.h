#pragma once
#include <DirectXMath.h>

// Clean-room MIT DirectXTK12 SimpleMath::Matrix subset for untouched Xbox-GDK-Samples.

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

}  // namespace SimpleMath
}  // namespace DirectX
