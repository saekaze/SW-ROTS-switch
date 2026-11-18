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

#include <algorithm>
#include <atomic>
#include <mutex>

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
#include <climits>
#include <cmath>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <cstdlib>
#include <cstring>
#include <vector>

X86_EXTERN_FUNC(__imp__sub_004C0F80);

namespace {
struct YuvWrite {
    uint32_t va;
    uint64_t value;  // two pixels
};

// two pixels sharing one chroma: per lane (int16)(chroma + luma table) >> 6, clamped to 0..255; the four lanes are the
// pixel's bytes. The saturating narrow (NEON vqmovun, SSE2 packus) is the clamp
inline uint64_t TwoPixels(const uint8_t* ch, const uint8_t* t0, const uint8_t* t1) {
#if defined(__aarch64__)
    const int16x4_t c = vld1_s16(reinterpret_cast<const int16_t*>(ch));
    const int16x8_t a = vcombine_s16(vld1_s16(reinterpret_cast<const int16_t*>(t0)),
                                     vld1_s16(reinterpret_cast<const int16_t*>(t1)));
    const uint8x8_t r = vqmovun_s16(vshrq_n_s16(vaddq_s16(vcombine_s16(c, c), a), 6));
    return vget_lane_u64(vreinterpret_u64_u8(r), 0);
#elif defined(__SSE2__)
    const __m128i c = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(ch));
    const __m128i a = _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(t0)),
                                         _mm_loadl_epi64(reinterpret_cast<const __m128i*>(t1)));
    const __m128i r = _mm_srai_epi16(_mm_add_epi16(_mm_unpacklo_epi64(c, c), a), 6);
    return uint64_t(_mm_cvtsi128_si64(_mm_packus_epi16(r, r)));
#else
    uint64_t out = 0;
    for (int p = 0; p < 2; p++) {
        const uint8_t* t = p ? t1 : t0;
        for (int k = 0; k < 4; k++) {
            uint16_t ck, tk;
            std::memcpy(&ck, ch + 2 * k, 2);
            std::memcpy(&tk, t + 2 * k, 2);
            int32_t v = int16_t(uint16_t(ck + tk)) >> 6;
            out |= uint64_t(v < 0 ? 0 : v > 255 ? 255 : v) << (32 * p + 8 * k);
        }
    }
    return out;
#endif
}

// the chroma of one column pair: the U and V table lanes added
inline void Chroma(const uint8_t* tu, const uint8_t* tv, uint8_t out[8]) {
    for (int k = 0; k < 4; k++) {
        uint16_t a, b;
        std::memcpy(&a, tu + 2 * k, 2);
        std::memcpy(&b, tv + 2 * k, 2);
        a = uint16_t(a + b);
        std::memcpy(out + 2 * k, &a, 2);
    }
}

// store(va, two pixels) for each pair of pixels, left to right, the two rows of a chroma row in turn
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
    const uint8_t* ty = mem + tab;
    const uint8_t* tu = mem + tab + 0x800;
    const uint8_t* tv = mem + tab + 0x1000;
    do {
        int32_t c = 0;
        const uint8_t *r0 = mem + y0, *r1 = mem + y1, *cu = mem + u, *cv = mem + v;
        do {
            for (int j = 0; j < 8; j++) {
                const uint32_t i = uint32_t(c + j), x = 2 * i;
                uint8_t ch[8];
                Chroma(tu + cu[i] * 8u, tv + cv[i] * 8u, ch);
                store(d0 + x * 4, TwoPixels(ch, ty + r0[x] * 8u, ty + r0[x + 1] * 8u));
                store(d1 + x * 4, TwoPixels(ch, ty + r1[x] * 8u, ty + r1[x + 1] * 8u));
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
        YuvToRgb(src, dsts, tab, row1, adv, [&](uint32_t va, uint64_t px) { mine.push_back({va, px}); }, last_c);
        auto t1 = std::chrono::steady_clock::now();
        __imp__sub_004C0F80(ctx, base);
        auto t2 = std::chrono::steady_clock::now();
        static double ns_native = 0, ns_orig = 0;
        ns_native += std::chrono::duration<double, std::nano>(t1 - t0).count();
        ns_orig += std::chrono::duration<double, std::nano>(t2 - t1).count();
        static int calls = 0, bad_calls = 0;
        size_t bad = 0;
        for (const YuvWrite& m : mine) bad += (R32(m.va) | uint64_t(R32(m.va + 4)) << 32) != m.value;
        calls++;
        if (bad) bad_calls++;
        if (bad || calls % 250 == 1)
            LOGR("yuv verify: call %d, %zu pixel pairs, %zu differ (%d calls with differences so far); native %.2f ms, "
                 "translated %.2f ms a call", calls, mine.size(), bad, bad_calls, ns_native / calls / 1e6,
                 ns_orig / calls / 1e6);
        return;
    }
    kernel::W32(0x65C890, 0x60D594);  // 004C1B30, called first: keeps its argument in a global
    uint8_t* const mem = static_cast<uint8_t*>(kernel::HostPtr(0));
    YuvToRgb(src, dsts, tab, row1, adv, [&](uint32_t va, uint64_t px) { std::memcpy(mem + va, &px, 8); }, last_c);
    // as the original leaves them: eax the row counter (0), ecx the last column, edx the byte mask; ret (cdecl)
    ctx.eax = 0;
    ctx.ecx = uint32_t(last_c);
    ctx.edx = 0xFF;
    ctx.esp += 4;
}

// ROTS: the movies' inverse DCT (CRI Sofdec, 004C4A30, cdecl: coefficients, int16 out, float scratch; SSE floats,
// two passes of two 4-column halves, the second rounding with cvtps2pi and packing with MMX packssdw). The same float
// operations in the same order, on the host's vectors, with no fused multiply-add (it would round differently): bit
// for bit the same. About 18% of a movie's CPU time translated (the PC harness's profile, 2026-11-18).
// ROTS_IDCT=original runs the translated function, ROTS_IDCT=verify both, compared.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#elif defined(__clang__)
#pragma clang fp contract(off)
#endif
namespace {
typedef float V4 __attribute__((vector_size(16)));

inline V4 Ld(const uint8_t* p) {
    V4 v;
    std::memcpy(&v, p, 16);
    return v;
}

// one pass of four columns: L are the eight loads in the function's order, O the eight results in its store order
inline void IdctPass(const V4 L[8], const uint8_t* k, V4 O[8]) {
    const V4 k0 = Ld(k), k1 = Ld(k + 0x10), k3 = Ld(k + 0x30), k4 = Ld(k + 0x40);
    V4 x0 = L[0], x2 = L[2], x4 = L[4], x6 = L[6], x1, x3, x5, x7;
    x1 = x2; x2 = x2 - x6; x6 = x6 + x1; x2 = x2 * k0;
    x1 = x0; x0 = x0 - x4; x4 = x4 + x1; x2 = x2 - x6;
    x1 = x4; x4 = x4 - x6; x6 = x6 + x1;
    x1 = x0; x0 = x0 - x2; x2 = x2 + x1;
    const V4 saved = x0;
    x1 = L[1]; x3 = L[3]; x5 = L[5]; x7 = L[7];
    x0 = x5; x5 = x5 - x3; x3 = x3 + x0;
    x0 = x1; x1 = x1 - x7; x7 = x7 + x0;
    x0 = x7; x7 = x7 - x3; x3 = x3 + x0;
    x0 = x5; x0 = x0 - x1; x0 = x0 * k4;
    x7 = x7 * k0; x1 = x1 * k3; x1 = x1 - x0; x1 = x1 - x3; x7 = x7 - x1;
    x5 = x5 * k1; x5 = x5 - x0; x5 = x5 - x7;
    x0 = x6; x6 = x6 - x3; x3 = x3 + x0;
    x0 = x2; x2 = x2 - x1; x1 = x1 + x0;
    x0 = x4; x4 = x4 - x5; x5 = x5 + x0;
    x0 = saved; const V4 t = x0; x0 = x0 - x7; x7 = x7 + t;
    O[0] = x3; O[1] = x1; O[2] = x7; O[3] = x5; O[4] = x4; O[5] = x0; O[6] = x2; O[7] = x6;
}

// cvtps2pi with the default rounding (to nearest, even), then packssdw's saturation
inline int16_t Round16(float f) {
    int32_t i = (f >= -2147483648.0f && f < 2147483648.0f) ? int32_t(std::nearbyint(f)) : INT32_MIN;
    return int16_t(i < -32768 ? -32768 : i > 32767 ? 32767 : i);
}

void Idct(uint8_t* mem, uint32_t src, uint32_t out, uint32_t tmp, uint32_t table) {
    const uint8_t* k = mem + table;
    for (int it = 0; it < 2; it++) {  // rows of the coefficients into the scratch, transposed
        const uint8_t* s = mem + src + it * 0x80;
        const V4 L[8] = {Ld(s), Ld(s + 0x10), Ld(s + 0x20), Ld(s + 0x30), Ld(s + 0x40), Ld(s + 0x50), Ld(s + 0x60), Ld(s + 0x70)};
        V4 O[8];
        IdctPass(L, k, O);
        uint8_t* d = mem + tmp + it * 0x80;
        for (int slot = 0; slot < 8; slot++)
            for (int lane = 0; lane < 4; lane++) {
                const float f = O[slot][lane];
                std::memcpy(d + slot * 4 + lane * 0x20, &f, 4);
            }
    }
    for (int it = 0; it < 2; it++) {  // columns of the scratch into the int16 block
        const uint8_t* s = mem + tmp + it * 0x10;
        const V4 L[8] = {Ld(s), Ld(s + 0x20), Ld(s + 0x40), Ld(s + 0x60), Ld(s + 0x80), Ld(s + 0xA0), Ld(s + 0xC0), Ld(s + 0xE0)};
        V4 O[8];
        IdctPass(L, k, O);
        uint8_t* d = mem + out + it * 8;
        for (int slot = 0; slot < 8; slot++) {
            int16_t r[4];
            for (int lane = 0; lane < 4; lane++) r[lane] = Round16(O[slot][lane]);
            std::memcpy(d + slot * 0x10, r, 8);
        }
    }
}
}  // namespace
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

X86_EXTERN_FUNC(__imp__sub_004C4A30);
X86_FUNC(sub_004C4A30) {
    static const int mode = [] {
        const char* e = std::getenv("ROTS_IDCT");
        return !e ? 0 : !std::strcmp(e, "original") ? 1 : !std::strcmp(e, "verify") ? 2 : 0;
    }();
    if (mode == 1) return __imp__sub_004C4A30(ctx, base);
    using kernel::R32;
    uint8_t* const mem = static_cast<uint8_t*>(kernel::HostPtr(0));
    const uint32_t src = R32(ctx.esp + 4), out = R32(ctx.esp + 8), tmp = R32(ctx.esp + 12), table = R32(0x848CD8);
    if (mode == 2) {
        uint8_t in[0x100], mine_out[0x80], mine_tmp[0x100];
        std::memcpy(in, mem + src, sizeof(in));
        Idct(mem, src, out, tmp, table);
        std::memcpy(mine_out, mem + out, sizeof(mine_out));
        std::memcpy(mine_tmp, mem + tmp, sizeof(mine_tmp));
        std::memcpy(mem + src, in, sizeof(in));  // in case the block and the scratch overlap
        __imp__sub_004C4A30(ctx, base);
        static int calls = 0, bad = 0;
        calls++;
        const bool differ = std::memcmp(mine_out, mem + out, sizeof(mine_out)) || std::memcmp(mine_tmp, mem + tmp, sizeof(mine_tmp));
        if (differ) bad++;
        if ((differ && bad <= 8) || calls % 20000 == 1)
            LOGR("idct verify: call %d, %d with differences so far%s", calls, bad, differ ? " (this one)" : "");
        return;
    }
    Idct(mem, src, out, tmp, table);
    // as the original leaves them: eax the table, ecx the pass counter (0); ret (cdecl)
    ctx.eax = table;
    ctx.ecx = 0;
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

// ROTS: a voice line's file media object freed while a line slot still plays it. The release thread (00079160) makes
// the last Release (005271F1) of each object a stopped line hands it (000792B0); on the Switch the main thread then
// entered the freed object's lock in its per-frame DoWork (005270F8) and waited forever (the freeze at 169 s,
// 2026-11-18: the lock read "owned by thread 0, count 1"), or called through its freed vtable (the crash of
// 2026-11-16). Never seen on the PC; the path that leaves a slot on an object it handed over is not found yet. So a
// last Release of an object a slot in use (the six at 0068CCB0, 0x70 each, +60 is 1 when free) still holds is held
// back, logged, and made when no slot holds it any more (checked at each release and each line's poll).
std::mutex g_media_lock;
std::vector<uint32_t> g_media_held;
static const bool kMediaReboot = (kernel::OnReboot([] {
    std::lock_guard<std::mutex> lock(g_media_lock);
    g_media_held.clear();
}), true);

int SlotHolding(uint32_t media) {
    const uint32_t slots = kernel::R32(0x68CF60), count = std::min<uint32_t>(kernel::R32(0x68CF64), 16);
    for (uint32_t i = 0; slots && i < count; i++) {
        const uint32_t slot = slots + i * 0x70;
        if (kernel::R32(slot + 0x60) != 1 && kernel::R32(slot) == media) return int(i);
    }
    return -1;
}

X86_EXTERN_FUNC(__imp__sub_005271F1);

// the held objects no slot holds now: their last Release, from this thread
void ReleaseHeld(X86Context& ctx, uint8_t* base) {
    std::vector<uint32_t> free;
    {
        std::lock_guard<std::mutex> lock(g_media_lock);
        for (auto it = g_media_held.begin(); it != g_media_held.end();) {
            if (SlotHolding(*it) < 0) {
                free.push_back(*it);
                it = g_media_held.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (free.empty()) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    for (uint32_t obj : free) {
        X86_PUSH32(obj);
        X86_PUSH32(0x791EDu);  // as the release thread's call
        __imp__sub_005271F1(ctx, base);  // ret 4
    }
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
}

X86_FUNC(sub_005271F1) {
    const uint32_t obj = kernel::R32(ctx.esp + 4);
    if (obj && kernel::R32(obj + 8) == 1) {
        const int slot = SlotHolding(obj);
        if (slot >= 0) {
            static int shown = 0;
            if (shown++ < 16) {
                kernel::Thread* t = kernel::Current();
                LOGR("dialogue: media object %08X released by thread %X (return %08X) while line slot %d still plays it; "
                     "held till the slot lets go", obj, t ? t->id : 0, kernel::R32(ctx.esp), slot);
            }
            {
                std::lock_guard<std::mutex> lock(g_media_lock);
                if (std::find(g_media_held.begin(), g_media_held.end(), obj) == g_media_held.end())
                    g_media_held.push_back(obj);
            }
            ctx.eax = 0;
            ctx.esp += 8;  // ret 4
            return;
        }
    }
    __imp__sub_005271F1(ctx, base);
    ReleaseHeld(ctx, base);
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
    if (!g_media_held.empty()) ReleaseHeld(ctx, base);  // a size read without the lock: a hint, checked under it
    // a slot already given back (+60 is 1) polled by a player that still points at it: its object may be freed
    if (line && kernel::R32(line + 0x60) == 1) {
        static int shown = 0;
        if (shown++ < 16) LOGR("dialogue: a player polls line slot %08X after it was given back; skipped", line);
        ctx.esp += 4;  // ret
        return;
    }
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
