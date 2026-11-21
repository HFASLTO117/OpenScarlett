#define OPENSCARLETT_BUILDING_GAMEINPUT 1
#include "openscarlett/input/gameinput_abi.h"
#include "openscarlett/core/telemetry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace {

constexpr const char* kModName = "GameInput.dll";

inline int32_t RecordHr(const char* sym, int32_t hr, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName,
        sym,
        hr >= 0 ? openscarlett::CallOutcome::Ok : openscarlett::CallOutcome::Error,
        hr,
        slot);
    return hr;
}

inline void RecordOk(const char* sym, int32_t slot = -1) {
    openscarlett::CallTelemetry::Global().Record(
        kModName, sym, openscarlett::CallOutcome::Ok, 0, slot);
}

bool GuidEquals(const GameInputGuid* a, const GameInputGuid& b) {
    return a && std::memcmp(a, &b, sizeof(GameInputGuid)) == 0;
}

uint64_t QueryTimestampMicros() {
#if defined(_WIN32)
    static LARGE_INTEGER s_freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (s_freq.QuadPart > 0) {
        return static_cast<uint64_t>((now.QuadPart * 1000000LL) / s_freq.QuadPart);
    }
#endif
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ===========================================================================
// Dynamic XInput Loader (xinput1_4.dll -> xinput9_1_0.dll fallback)
// ===========================================================================
struct OsXInputGamepad {
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};
static_assert(sizeof(OsXInputGamepad) == 12, "XINPUT_GAMEPAD ABI mismatch");

struct OsXInputState {
    uint32_t dwPacketNumber;
    OsXInputGamepad Gamepad;
};
static_assert(sizeof(OsXInputState) == 16, "XINPUT_STATE ABI mismatch");

struct OsXInputVibration {
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};
static_assert(sizeof(OsXInputVibration) == 4, "XINPUT_VIBRATION ABI mismatch");

constexpr uint16_t kXiDpadUp = 0x0001u;
constexpr uint16_t kXiDpadDown = 0x0002u;
constexpr uint16_t kXiDpadLeft = 0x0004u;
constexpr uint16_t kXiDpadRight = 0x0008u;
constexpr uint16_t kXiStart = 0x0010u;
constexpr uint16_t kXiBack = 0x0020u;
constexpr uint16_t kXiLeftThumb = 0x0040u;
constexpr uint16_t kXiRightThumb = 0x0080u;
constexpr uint16_t kXiLeftShoulder = 0x0100u;
constexpr uint16_t kXiRightShoulder = 0x0200u;
constexpr uint16_t kXiA = 0x1000u;
constexpr uint16_t kXiB = 0x2000u;
constexpr uint16_t kXiX = 0x4000u;
constexpr uint16_t kXiY = 0x8000u;

struct XInputApi {
#if defined(_WIN32)
    using PfnGetState = DWORD(WINAPI*)(DWORD, OsXInputState*);
    using PfnSetState = DWORD(WINAPI*)(DWORD, OsXInputVibration*);

    HMODULE module = nullptr;
    PfnGetState get_state = nullptr;
    PfnSetState set_state = nullptr;
#endif
};

const XInputApi& GetXInput() {
    static XInputApi api = []() {
        XInputApi a{};
#if defined(_WIN32)
        a.module = LoadLibraryA("xinput1_4.dll");
        if (!a.module) {
            a.module = LoadLibraryA("xinput9_1_0.dll");
        }
        if (a.module) {
            a.get_state = reinterpret_cast<XInputApi::PfnGetState>(
                GetProcAddress(a.module, "XInputGetState"));
            a.set_state = reinterpret_cast<XInputApi::PfnSetState>(
                GetProcAddress(a.module, "XInputSetState"));
        }
#endif
        return a;
    }();
    return api;
}

float NormalizeStick(int16_t value, int16_t deadzone) {
    const int32_t v = static_cast<int32_t>(value);
    if (v > deadzone) {
        return std::clamp(
            static_cast<float>(v - deadzone) / static_cast<float>(32767 - deadzone), 0.0f, 1.0f);
    }
    if (v < -deadzone) {
        return std::clamp(
            static_cast<float>(v + deadzone) / static_cast<float>(32768 - deadzone), -1.0f, 0.0f);
    }
    return 0.0f;
}

float NormalizeTrigger(uint8_t value, uint8_t threshold) {
    if (value <= threshold) return 0.0f;
    return std::clamp(
        static_cast<float>(value - threshold) / static_cast<float>(255 - threshold), 0.0f, 1.0f);
}

void PollGamepadAndKeyboard(
    uint32_t preferred_slot,
    GameInputGamepadState& out_gamepad,
    std::vector<GameInputKeyState>& out_keys,
    uint32_t& out_active_slot) {
    out_gamepad = GameInputGamepadState{};
    out_keys.clear();
    out_active_slot = preferred_slot;

#if defined(_WIN32)
    const XInputApi& xi = GetXInput();
    if (xi.get_state) {
        OsXInputState st{};
        bool found = false;
        if (xi.get_state(preferred_slot, &st) == ERROR_SUCCESS) {
            found = true;
            out_active_slot = preferred_slot;
        } else {
            for (DWORD slot = 0; slot < 4; ++slot) {
                if (xi.get_state(slot, &st) == ERROR_SUCCESS) {
                    found = true;
                    out_active_slot = static_cast<uint32_t>(slot);
                    break;
                }
            }
        }
        if (found) {
            const uint16_t b = st.Gamepad.wButtons;
            uint32_t gb = GameInputGamepadNone;
            if (b & kXiDpadUp) gb |= GameInputGamepadDPadUp;
            if (b & kXiDpadDown) gb |= GameInputGamepadDPadDown;
            if (b & kXiDpadLeft) gb |= GameInputGamepadDPadLeft;
            if (b & kXiDpadRight) gb |= GameInputGamepadDPadRight;
            if (b & kXiStart) gb |= GameInputGamepadMenu;
            if (b & kXiBack) gb |= GameInputGamepadView;
            if (b & kXiLeftThumb) gb |= GameInputGamepadLeftThumbstick;
            if (b & kXiRightThumb) gb |= GameInputGamepadRightThumbstick;
            if (b & kXiLeftShoulder) gb |= GameInputGamepadLeftShoulder;
            if (b & kXiRightShoulder) gb |= GameInputGamepadRightShoulder;
            if (b & kXiA) gb |= GameInputGamepadA;
            if (b & kXiB) gb |= GameInputGamepadB;
            if (b & kXiX) gb |= GameInputGamepadX;
            if (b & kXiY) gb |= GameInputGamepadY;
            out_gamepad.buttons = gb;
            out_gamepad.leftTrigger = NormalizeTrigger(st.Gamepad.bLeftTrigger, 30);
            out_gamepad.rightTrigger = NormalizeTrigger(st.Gamepad.bRightTrigger, 30);
            out_gamepad.leftThumbstickX = NormalizeStick(st.Gamepad.sThumbLX, 7849);
            out_gamepad.leftThumbstickY = NormalizeStick(st.Gamepad.sThumbLY, 7849);
            out_gamepad.rightThumbstickX = NormalizeStick(st.Gamepad.sThumbRX, 8689);
            out_gamepad.rightThumbstickY = NormalizeStick(st.Gamepad.sThumbRY, 8689);
        }
    }

    // Merge Win32 keyboard fallback for playability without physical controller:
    // WASD -> left stick, IJKL -> right stick, Space/Enter -> A, Backspace/Ctrl -> B,
    // E -> X, Q -> Y, Shift -> LB/RB, Z/C -> LT/RT, Esc -> Menu, Tab -> View.
    auto is_down = [](int vk) -> bool {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };

    float kb_lx = 0.0f;
    float kb_ly = 0.0f;
    if (is_down('W')) kb_ly += 1.0f;
    if (is_down('S')) kb_ly -= 1.0f;
    if (is_down('D')) kb_lx += 1.0f;
    if (is_down('A')) kb_lx -= 1.0f;
    if (kb_lx != 0.0f) {
        out_gamepad.leftThumbstickX = std::clamp(out_gamepad.leftThumbstickX + kb_lx, -1.0f, 1.0f);
    }
    if (kb_ly != 0.0f) {
        out_gamepad.leftThumbstickY = std::clamp(out_gamepad.leftThumbstickY + kb_ly, -1.0f, 1.0f);
    }

    float kb_rx = 0.0f;
    float kb_ry = 0.0f;
    if (is_down('I')) kb_ry += 1.0f;
    if (is_down('K')) kb_ry -= 1.0f;
    if (is_down('L')) kb_rx += 1.0f;
    if (is_down('J')) kb_rx -= 1.0f;
    if (kb_rx != 0.0f) {
        out_gamepad.rightThumbstickX = std::clamp(out_gamepad.rightThumbstickX + kb_rx, -1.0f, 1.0f);
    }
    if (kb_ry != 0.0f) {
        out_gamepad.rightThumbstickY = std::clamp(out_gamepad.rightThumbstickY + kb_ry, -1.0f, 1.0f);
    }

    if (is_down(VK_UP)) out_gamepad.buttons |= GameInputGamepadDPadUp;
    if (is_down(VK_DOWN)) out_gamepad.buttons |= GameInputGamepadDPadDown;
    if (is_down(VK_LEFT)) out_gamepad.buttons |= GameInputGamepadDPadLeft;
    if (is_down(VK_RIGHT)) out_gamepad.buttons |= GameInputGamepadDPadRight;
    if (is_down(VK_SPACE) || is_down(VK_RETURN)) out_gamepad.buttons |= GameInputGamepadA;
    if (is_down(VK_BACK) || is_down(VK_LCONTROL) || is_down(VK_RCONTROL)) {
        out_gamepad.buttons |= GameInputGamepadB;
    }
    if (is_down('E')) out_gamepad.buttons |= GameInputGamepadX;
    if (is_down('Q')) out_gamepad.buttons |= GameInputGamepadY;
    if (is_down(VK_LSHIFT)) out_gamepad.buttons |= GameInputGamepadLeftShoulder;
    if (is_down(VK_RSHIFT)) out_gamepad.buttons |= GameInputGamepadRightShoulder;
    if (is_down(VK_ESCAPE)) out_gamepad.buttons |= GameInputGamepadMenu;
    if (is_down(VK_TAB)) out_gamepad.buttons |= GameInputGamepadView;
    if (is_down('Z')) out_gamepad.leftTrigger = std::max(out_gamepad.leftTrigger, 1.0f);
    if (is_down('C')) out_gamepad.rightTrigger = std::max(out_gamepad.rightTrigger, 1.0f);

    // Collect currently pressed Win32 keys for GetKeyState:
    // full A-Z, 0-9, F1-F12, arrows, modifiers, Enter, Space, Esc, Tab, Backspace.
    static constexpr uint8_t kTrackedVks[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
        'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
        VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6,
        VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
        VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
        VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
        VK_RETURN, VK_SPACE, VK_ESCAPE, VK_TAB, VK_BACK
    };
    for (uint8_t vk : kTrackedVks) {
        if (is_down(vk)) {
            GameInputKeyState ks{};
            ks.virtualKey = vk;
            ks.scanCode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
            ks.codePoint = (vk >= 0x20 && vk <= 0x7E) ? vk : 0u;
            ks.isDeadKey = false;
            out_keys.push_back(ks);
        }
    }
#else
    (void)preferred_slot;
#endif
}

// ===========================================================================
// COM Implementations: IGameInputDevice, IGameInputReading, IGameInput
// ===========================================================================
class GameInputDeviceImpl final : public IGameInputDevice {
public:
    explicit GameInputDeviceImpl(uint32_t xinput_slot) : xinput_slot_(xinput_slot) {
        std::memset(&info_, 0, sizeof(info_));
        info_.infoSize = sizeof(GameInputDeviceInfo);
        info_.vendorId = 0x045Eu;   // Microsoft
        info_.productId = 0x0B12u;  // Xbox Series X|S Controller
        info_.revisionNumber = 0x0500u;
        info_.deviceFamily = GameInputFamilyXboxOne;
        info_.supportedInput = GameInputKindGamepad | GameInputKindController |
                               GameInputKindKeyboard | GameInputKindUiNavigation;
        info_.supportedRumbleMotors = GameInputRumbleLowFrequency |
                                      GameInputRumbleHighFrequency |
                                      GameInputRumbleLeftTrigger |
                                      GameInputRumbleRightTrigger;
        info_.deviceId[0] = 0x4Fu;  // 'O'
        info_.deviceId[1] = 0x53u;  // 'S'
        info_.deviceId[2] = static_cast<uint8_t>(xinput_slot & 0xFFu);
        info_.displayName = "OpenScarlett Xbox Wireless Controller (XInput + Keyboard)";
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_GameInput_IUnknown) ||
            GuidEquals(riid, IID_IGameInputDevice)) {
            AddRef();
            *ppvObject = static_cast<IGameInputDevice*>(this);
            return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_S_OK, 0);
        }
        return RecordHr("IGameInputDevice::QueryInterface", GAMEINPUT_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() override {
        RecordOk("IGameInputDevice::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() override {
        RecordOk("IGameInputDevice::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    const GameInputDeviceInfo* OPENSCARLETT_GAMEINPUT_CALL GetDeviceInfo() override {
        RecordOk("IGameInputDevice::GetDeviceInfo", 3);
        return &info_;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetDeviceStatus() override {
        RecordOk("IGameInputDevice::GetDeviceStatus", 4);
        return GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled;
    }

    void OPENSCARLETT_GAMEINPUT_CALL SetRumbleState(const GameInputRumbleParams* params) override {
        RecordOk("IGameInputDevice::SetRumbleState", 5);
        GameInputRumbleParams clamped{};
        if (params) {
            clamped.lowFrequency = std::clamp(params->lowFrequency, 0.0f, 1.0f);
            clamped.highFrequency = std::clamp(params->highFrequency, 0.0f, 1.0f);
            clamped.leftTrigger = std::clamp(params->leftTrigger, 0.0f, 1.0f);
            clamped.rightTrigger = std::clamp(params->rightTrigger, 0.0f, 1.0f);
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            last_rumble_ = clamped;
        }
#if defined(_WIN32)
        const XInputApi& xi = GetXInput();
        if (xi.set_state) {
            const float low = std::max(clamped.lowFrequency, clamped.leftTrigger * 0.5f);
            const float high = std::max(clamped.highFrequency, clamped.rightTrigger * 0.5f);
            OsXInputVibration vib{};
            vib.wLeftMotorSpeed = static_cast<uint16_t>(std::lround(low * 65535.0f));
            vib.wRightMotorSpeed = static_cast<uint16_t>(std::lround(high * 65535.0f));
            xi.set_state(xinput_slot_.load(std::memory_order_relaxed), &vib);
        }
#endif
    }

    void SetActiveSlot(uint32_t slot) {
        xinput_slot_.store(slot, std::memory_order_relaxed);
    }

    uint32_t ActiveSlot() const {
        return xinput_slot_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> xinput_slot_{0};
    GameInputDeviceInfo info_{};
    std::mutex mutex_;
    GameInputRumbleParams last_rumble_{};
};

class GameInputReadingImpl final : public IGameInputReading {
public:
    GameInputReadingImpl(
        GameInputKind kind,
        uint64_t seq,
        uint64_t timestamp,
        GameInputDeviceImpl* device,
        const GameInputGamepadState& gamepad,
        std::vector<GameInputKeyState> keys)
        : kind_(kind),
          seq_(seq),
          timestamp_(timestamp),
          device_(device),
          gamepad_(gamepad),
          keys_(std::move(keys)) {
        if (device_) {
            device_->AddRef();
        }
    }

    ~GameInputReadingImpl() {
        if (device_) {
            device_->Release();
            device_ = nullptr;
        }
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_GameInput_IUnknown) ||
            GuidEquals(riid, IID_IGameInputReading)) {
            AddRef();
            *ppvObject = static_cast<IGameInputReading*>(this);
            return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_S_OK, 0);
        }
        return RecordHr("IGameInputReading::QueryInterface", GAMEINPUT_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() override {
        RecordOk("IGameInputReading::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() override {
        RecordOk("IGameInputReading::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    GameInputKind OPENSCARLETT_GAMEINPUT_CALL GetInputKind() override {
        RecordOk("IGameInputReading::GetInputKind", 3);
        return kind_;
    }

    uint64_t OPENSCARLETT_GAMEINPUT_CALL GetSequenceNumber(GameInputKind /*inputKind*/) override {
        RecordOk("IGameInputReading::GetSequenceNumber", 4);
        return seq_;
    }

    uint64_t OPENSCARLETT_GAMEINPUT_CALL GetTimestamp() override {
        RecordOk("IGameInputReading::GetTimestamp", 5);
        return timestamp_;
    }

    void OPENSCARLETT_GAMEINPUT_CALL GetDevice(IGameInputDevice** device) override {
        RecordOk("IGameInputReading::GetDevice", 6);
        if (!device) return;
        *device = device_;
        if (device_) {
            device_->AddRef();
        }
    }

    bool OPENSCARLETT_GAMEINPUT_CALL GetGamepadState(GameInputGamepadState* state) override {
        RecordOk("IGameInputReading::GetGamepadState", 7);
        if (!state) return false;
        *state = gamepad_;
        return (kind_ & GameInputKindGamepad) != 0;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyCount() override {
        RecordOk("IGameInputReading::GetKeyCount", 8);
        return static_cast<uint32_t>(keys_.size());
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL GetKeyState(
        uint32_t stateArrayCount, GameInputKeyState* stateArray) override {
        RecordOk("IGameInputReading::GetKeyState", 9);
        if (!stateArray || stateArrayCount == 0) return 0;
        const uint32_t count = std::min<uint32_t>(
            stateArrayCount, static_cast<uint32_t>(keys_.size()));
        if (count > 0) {
            std::memcpy(stateArray, keys_.data(), count * sizeof(GameInputKeyState));
        }
        return count;
    }

    uint64_t Sequence() const { return seq_; }
    GameInputKind KindRaw() const { return kind_; }
    IGameInputDevice* DevicePtr() const { return device_; }

private:
    std::atomic<uint32_t> ref_count_{1};
    GameInputKind kind_;
    uint64_t seq_;
    uint64_t timestamp_;
    GameInputDeviceImpl* device_;
    GameInputGamepadState gamepad_;
    std::vector<GameInputKeyState> keys_;
};

class GameInputImpl;

struct LiveGameInputRegistry {
    std::mutex mutex;
    std::unordered_set<GameInputImpl*> instances;
};

LiveGameInputRegistry& GetLiveGameInputRegistry() {
    static LiveGameInputRegistry reg;
    return reg;
}

class GameInputImpl final : public IGameInput {
public:
    GameInputImpl() {
        default_device_ = new GameInputDeviceImpl(0);
        auto& reg = GetLiveGameInputRegistry();
        std::lock_guard<std::mutex> lk(reg.mutex);
        reg.instances.insert(this);
    }

    ~GameInputImpl() {
        {
            auto& reg = GetLiveGameInputRegistry();
            std::lock_guard<std::mutex> lk(reg.mutex);
            reg.instances.erase(this);
        }
        for (auto* r : history_) {
            if (r) r->Release();
        }
        history_.clear();
        if (default_device_) {
            default_device_->Release();
            default_device_ = nullptr;
        }
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL QueryInterface(
        const GameInputGuid* riid, void** ppvObject) override {
        if (!ppvObject) return RecordHr("IGameInput::QueryInterface", GAMEINPUT_E_POINTER, 0);
        *ppvObject = nullptr;
        if (!riid) return RecordHr("IGameInput::QueryInterface", GAMEINPUT_E_INVALIDARG, 0);
        if (GuidEquals(riid, IID_OS_GameInput_IUnknown) ||
            GuidEquals(riid, IID_IGameInput)) {
            AddRef();
            *ppvObject = static_cast<IGameInput*>(this);
            return RecordHr("IGameInput::QueryInterface", GAMEINPUT_S_OK, 0);
        }
        return RecordHr("IGameInput::QueryInterface", GAMEINPUT_E_NOINTERFACE, 0);
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL AddRef() override {
        RecordOk("IGameInput::AddRef", 1);
        return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    uint32_t OPENSCARLETT_GAMEINPUT_CALL Release() override {
        RecordOk("IGameInput::Release", 2);
        const uint32_t rem = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (rem == 0) {
            delete this;
        }
        return rem;
    }

    uint64_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentTimestamp() override {
        RecordOk("IGameInput::GetCurrentTimestamp", 3);
        return QueryTimestampMicros();
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL GetCurrentReading(
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!reading) {
            return RecordHr("IGameInput::GetCurrentReading", GAMEINPUT_E_POINTER, 4);
        }
        *reading = nullptr;

        GameInputDeviceImpl* target_dev = device
                                              ? static_cast<GameInputDeviceImpl*>(device)
                                              : default_device_;

        GameInputGamepadState gp{};
        std::vector<GameInputKeyState> keys;
        const bool input_allowed = ShouldAllowInput();

        bool use_injected = false;
        if (input_allowed) {
            std::lock_guard<std::mutex> lk(mutex_);
            if (test_inject_enabled_) {
                gp = injected_gamepad_;
                keys = injected_keys_;
                use_injected = true;
            }
        }

        if (input_allowed && !use_injected) {
            uint32_t active_slot = target_dev->ActiveSlot();
            PollGamepadAndKeyboard(active_slot, gp, keys, active_slot);
            target_dev->SetActiveSlot(active_slot);
        }

        const GameInputKind resolved_kind =
            (inputKind == GameInputKindUnknown)
                ? static_cast<GameInputKind>(GameInputKindGamepad | GameInputKindKeyboard)
                : inputKind;

        std::lock_guard<std::mutex> lk(mutex_);
        const uint64_t seq = next_seq_++;
        const uint64_t raw_ts = QueryTimestampMicros();
        const uint64_t ts = (raw_ts >= last_ts_) ? raw_ts : last_ts_;
        last_ts_ = ts;

        auto* r = new (std::nothrow) GameInputReadingImpl(
            resolved_kind, seq, ts, target_dev, gp, std::move(keys));
        if (!r) {
            return RecordHr("IGameInput::GetCurrentReading", GAMEINPUT_E_OUTOFMEMORY, 4);
        }

        // Keep history copy (bounded to 64 readings).
        r->AddRef();
        history_.push_back(r);
        while (history_.size() > 64) {
            history_.front()->Release();
            history_.pop_front();
        }

        *reading = r;
        return RecordHr("IGameInput::GetCurrentReading", GAMEINPUT_S_OK, 4);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL GetNextReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!referenceReading || !reading) {
            return RecordHr("IGameInput::GetNextReading", GAMEINPUT_E_INVALIDARG, 5);
        }
        *reading = nullptr;
        const uint64_t ref_seq = static_cast<GameInputReadingImpl*>(referenceReading)->Sequence();
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto* r : history_) {
            if (r->Sequence() <= ref_seq) continue;
            if (inputKind != GameInputKindUnknown && (r->KindRaw() & inputKind) == 0) continue;
            if (device && r->DevicePtr() != device) continue;
            r->AddRef();
            *reading = r;
            return RecordHr("IGameInput::GetNextReading", GAMEINPUT_S_OK, 5);
        }
        return RecordHr("IGameInput::GetNextReading", GAMEINPUT_E_READING_NOT_FOUND, 5);
    }

    int32_t OPENSCARLETT_GAMEINPUT_CALL GetPreviousReading(
        IGameInputReading* referenceReading,
        GameInputKind inputKind,
        IGameInputDevice* device,
        IGameInputReading** reading) override {
        if (!referenceReading || !reading) {
            return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_E_INVALIDARG, 6);
        }
        *reading = nullptr;
        const uint64_t ref_seq = static_cast<GameInputReadingImpl*>(referenceReading)->Sequence();
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            auto* r = *it;
            if (r->Sequence() >= ref_seq) continue;
            if (inputKind != GameInputKindUnknown && (r->KindRaw() & inputKind) == 0) continue;
            if (device && r->DevicePtr() != device) continue;
            r->AddRef();
            *reading = r;
            return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_S_OK, 6);
        }
        return RecordHr("IGameInput::GetPreviousReading", GAMEINPUT_E_READING_NOT_FOUND, 6);
    }

    void OPENSCARLETT_GAMEINPUT_CALL SetFocusPolicy(uint32_t policy) override {
        RecordOk("IGameInput::SetFocusPolicy", 7);
        focus_policy_.store(policy, std::memory_order_release);
    }

    void SetTargetWindow(void* hwnd) {
        target_hwnd_.store(hwnd, std::memory_order_release);
    }

    void SetInjectedTestState(
        const GameInputGamepadState* gp,
        const GameInputKeyState* keys,
        uint32_t key_count,
        bool enable) {
        std::lock_guard<std::mutex> lk(mutex_);
        test_inject_enabled_ = enable;
        if (!enable) {
            injected_gamepad_ = GameInputGamepadState{};
            injected_keys_.clear();
            return;
        }
        injected_gamepad_ = gp ? *gp : GameInputGamepadState{};
        injected_keys_.clear();
        if (keys && key_count > 0u) {
            injected_keys_.assign(keys, keys + key_count);
        }
    }

private:
    bool ShouldAllowInput() const noexcept {
        const uint32_t policy = focus_policy_.load(std::memory_order_acquire);
        if ((policy & GameInputEnableBackgroundInput) != 0u) {
            return true;
        }
        const uint32_t fg_gate_mask =
            GameInputDisableBackgroundInput | GameInputExclusiveForegroundInput;
        if ((policy & fg_gate_mask) == 0u) {
            // GameInputDefaultFocusPolicy allows input cleanly in console/headless hosts.
            return true;
        }
#if defined(_WIN32)
        HWND fg = GetForegroundWindow();
        if (!fg) {
            return false;
        }
        HWND target = reinterpret_cast<HWND>(target_hwnd_.load(std::memory_order_acquire));
        if (target && IsWindow(target) != FALSE) {
            return (fg == target) ||
                   (IsChild(target, fg) != FALSE) ||
                   (GetAncestor(fg, GA_ROOTOWNER) == target);
        }
        DWORD fg_pid = 0;
        GetWindowThreadProcessId(fg, &fg_pid);
        return fg_pid == GetCurrentProcessId();
#else
        return true;
#endif
    }

    std::atomic<uint32_t> ref_count_{1};
    std::atomic<uint32_t> focus_policy_{GameInputDefaultFocusPolicy};
    std::atomic<void*> target_hwnd_{nullptr};
    GameInputDeviceImpl* default_device_ = nullptr;
    std::mutex mutex_;
    uint64_t next_seq_ = 1;
    uint64_t last_ts_ = 0;
    std::deque<GameInputReadingImpl*> history_;
    bool test_inject_enabled_ = false;
    GameInputGamepadState injected_gamepad_{};
    std::vector<GameInputKeyState> injected_keys_;
};

GameInputImpl* AcquireLiveGameInput(IGameInput* input) {
    if (!input) return nullptr;
    auto* candidate = static_cast<GameInputImpl*>(input);
    auto& reg = GetLiveGameInputRegistry();
    std::lock_guard<std::mutex> lk(reg.mutex);
    if (reg.instances.find(candidate) == reg.instances.end()) {
        return nullptr;
    }
    candidate->AddRef();
    return candidate;
}

}  // namespace

extern "C" {

int32_t OPENSCARLETT_GAMEINPUT_CALL GameInputCreate(IGameInput** gameInput) {
    if (!gameInput) {
        return RecordHr("GameInputCreate", GAMEINPUT_E_POINTER);
    }
    *gameInput = nullptr;
    auto* obj = new (std::nothrow) GameInputImpl();
    if (!obj) {
        return RecordHr("GameInputCreate", GAMEINPUT_E_OUTOFMEMORY);
    }
    *gameInput = obj;
    return RecordHr("GameInputCreate", GAMEINPUT_S_OK);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputSetTargetWindow(
    IGameInput* input, void* hwnd) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputSetTargetWindow", GAMEINPUT_E_INVALIDARG);
    }
    impl->SetTargetWindow(hwnd);
    impl->Release();
    return RecordHr("OpenScarlettGameInputSetTargetWindow", GAMEINPUT_S_OK);
}

int32_t OPENSCARLETT_GAMEINPUT_CALL OpenScarlettGameInputInjectTestState(
    IGameInput* input,
    const GameInputGamepadState* gp,
    const GameInputKeyState* keys,
    uint32_t key_count,
    int enable) {
    GameInputImpl* impl = AcquireLiveGameInput(input);
    if (!impl) {
        return RecordHr("OpenScarlettGameInputInjectTestState", GAMEINPUT_E_INVALIDARG);
    }
    if (enable != 0 && key_count > 0u && !keys) {
        impl->Release();
        return RecordHr("OpenScarlettGameInputInjectTestState", GAMEINPUT_E_INVALIDARG);
    }
    impl->SetInjectedTestState(gp, keys, key_count, enable != 0);
    impl->Release();
    return RecordHr("OpenScarlettGameInputInjectTestState", GAMEINPUT_S_OK);
}

}  // extern "C"
