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

// ROTS: the dialogue player's per-frame poll (00078EB0, thiscall on its line slot): the slot's first field is the line's
// file media object (XFileCreateMediaObjectAsync, B00A9; vtable 0060B83C); the poll calls its method 9 (DoWork,
// 005270F8), then 00078DF0 hands its packets on through the same object. A slot left with a freed object (see the start
// hook below) is skipped instead of calling into freed memory, and logged: a backstop for any other such path.
X86_EXTERN_FUNC(__imp__sub_00078EB0);
X86_FUNC(sub_00078EB0) {
    const uint32_t line = ctx.ecx;
    const uint32_t media = line ? kernel::R32(line) : 0;
    const uint32_t vtable = media ? kernel::R32(media) : 0;
    if (vtable != 0x60B83Cu) {
        static int shown = 0;
        if (shown++ < 16)
            LOGR("dialogue: line %08X has no live media object (%08X, vtable %08X); its poll is skipped", line, media,
                 vtable);
        ctx.esp += 4;  // ret
        return;
    }
    __imp__sub_00078EB0(ctx, base);
}

// ROTS: the plain dialogue player's stop (00079390, vtable 00566878) gives back only the line's first stream (+D4);
// the start takes two for a 4-channel line and three for a 6-channel one (the channels byte at +11 of the sound's
// definition at +10; 00077652 fills +D8 and +DC). The other player's stop (00079430) gives back all three. A plain
// player playing such a line would keep a stream each time, till the pool of 12 ran out (the crash above). Given back
// here as 00079430 does, and logged.
X86_EXTERN_FUNC(__imp__sub_00079390);
X86_EXTERN_FUNC(sub_00078C10);
X86_FUNC(sub_00079390) {
    const uint32_t player = ctx.ecx;
    const uint32_t def = kernel::R32(player + 0x10);
    const uint32_t channels = def ? kernel::R16(def + 0x10) >> 8 : 2;  // the byte at +11
    const bool started = kernel::R32(player + 0xE0) != 0 && kernel::R32(player + 0x14) == 0;
    const uint32_t extra[2] = {kernel::R32(player + 0xD8), channels == 6 ? kernel::R32(player + 0xDC) : 0xFFFFFFFFu};
    __imp__sub_00079390(ctx, base);
    if (!started || (channels != 4 && channels != 6)) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    for (uint32_t index : extra) {
        if (index >= 12) continue;  // the pool's size (0068CC94 is 12)
        X86_PUSH32(index);
        X86_PUSH32(0x7948Au);  // as the call there
        sub_00078C10(ctx, base);
        ctx.esp += 4;  // cdecl
        static int shown = 0;
        if (shown++ < 8) LOGR("dialogue: a %u-channel line's stream %u given back at its stop", channels, index);
    }
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
}

// ROTS: the line slot allocator (00078FD0) leaves the slot's media object field as the last line left it; the line's
// own open (000790F0, XFileCreateMediaObjectAsync) writes it only when it succeeds. Cleared here, so a slot whose line
// never opened holds null (the poll below skips it, the release queue 000792B0 skips null) instead of a freed object.
X86_EXTERN_FUNC(__imp__sub_00078FD0);
X86_FUNC(sub_00078FD0) {
    __imp__sub_00078FD0(ctx, base);
    if (ctx.eax) kernel::W32(ctx.eax, 0);
}

// ROTS: the dialogue player's start (00077110, thiscall, 6 arguments, the first the result it writes). It takes a line
// slot (00078FD0, kept at +E0), then streams from the game's own pool of DirectSound streams (00078B90; the pool at
// 0068CC80: used +4, free +8, peak +C, entries +10, count +14). When the pool has no free stream (0xFFFFFFE2) the
// 2-stream path (0007767C) returns without giving the slot back and without opening the line's media object, so the
// slot keeps the object of the line that played in it before, freed since by the release thread (00079160). The
// per-frame poll (00079C60 -> 00078EB0) then called a freed object: the crash at 00078EBB on the Switch, 2026-11-16.
// Every other failure path of the function frees the slot and clears the player (000772C4); this does the same.
X86_EXTERN_FUNC(__imp__sub_00077110);
X86_EXTERN_FUNC(sub_00079090);
X86_FUNC(sub_00077110) {
    const uint32_t player = ctx.ecx;
    const uint32_t result = kernel::R32(ctx.esp + 4);
    static const uint32_t cap = [] {  // test knob: ROTS_DIALOGUE_STREAMS=n shrinks the pool to provoke the failure
        const char* e = std::getenv("ROTS_DIALOGUE_STREAMS");
        return e ? uint32_t(std::atoi(e)) : 0u;
    }();
    if (cap && kernel::R32(0x68CC94) > cap) kernel::W32(0x68CC94, cap);
    static int traced = 0;
    if (cap && traced++ < 40)
        LOGR("dialogue: start (player %08X) with %u of %u streams in use", player, kernel::R32(0x68CC84),
             kernel::R32(0x68CC94));
    __imp__sub_00077110(ctx, base);
    static uint32_t peak = 0;  // the pool's peak (+C) logged as it grows: a leak shows as a climb to the count
    if (kernel::R32(0x68CC8C) > peak) {
        peak = kernel::R32(0x68CC8C);
        if (peak >= 6) LOGR("dialogue: up to %u of %u streams in use", peak, kernel::R32(0x68CC94));
    }
    const uint32_t err = kernel::R32(result);
    const uint32_t line = kernel::R32(player + 0xE0);
    if (!err || !line) return;
    static int shown = 0;
    if (shown++ < 16)
        LOGR("dialogue: a line could not start (%08X) with %u of %u streams in use; its slot is given back", err,
             kernel::R32(0x68CC84), kernel::R32(0x68CC94));
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    X86_PUSH32(line);
    X86_PUSH32(0x772C1u);  // as the call there
    sub_00079090(ctx, base);
    ctx.esp += 4;  // cdecl
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
    for (uint32_t o = 0x10; o <= 0x30; o += 4) kernel::W32(player + o, 0);
    kernel::W32(player + 0xD4, 0xFFFFFFFFu);
    kernel::W32(player + 0xD8, 0xFFFFFFFFu);
    kernel::W32(player + 0xDC, 0xFFFFFFFFu);
    kernel::W32(player + 0x70, 0);
    kernel::W32(player + 0x74, 0);
    kernel::W32(player + 0x78, 0);
    kernel::W32(player + 0xE0, 0);
}
