// Where Horizon runs each thread (core_layout = true in the settings), as sa2-nx's core layout does: the title thread
// alone on core 0, the render thread on core 1, every other thread (guest threads, audio, the shader compiler) on cores
// 1 and 2, preferring 2. Without it every thread has mask 7 at the same priority, and the title thread, the frame's
// bottleneck, shares its core with whatever woke up. Each thread sets its own mask: setting another running thread's
// can wait for it to leave its core, and with busy loader threads hang (sa2-nx's emulator runs).
#include <switch.h>

#include "core/log.h"
#include "host.h"

// role: 0 the title thread (the one that presents), 1 the render thread, 2 any other
extern "C" void sheroes_place_thread(int role) {
    if (!host::CoreLayout()) return;
    static constexpr struct { s32 core; u32 mask; const char* name; } kPlaces[] = {
        {0, 0b001, "title"}, {1, 0b010, "render"}, {2, 0b110, "other"}};
    if (role < 0 || role > 2) return;
    const auto& p = kPlaces[role];
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, p.core, p.mask);
    if (role != 2 || R_FAILED(rc))
        LOGR("threads: %s thread to core %d (mask %X)%s", p.name, int(p.core), unsigned(p.mask),
             R_FAILED(rc) ? " refused" : "");
}
