#pragma once
#include <d3d12_xs.h>

#include "RenderTargetState.h"
#include "ResourceUploadBatch.h"

// Clean-room MIT DirectXTK12 SpriteBatch subset for untouched Xbox-GDK-Samples.
// Accepts HUD batching calls cleanly while letting the 3D scene render unimpeded.

namespace DirectX {

class SpriteBatchPipelineStateDescription {
public:
    explicit SpriteBatchPipelineStateDescription(const RenderTargetState& rtState) noexcept
        : renderTargetState(rtState) {}

    RenderTargetState renderTargetState;
};

class SpriteBatch {
public:
    SpriteBatch(
        ID3D12Device*,
        ResourceUploadBatch&,
        const SpriteBatchPipelineStateDescription&) noexcept
        : m_viewport{}, m_cmdList(nullptr) {}

    SpriteBatch(const SpriteBatch&) = delete;
    SpriteBatch& operator=(const SpriteBatch&) = delete;

    void SetViewport(const D3D12_VIEWPORT& viewport) noexcept { m_viewport = viewport; }

    void Begin(ID3D12GraphicsCommandList* commandList) noexcept { m_cmdList = commandList; }

    void End() noexcept { m_cmdList = nullptr; }

private:
    D3D12_VIEWPORT m_viewport;
    ID3D12GraphicsCommandList* m_cmdList;
};

}  // namespace DirectX
