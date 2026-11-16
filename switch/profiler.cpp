// Sampling profiler (profile = true in the settings), after the one in R-drg's defjam-sw (src/switch_nx.c, MIT).
// The console has no profiler and the emulator's costs are not the console's. A thread above the game's wakes every
// millisecond, pauses each busy registered thread (svcSetThreadActivity), reads its pc, lr and up to five return
// addresses of the x29 chain (svcGetThreadContext3) and lets it run again. "Busy" is decided every 100 ms from the
// threads' tick counts: over 10 % of a core. Samples go to logs/prof.bin every 10 s as 32 byte records (thread slot,
// time, pc, lr, returns, as offsets into the NRO); the log names the slots. tools/prof_report.py turns them into
// tables per function with the ELF: nothing is resolved on the console. A few percent of one core while on.
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/log.h"
#include "host.h"
#include "kernel/kernel.h"

// the NRO's load address: _start is the first instruction, ELF address 0
extern "C" void _start(void);

namespace {

struct Sample {
    uint16_t slot, time;  // time in 10 ms units since the profiler started (wraps after 655 s)
    uint32_t pc, lr, ret[5];
};
static_assert(sizeof(Sample) == 32);

constexpr int kMaxThreads = 128;  // ROTS: every soft reboot makes the title's threads again
constexpr unsigned kMaxSamples = 60000;  // 10 s at 1 kHz of up to six busy threads

std::mutex g_lock;
Handle g_handles[kMaxThreads];
char g_names[kMaxThreads][24];
int g_count = 0;

Sample g_samples[kMaxSamples];
unsigned g_n = 0;
uint64_t g_t0 = 0;
std::string g_path;
Thread g_thread;

uint32_t Offset(uint64_t a) {
    uint64_t base = reinterpret_cast<uintptr_t>(&_start);
    return a >= base && a - base < 0xFFFFFFFFull ? uint32_t(a - base) : 0xFFFFFFFFu;
}

void Flush() {
    if (!g_n) return;
    if (FILE* f = std::fopen(g_path.c_str(), "ab")) {
        std::fwrite(g_samples, sizeof(Sample), g_n, f);
        std::fclose(f);
    }
    g_n = 0;
}

void LogSlots() {
    std::string line = "prof: slots";
    std::lock_guard lock(g_lock);
    for (int i = 0; i < g_count; i++) line += " " + std::to_string(i) + "=" + g_names[i];
    LOGR("%s", line.c_str());
}

void Loop(void*) {
    Handle h[kMaxThreads];
    uint64_t last[kMaxThreads] = {}, t_list = 0, t_flush = armGetSystemTick();
    bool busy[kMaxThreads] = {};
    int n = 0;
    const uint64_t freq = armGetSystemTickFreq();
    for (;;) {
        svcSleepThread(1000000ull);
        uint64_t now = armGetSystemTick();
        if (now - t_list > freq / 10) {
            {
                std::lock_guard lock(g_lock);
                n = g_count;
                std::memcpy(h, g_handles, sizeof(Handle) * size_t(n));
            }
            uint64_t span = now - t_list;
            for (int i = 0; i < n; i++) {
                uint64_t t = 0;
                if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, h[i], uint64_t(-1)))) t = 0;
                busy[i] = t_list && (t - last[i]) * 10 > span;  // thread ticks run at the system tick's 19.2 MHz
                last[i] = t;
            }
            t_list = now;
        }
        for (int i = 0; i < n && g_n < kMaxSamples; i++) {
            if (!busy[i] || R_FAILED(svcSetThreadActivity(h[i], ThreadActivity_Paused))) continue;
            ThreadContext ctx;
            if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h[i]))) {
                Sample& o = g_samples[g_n++];
                o.slot = uint16_t(i);
                o.time = uint16_t((now - g_t0) / (freq / 100));
                o.pc = Offset(ctx.pc.x);
                o.lr = Offset(ctx.lr);
                std::memset(o.ret, 0xFF, sizeof(o.ret));
                // the x29 chain inside the stack's own mapping: the thread is paused and its stack is ours
                MemoryInfo mi;
                u32 pi;
                if (!(ctx.fp & 7) && R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.fp)) && (mi.perm & Perm_R)) {
                    uint64_t fp = ctx.fp, lo = mi.addr, hi = mi.addr + mi.size;
                    for (int k = 0; k < 5 && fp >= lo && fp + 16 <= hi && !(fp & 7); k++) {
                        const uint64_t* frame = reinterpret_cast<const uint64_t*>(fp);
                        o.ret[k] = Offset(frame[1]);
                        if (frame[0] <= fp) break;
                        fp = frame[0];
                    }
                }
            }
            svcSetThreadActivity(h[i], ThreadActivity_Runnable);
        }
        if (now - t_flush > freq * 10) {
            LogSlots();
            Flush();
            t_flush = armGetSystemTick();
        }
    }
}

}  // namespace

// every named host thread (guest threads, the render thread) offers itself; read only while profiling
extern "C" void sheroes_profile_thread(const char* name) {
    {
        // where Horizon runs it: a thread made without a core mask stays on the core it was made on
        s32 core = -1, prio = -1;
        u64 mask = 0;
        svcGetThreadCoreMask(&core, &mask, CUR_THREAD_HANDLE);
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        LOGR("threads: %s on core %d (mask %llX), priority %X", name, int(core), (unsigned long long)mask, unsigned(prio));
    }
    std::lock_guard lock(g_lock);
    if (g_count >= kMaxThreads) return;
    g_handles[g_count] = threadGetCurHandle();
    std::snprintf(g_names[g_count], sizeof(g_names[0]), "%s", name);
    for (char* c = g_names[g_count]; *c; c++)
        if (*c == ' ' || *c == '=') *c = '_';
    g_count++;
}

namespace {

// ROTS: the freeze watchdog. No frame shown for 3 s: every registered thread's pc, lr and x29 chain go to the log, as
// offsets into the NRO (tools/stall_report.py names them with the build's ELF). A Switch freeze in mission 1 left no
// trace in the log and does not happen on the PC (2026-11-16)
std::atomic<uint64_t> g_last_present{0}, g_presents{0};

void DumpThreads(const char* why) {
    Handle h[kMaxThreads];
    char names[kMaxThreads][24];
    int n;
    {
        std::lock_guard lock(g_lock);
        n = g_count;
        std::memcpy(h, g_handles, sizeof(Handle) * size_t(n));
        std::memcpy(names, g_names, sizeof(names[0]) * size_t(n));
    }
    // all contexts first, then the log: a paused thread may hold the log's lock
    struct Ctx {
        bool ok;
        uint32_t rc;
        uint64_t pc, lr, ret[24];
        int depth;
    };
    static Ctx c[kMaxThreads];
    for (int i = 0; i < n; i++) {
        c[i] = {};
        Result pr = svcSetThreadActivity(h[i], ThreadActivity_Paused);
        c[i].rc = pr;
        if (R_FAILED(pr)) continue;
        ThreadContext ctx;
        if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h[i]))) {
            c[i].ok = true;
            c[i].pc = ctx.pc.x;
            c[i].lr = ctx.lr;
            MemoryInfo mi;
            u32 pi;
            if (!(ctx.fp & 7) && R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx.fp)) && (mi.perm & Perm_R)) {
                uint64_t fp = ctx.fp, lo = mi.addr, hi = mi.addr + mi.size;
                for (int k = 0; k < 24 && fp >= lo && fp + 16 <= hi && !(fp & 7); k++) {
                    const uint64_t* frame = reinterpret_cast<const uint64_t*>(fp);
                    c[i].ret[c[i].depth++] = frame[1];
                    if (frame[0] <= fp) break;
                    fp = frame[0];
                }
            }
        }
        svcSetThreadActivity(h[i], ThreadActivity_Runnable);
    }
    LOGR("stall: %s; NRO base %p, offsets below are into rots.nro", why, reinterpret_cast<void*>(&_start));
    // the threads that could not be paused: ended ones (their handle is gone), or not pausable
    std::string gone;
    for (int i = 0; i < n; i++)
        if (!c[i].ok) gone += std::string(gone.empty() ? "" : ", ") + names[i] + " (" + std::to_string(c[i].rc & 0x3FFFFF) + ")";
    if (!gone.empty()) LOGR("stall: not paused: %s", gone.c_str());
    // the title's threads as the kernel sees them: what each waits for, a critical section's owner
    for (const std::string& line : kernel::DescribeThreads()) LOGR("stall: %s", line.c_str());
    for (int i = 0; i < n; i++) {
        if (!c[i].ok) continue;
        char line[512];
        int at = std::snprintf(line, sizeof line, "stall: %s pc %08X lr %08X:", names[i], Offset(c[i].pc), Offset(c[i].lr));
        for (int k = 0; k < c[i].depth && at < (int)sizeof line - 12; k++)
            at += std::snprintf(line + at, sizeof line - size_t(at), " %08X", Offset(c[i].ret[k]));
        LOGR("%s", line);
    }
    logx::Flush();
}

void Watchdog(void*) {
    const uint64_t freq = armGetSystemTickFreq();
    uint64_t dumped_at = 0;
    int dumps = 0;
    for (;;) {
        svcSleepThread(500000000ull);
        uint64_t last = g_last_present.load(), now = armGetSystemTick();
        if (g_presents.load() < 120 || !last) continue;
        uint64_t stalled = now - last;
        if (stalled < freq * 3) {
            dumped_at = 0;
            continue;
        }
        // once at 3 s, again at 15 s (did anything move), then quiet until frames come back; at most 6 a session
        bool again = dumped_at == 1 && stalled > freq * 15;
        if ((dumped_at == 0 || again) && dumps < 6) {
            char why[64];
            std::snprintf(why, sizeof why, "no frame for %.1f s", double(stalled) / double(freq));
            DumpThreads(why);
            dumps++;
            dumped_at = dumped_at == 0 ? 1 : 2;
        }
    }
}

Thread g_watchdog;

}  // namespace

namespace host {

void NotePresent() {
    g_last_present.store(armGetSystemTick(), std::memory_order_relaxed);
    g_presents.fetch_add(1, std::memory_order_relaxed);
}

void StartWatchdog() {
    if (R_FAILED(threadCreate(&g_watchdog, Watchdog, nullptr, nullptr, 0x8000, 0x2A, -2)) ||
        R_FAILED(threadStart(&g_watchdog)))
        LOGE("stall: the freeze watchdog could not start");
}

void StartProfiler() {
    g_path = (app::ExeDir() / "logs" / "prof.bin").string();
    std::remove(g_path.c_str());
    g_t0 = armGetSystemTick();
    // 0x2A: above every game and host thread, so it samples on time
    if (R_FAILED(threadCreate(&g_thread, Loop, nullptr, nullptr, 0x4000, 0x2A, -2)) || R_FAILED(threadStart(&g_thread))) {
        LOGE("prof: the profiler thread could not start");
        return;
    }
    LOGR("prof: sampling busy threads at 1 kHz into logs/prof.bin (NRO base %p)", reinterpret_cast<void*>(&_start));
}

}  // namespace host
