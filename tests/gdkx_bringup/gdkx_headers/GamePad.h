#pragma once
// Clean-room MIT DirectXTK12 GamePad subset for untouched Xbox-GDK-Samples.

namespace DirectX {

class GamePad {
public:
    static constexpr int c_MergedInput = -1;

    struct State {
        bool IsConnected() const noexcept { return false; }
        bool IsViewPressed() const noexcept { return false; }
    };

    class ButtonStateTracker {
    public:
        void Update(const State&) noexcept {}
        void Reset() noexcept {}
    };

    State GetState(int) noexcept { return {}; }
};

}  // namespace DirectX
