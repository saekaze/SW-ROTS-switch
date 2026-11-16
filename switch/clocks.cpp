// CPU, GPU and memory clocks from the settings file (cpu_mhz, gpu_mhz, mem_mhz; 0 leaves the system's), set through
// clkrst (pcv before 8.0.0) and put back as they were when the title exits. Capped at the console's own top rates:
// a rate above them needs an overclocking sysmodule, which this does not ask for.
#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/log.h"
#include "host.h"
#include "kernel/kernel.h"

namespace {

struct Domain {
    PcvModuleId id;   // clkrst, 8.0.0 on
    PcvModule old;    // pcv, before
    const char* name;
    uint32_t max_mhz;
    uint32_t want_mhz = 0;     // 0: the system's
    uint32_t original_hz = 0;  // read before the first change, put back at exit
};

Domain g_domains[3] = {
    {PcvModuleId_CpuBus, PcvModule_CpuBus, "cpu", 1785},
    {PcvModuleId_GPU, PcvModule_GPU, "gpu", 921},
    {PcvModuleId_EMC, PcvModule_EMC, "mem", 1600},
};

bool UseClkrst() { return hosversionAtLeast(8, 0, 0); }

// ROTS: movie_boost, the CPU rate while a movie streams (0: off). The CRI Sofdec decoder is the title's own x86 code,
// translated: at the stock 1020 MHz it decodes about 29 frames a second of the 29.97 the movies run at, so they fell
// further behind their sound the longer they ran (Switch, 2026-11-16)
uint32_t g_movie_boost_mhz = 1785;
bool g_movie_boosting = false;

// ROTS: sys-clk with a profile for this title (or a global one) sets its own CPU rate every few hundred ms, so the
// movie boost held for well under a second at a time (the user's log: "cpu at 1020 MHz instead of 1785", every second
// of a movie). sys-clk's own override (its IPC "sys:clk", API 4: command 8 takes the module and Hz, 0 Hz clears it)
// wins over its profiles: the boost asks for it while a movie plays and clears it when the movie ends or the title
// exits. Asked only when sys-clk is running (checked without waiting for it) and speaks API 4.
Service g_sysclk;
bool g_sysclk_tried = false, g_sysclk_open = false, g_sysclk_overriding = false;

bool SysClkOpen() {
    if (g_sysclk_tried) return g_sysclk_open;
    g_sysclk_tried = true;
    // Atmosphere's sm extension AtmosphereHasService (65100): asking sm for a service nobody registered would wait
    // for it forever. sm speaks TIPC from 12.0.0 on, CMIF before
    const SmServiceName name = smEncodeName("sys:clk");
    u8 has = 0;
    Result rc = hosversionAtLeast(12, 0, 0) ? tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, has)
                                            : serviceDispatchInOut(smGetServiceSession(), 65100, name, has);
    if (R_FAILED(rc) || !has) return false;
    if (R_FAILED(smGetService(&g_sysclk, "sys:clk"))) return false;
    u32 api = 0;
    if (R_FAILED(serviceDispatchOut(&g_sysclk, 0, api)) || api != 4) {
        LOGR("clocks: sys-clk found, API %u (the override needs 4); not asked", api);
        serviceClose(&g_sysclk);
        return false;
    }
    g_sysclk_open = true;
    LOGR("clocks: sys-clk found; the movie boost asks it for an override");
    return true;
}

// module 0 is the CPU; hz 0 clears the override
bool SysClkOverride(u32 module, u32 hz) {
    if (!SysClkOpen()) return false;
    const struct {
        u32 module, hz;
    } in = {module, hz};
    Result rc = serviceDispatchIn(&g_sysclk, 8, in);
    if (R_FAILED(rc)) {
        LOGR("clocks: sys-clk override of %u Hz failed (0x%X)", hz, rc);
        return false;
    }
    g_sysclk_overriding = hz != 0;
    return true;
}

// what a domain is held at now: its asked rate, or the movie boost's when that is higher
uint32_t Want(const Domain& d) {
    if (g_movie_boosting && &d == &g_domains[0]) return std::max(d.want_mhz, g_movie_boost_mhz);
    return d.want_mhz;
}

uint32_t GetHz(const Domain& d) {
    uint32_t hz = 0;
    if (UseClkrst()) {
        ClkrstSession s;
        if (R_SUCCEEDED(clkrstOpenSession(&s, d.id, 3))) {
            clkrstGetClockRate(&s, &hz);
            clkrstCloseSession(&s);
        }
    } else {
        pcvGetClockRate(d.old, &hz);
    }
    return hz;
}

bool SetHz(const Domain& d, uint32_t hz) {
    Result rc;
    if (UseClkrst()) {
        ClkrstSession s;
        rc = clkrstOpenSession(&s, d.id, 3);
        if (R_SUCCEEDED(rc)) {
            rc = clkrstSetClockRate(&s, hz);
            clkrstCloseSession(&s);
        }
    } else {
        rc = pcvSetClockRate(d.old, hz);
    }
    return R_SUCCEEDED(rc);
}

bool g_open = false;

bool Open() {
    if (g_open) return true;
    Result rc = UseClkrst() ? clkrstInitialize() : pcvInitialize();
    if (R_FAILED(rc)) {
        LOGE("clocks: %s unavailable (0x%X)", UseClkrst() ? "clkrst" : "pcv", rc);
        return false;
    }
    g_open = true;
    return true;
}

}  // namespace

namespace host {

void SetClockWanted(const char* name, uint32_t mhz) {
    for (Domain& d : g_domains)
        if (!std::strcmp(d.name, name)) d.want_mhz = mhz;
}

void SetMovieBoost(uint32_t mhz) { g_movie_boost_mhz = mhz; }
uint32_t MovieBoost() { return g_movie_boost_mhz; }

uint32_t ClockWanted(const char* name) {
    for (const Domain& d : g_domains)
        if (!std::strcmp(d.name, name)) return d.want_mhz;
    return 0;
}

// at start and after each docked/handheld change, which puts the system's back
void ApplyClocks() {
    bool any = false;
    for (const Domain& d : g_domains) any |= d.want_mhz != 0;
    if (!any || !Open()) return;
    for (Domain& d : g_domains) {
        if (!d.want_mhz) continue;
        if (!d.original_hz) d.original_hz = GetHz(d);
        uint32_t mhz = std::min(d.want_mhz, d.max_mhz);
        bool ok = SetHz(d, mhz * 1000000u);
        LOGR("clocks: %s %u MHz asked%s, now %u MHz (was %u MHz)", d.name, d.want_mhz,
             mhz != d.want_mhz ? ", capped" : "", GetHz(d) / 1000000u, d.original_hz / 1000000u);
        if (!ok) LOGE("clocks: %s could not be set", d.name);
    }
}

// once a second: the system or a clock sysmodule (sys-clk, Horizon OC) may set its own rate after ours; it is logged
// and ours set again, so a log shows whether the asked clock held. BREXTREME: the three rates are logged at start and
// whenever one changes, asked or not, so a clock changed from Horizon OC mid-game shows in the log with its time
void WatchClocks() {
    static uint64_t next = 0;
    static uint32_t resets = 0, shown[3] = {};
    static bool failed = false;
    uint64_t now = armTicksToNs(armGetSystemTick());
    if (now < next) return;
    next = now + 1000000000ull;
    if (!g_open) {
        if (failed) return;
        failed = !Open();
        if (failed) return;
    }
    // a movie file read in the last 3 s: one is playing (Sofdec reads its stream about every 0.6 s)
    uint64_t movie = kernel::MovieReadMs(), now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now().time_since_epoch())
                                                       .count());
    bool boost = g_movie_boost_mhz && movie && now_ms - movie < 3000;
    if (boost != g_movie_boosting) {
        Domain& cpu = g_domains[0];
        g_movie_boosting = boost;
        if (boost && !cpu.original_hz) cpu.original_hz = GetHz(cpu);
        uint32_t hz = Want(cpu) ? std::min(Want(cpu), cpu.max_mhz) * 1000000u : cpu.original_hz;
        if (boost) SysClkOverride(0, std::min(g_movie_boost_mhz, cpu.max_mhz) * 1000000u);
        else if (g_sysclk_overriding) SysClkOverride(0, 0);
        if (hz) SetHz(cpu, hz);
        LOGR("clocks: movie %s, cpu %u MHz", boost ? "playing" : "over", GetHz(cpu) / 1000000u);
    }
    uint32_t mhz[3];
    for (int i = 0; i < 3; i++) mhz[i] = GetHz(g_domains[i]) / 1000000u;
    if (std::memcmp(mhz, shown, sizeof(mhz))) {
        std::memcpy(shown, mhz, sizeof(mhz));
        LOGR("clocks: now cpu %u, gpu %u, mem %u MHz", mhz[0], mhz[1], mhz[2]);
    }
    for (Domain& d : g_domains) {
        if (!Want(d)) continue;
        uint32_t want = std::min(Want(d), d.max_mhz), is = GetHz(d) / 1000000u;
        if (is == want) continue;
        ++resets;
        if (resets <= 10 || resets % 60 == 0)
            LOGR("clocks: %s at %u MHz instead of %u (set by the system or a clock sysmodule; %u times), set again", d.name,
                 is, want, resets);
        SetHz(d, want * 1000000u);
    }
}

void RestoreClocks() {
    if (g_sysclk_overriding) SysClkOverride(0, 0);
    if (g_sysclk_open) {
        serviceClose(&g_sysclk);
        g_sysclk_open = false;
    }
    if (!g_open) return;
    for (Domain& d : g_domains)
        if (d.original_hz) {
            SetHz(d, d.original_hz);
            d.original_hz = 0;
        }
    if (UseClkrst()) clkrstExit();
    else pcvExit();
    g_open = false;
}

}  // namespace host

// libnx calls this on the way out of a normal exit: the system's clocks back
extern "C" void userAppExit(void) { host::RestoreClocks(); }
