// The port's own menu: + and - together open and close it (the old xboxrecomp build's menu, src/sheroes_menu.c on
// main, brought over). While it is open the game sees no input. D-pad or left stick: up and down choose a row, left
// and right change it, A changes it too, B closes. Closing saves rots-settings.toml. The FPS counter and the menu
// are drawn on the CPU into small pictures the renderer lays over the frame (deko.cpp DrawFrame). Font: font8x8.h
// (public domain).
#include "menu.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "app/app.h"
#include "core/log.h"
#include "font8x8.h"
#include "host.h"
#include "kernel/kernel.h"

// the menu's settings, kept in the settings file through the game's options (game/src/common/game_info.cpp)
namespace sheroes {
extern uint32_t g_button_map;   // 0: by buttons_by_name; else 8 nibbles, each a Switch button slot + 1
extern bool g_invert_x, g_invert_y, g_fps_counter;
extern uint32_t g_stick_response;  // left stick left/right: 0 linear, 1 gentle, 2 gentler
}
namespace xdk {
extern uint32_t g_music_pct, g_effects_pct;  // dsound.cpp
}
namespace host {
void SetButtonsByName(bool on);  // main.cpp
void SetSmaa(bool on);           // main.cpp
}
namespace input {
void ShowControllers();  // input.cpp
uint64_t NavButtons();
}

namespace menu {

namespace {

// Xbox A B X Y black white left trigger right trigger, by the Switch button slot that fills each:
// slots are A B X Y ZR ZL L R
const char* const kSlotName[8] = {"A", "B", "X", "Y", "ZR", "ZL", "L", "R"};
const uint8_t kByPosition[8] = {1, 0, 3, 2, 4, 5, 6, 7};
const uint8_t kByName[8] = {0, 1, 2, 3, 4, 5, 6, 7};
const uint32_t kRenderPct[] = {50, 67, 75, 100};
const uint32_t kAniso[] = {0, 2, 4, 8, 16};
const uint32_t kVolume[] = {0, 25, 50, 75, 100, 125, 150, 200, 250, 300};
constexpr uint32_t kBoostMhz = 1785;
template <class T, size_t N>
constexpr int Count(const T (&)[N]) {
    return int(N);
}

std::mutex g_lock;
std::atomic<bool> g_open{false};
uint8_t g_map[8];  // the map in use

enum Row {
    R_TITLE_CTRL, R_LAYOUT, R_MAP0, R_MAP7 = R_MAP0 + 7, R_INV_X, R_INV_Y, R_STICK, R_CONTROLLERS,
    R_TITLE_DISP, R_SCALE, R_ANISO, R_SMAA, R_FPS, R_MUSIC, R_EFFECTS, R_BOOST,
    R_DEFAULTS, R_CLOSE, R_COUNT
};
int g_sel = R_LAYOUT;

const char* const kMapLabel[8] = {"  Xbox A (bottom)", "  Xbox B (right)", "  Xbox X (left)", "  Xbox Y (top)",
                                  "  Xbox Black",      "  Xbox White",     "  Xbox L trigger", "  Xbox R trigger"};

void MapFromSettings() {
    if (sheroes::g_button_map) {
        for (int k = 0; k < 8; k++) g_map[k] = uint8_t(std::clamp<int>(int((sheroes::g_button_map >> (k * 4)) & 15) - 1, 0, 7));
    } else {
        std::memcpy(g_map, host::ButtonsByName() ? kByName : kByPosition, 8);
    }
}

void MapToSettings() {
    if (!std::memcmp(g_map, kByPosition, 8) || !std::memcmp(g_map, kByName, 8)) {
        host::SetButtonsByName(!std::memcmp(g_map, kByName, 8));
        sheroes::g_button_map = 0;
        return;
    }
    uint32_t m = 0;
    for (int k = 0; k < 8; k++) m |= uint32_t(g_map[k] + 1) << (k * 4);
    sheroes::g_button_map = m;
}

int Layout() {  // 0 Xbox positions, 1 Switch labels, 2 custom
    return !std::memcmp(g_map, kByPosition, 8) ? 0 : !std::memcmp(g_map, kByName, 8) ? 1 : 2;
}

int IndexOf(const uint32_t* v, int n, uint32_t x) {
    int best = 0;
    for (int i = 0; i < n; i++)
        if (v[i] <= x) best = i;
    return best;
}

bool Selectable(int r) { return r != R_TITLE_CTRL && r != R_TITLE_DISP; }

int Wrap(int v, int d, int n) { return ((v + d) % n + n) % n; }

void Boost(bool on) {
    host::SetClockWanted("cpu", on ? kBoostMhz : 0);
    if (!on) host::RestoreClocks();
    host::ApplyClocks();
}

void RowText(int r, const char*& label, char* val, size_t vn, const char*& note) {
    static const char* const layouts[] = {"Xbox positions", "Switch labels", "Custom"};
    const app::Settings& s = app::Get();
    label = "";
    note = "";
    val[0] = 0;
    if (r >= R_MAP0 && r <= R_MAP7) {
        label = kMapLabel[r - R_MAP0];
        std::snprintf(val, vn, "%s", kSlotName[g_map[r - R_MAP0]]);
        note = "The Switch button for this Xbox one";
        return;
    }
    switch (r) {
        case R_TITLE_CTRL: label = "CONTROLS"; break;
        case R_LAYOUT:
            label = "Button layout";
            std::snprintf(val, vn, "%s", layouts[Layout()]);
            note = Layout() == 0 ? "Buttons where the Xbox had them" : Layout() == 1 ? "Switch A is Xbox A, and so on" : "Set below";
            break;
        case R_INV_X:
            label = "Invert camera L/R";
            std::snprintf(val, vn, "%s", sheroes::g_invert_x ? "On" : "Off");
            note = "Right stick, left and right";
            break;
        case R_INV_Y:
            label = "Invert camera U/D";
            std::snprintf(val, vn, "%s", sheroes::g_invert_y ? "On" : "Off");
            note = "Right stick, up and down";
            break;
        case R_STICK: {
            static const char* const kNames[3] = {"Linear", "Gentle", "Gentler"};
            label = "Steering response";
            std::snprintf(val, vn, "%s", kNames[std::min<uint32_t>(sheroes::g_stick_response, 2)]);
            note = "Finer slight left and right turns";
            break;
        }
        case R_CONTROLLERS:
            label = "Controllers screen";
            note = "Press A: pair or split Joy-Cons";
            break;
        case R_TITLE_DISP: label = "DISPLAY AND SOUND"; break;
        case R_SCALE:
            label = "Resolution";
            std::snprintf(val, vn, "%u%%", s.render_percent);
            note = "Of the screen's height. Lower is faster";
            break;
        case R_ANISO:
            label = "Texture filtering";
            if (s.aniso) std::snprintf(val, vn, "%ux", s.aniso);
            else std::snprintf(val, vn, "Off");
            note = "Sharper floors at an angle";
            break;
        case R_SMAA:
            label = "Smoothing (SMAA)";
            std::snprintf(val, vn, "%s", host::Smaa() ? "On" : "Off");
            note = "Softens jagged edges. Costs GPU time";
            break;
        case R_FPS:
            label = "FPS counter";
            std::snprintf(val, vn, "%s", sheroes::g_fps_counter ? "On" : "Off");
            note = "Top left corner";
            break;
        case R_MUSIC:
            label = "Music volume";
            std::snprintf(val, vn, "%u%%", xdk::g_music_pct);
            note = "Stereo sounds: the music";
            break;
        case R_EFFECTS:
            label = "Effects volume";
            std::snprintf(val, vn, "%u%%", xdk::g_effects_pct);
            note = "Mono sounds: effects and voices";
            break;
        case R_BOOST:
            label = "CPU boost (1785 MHz)";
            std::snprintf(val, vn, "%s", host::ClockWanted("cpu") ? "On" : "Off");
            note = "Full CPU speed";
            break;
        case R_DEFAULTS:
            label = "Reset to defaults";
            note = "Press A";
            break;
        case R_CLOSE:
            label = "Save and close";
            note = "Press A (or B, or + and -)";
            break;
    }
}

void SetSmaa(bool on) { host::SetSmaa(on); }

void Change(int r, int d) {
    app::Settings& s = app::Get();
    if (r >= R_MAP0 && r <= R_MAP7) {
        g_map[r - R_MAP0] = uint8_t(Wrap(g_map[r - R_MAP0], d, 8));
        MapToSettings();
        return;
    }
    switch (r) {
        case R_LAYOUT:
            std::memcpy(g_map, Wrap(Layout() == 2 ? 1 : Layout(), d, 2) ? kByName : kByPosition, 8);
            MapToSettings();
            break;
        case R_INV_X: sheroes::g_invert_x = !sheroes::g_invert_x; break;
        case R_INV_Y: sheroes::g_invert_y = !sheroes::g_invert_y; break;
        case R_STICK: sheroes::g_stick_response = uint32_t(Wrap(int(std::min<uint32_t>(sheroes::g_stick_response, 2)), d, 3)); break;
        case R_SCALE: s.render_percent = kRenderPct[Wrap(IndexOf(kRenderPct, Count(kRenderPct), s.render_percent), d, Count(kRenderPct))]; break;
        case R_ANISO: s.aniso = kAniso[Wrap(IndexOf(kAniso, Count(kAniso), s.aniso), d, Count(kAniso))]; break;
        case R_SMAA: SetSmaa(!host::Smaa()); break;
        case R_FPS: sheroes::g_fps_counter = !sheroes::g_fps_counter; break;
        case R_MUSIC: xdk::g_music_pct = kVolume[Wrap(IndexOf(kVolume, Count(kVolume), xdk::g_music_pct), d, Count(kVolume))]; break;
        case R_EFFECTS: xdk::g_effects_pct = kVolume[Wrap(IndexOf(kVolume, Count(kVolume), xdk::g_effects_pct), d, Count(kVolume))]; break;
        case R_BOOST: Boost(!host::ClockWanted("cpu")); break;
    }
}

void Defaults() {
    app::Settings& s = app::Get();
    std::memcpy(g_map, kByPosition, 8);
    MapToSettings();
    sheroes::g_invert_x = sheroes::g_invert_y = false;
    sheroes::g_stick_response = 0;
    sheroes::g_fps_counter = false;
    s.render_percent = 100;
    s.aniso = 2;
    SetSmaa(false);
    xdk::g_music_pct = xdk::g_effects_pct = 100;
    if (host::ClockWanted("cpu")) Boost(false);
}

void Close() {
    g_open = false;
    app::Save();
    LOGR("menu: closed, settings saved");
}

void Select(int d) {
    int r = g_sel;
    do r = Wrap(r, d, R_COUNT);
    while (!Selectable(r));
    g_sel = r;
}

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void Input(uint64_t b, const input::Pad& p) {
    enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_A = 16, K_B = 32 };
    static unsigned held = 0;
    static uint64_t next_repeat = 0;
    unsigned k = 0;
    uint64_t t = NowMs();
    if ((b & HidNpadButton_Up) || p.ly > 16000) k |= K_UP;
    if ((b & HidNpadButton_Down) || p.ly < -16000) k |= K_DOWN;
    if ((b & HidNpadButton_Left) || p.lx < -16000) k |= K_LEFT;
    if ((b & HidNpadButton_Right) || p.lx > 16000) k |= K_RIGHT;
    if (b & HidNpadButton_A) k |= K_A;
    if (b & HidNpadButton_B) k |= K_B;
    unsigned edge = k & ~held;
    const unsigned dirs = K_UP | K_DOWN | K_LEFT | K_RIGHT;
    if (k & dirs) {
        if (edge & dirs) next_repeat = t + 350;
        else if (t >= next_repeat) {
            edge |= k & dirs;
            next_repeat = t + 110;
        }
    }
    held = k;
    if (edge & K_UP) Select(-1);
    if (edge & K_DOWN) Select(1);
    if (edge & K_LEFT) Change(g_sel, -1);
    if (edge & K_RIGHT) Change(g_sel, 1);
    if (edge & K_A) {
        if (g_sel == R_CLOSE) Close();
        else if (g_sel == R_DEFAULTS) Defaults();
        else if (g_sel == R_CONTROLLERS) input::ShowControllers();
        else Change(g_sel, 1);
    }
    if (edge & K_B) Close();
}

// ---- drawing

constexpr int kCols = 40;
constexpr int kRows = R_COUNT + 2;  // title, the rows, the note
constexpr int kLine = 9;            // font rows per line
// BGRA, premultiplied by alpha (the renderer blends one, one minus source alpha)
constexpr uint32_t Px(uint32_t argb) {
    uint32_t a = argb >> 24, r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    r = r * a / 255, g = g * a / 255, b = b * a / 255;
    return a << 24 | r << 16 | g << 8 | b;
}
constexpr uint32_t C_BG = Px(0xF0100C10u), C_BORDER = Px(0xFF8C1C1Cu), C_SEL = Px(0xFF3C1616u),
                   C_TEXT = Px(0xFFC8C8C8u), C_HEAD = Px(0xFFD04040u), C_TITLE = Px(0xFFE8D8A0u),
                   C_VALUE = Px(0xFFE8D8A0u), C_NOTE = Px(0xFF909090u), C_WHITE = Px(0xFFFFFFFFu);

struct Canvas {
    uint32_t* px;
    int w, h, fx, fy;
    void Fill(int x, int y, int fw, int fh, uint32_t c) {
        for (int j = std::max(y, 0); j < y + fh && j < h; j++)
            for (int i = std::max(x, 0); i < x + fw && i < w; i++) px[j * w + i] = c;
    }
    void Text(int x, int y, const char* s, uint32_t c) {
        for (; *s; s++, x += 8 * fx) {
            const unsigned char* g = sheroes_font8x8[uint8_t(*s) & 0x7F];
            for (int r = 0; r < 8; r++)
                for (int b = 0; b < 8; b++)
                    if (g[r] & (1u << b)) Fill(x + b * fx, y + r * fy, fx, fy, c);
        }
    }
};

void DrawMenu(Canvas& c) {
    int lh = kLine * c.fy, pad = 2 * c.fx, x0 = pad + 4 * c.fx, right = c.w - pad - 4 * c.fx;
    const char* note = "";
    c.Fill(0, 0, c.w, c.h, C_BG);
    c.Fill(0, 0, c.w, c.fy, C_BORDER);
    c.Fill(0, c.h - c.fy, c.w, c.fy, C_BORDER);
    c.Fill(0, 0, c.fx, c.h, C_BORDER);
    c.Fill(c.w - c.fx, 0, c.fx, c.h, C_BORDER);
    int y = pad + c.fy;
    c.Text(x0, y, "SONIC HEROES: PORT MENU", C_TITLE);
    y += lh;
    for (int r = 0; r < R_COUNT; r++, y += lh) {
        const char *label, *rn;
        char val[32];
        RowText(r, label, val, sizeof(val), rn);
        if (r == g_sel) {
            c.Fill(pad, y - (c.fy + 1) / 2, c.w - 2 * pad, lh, C_SEL);
            note = rn;
        }
        c.Text(x0, y, label, !Selectable(r) ? C_HEAD : r == g_sel ? C_WHITE : C_TEXT);
        if (val[0]) {
            char b[40];
            std::snprintf(b, sizeof(b), r == g_sel ? "< %s >" : "%s", val);
            c.Text(right - int(std::strlen(b)) * 8 * c.fx, y, b, C_VALUE);
        }
    }
    c.Text(x0, y, note, C_NOTE);
}

std::atomic<int> g_fps_shown{0};

}  // namespace

void Init() {
    std::lock_guard lock(g_lock);
    MapFromSettings();
    if (host::ClockWanted("cpu")) LOGR("menu: CPU boost on (%u MHz)", host::ClockWanted("cpu"));
}

bool IsOpen() { return g_open; }

void ButtonMap(uint8_t out[8]) {
    std::lock_guard lock(g_lock);
    std::memcpy(out, g_map, 8);
}

void OnPad(int port, input::Pad& p) {
    if (port == 0) {
        static bool combo_held = false, quiet = false;
        uint64_t b = input::NavButtons();
        bool combo = (b & HidNpadButton_Plus) && (b & HidNpadButton_Minus);
        {
            std::lock_guard lock(g_lock);
            if (combo && !combo_held) {
                if (g_open) Close();
                else {
                    MapFromSettings();
                    g_open = true;
                    LOGR("menu: open");
                }
                quiet = true;
            } else if (g_open && !combo) {
                Input(b, p);
                if (!g_open) quiet = true;
            }
        }
        combo_held = combo;
        // ROTS: + alone during a movie skips it (testing; the game itself lets few of them be skipped)
        static bool plus_held = false;
        bool plus = (b & HidNpadButton_Plus) && !(b & HidNpadButton_Minus);
        if (plus && !plus_held && !g_open) {
            uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
            uint64_t movie = kernel::MovieReadMs();
            if (movie && now - movie < 3000 && kernel::SkipMovie()) LOGR("menu: movie skipped with +");
        }
        plus_held = plus;
        // after the menu: nothing until every button is let go, so the press that closed it does not reach the game
        constexpr uint64_t kAny = HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_L |
                                  HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Plus |
                                  HidNpadButton_Minus | HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left |
                                  HidNpadButton_Right | HidNpadButton_StickL | HidNpadButton_StickR;
        if (quiet && !g_open && !(b & kAny)) quiet = false;
        if (g_open || quiet) {
            const bool connected = p.connected;
            p = input::Pad{};
            p.connected = connected;
            return;
        }
    }
    // steering: a curve on the left stick's left/right only, so a slight sideways push while running gives a smaller
    // turn; up/down and a full sideways push are unchanged (Mark: slight left and right at full speed too twitchy)
    if (uint32_t curve = std::min<uint32_t>(sheroes::g_stick_response, 2)) {
        float x = std::clamp(p.lx / 32767.0f, -1.0f, 1.0f);
        x = std::copysign(std::pow(std::fabs(x), curve == 1 ? 1.5f : 2.0f), x);
        p.lx = int16_t(x * 32767.0f);
    }
    if (sheroes::g_invert_x) p.rx = int16_t(p.rx == -32768 ? 32767 : -p.rx);
    if (sheroes::g_invert_y) p.ry = int16_t(p.ry == -32768 ? 32767 : -p.ry);
}

void Frame(double fps) { g_fps_shown = int(fps + 0.5); }

int Overlays(int sw, int sh, Picture out[2]) {
    int n = 0;
    // 8x8 glyphs at 16x16 of a 640x480 picture, scaled to the screen
    int fx = std::max(1, int(2.0 * sh / 480 + 0.5)), fy = fx;
    while (fx > 1 && kCols * 8 * fx + 4 * fx > sw) fx--;
    while (fy > 1 && kRows * kLine * fy + 4 * fy > sh) fy--;
    fx = fy = std::min(fx, fy);
    std::lock_guard lock(g_lock);
    if (g_open) {
        Picture& p = out[n++];
        p.w = kCols * 8 * fx + 4 * fx;
        p.h = kRows * kLine * fy + 4 * fy;
        p.x = (sw - p.w) / 2;
        p.y = (sh - p.h) / 2;
        p.px.assign(size_t(p.w) * p.h * 4, 0);
        Canvas c{reinterpret_cast<uint32_t*>(p.px.data()), p.w, p.h, fx, fy};
        DrawMenu(c);
    }
    if (sheroes::g_fps_counter) {
        int hx = std::max(1, fx / 2), hy = hx;
        Picture& p = out[n++];
        p.w = 9 * 8 * hx;
        p.h = 10 * hy;
        p.x = 8 * hx;
        p.y = 4 * hy;
        p.px.assign(size_t(p.w) * p.h * 4, 0);
        Canvas c{reinterpret_cast<uint32_t*>(p.px.data()), p.w, p.h, hx, hy};
        c.Fill(0, 0, c.w, c.h, Px(0xC0000000u));
        char b[16];
        std::snprintf(b, sizeof(b), "%3d FPS", g_fps_shown.load());
        c.Text(hx, hy, b, Px(0xFF60FF60u));
    }
    return n;
}

}  // namespace menu
