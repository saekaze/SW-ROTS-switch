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

// ROTS: the movies' YUV to RGB conversion (CRI Sofdec, 004C0F80), natively. Movies fell further behind their sound
// on the Switch; sampled on the PC this one function is half of the decoder's busy time. The original is MMX: per
// pixel three lookups in a 16 bit table (Y at +0, U at +0x800, V at +0x1000, four lanes of 16 bit each), added with
// wrap, shifted right arithmetically by 6, saturated to bytes (packuswb) and stored as one 32 bit pixel. 4:2:0: each
// chroma pair covers 2x2 pixels; two output rows a pass, 8 chroma samples (16 pixels) a step, do-while loops as the
// original (cdecl: src planes struct, dst struct, table, second row offset, row pair advance).
//   src: +0 Y, +4 U, +8 V, +0xC Y pitch, +0x10 U pitch, +0x14 V pitch;  dst: +0 pixels, +4 width, +8 height
// ROTS_YUV=original keeps the translated code; ROTS_YUV=verify (the harness) runs both and logs any difference.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

X86_EXTERN_FUNC(__imp__sub_004C0F80);

namespace {
struct YuvWrite {
    uint32_t va, value;
};

template <class Store>
void YuvToRgb(uint32_t src, uint32_t dsts, uint32_t tab, uint32_t row1, uint32_t adv, Store&& store, int32_t& last_c) {
    using kernel::R32;
    const uint8_t* mem = static_cast<const uint8_t*>(kernel::HostPtr(0));
    uint32_t y0 = R32(src), u = R32(src + 4), v = R32(src + 8);
    const uint32_t py = R32(src + 0xC), pu = R32(src + 0x10), pv = R32(src + 0x14);
    uint32_t d0 = R32(dsts), d1 = d0 + row1;
    const int32_t w2 = int32_t(R32(dsts + 4)) / 2;
    int32_t h2 = int32_t(R32(dsts + 8)) / 2;
    uint32_t y1 = y0 + py;
    auto lanes = [&](uint32_t va, uint16_t out[4]) { std::memcpy(out, mem + va, 8); };
    auto pixel = [&](const uint16_t c[4], uint32_t ty) {
        uint16_t t[4];
        lanes(ty, t);
        uint32_t px = 0;
        for (int k = 0; k < 4; k++) {
            int32_t s = int16_t(uint16_t(c[k] + t[k])) >> 6;
            px |= uint32_t(s < 0 ? 0 : s > 255 ? 255 : s) << (8 * k);
        }
        return px;
    };
    do {
        int32_t c = 0;
        do {
            for (int j = 0; j < 8; j++) {
                uint16_t tu[4], tv[4], ch[4];
                lanes(tab + 0x800 + mem[u + uint32_t(c + j)] * 8u, tu);
                lanes(tab + 0x1000 + mem[v + uint32_t(c + j)] * 8u, tv);
                for (int k = 0; k < 4; k++) ch[k] = uint16_t(tu[k] + tv[k]);
                for (uint32_t p = 0; p < 2; p++) {
                    uint32_t x = 2u * uint32_t(c + j) + p;
                    store(d0 + x * 4, pixel(ch, tab + mem[y0 + x] * 8u));
                    store(d1 + x * 4, pixel(ch, tab + mem[y1 + x] * 8u));
                }
            }
            c += 8;
        } while (c < w2);
        last_c = c;
        y0 += 2 * py;
        y1 += 2 * py;
        v += pv;
        u += pu;
        d0 += adv;
        d1 += adv;
    } while (--h2 != 0);
}
}  // namespace

X86_FUNC(sub_004C0F80) {
    static const int mode = [] {
        const char* e = std::getenv("ROTS_YUV");
        return !e ? 0 : !std::strcmp(e, "original") ? 1 : !std::strcmp(e, "verify") ? 2 : 0;
    }();
    if (mode == 1) return __imp__sub_004C0F80(ctx, base);
    using kernel::R32;
    const uint32_t esp = ctx.esp;
    const uint32_t src = R32(esp + 4), dsts = R32(esp + 8), tab = R32(esp + 12), row1 = R32(esp + 16), adv = R32(esp + 20);
    int32_t last_c = 0;
    if (mode == 2) {
        // both, the original last (it writes guest memory and returns itself); then compare
        std::vector<YuvWrite> mine;
        mine.reserve(320000);
        auto t0 = std::chrono::steady_clock::now();
        YuvToRgb(src, dsts, tab, row1, adv, [&](uint32_t va, uint32_t px) { mine.push_back({va, px}); }, last_c);
        auto t1 = std::chrono::steady_clock::now();
        __imp__sub_004C0F80(ctx, base);
        auto t2 = std::chrono::steady_clock::now();
        static double ns_native = 0, ns_orig = 0;
        ns_native += std::chrono::duration<double, std::nano>(t1 - t0).count();
        ns_orig += std::chrono::duration<double, std::nano>(t2 - t1).count();
        static int calls = 0, bad_calls = 0;
        size_t bad = 0;
        for (const YuvWrite& m : mine) bad += R32(m.va) != m.value;
        calls++;
        if (bad) bad_calls++;
        if (bad || calls % 250 == 1)
            LOGR("yuv verify: call %d, %zu pixels, %zu differ (%d calls with differences so far); native %.2f ms, "
                 "translated %.2f ms a call", calls, mine.size(), bad, bad_calls, ns_native / calls / 1e6,
                 ns_orig / calls / 1e6);
        return;
    }
    kernel::W32(0x65C890, 0x60D594);  // 004C1B30, called first: keeps its argument in a global
    YuvToRgb(src, dsts, tab, row1, adv, [&](uint32_t va, uint32_t px) { kernel::W32(va, px); }, last_c);
    // as the original leaves them: eax the row counter (0), ecx the last column, edx the byte mask; ret (cdecl)
    ctx.eax = 0;
    ctx.ecx = uint32_t(last_c);
    ctx.edx = 0xFF;
    ctx.esp += 4;
}

// ROTS: B0011 at 0051D760, DirectSoundGetSampleTime: the library reads the APU's sample counter (0xFE80200C) once its
// own DirectSoundCreate ran, else answers 0. The HLE replaces DirectSoundCreate, so it was always 0, and the dialogue
// player (0x79C60) times each line by it. The mixer's 48 kHz count instead.
namespace xdk {
uint32_t DsSampleTime();
}
X86_FUNC(sub_0051D760) {
    ctx.eax = xdk::DsSampleTime();
    ctx.esp += 4;  // ret
}
