// controllers through libnx's HID: the handheld Joy-Cons or a pair/Pro Controller as player 1, players 2-4 on their
// own pads. The face buttons by position by default, as an Xbox pad lies under the hand (MVP throws to the bases by
// the diamond they make): bottom B is the Xbox A, right A is B, left Y is X, top X is Y. buttons_by_name = true in the
// settings maps them by letter instead. L and R are the triggers, which the game shows as L and R; ZL and ZR the
// white and black buttons; + is Start and - is Back. A lone Joy-Con, held sideways, is a pad of its own (ReadSingle):
// two players on the console's own pair. + and - together open the system's controllers screen to split or join them.
#include "core/input.h"

#include <switch.h>

#include <algorithm>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "host.h"
#include "menu.h"

namespace input {

namespace {

std::mutex g_lock;
PadState g_pads[4];
HidVibrationDeviceHandle g_vib[4][2];
bool g_vib_ok[4];
// a Joy-Con on its own has one motor, under its own style: [port][0 left, 1 right]
HidVibrationDeviceHandle g_vib_single[4][2];
bool g_vib_single_ok[4][2];
int g_single[4] = {-1, -1, -1, -1};  // the port's pad is a lone Joy-Con: 0 left, 1 right, -1 not
bool g_stopped = false;  // HID is about to close: no more calls into it

// A Joy-Con on its own, held sideways (the hold type is horizontal), rail to the player: its four buttons make the
// diamond, turned a quarter. The left one's arrows: Left at the bottom (Xbox A), Down on the right (B), Up on the
// left (X), Right on top (Y); the right one's: A at the bottom, X on the right, B on the left, Y on top. SL and SR,
// now the shoulders, are the white and black buttons; L/ZL or R/ZR, at the far end, the triggers; - or + is Start
// and the stick's click Back. HID does not turn the stick for this hold: Read turns it a quarter, as the diamond.
void ReadSingle(bool right, u64 b, Pad& p) {
    constexpr u64 kLeft[8] = {HidNpadButton_Left,   HidNpadButton_Down,   HidNpadButton_Up, HidNpadButton_Right,
                              HidNpadButton_LeftSR, HidNpadButton_LeftSL, HidNpadButton_ZL, HidNpadButton_L};
    constexpr u64 kRight[8] = {HidNpadButton_A,       HidNpadButton_X,       HidNpadButton_B, HidNpadButton_Y,
                               HidNpadButton_RightSR, HidNpadButton_RightSL, HidNpadButton_ZR, HidNpadButton_R};
    const u64* map = right ? kRight : kLeft;
    for (int i = 0; i < 8; i++) p.analog[i] = (b & map[i]) ? 0xFF : 0;
    if (b & (right ? HidNpadButton_Plus : HidNpadButton_Minus)) p.buttons |= 1u << 4;  // start
    if (b & (right ? HidNpadButton_StickR : HidNpadButton_StickL)) p.buttons |= 1u << 5;  // back
}

void Read(PadState& s, Pad& p, int port) {
    if (g_stopped) return;
    padUpdate(&s);
    g_single[port] = -1;
    if (!padIsConnected(&s)) return;
    p.connected = true;
    u64 b = padGetButtons(&s);
    const u32 style = padGetStyleSet(&s);
    if (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight) &&
        !(style & (HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual))) {
        const bool right = style & HidNpadStyleTag_NpadJoyRight;
        g_single[port] = right ? 1 : 0;
        ReadSingle(right, b, p);
        // upright axes, held sideways: the left one's stick up is the player's left, the right one's its right
        // (tested on the console: unturned, right crouched and up walked forward on the left one)
        HidAnalogStickState st = padGetStickPos(&s, right ? 1 : 0);
        const int x = std::clamp(st.x, -32767, 32767), y = std::clamp(st.y, -32767, 32767);
        p.lx = int16_t(right ? y : -y);
        p.ly = int16_t(right ? -x : x);
        return;
    }
    // dpad up/down/left/right, start, back, left thumb, right thumb
    constexpr u64 kButtons[8] = {HidNpadButton_Up,   HidNpadButton_Down,  HidNpadButton_Left,     HidNpadButton_Right,
                                 HidNpadButton_Plus, HidNpadButton_Minus, HidNpadButton_StickL, HidNpadButton_StickR};
    for (int i = 0; i < 8; i++)
        if (b & kButtons[i]) p.buttons |= uint16_t(1u << i);
    // A B X Y black white left trigger right trigger
    constexpr u64 kSlot[8] = {HidNpadButton_A,  HidNpadButton_B,  HidNpadButton_X, HidNpadButton_Y,
                              HidNpadButton_ZR, HidNpadButton_ZL, HidNpadButton_L, HidNpadButton_R};
    uint8_t slot[8];
    menu::ButtonMap(slot);
    for (int i = 0; i < 8; i++) p.analog[i] = (b & kSlot[slot[i] & 7]) ? 0xFF : 0;
    // libnx's sticks are +-32767 with up positive, as the console's
    HidAnalogStickState l = padGetStickPos(&s, 0), r = padGetStickPos(&s, 1);
    p.lx = int16_t(std::clamp(l.x, -32767, 32767));
    p.ly = int16_t(std::clamp(l.y, -32767, 32767));
    p.rx = int16_t(std::clamp(r.x, -32767, 32767));
    p.ry = int16_t(std::clamp(r.y, -32767, 32767));
}

}  // namespace

void Init() {
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pads[0]);  // player 1 and the handheld Joy-Cons
    padInitialize(&g_pads[1], HidNpadIdType_No2);
    padInitialize(&g_pads[2], HidNpadIdType_No3);
    padInitialize(&g_pads[3], HidNpadIdType_No4);
    const HidNpadIdType ids[4] = {HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4};
    for (int i = 0; i < 4; i++) {
        g_vib_ok[i] = R_SUCCEEDED(hidInitializeVibrationDevices(g_vib[i], 2, ids[i], HidNpadStyleTag_NpadFullKey));
        g_vib_single_ok[i][0] =
            R_SUCCEEDED(hidInitializeVibrationDevices(&g_vib_single[i][0], 1, ids[i], HidNpadStyleTag_NpadJoyLeft));
        g_vib_single_ok[i][1] =
            R_SUCCEEDED(hidInitializeVibrationDevices(&g_vib_single[i][1], 1, ids[i], HidNpadStyleTag_NpadJoyRight));
    }
    // a lone Joy-Con is a sideways pad (the console's own default is upright)
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
    LOGI("input: libnx HID, 4 ports, lone Joy-Cons sideways; + and - together for the port menu");
}

// the system's controllers screen, where each Joy-Con of a pair can become a player of its own (SL + SR on it) or two
// lone ones a pair again. sheroes-nx: from the port menu (+ and - open the menu now)
void ShowControllers() {
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_min = 1;
    arg.hdr.player_count_max = 4;
    Result rc = hidLaShowControllerSupport(nullptr, &arg);
    LOGI("input: controllers screen closed (0x%X)", rc);
}

Pad Poll(int port) {
    Pad p;
    if (port < 0 || port > 3) return p;
    {
        std::lock_guard lock(g_lock);
        Read(g_pads[port], p, port);
    }
    // outside the lock: the port menu (+ and -), whose controllers screen takes over until the player leaves it
    if (p.connected && !g_stopped) menu::OnPad(port, p);
    if (app::kGame.pad) app::kGame.pad(port, p);
    return p;
}

// the motors run until set again
void Rumble(int port, uint16_t left, uint16_t right) {
    if (app::kGame.rumble) app::kGame.rumble(port, left, right);
    if (port < 0 || port > 3) return;
    std::lock_guard lock(g_lock);
    if (g_stopped) return;
    if (const int side = g_single[port]; side >= 0) {
        if (!g_vib_single_ok[port][side]) return;
        const float amp = std::max(left, right) / 65535.0f;
        HidVibrationValue v = {amp, 160.0f, amp, 320.0f};
        hidSendVibrationValues(&g_vib_single[port][side], &v, 1);
        return;
    }
    if (!g_vib_ok[port]) return;
    HidVibrationValue v[2] = {};
    float amp[2] = {left / 65535.0f, right / 65535.0f};
    for (int i = 0; i < 2; i++) {
        v[i].amp_low = amp[i];
        v[i].freq_low = 160.0f;
        v[i].amp_high = amp[i];
        v[i].freq_high = 320.0f;
    }
    hidSendVibrationValues(g_vib[port], v, 2);
}

// the lock waits out a poll in progress, so nothing is inside HID when the caller goes on to close it
void Stop() {
    std::lock_guard lock(g_lock);
    g_stopped = true;
}

uint64_t NavButtons() {
    std::lock_guard lock(g_lock);
    return !g_stopped && padIsConnected(&g_pads[0]) ? padGetButtons(&g_pads[0]) : 0;
}

void NavStick(float& x, float& y) {
    std::lock_guard lock(g_lock);
    HidAnalogStickState l = padGetStickPos(&g_pads[0], 0);
    x = l.x / 32767.0f;
    y = l.y / 32767.0f;
}

}  // namespace input
