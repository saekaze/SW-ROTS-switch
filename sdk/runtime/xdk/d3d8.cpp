// Direct3D 8 (XDK 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "app/app.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_SetBackBufferScale
#define SYM_D3DDevice_SetBackBufferScale FFFFFE04
#define HAS_D3DDevice_SetBackBufferScale 0
#endif
#ifndef SYM_D3DDevice_UpdateOverlay
#define SYM_D3DDevice_UpdateOverlay FFFFFE05
#define HAS_D3DDevice_UpdateOverlay 0
#endif
void MirrorGamma(const uint8_t* ramp);  // sheroes-nx, below
#ifndef SYM_D3DDevice_LoadVertexShaderProgram
#define SYM_D3DDevice_LoadVertexShaderProgram FFFFFE06
#define HAS_D3DDevice_LoadVertexShaderProgram 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
// STREET: every write to g_state marks its blocks for the render thread (gpu::g_track_state)
#define TOUCH(field) gpu::TouchState(g_state, &(field), sizeof(field))
std::unique_ptr<gpu::Backend> g_backend;
#ifdef __SWITCH__
// BREXTREME: every HLE call takes this lock, nearly always on the title thread with nobody else wanting it: libnx's
// recursive mutex (and before it std::recursive_mutex over pthreads) cost ~4.6 % of that thread on the console. Here
// one compare-and-swap to take it and a release store to let it go; a thread that finds it held yields until it is free.
// The owner is the thread's TLS block (tpidrro_el0, as libnx's armGetTls)
struct HleLock {
    std::atomic<uintptr_t> owner{0};
    uint32_t depth = 0;  // the owner's
    static uintptr_t Self() {
        uintptr_t t;
        __asm__("mrs %0, tpidrro_el0" : "=r"(t));
        return t;
    }
    void lock() {
        const uintptr_t me = Self();
        if (owner.load(std::memory_order_relaxed) == me) {
            depth++;
            return;
        }
        for (uintptr_t free = 0; !owner.compare_exchange_weak(free, me, std::memory_order_acquire,
                                                             std::memory_order_relaxed);
             free = 0)
            std::this_thread::yield();
        depth = 1;
    }
    void unlock() {
        if (--depth == 0) owner.store(0, std::memory_order_release);
    }
};
#else
using HleLock = std::recursive_mutex;
#endif
HleLock g_lock;

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    uint32_t need = std::max<uint32_t>(0x10000, (bytes + 0x1FFF) & ~0xFFFu);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    uint32_t pitch = (w * bpp + 63) & ~63u;
    uint32_t data = mem::AllocContiguous(pitch * h, 0, 0xFFFFFFFF, 0x1000);
    uint32_t hdr = mem::AllocSystem(24);
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (pitch / 64 - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

// equal blocks of whole 8 byte words, inline: memcmp, five calls a draw here, was 2 % of the title thread on the Switch
bool SameWords(const void* a, const void* b, size_t n) {
    uint64_t diff = 0;
    for (size_t i = 0; i < n; i += 8) {
        uint64_t x, y;
        std::memcpy(&x, static_cast<const uint8_t*>(a) + i, 8);
        std::memcpy(&y, static_cast<const uint8_t*>(b) + i, 8);
        diff |= x ^ y;
    }
    return !diff;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them.
    // BREXTREME: each block compared whole first, it rarely changes between draws: one load and compare per state
    // (132 a draw) was 6 % of the title thread on the Switch
    constexpr uint32_t kRsDeferred = gpu::kRsComplexFirst - gpu::kRsDeferredFirst;
    static_assert(kRsDeferred * 4 % 8 == 0 && gpu::kTssBumpEnvMat00 * 4 % 8 == 0);
    if (!SameWords(&G<uint8_t>(kRenderState + gpu::kRsDeferredFirst * 4), &g_state.rs[gpu::kRsDeferredFirst],
                   kRsDeferred * 4))
        for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) {
            uint32_t v = R32(kRenderState + i * 4);
            if (v != g_state.rs[i]) TOUCH(g_state.rs[i]), g_state.rs[i] = v;
        }
    for (int s = 0; s < 4; s++) {
        const uint32_t stage = kTextureState + s * kStageStates * 4;
        if (SameWords(&G<uint8_t>(stage), g_state.tss[s].data(), gpu::kTssBumpEnvMat00 * 4)) continue;
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) {
            uint32_t v = R32(stage + i * 4);
            if (v != g_state.tss[s][i]) TOUCH(g_state.tss[s][i]), g_state.tss[s][i] = v;
        }
    }
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    TOUCH(g_state.zscale);
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        gpu::TouchVsc(g_state, 58, 2);
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    TOUCH(g_state.rs[rs]);
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }

}  // namespace d3d

using namespace d3d;

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t pp = a[4];  // D3DPRESENT_PARAMETERS
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8), count = std::max<uint32_t>(R32(pp + 12), 1);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGR("d3d: CreateDevice %ux%u format %X buffers %u depth %u format %X interval %X flags %X swap effect %u", w, h,
         fmt, count, auto_depth, depth_fmt, interval, R32(pp + 0x28), R32(pp + 0x14));
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    {
        // sheroes-nx: the console's CreateDevice starts with the identity ramp (see MirrorGamma)
        uint8_t identity[768];
        for (int i = 0; i < 768; i++) identity[i] = uint8_t(i & 0xFF);
        MirrorGamma(identity);
    }
    W32(kDevicePtr, kDevice);
    ScratchPush();

    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    g_buffer_count = std::min<uint32_t>(count + 1, 3);
    for (uint32_t i = 0; i < g_buffer_count; i++) g_buffers[i] = NewSurface(w, h, color_fmt, 4);
    g_depth = 0;
    if (auto_depth) {
        uint32_t zf = gpu::LinearDepthFormat(depth_fmt);
        g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);  // 0x30 LIN_D16, 0x31 LIN_F16
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) LOGE("d3d: renderer init failed");
        if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state = gpu::State{};
    gpu::TouchAllState();
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    TOUCH(g_state.rt);
    TOUCH(g_state.zs);
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
    ApplyDefaultStates(ctx);
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

// ROTS: the title releases the device in a loop until the count reaches 0 (0x00206BB5); the device stays alive
// on the host side, CreateDevice is called again and reuses the renderer.
HLE_STDCALL(D3DDevice_Release, 0) { return 0; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<HleLock> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (v >= X86CodeBase && v < X86CodeEnd) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
#ifdef __SWITCH__
    // BREXTREME: flag 0x40 makes the lock return the data's address in the write combined view at 0xF0000000, which
    // the Switch does not map (memory.cpp: a second view of the same memory is refused); without it the same bytes
    // come at their usual 0x80000000 address. Bloody Roar Extreme locks a 640x480 surface this way at a KO and read
    // past the guest space (crash in sub_00035940 at F159DBFC)
    if (a[5] & 0x40) W32(ctx.esp + 0x18, a[5] & ~0x40u);
#endif
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<HleLock> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<HleLock> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<HleLock> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<HleLock> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<HleLock> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<HleLock> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
// BREXTREME: the video overlay. The original programs the NV2A's overlay registers through the miniport, which the
// host does not have; the console put the overlay on screen at scan-out, over the frame buffer (where the colour key
// matches, when one is on). Bloody Roar Extreme shows its WMV movies this way, decoded by WMVDEC into a YUY2 surface,
// colour key off. Here UpdateOverlay decodes each frame into a surface of the back buffer's size and format (black
// around the destination rect) that every backend uploads, as the CPU wrote it; two surfaces in turn, so a frame
// being written is not the one the render thread still reads. While the overlay is on, a Swap shows that surface
// instead of the frame buffer it covers: the game swaps black frames under some movies (the arcade prologue), which
// showed in turn with the movie and flickered. UpdateOverlay presents by itself only when the game is not swapping
// (the boot movies): two presents a frame waited for two vertical blanks and starved the movie's sound on the Switch.
namespace {
uint32_t g_overlay[2] = {}, g_overlay_next = 0;
uint32_t g_overlay_shown = 0;  // the surface holding the newest frame, 0 when the overlay is off
std::chrono::steady_clock::time_point g_overlay_time{}, g_last_swap{};
constexpr auto kOverlayStale = std::chrono::milliseconds(250);  // a movie that stopped updating no longer covers swaps

bool OverlayCovers() {
    return g_overlay_shown && std::chrono::steady_clock::now() - g_overlay_time < kOverlayStale;
}

inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// one packed 4:2:2 pair to two X8R8G8B8 pixels, BT.601 studio range as texture.cpp decodes it
inline void Yuv422(const uint8_t* p, bool yuy2, uint32_t* o, bool two) {
    int y0 = yuy2 ? p[0] : p[1], u = yuy2 ? p[1] : p[0], y1 = yuy2 ? p[2] : p[3], v = yuy2 ? p[3] : p[2];
    int d = u - 128, e = v - 128;
    int r = 409 * e + 128, g = -100 * d - 208 * e + 128, b = 516 * d + 128;
    int c = 298 * (y0 - 16);
    o[0] = 0xFF000000u | uint32_t(Clamp8((c + r) >> 8)) << 16 | uint32_t(Clamp8((c + g) >> 8)) << 8 | Clamp8((c + b) >> 8);
    if (!two) return;
    c = 298 * (y1 - 16);
    o[1] = 0xFF000000u | uint32_t(Clamp8((c + r) >> 8)) << 16 | uint32_t(Clamp8((c + g) >> 8)) << 8 | Clamp8((c + b) >> 8);
}
}  // namespace

HLE_STDCALL(D3DDevice_EnableOverlay, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    if (!a[0]) g_overlay_shown = 0;
    return 0;
}
#if HAS_D3DDevice_UpdateOverlay
// (surface, src rect, dst rect, colour key enable, colour key); the colour key is not applied
HLE_STDCALL(D3DDevice_UpdateOverlay, 5) {
    std::lock_guard<HleLock> lock(g_lock);
    gpu::TextureDesc src, bb;
    if (!g_backend || !gpu::Describe(a[0], src) || (src.format != 0x24 && src.format != 0x25) || !src.linear ||
        !gpu::Describe(g_buffers[1], bb)) {
        LOG_FIRST(4, "d3d: UpdateOverlay surface %08X not shown (not a linear YUY2/UYVY surface)", a[0]);
        return 0;
    }
    if (a[3]) LOG_FIRST(1, "d3d: UpdateOverlay with colour key %08X, not applied", a[4]);
    uint32_t& out = g_overlay[g_overlay_next];
    g_overlay_next ^= 1;
    gpu::TextureDesc dst;
    if (!out || !gpu::Describe(out, dst) || dst.width != bb.width || dst.height != bb.height) {
        out = NewSurface(bb.width, bb.height, (R32(g_buffers[1] + 12) >> 8) & 0xFF, 4);
        gpu::Describe(out, dst);
        LOG_FIRST(4, "d3d: UpdateOverlay %ux%u frames shown in a %ux%u surface", src.width, src.height, dst.width, dst.height);
    }
    auto rect = [](uint32_t r, int32_t w, int32_t h, int32_t (&v)[4]) {
        v[0] = 0, v[1] = 0, v[2] = w, v[3] = h;
        if (r) for (int i = 0; i < 4; i++) v[i] = int32_t(R32(r + i * 4));
    };
    int32_t sr[4], dr[4];
    rect(a[1], int32_t(src.width), int32_t(src.height), sr);
    rect(a[2], int32_t(dst.width), int32_t(dst.height), dr);
    sr[0] = std::clamp(sr[0], 0, int32_t(src.width) - 1), sr[2] = std::clamp(sr[2], sr[0] + 1, int32_t(src.width));
    sr[1] = std::clamp(sr[1], 0, int32_t(src.height) - 1), sr[3] = std::clamp(sr[3], sr[1] + 1, int32_t(src.height));
    dr[0] = std::clamp(dr[0], 0, int32_t(dst.width)), dr[2] = std::clamp(dr[2], dr[0], int32_t(dst.width));
    dr[1] = std::clamp(dr[1], 0, int32_t(dst.height)), dr[3] = std::clamp(dr[3], dr[1], int32_t(dst.height));
    const auto t0 = std::chrono::steady_clock::now();
    uint8_t* o = static_cast<uint8_t*>(HostPtr(dst.data));
    const uint8_t* in = static_cast<const uint8_t*>(HostPtr(src.data));
    const bool yuy2 = src.format == 0x24;
    const int32_t dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    for (int32_t y = 0; y < int32_t(dst.height); y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(o + size_t(y) * dst.pitch);
        if (y < dr[1] || y >= dr[3] || !dw) {
            std::fill(row, row + dst.width, 0xFF000000u);
            continue;
        }
        std::fill(row, row + dr[0], 0xFF000000u);
        std::fill(row + dr[2], row + dst.width, 0xFF000000u);
        if (dw == sw && dh == sh && !(sr[0] & 1)) {
            // one to one (the usual case: a 640x480 movie on a 640x480 screen): converted straight, no float
            const uint8_t* line = in + size_t(sr[1] + y - dr[1]) * src.pitch + size_t(sr[0]) * 2;
            for (int32_t x = 0; x < dw; x += 2) Yuv422(line + x * 2, yuy2, row + dr[0] + x, x + 1 < dw);
            continue;
        }
        // scaled: the nearest source line, linear across it in 8 bit fixed point
        int32_t sy = sr[1] + int32_t((int64_t(y - dr[1]) * sh + sh / 2) / std::max(dh, 1));
        const uint8_t* line = in + size_t(std::min(sy, sr[3] - 1)) * src.pitch;
        uint32_t step = uint32_t((uint64_t(sw) << 16) / uint64_t(dw)), fx = uint32_t(sr[0]) << 16;
        for (int32_t x = 0; x < dw; x++, fx += step) {
            uint32_t sx = std::min(fx >> 16, uint32_t(sr[2] - 1));
            uint32_t px[2];
            Yuv422(line + (sx & ~1u) * 2, yuy2, px, true);
            row[dr[0] + x] = px[sx & 1];
        }
    }
    gpu::MarkDirty(out);
    g_overlay_shown = out;
    auto now = std::chrono::steady_clock::now();
    {
        // every 5 s of a movie: the frames it got (a 30 fps movie below 30 lags, and its sound with it) and what the
        // conversion here cost of each
        static auto window = now;
        static uint32_t frames = 0;
        static double convert_ms = 0;
        frames++;
        convert_ms += std::chrono::duration<double, std::milli>(now - t0).count();
        double secs = std::chrono::duration<double>(now - window).count();
        if (secs >= 5.0 || now - g_overlay_time > std::chrono::seconds(1)) {
            if (frames > 1 && secs < 30.0)
                LOGR("d3d: video overlay %.1f frames a second, %.2f ms each to convert, %s", frames / secs,
                     convert_ms / frames, now - g_last_swap > kOverlayStale ? "presented here" : "shown by the swaps");
            window = now;
            frames = 0;
            convert_ms = 0;
        }
    }
    g_overlay_time = now;
    // the game swapping meanwhile shows it at its next Swap, so a frame is presented once
    if (now - g_last_swap > kOverlayStale) g_backend->Present(SurfaceOf(out));
    return 0;
}
#endif
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t front = g_buffers[0], size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

// sheroes-nx: the ramp is also kept where the console keeps it, both slots of the device's pair (CDevice_GammaRamps,
// 0x300 bytes each), so the console's own D3DDevice_GetGammaRamp reads it back. Sonic Heroes fades by reading the
// current ramp (GetGammaRamp 0x0032AF30, not replaced: it reads [CDevice + 0x240C] for the slot and copies from
// CDevice + 0x1E04 + slot * 0x300) and scaling it; with that memory left at zero the screen stayed black after the
// title screen while the menu drew normally (PC frame capture 2026-11-10)
void MirrorGamma(const uint8_t* ramp) {
    if (XDK_CDevice_GammaRamps == xdk::kUnknown) return;
    for (uint32_t i = 0; i < 2; i++) std::memcpy(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300), ramp, 768);
}

HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    MirrorGamma(ramp);
    LOG_FIRST(24, "d3d: SetGammaRamp flags %X: red %u %u %u, green %u %u %u, blue %u %u %u", a[0], ramp[0], ramp[128],
              ramp[255], ramp[256], ramp[384], ramp[511], ramp[512], ramp[640], ramp[767]);
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget), zs = a[1];
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget), old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    TOUCH(g_state.rt);
    TOUCH(g_state.zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    TOUCH(g_state.viewport);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
}  // namespace

HLE_STDCALL(D3DDevice_Swap, 1) {
    static unsigned boot_swaps = 0;
    const bool trace_boot = std::getenv("SHEROES_CAPTURE_BOOT") && boot_swaps++ < 120;
    if (trace_boot) LOGR("SHEROES swap: enter");
    app::SplashFirstFrame();
    if (trace_boot) LOGR("SHEROES swap: splash complete");
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<HleLock> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<HleLock> lock(g_lock);
    g_presents++;
    // pacing: a frame of the title thread (swap to swap) over 50 ms, the game's own time included; the render thread's
    // presents are logged apart (deko.cpp), so a hitch says which side it came from
    {
        static auto last = std::chrono::steady_clock::now();
        static unsigned reported = 0;
        static const double limit = [] {  // SHEROES_HITCH_MS lowers it on a fast PC
            const char* v = std::getenv("SHEROES_HITCH_MS");
            return v ? std::atof(v) : 50.0;
        }();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        if (ms > limit && g_presents > 2 && reported < 400) {
            reported++;
            LOGR("pacing: title thread frame of %.0f ms (swap %u)", ms, g_presents.load());
        }
    }
    // diagnostic: SHEROES_SHOT_SWAPS=N captures each of the first N frames and logs what made it
    static const int shot_swaps = [] {
        const char* v = std::getenv("SHEROES_SHOT_SWAPS");
        return v ? std::atoi(v) : 0;
    }();
    if (int(g_presents.load()) <= shot_swaps) {
        static uint64_t draws_before = 0;
        uint64_t draws = gpu::g_perf_draws.load();
        LOGR("d3d: swap %u: %llu draws, render target %08X, back buffer %08X", g_presents.load(),
             (unsigned long long)(draws - draws_before), Dev(kDevRenderTarget), R32(g_buffers[1] + 4));
        draws_before = draws;
        gpu::g_screenshot.store(true);
    }
    static uint32_t last_flags = ~0u;
    static unsigned flag_reports = 0;
    if (a[0] != last_flags && flag_reports < 16) {
        last_flags = a[0];
        ++flag_reports;
        LOGR("d3d: Swap flags %X from swap %u", a[0], g_presents.load());
    }
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    if (trace_boot) LOGR("SHEROES swap: walk complete, presenting");
    const uint32_t shown = R32(g_buffers[1] + 4);
    g_last_swap = std::chrono::steady_clock::now();
    // the video overlay covers the frame buffer while it is on (SHEROES, see UpdateOverlay)
    const bool covered = OverlayCovers();
    if (covered) LOG_FIRST(2, "d3d: swap %u shows the video overlay over the frame buffer", g_presents.load());
    if (g_backend) g_backend->Present(covered ? SurfaceOf(g_overlay_shown) : SurfaceOf(g_buffers[1]));
    if (trace_boot) LOGR("SHEROES swap: present complete");
    app::Pace();
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    uint32_t last = g_buffers[g_buffer_count - 1], data = R32(last + 4), lk = R32(last + 8);
    for (uint32_t i = g_buffer_count - 1; i > 0; i--) {
        W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
        W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
    }
    W32(g_buffers[0] + 4, data);
    W32(g_buffers[0] + 8, lk);
    if (gpu::g_swap_keeps_back_buffer && g_backend && g_buffer_count > 1) {
        // the frame just shown, wherever its memory went, into the new back buffer
        for (uint32_t i = 0; i < g_buffer_count; i++)
            if (i != 1 && R32(g_buffers[i] + 4) == shown) {
                g_backend->Copy(SurfaceOf(g_buffers[i]), SurfaceOf(g_buffers[1]), 0, 0, 0);
                break;
            }
    }
    TOUCH(g_state.rt);  // the same header, its memory rotated
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<HleLock> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<HleLock> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t v = a[0];
    // STREET XBE 00537DD8 tests EBP, 00537DE1 jumps to the common
    // update at 00537FC3 when null; preserve the existing viewport.
    TOUCH(g_state.viewport);
    TOUCH(g_state.scissor_count);
    if (v)
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<HleLock> lock(g_lock);
    TOUCH(g_state.scissor_count);
    TOUCH(g_state.scissors);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    TOUCH(g_state.ss_offset_x);
    TOUCH(g_state.ss_offset_y);
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

#if HAS_D3DDevice_SetBackBufferScale
// FIFA Street (0x25DC80): the library stores the scale times the multisample factors in the device and then rebuilds
// its frame buffer setup through driver state the host does not make (it read a null pointer). The host draws the
// back buffer at its own size, so the scale is kept for the library code that reads it and nothing else is done.
HLE_STDCALL(D3DDevice_SetBackBufferScale, 2) {
    float x = ArgF(a, 0), y = ArgF(a, 1);
    LOG_FIRST(8, "d3d: SetBackBufferScale %.3f %.3f (kept in the device, not applied)", x, y);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &x, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &y, 4);
    }
    return 0;
}
#endif

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    gpu::TouchSimple(g_state, method >> 2);
    g_state.simple[method >> 2] = value;
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        gpu::TouchSimple(g_state, method >> 2);
        g_state.simple[method >> 2] = v;
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    TOUCH(g_state.tss[a[0] & 3]);
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    TOUCH(g_state.tss[a[0] & 3]);
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    TOUCH(g_state.tss[a[0] & 3]);
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    TOUCH(g_state.tss[a[0] & 3]);
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        TOUCH(g_state.tss[stage][type]);
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    TOUCH(g_state.transforms[pc]);
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    TOUCH(g_state.material);
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<HleLock> lock(g_lock);
        TOUCH(g_state.lights[i]);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<HleLock> lock(g_lock);
        TOUCH(g_state.light_enable);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush)); // Submit queued draws before replacing their texture state.
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    Release(ctx, old);
    TOUCH(g_state.textures[stage]);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush)); // Preserve palette state of queued indexed-font draws.
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    TOUCH(g_state.palettes[stage]);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    TOUCH(g_state.textures[stage]);  // the same header, new fields
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    Release(ctx, old);
    TOUCH(g_state.streams[n]);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    Release(ctx, old);
    TOUCH(g_state.index_buffer);
    TOUCH(g_state.index_data);
    TOUCH(g_state.base_vertex);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        for (uint32_t i = 0; i < 1024; i++) {
            uint32_t t = R32(decl + i * 4);
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) break;
        }
    }
    if (func) {
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        for (uint32_t i = 0; i < n * 4; i++) vs->program.push_back(R32(func + 4 + i * 4));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<HleLock> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush)); // Queued vertices retain the declaration used when submitted.
    uint32_t h = a[0];
    // STREET native SetVertexShader at 0053A134 stores the handle at device+0798.
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_VertexShader));
    SetDev(XDK_CDevice_VertexShader + 4, h);
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) gpu::TouchVsc(g_state, reg), g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    uint32_t reg = a[0];
    gpu::TouchVsc(g_state, reg);
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    uint32_t reg = a[0];
    gpu::TouchVsc(g_state, reg);
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    uint32_t reg = a[0], src = a[1];
    gpu::TouchVsc(g_state, reg, 4);
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    gpu::TouchVsc(g_state, reg, dwords / 4);
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

// sheroes-nx: the vertex program memory. RenderWare's Xbox driver (Sonic Heroes 0x002BFB40) loads a program with
// LoadVertexShaderProgram(program, 12) and runs it with SelectVertexShader(handle, 12) whenever its program changes;
// the handles come from CreateVertexShader with a declaration and no function. The Xbox keeps 136 instructions of
// program memory: Load* write into it, SelectVertexShader takes the handle's declaration and the program from the
// slot up to the instruction with the final flag (dword 3, bit 0), as the NV2A runs it.
namespace {
constexpr uint32_t kVsSlots = 136;
std::array<uint32_t, kVsSlots * 4> g_vs_mem{};
std::unordered_map<uint64_t, std::shared_ptr<gpu::VertexShader>> g_vs_selected;  // by declaration and program
uint32_t g_vs_loads = 0;

void LoadProgram(const uint32_t* words, uint32_t count, uint32_t address) {
    for (uint32_t i = 0; i < count && address + i < kVsSlots; i++) std::memcpy(&g_vs_mem[(address + i) * 4], words + i * 4, 16);
    g_vs_loads++;
}
}  // namespace

#if HAS_D3DDevice_LoadVertexShaderProgram
// (function, address): the function as CreateVertexShader takes it, a header (type, count << 16) and the instructions
HLE_STDCALL(D3DDevice_LoadVertexShaderProgram, 2) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t f = a[0], n = R32(f) >> 16;
    LOG_FIRST(8, "d3d: LoadVertexShaderProgram %08X, %u instructions at %u", f, n, a[1]);
    LoadProgram(static_cast<const uint32_t*>(HostPtr(f + 4)), n, a[1]);
    return 0;
}
#endif

HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<HleLock> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end() || it->second->program.empty()) {
        LOG_FIRST(8, "d3d: LoadVertexShader %08X at %u: no program for that handle", a[0], a[1]);
        return 0;
    }
    LoadProgram(it->second->program.data(), uint32_t(it->second->program.size() / 4), a[1]);
    return 0;
}

HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush));  // queued vertices keep the shader they were given with
    uint32_t h = a[0], address = a[1];
    std::shared_ptr<gpu::VertexShader> decl;
    if (h) {
        auto it = g_vs.find(h);
        if (it == g_vs.end()) {
            LOG_FIRST(8, "d3d: SelectVertexShader with unknown handle %08X", h);
            return 0;
        }
        decl = it->second;
    } else if (g_state.vs) {
        decl = g_state.vs;  // a null handle keeps the current declaration
        h = g_state.vs_handle;
    } else {
        LOG_FIRST(8, "d3d: SelectVertexShader with no handle and no shader set");
        return 0;
    }
    std::vector<uint32_t> program;
    for (uint32_t i = address; i < kVsSlots; i++) {
        program.insert(program.end(), &g_vs_mem[i * 4], &g_vs_mem[i * 4] + 4);
        if (g_vs_mem[i * 4 + 3] & 1) break;
    }
    uint64_t key = gpu::Hash(program.data(), program.size() * 4, uint64_t(decl->handle) * 0x9E3779B97F4A7C15ull);
    auto& vs = g_vs_selected[key];
    if (!vs || vs->program != program || vs->decl != decl->decl) {
        vs = std::make_shared<gpu::VertexShader>(*decl);
        vs->program = std::move(program);
        vs->program_type = 0x2078;
        vs->hash = 0;
        LOG_FIRST(32, "d3d: SelectVertexShader %08X at %u: %zu instructions (%zu programs so far)", h, address,
                  vs->program.size() / 4, g_vs_selected.size());
    }
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_VertexShader));
    SetDev(XDK_CDevice_VertexShader + 4, h | 1);
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = h | 1;
    g_state.vs = vs;
    for (auto& [reg, v] : decl->constants)
        if (reg < 192) gpu::TouchVsc(g_state, reg), g_state.vsc[reg] = v;
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<HleLock> lock(g_lock);
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    // sheroes-nx: the object laid out like the XDK's (5659: CreatePixelShader 0x3377E0 allocates 0xFC bytes: refcount
    // 1 at +0, 1 at +4, +8 points at the definition copied to +0xC). RenderWare learns that layout by scanning the
    // object for its definition (0x2908C0, called from 0x2E13FF while loading a stage) and read past a 16-byte record
    uint32_t rec = mem::AllocSystem(0xFC, 16);
    W32(rec + 0, 1);
    W32(rec + 4, 1);
    W32(rec + 8, rec + 0xC);
    std::memcpy(HostPtr(rec + 0xC), ps->def, sizeof(ps->def));
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        const void* def = HostPtr(R32(a[0] + XDK_PixelShader_Definition));
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    auto put = [&](uint32_t method, uint32_t v) {
        gpu::TouchSimple(g_state, method >> 2);
        g_state.simple[method >> 2] = v;
    };
    for (int i = 0; i < 8; i++) {
        put(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        put(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        put(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        put(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        put(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        put(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    put(gpu::kMPsFinalAbcd, d[8]);
    put(gpu::kMPsFinalEfgh, d[9]);
    put(gpu::kMPsCompareMode, d[42]);
    put(gpu::kMPsFinalConstant0, d[43]);
    put(gpu::kMPsFinalConstant1, d[44]);
    put(gpu::kMPsCombinerCount, d[53]);
    put(gpu::kMPsDotMapping, d[55]);
    put(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<HleLock> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    TOUCH(g_state.psc);
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    return 0;
}

// drawing

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncDeferred();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    SyncDeferred();
    auto stream = g_state.streams[0];
    TOUCH(g_state.streams[0]);
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    TOUCH(g_state.streams[0]);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64];                    // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex
std::vector<uint32_t> g_im_inline;      // NV097_INLINE_ARRAY words across packets

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) gpu::TouchSimple(g_state, gpu::kMCountEnable >> 2), g_state.simple[gpu::kMCountEnable >> 2] = v;
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (prim && !g_im_inline.empty()) {
        uint32_t stride = VertexBytes(), bytes = uint32_t(g_im_inline.size()) * 4;
        if (stride && bytes % stride == 0) {
            static unsigned reports = 0;
            if (reports++ < 8) LOGR("inline draw: primitive=%u count=%u stride=%u", prim, bytes / stride, stride);
            DrawUp(prim, bytes / stride, g_im_inline.data(), stride);
        } else LOGR("inline draw rejected: bytes=%u stride=%u", bytes, stride);
        g_im_inline.clear();
        return;
    }
    if (!prim || !count) return;
    if (!(handle & 1) && (handle & 0xE) == 4) LOG_FIRST(1, "d3d: pretransformed immediate mode vertices are not supported");
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    TOUCH(g_state.vs_handle);
    g_state.vs_handle = handle;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            float f, *a = g_im_attr;
            bool last = true;  // the slot's last component
            std::memcpy(&f, &v, 4);
            ApplyVisibilityMethod(m, v);
            if (m == 0x1818) {
                if (g_im_prim) g_im_inline.push_back(v);
                continue;
            }
            if (m == 0x17FC) {  // begin/end, 0 ends
                if (v) {
                    g_im_prim = v;
                    g_im_mask = 0;
                    g_im_vertices.clear();
                    g_im_inline.clear();
                } else {
                    EndPrimitive();
                }
                continue;
            } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
                a += (m - 0x1880) / 8 * 4;
                last = (m - 0x1880) / 4 & 1;
                a[last] = f;
                a[2] = 0;
                a[3] = 1;
            } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
                a += (m - 0x1940) / 4 * 4;
                for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
            } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
                a += (m - 0x1A00) / 16 * 4;
                last = ((m - 0x1A00) / 4 & 3) == 3;
                a[(m - 0x1A00) / 4 & 3] = f;
            } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
                last = m == 0x1508;
                a[(m - 0x1500) / 4] = f;
                a[3] = 1;
            } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
                last = m == 0x1524;
                a[(m - 0x1518) / 4] = f;
            } else {
                continue;
            }
            if (!g_im_prim) continue;
            g_im_mask |= 1u << (a - g_im_attr) / 4;
            if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

#if HAS_D3DDevice_DrawIndexedVerticesUP
// DrawIndexedVerticesUP(prim, index count, 16-bit indices, vertices, stride): de-indexed into a plain vertex list,
// which keeps every primitive type (lists, strips, fans) as the indices order it. ROTS: menus and HUD, mission 1.
HLE_STDCALL(D3DDevice_DrawIndexedVerticesUP, 5) {
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t prim = a[0], count = a[1], idx = a[2], verts = a[3], stride = a[4];
    if (!count || !stride || !idx || !verts) return 0;
    static std::vector<uint8_t> flat;
    flat.resize(size_t(count) * stride);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t v;
        std::memcpy(&v, HostPtr(idx + i * 2), 2);
        std::memcpy(flat.data() + size_t(i) * stride, HostPtr(verts + uint32_t(v) * stride), stride);
    }
    DrawUp(prim, count, flat.data(), stride);
    return 0;
}
#endif

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline, any other method as a simple state.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    g_push_begin = ScratchPush(a[0] * 4);
    return g_push_begin;
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<HleLock> lock(g_lock);
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    for (uint32_t p = g_push_begin, end = a[0]; g_push_begin && p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: EndPush stops at command %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (m == kInlineArray) {
                if (prim) vertices.push_back(v);
            } else if (m != kBeginEnd) {
                if (m >> 2 < g_state.simple.size()) gpu::TouchSimple(g_state, m >> 2), g_state.simple[m >> 2] = v;
            } else if (v) {
                prim = v;
                vertices.clear();
            } else if (prim) {
                uint32_t stride = VertexBytes(), bytes = uint32_t(vertices.size()) * 4;
                if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
                else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
                prim = 0;
            }
        }
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<HleLock> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}

// ROTS soft reboot: the title's allocations this module kept (push scratch, the UP ring, the device's surfaces) are
// given back with the rest of its memory; the renderer finishes what it has first. CreateDevice makes them again.
static const bool kD3dRebootHook = (kernel::OnReboot([] {
    std::lock_guard<HleLock> lock(g_lock);
    if (g_backend) g_backend->Flush();
    g_scratch_push = g_scratch_size = g_walk = 0;
    g_up_ring = g_up_pos = g_up_header = 0;
    g_up_fence[0] = g_up_fence[1] = 0;
    g_push_begin = 0;
    g_im_prim = g_im_mask = 0;
    for (auto& b : g_buffers) b = 0;
    g_depth = 0;
    g_overlay[0] = g_overlay[1] = 0;
    g_overlay_shown = 0;
}), true);
