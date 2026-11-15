#pragma once
// The port's own menu (+ and - together) and the FPS counter (menu.cpp)
#include <cstdint>
#include <vector>

#include "core/input.h"

namespace menu {

// a picture laid over the frame at x, y of the screen: BGRA, premultiplied by alpha
struct Picture {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<uint8_t> px;
};

void Init();  // after the settings load
bool IsOpen();
void ButtonMap(uint8_t out[8]);       // Xbox A B X Y black white LT RT: the Switch slot (A B X Y ZR ZL L R) of each
void OnPad(int port, input::Pad& p);  // each poll: the menu's input, the camera inversion
void Frame(double fps);               // the frame rate shown by the counter
int Overlays(int screen_w, int screen_h, Picture out[2]);  // render thread: what to lay over the frame now

}  // namespace menu
