// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
//
// (street-nx's EA sound guard fix lived here; sheroes-nx has none yet.)
#include "kernel/kernel.h"
#include "core/log.h"

#include <atomic>

// ROTS: DirectSound exports the HLE does not cover yet, found by the warning at boot ("exports running their original
// code") and a crash in mission 1. These four take a stream or buffer the HLE made, step to this + 4 and write the 3D
// parameters into the library's own voice object through [obj + 0x10] + 0xB4, which a host-made object does not have:
// a segfault after a few minutes of play (sub_0051F17B from B009B). The HLE mixer has no cones or 3D rolloff for these,
// so they only answer S_OK. Their shapes, from 5849 code:
//   B0093 at 0051F8E3  (this, float, apply)        ret 0xC   -> 0051F647 -> 0051F0C3
//   B0094 at 0051F8FB  (this, float, apply)        ret 0xC   -> 0051F6A1
//   B009A at 0051F913  (this, x, y, z, apply)      ret 0x14  -> 0051F6FB
//   B009B at 0051F93C  (this, inside, outside, apply) ret 0x10 -> jmp 0051F766 -> 0051F17B
namespace {
void Once(std::atomic<bool>& seen, const char* what) {
    if (!seen.exchange(true)) LOGI("dsound: %s answered S_OK (3D parameter on a host stream)", what);
}
}  // namespace

X86_FUNC(sub_0051F8E3) {
    static std::atomic<bool> s;
    Once(s, "B0093");
    ::kernel::Return(ctx, 0, 3);
}
X86_FUNC(sub_0051F8FB) {
    static std::atomic<bool> s;
    Once(s, "B0094");
    ::kernel::Return(ctx, 0, 3);
}
X86_FUNC(sub_0051F913) {
    static std::atomic<bool> s;
    Once(s, "B009A");
    ::kernel::Return(ctx, 0, 5);
}
X86_FUNC(sub_0051F93C) {
    static std::atomic<bool> s;
    Once(s, "B009B");
    ::kernel::Return(ctx, 0, 4);
}

// B0080 at 0051EA12 (this, a, b, c) also reads the library's voice ([this + 0x24]); not seen called yet. Logged so a
// report shows it if it ever is.
X86_EXTERN_FUNC(__imp__sub_0051EA12);
X86_FUNC(sub_0051EA12) {
    static std::atomic<bool> s;
    if (!s.exchange(true)) LOGW("dsound: B0080 at 0051EA12 runs its original code");
    __imp__sub_0051EA12(ctx, base);
}
