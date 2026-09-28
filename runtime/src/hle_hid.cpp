// nn::hid at the API level: Npad state from an XInput controller or the keyboard. Vibration is accepted and dropped.
#include <string>
#include <vector>

#include <windows.h>
#include <xinput.h>

#include <nx/runtime.h>

namespace nx {

extern HWND g_window;

namespace {

// NpadButton bits.
enum : uint64_t {
    kA = 1ull << 0, kB = 1ull << 1, kX = 1ull << 2, kY = 1ull << 3, kStickL = 1ull << 4, kStickR = 1ull << 5,
    kL = 1ull << 6, kR = 1ull << 7, kZL = 1ull << 8, kZR = 1ull << 9, kPlus = 1ull << 10, kMinus = 1ull << 11,
    kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14, kDown = 1ull << 15,
    kStickLLeft = 1ull << 16, kStickLUp = 1ull << 17, kStickLRight = 1ull << 18, kStickLDown = 1ull << 19,
    kStickRLeft = 1ull << 20, kStickRUp = 1ull << 21, kStickRRight = 1ull << 22, kStickRDown = 1ull << 23,
};
constexpr uint32_t kStyleFullKey = 1u << 0;
constexpr int32_t kStickMax = 0x7FFF;

uint64_t g_sampling = 0;

bool key(int vk) { return g_window && GetForegroundWindow() == g_window && (GetAsyncKeyState(vk) & 0x8000); }

struct Pad {
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// Scripted input for automated runs: NX_INPUT="3300:A,3900:A,4200:Down" holds the button for 8 frames from the
// given presented frame.
struct Scripted {
    uint64_t frame;
    uint64_t buttons;
};

std::vector<Scripted> input_script() {
    std::vector<Scripted> v;
    const char* e = std::getenv("NX_INPUT");
    if (!e) return v;
    std::string s = e;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t comma = s.find(',', pos);
        std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string b = item.substr(colon + 1);
            uint64_t bits = b == "A" ? kA : b == "B" ? kB : b == "X" ? kX : b == "Y" ? kY : b == "Plus" ? kPlus :
                            b == "Minus" ? kMinus : b == "Up" ? kUp : b == "Down" ? kDown : b == "Left" ? kLeft :
                            b == "Right" ? kRight : b == "L" ? kL : b == "R" ? kR : 0;
            v.push_back({std::strtoull(item.c_str(), nullptr, 10), bits});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return v;
}

Pad read_pad() {
    Pad p;
    static const std::vector<Scripted> script = input_script();
    for (const auto& s : script)
        if (g_frame >= s.frame && g_frame < s.frame + 8) p.buttons |= s.buttons;
    XINPUT_STATE xs{};
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        const auto& g = xs.Gamepad;
        // Nintendo layout: A is the right face button, B the bottom one.
        if (g.wButtons & XINPUT_GAMEPAD_B) p.buttons |= kA;
        if (g.wButtons & XINPUT_GAMEPAD_A) p.buttons |= kB;
        if (g.wButtons & XINPUT_GAMEPAD_Y) p.buttons |= kX;
        if (g.wButtons & XINPUT_GAMEPAD_X) p.buttons |= kY;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) p.buttons |= kL;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) p.buttons |= kR;
        if (g.bLeftTrigger > 100) p.buttons |= kZL;
        if (g.bRightTrigger > 100) p.buttons |= kZR;
        if (g.wButtons & XINPUT_GAMEPAD_START) p.buttons |= kPlus;
        if (g.wButtons & XINPUT_GAMEPAD_BACK) p.buttons |= kMinus;
        if (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) p.buttons |= kStickL;
        if (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) p.buttons |= kStickR;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) p.buttons |= kLeft;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) p.buttons |= kUp;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) p.buttons |= kRight;
        if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) p.buttons |= kDown;
        auto dz = [](int v) { return (v > -7849 && v < 7849) ? 0 : std::max(-kStickMax, std::min(kStickMax, v)); };
        p.lx = dz(g.sThumbLX);
        p.ly = dz(g.sThumbLY);
        p.rx = dz(g.sThumbRX);
        p.ry = dz(g.sThumbRY);
    }
    // Keyboard: WASD / arrows move, Space A, Shift B (dash), J X, K Y, Q/E L/R, Enter +, Esc -.
    if (key('A') || key(VK_LEFT)) p.lx = -kStickMax;
    if (key('D') || key(VK_RIGHT)) p.lx = kStickMax;
    if (key('W') || key(VK_UP)) p.ly = kStickMax;
    if (key('S') || key(VK_DOWN)) p.ly = -kStickMax;
    if (key(VK_SPACE)) p.buttons |= kA;
    if (key(VK_SHIFT)) p.buttons |= kB;
    if (key('J')) p.buttons |= kX;
    if (key('K')) p.buttons |= kY;
    if (key('Q')) p.buttons |= kL;
    if (key('E')) p.buttons |= kR;
    if (key('Z')) p.buttons |= kZL;
    if (key('C')) p.buttons |= kZR;
    if (key(VK_RETURN)) p.buttons |= kPlus;
    if (key(VK_ESCAPE)) p.buttons |= kMinus;
    // Stick directions as buttons, like the hid sysmodule reports them.
    constexpr int32_t t = kStickMax / 2;
    if (p.lx < -t) p.buttons |= kStickLLeft;
    if (p.lx > t) p.buttons |= kStickLRight;
    if (p.ly > t) p.buttons |= kStickLUp;
    if (p.ly < -t) p.buttons |= kStickLDown;
    if (p.rx < -t) p.buttons |= kStickRLeft;
    if (p.rx > t) p.buttons |= kStickRRight;
    if (p.ry > t) p.buttons |= kStickRUp;
    if (p.ry < -t) p.buttons |= kStickRDown;
    return p;
}

// NpadFullKeyState / NpadJoyDualState / NpadHandheldState:
// {s64 sampling; u64 buttons; {s32 x, y} left; {s32 x, y} right; u32 attributes; pad}
void GetNpadState(CPU& c) {
    uint64_t out = c.x[0];
    uint32_t id = ld<uint32_t>(c.x[1]);
    std::memset(reinterpret_cast<void*>(out), 0, 0x28);
    st<int64_t>(out, int64_t(++g_sampling));
    if (id != 0) return;  // only player 1 is connected
    Pad p = read_pad();
    st<uint64_t>(out + 8, p.buttons);
    st<int32_t>(out + 16, p.lx);
    st<int32_t>(out + 20, p.ly);
    st<int32_t>(out + 24, p.rx);
    st<int32_t>(out + 28, p.ry);
    st<uint32_t>(out + 32, 1);  // IsConnected
}

void GetNpadStyleSet(CPU& c) { c.x[0] = ld<uint32_t>(c.x[0]) == 0 ? kStyleFullKey : 0; }

void BindNpadStyleSetUpdateEvent(CPU& c) {  // (const u32& id, SystemEventType*, EventClearMode)
    uint64_t create = find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    uint64_t ev = c.x[1], mode = c.x[2];
    c.x[0] = ev;
    c.x[1] = mode;
    c.x[2] = 0;
    call(c, create);
}

void GetVibrationDeviceHandles(CPU& c) { c.x[0] = 0; }  // no vibration devices
void ShowControllerSupport(CPU& c) {                     // (ControllerSupportResultInfo*, const Arg&)
    st<int8_t>(c.x[0], 1);        // player count
    st<uint32_t>(c.x[0] + 4, 0);  // selected npad id: No1
    c.x[0] = 0;
}
void noop(CPU& c) { c.x[0] = 0; }

}  // namespace

NX_HLE("_ZN2nn3hid14InitializeNpadEv", noop);
NX_HLE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", noop);
NX_HLE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", noop);
NX_HLE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", noop);
NX_HLE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", BindNpadStyleSetUpdateEvent);
NX_HLE("_ZN2nn3hid15GetNpadStyleSetERKj", GetNpadStyleSet);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", GetNpadState);
NX_HLE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", GetVibrationDeviceHandles);
NX_HLE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", noop);
NX_HLE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", ShowControllerSupport);

}  // namespace nx
