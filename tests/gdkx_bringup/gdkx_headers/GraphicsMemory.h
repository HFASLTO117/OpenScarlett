#pragma once
// Clean-room MIT DirectXTK12 GraphicsMemory subset for untouched Xbox-GDK-Samples.
namespace DirectX {
class GraphicsMemory {
public:
    explicit GraphicsMemory(void*) noexcept {}
    void Commit(void*) noexcept {}
};
}  // namespace DirectX
