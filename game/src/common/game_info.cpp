#include "app/app.h"
#include "core/input.h"
#include "gpu/backend.h"

namespace xdk {
extern uint32_t g_music_pct, g_effects_pct;  // dsound.cpp
}

namespace sheroes {
// the Switch port menu's settings (switch/menu.cpp), kept in the settings file
uint32_t g_button_map = 0;  // 0: as buttons_by_name; else 8 nibbles, each a Switch button slot + 1
bool g_invert_x = false, g_invert_y = false, g_fps_counter = false;
uint32_t g_stick_response = 0;  // left stick left/right: 0 linear, 1 gentle, 2 gentler (Mark, 2026-11-13: Sonic felt touchy)
void ScriptPad(int port, input::Pad& pad);  // pad_script.cpp
}

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Star Wars Episode III: Revenge of the Sith",              // window and splash
    .short_title = L"Revenge of the Sith",        // message boxes
    .name = L"ROTS",                 // window classes, crash dumps, handover event
    .log = "rots",                   // log file prefix
    .settings = "rots-settings.toml",  // settings file beside the exe
    .splash = L"splash",                  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "ROTS"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"button_map", nullptr, &sheroes::g_button_map},
                {"invert_camera_x", &sheroes::g_invert_x, nullptr},
                {"invert_camera_y", &sheroes::g_invert_y, nullptr},
                {"fps_counter", &sheroes::g_fps_counter, nullptr},
                {"stick_response", nullptr, &sheroes::g_stick_response},
                {"music_volume", nullptr, &xdk::g_music_pct},
                {"effects_volume", nullptr, &xdk::g_effects_pct}},  // game settings kept in the settings file
    .load = nullptr,                      // reads more from the settings file's toml::table
    .save = nullptr,                      // writes more into it
    .init = nullptr,                      // after the settings load, before the window and boot
    .loaded = nullptr,                    // image in memory, before its entry point runs
    .window = nullptr,                    // the game window is up
    .message = nullptr,                   // window messages first; true takes one
    .ui = nullptr,                        // ImGui drawing each frame
    .pad = sheroes::ScriptPad,          // scripted test presses (pad_script.cpp), none unless asked for
    .rumble = nullptr,                    // changes a rumble before it is set
};
