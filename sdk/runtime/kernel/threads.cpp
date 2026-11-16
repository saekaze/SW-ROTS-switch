#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "core/log.h"
#include <vector>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace kernel {

namespace {

thread_local ThreadRef t_current;
std::mutex g_threads_lock;
std::vector<ThreadRef> g_threads;
std::atomic<uint32_t> g_next_id{0x10};

// guest structures per thread
constexpr uint32_t kKpcrSize = 0x400;
constexpr uint32_t kEthreadSize = 0x200;

void SetupGuestBlocks(Thread& t, uint32_t stack_size, uint32_t tls_size) {
    stack_size = std::max<uint32_t>(stack_size, 0x10000);
    tls_size = (tls_size + 3) & ~3u;
    t.stack_top = mem::AllocStack(stack_size + tls_size + 0x1000);
    t.stack_size = stack_size;
    t.kpcr = mem::AllocSystem(kKpcrSize, 64);
    t.ethread = mem::AllocSystem(kEthreadSize, 64);
    // TLS sits at the top of the stack: StackBase + tls_index * 4 must equal TlsData
    t.tls = t.stack_top - tls_size;
    uint32_t k = t.kpcr;
    W32(k + 0x00, 0xFFFFFFFF);                  // NtTib.ExceptionList
    W32(k + 0x04, t.stack_top);                 // NtTib.StackBase
    W32(k + 0x08, t.stack_top - stack_size);    // NtTib.StackLimit
    W32(k + 0x18, k);                           // NtTib.Self
    W32(k + 0x1C, k);                           // SelfPcr
    W32(k + 0x20, k + 0x28);                    // Prcb
    G<uint8_t>(k + 0x24) = 0;                   // Irql
    W32(k + 0x28, t.ethread);                   // PrcbData.CurrentThread
    uint32_t e = t.ethread;
    G<uint8_t>(e + 0) = 6;                      // dispatcher type ThreadObject
    G<uint8_t>(e + 2) = uint8_t(kEthreadSize / 4);  // Header.Size in dwords
    W32(e + 0x1C, t.stack_top);                 // KTHREAD.StackBase
    W32(e + 0x20, t.stack_top - stack_size);    // KTHREAD.StackLimit
    W32(e + 0x28, t.tls);                       // KTHREAD.TlsData
    W32(e + 0x12C, t.id);                       // ETHREAD.UniqueThread
}

void InitContext(Thread& t, X86Context& ctx) {
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.esp = (t.tls & ~15u) - 16;
    ctx.fs_base = t.kpcr;
    ctx.fcw = 0x27F;  // x87 53 bit precision, exceptions masked
    ctx.mxcsr = 0x1F80;  // SSE exceptions masked, round to nearest
    ctx.thread_handle = 0;
    t.ctx = &ctx;
}

void Register(const ThreadRef& t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.push_back(t);
}

void Unregister(Thread* t) {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    g_threads.erase(std::remove_if(g_threads.begin(), g_threads.end(), [t](const ThreadRef& r) { return r.get() == t; }),
                    g_threads.end());
}

void MarkExited(Thread& t, uint32_t code) {
    t.exit_code = code;
    std::lock_guard<std::mutex> lock(DispatchLock());
    SetSignal(t, 1);
    SignalChanged();
}

#ifdef __SWITCH__
extern "C" void sheroes_profile_thread(const char* name) __attribute__((weak));
extern "C" void sheroes_place_thread(int role) __attribute__((weak));  // switch/cores.cpp
#endif

void NameHostThread(const char* name) {
#ifdef _WIN32
    wchar_t w[64];
    size_t i = 0;
    for (; name[i] && i < 63; i++) w[i] = wchar_t(name[i]);
    w[i] = 0;
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static Fn set = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set) set(GetCurrentThread(), w);
#elif defined(__SWITCH__)
    // newlib has no pthread_setname_np; the Switch layer's profiler keeps the name and handle (switch/profiler.cpp)
    if (sheroes_profile_thread) sheroes_profile_thread(name);
    if (sheroes_place_thread) sheroes_place_thread(2);  // off the title thread's core; it moves itself back
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

struct SpawnExit {};  // ExitThread: back to the thread's root

// recompiled code nests host frames as deep as the guest's calls, give it room
void Spawn(std::function<void()> fn) {
    auto* f = new std::function<void()>(std::move(fn));
#ifdef _WIN32
    auto entry = [](void* p) -> DWORD {
        std::unique_ptr<std::function<void()>> g(static_cast<std::function<void()>*>(p));
        (*g)();
        return 0;
    };
    HANDLE h = ::CreateThread(nullptr, 64u << 20, entry, f, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (h) ::CloseHandle(h);
#else
    // ROTS: finished threads are joined here, at the next spawn, instead of detached. libnx has no detach: a
    // detached thread's stack was never given back, and on the Switch a stack is real memory, not a reservation.
    // Revenge of the Sith starts four decoder threads per movie; by the second movie thread creation failed, the
    // decoder stalled and its sound looped (console, 2026-11-16).
    struct Running {
        pthread_t th;
        std::shared_ptr<std::atomic<bool>> done;
    };
    static std::mutex reap_lock;
    static std::vector<Running> running;
    {
        std::lock_guard<std::mutex> lock(reap_lock);
        for (auto it = running.begin(); it != running.end();) {
            if (it->done->load(std::memory_order_acquire)) {
                pthread_join(it->th, nullptr);
                it = running.erase(it);
            } else {
                ++it;
            }
        }
    }
    struct Start {
        std::function<void()>* fn;
        std::shared_ptr<std::atomic<bool>> done;
    };
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto* start = new Start{f, done};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef __SWITCH__
    pthread_attr_setstacksize(&attr, 32u << 20);  // real memory on the Switch; the xboxrecomp port ran this game on 8
#else
    pthread_attr_setstacksize(&attr, 64u << 20);
#endif
    pthread_t th;
    int rc = pthread_create(&th, &attr, [](void* p) -> void* {
        std::unique_ptr<Start> st(static_cast<Start*>(p));
        {
            std::unique_ptr<std::function<void()>> g(st->fn);
            try {
                (*g)();
            } catch (const SpawnExit&) {
            }
        }
        st->done->store(true, std::memory_order_release);
        return nullptr;
    }, start);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        LOGE("threads: a host thread could not be created (%d)", rc);
        delete start;
        delete f;
        return;
    }
    std::lock_guard<std::mutex> lock(reap_lock);
    running.push_back({th, done});
#endif
}

}  // namespace

Thread* Current() { return t_current.get(); }

void NoteCall(const char* what) {
    if (Thread* t = t_current.get()) t->Note(what);
}

std::vector<std::string> DescribeThreads() {
    std::vector<std::string> out;
    std::vector<ThreadRef> all;
    {
        std::lock_guard<std::mutex> lock(g_threads_lock);
        all = g_threads;
    }
    auto by_ethread = [&](uint32_t e) -> uint32_t {
        for (auto& t : all)
            if (t && t->ethread == e) return t->id;
        return 0;
    };
    for (auto& t : all) {
        if (!t || !t->guest) continue;
        char b[512];
        int at = std::snprintf(b, sizeof b, "thread %X (from %08X): ", t->id, t->start_routine);
        if (!t->waiting.load(std::memory_order_acquire)) {
            at += std::snprintf(b + at, sizeof b - size_t(at), "running");
        } else if (uint32_t cs = t->wait_cs) {
            uint32_t owner = R32(cs + 0x18);
            at += std::snprintf(b + at, sizeof b - size_t(at),
                                "waits to enter critical section %08X, owned by thread %X (ethread %08X, recursion %u, "
                                "lock count %d)",
                                cs, by_ethread(owner), owner, R32(cs + 0x14), int32_t(R32(cs + 0x10)));
        } else {
            at += std::snprintf(b + at, sizeof b - size_t(at), "waits%s for", t->wait_timeout == INT64_MAX ? "" : " (timed)");
            for (uint32_t i = 0; i < t->wait_n && at < (int)sizeof b - 32; i++)
                at += std::snprintf(b + at, sizeof b - size_t(at), " %u:%08X", t->wait_type[i], t->wait_obj[i]);
        }
        out.push_back(std::string(b) + "; last calls: " + RecentCalls(*t));
    }
    return out;
}

std::string RecentCalls(const Thread& t) {
    std::string out;
    uint32_t n = t.recent_n, first = n > 16 ? n - 16 : 0;
    for (uint32_t i = first; i < n; i++) {
        if (!out.empty()) out += ", ";
        out += t.recent[i & 15] ? t.recent[i & 15] : "?";
    }
    return out.empty() ? "none" : out;
}
ThreadRef CurrentRef() { return t_current; }
X86Context& CurrentContext() { return *t_current->ctx; }

std::vector<ThreadRef> Threads() {
    std::lock_guard<std::mutex> lock(g_threads_lock);
    return g_threads;
}

// STREET: the title's priorities (KeSetBasePriorityThread: an increment on normal, 16 for time critical) on the
// Switch's threads. With every thread at 0x3B the main thread could run ahead of FIFA's time critical audio thread,
// which on the Xbox always ran first: it set a sound channel's volume before the audio thread had made the channel's
// voice (0x220C48, a call through a null object) whenever a thread was held back (a long frame, the profiler).
void ApplyPriority(Thread& t) {
#ifdef __SWITCH__
    uint32_t h = t.host;
    if (!h) return;
    int p = t.priority;
    u32 prio = p >= 16 ? 0x30 : p <= -16 ? 0x3F : u32(0x3B - std::clamp(p, -2, 2));
    Result rc = svcSetThreadPriority(Handle(h), prio);
    LOGI("threads: guest %X priority %d -> %X%s", t.id, p, unsigned(prio), R_FAILED(rc) ? " (refused)" : "");
#else
    (void)t;
#endif
}

struct StartInfo {
    uint32_t system_routine, start_routine, start_context;
};
static std::unordered_map<Thread*, StartInfo> g_start;
static std::mutex g_start_lock;

ThreadRef CreateThread(uint32_t stack_size, uint32_t tls_size, uint32_t system_routine, uint32_t start_routine,
                       uint32_t start_context, bool suspended) {
    auto t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, stack_size, tls_size);
    Bind(t, t->ethread);
    if (suspended) t->suspend = 1;
    std::lock_guard<std::mutex> lock(g_start_lock);
    g_start[t.get()] = {system_routine, start_routine, start_context};
    return t;
}

void StartThread(ThreadRef t) {
    t->guest = true;
    Register(t);
    Spawn([t] {
        t_current = t;
        mem::SetGuestOwned(true);  // ROTS: what a title thread allocates goes back at a soft reboot
        char name[32];
        std::snprintf(name, sizeof(name), "guest %X", t->id);
        NameHostThread(name);
#ifdef __SWITCH__
        t->host = threadGetCurHandle();
        ApplyPriority(*t);  // a priority set while it was suspended
#endif
        X86Context ctx;
        InitContext(*t, ctx);
        {
            std::unique_lock<std::mutex> lock(t->apc_lock);
            t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0 || Rebooting(); });
        }
        StartInfo si;
        {
            std::lock_guard<std::mutex> lock(g_start_lock);
            si = g_start[t.get()];
            g_start.erase(t.get());
        }
        t->start_routine = si.start_routine;
        try {
            RebootPoint();
            LOGI("thread %X starts at %08X (%08X, %08X)", t->id, si.system_routine ? si.system_routine : si.start_routine,
                 si.start_routine, si.start_context);
            uint32_t code;
            if (si.system_routine)
                code = CallGuest(ctx, si.system_routine, {si.start_routine, si.start_context});
            else
                code = CallGuest(ctx, si.start_routine, {si.start_context});
            // ROTS: report level, so the Switch's log has it: a title thread that ends outside a reboot
            LOGR("threads: title thread %X (from %08X) returned %08X; its last calls: %s", t->id,
                 si.system_routine ? si.start_routine : si.start_routine, code, RecentCalls(*t).c_str());
            MarkExited(*t, code);
        } catch (const GuestReboot&) {
            LOGI("thread %X ends for the reboot", t->id);
        } catch (const SpawnExit&) {
            throw;
        } catch (...) {
            LOGR("threads: title thread %X (from %08X) ended by an exception; its last calls: %s", t->id,
                 si.start_routine, RecentCalls(*t).c_str());
            logx::Flush();
            throw;
        }
        Unregister(t.get());
        t_current.reset();
    });
}

// ROTS soft reboot

namespace {
std::atomic<bool> g_reboot{false};
std::atomic<int> g_host_in_guest{0};
std::mutex g_hooks_lock;
std::vector<std::function<void()>> g_hooks;
}  // namespace

void WakeForReboot();  // objects.cpp
void ResetObjects();

bool Rebooting() { return g_reboot.load(std::memory_order_acquire); }

void RebootPoint() {
    if (!g_reboot.load(std::memory_order_relaxed)) return;
    Thread* t = Current();
    if (t && t->guest) throw GuestReboot{};
}

bool HostGuestEnter() {
    g_host_in_guest.fetch_add(1);
    if (g_reboot.load()) {
        g_host_in_guest.fetch_sub(1);
        return false;
    }
    return true;
}
void HostGuestLeave() { g_host_in_guest.fetch_sub(1); }

void OnReboot(std::function<void()> reset) {
    std::lock_guard<std::mutex> lock(g_hooks_lock);
    g_hooks.push_back(std::move(reset));
}

bool SoftRebootEnabled() {
#ifdef __SWITCH__
    return true;
#else
    const char* e = std::getenv("XBE_SOFT_REBOOT");
    return e && *e == '1';
#endif
}

static void RebootMain() {
    using namespace std::chrono;
    NameHostThread("reboot");
    auto t0 = steady_clock::now();
    int reported = 0;
    for (;;) {
        int guests = 0;
        for (auto& t : Threads()) guests += t->guest;
        if (!guests && g_host_in_guest.load() == 0) break;
        auto s = duration_cast<seconds>(steady_clock::now() - t0).count();
        if (s >= 5 * (reported + 1)) {
            reported++;
            for (auto& t : Threads())
                if (t->guest) LOGW("reboot: title thread %X has not ended after %lld s", t->id, (long long)s);
            if (g_host_in_guest.load()) LOGW("reboot: %d host calls into the title still running", g_host_in_guest.load());
            WakeForReboot();
            for (auto& t : Threads()) {
                std::lock_guard<std::mutex> l(t->apc_lock);
                t->resume_cv.notify_all();
            }
        }
        if (s >= 30) {
            LOGE("reboot: going on with title threads still running");
            break;
        }
        std::this_thread::sleep_for(milliseconds(2));
    }
    {
        std::lock_guard<std::mutex> lock(g_start_lock);
        g_start.clear();
    }
    std::vector<std::function<void()>> hooks;
    {
        std::lock_guard<std::mutex> lock(g_hooks_lock);
        hooks = g_hooks;
    }
    for (auto& h : hooks) h();
    ResetObjects();
    size_t freed = mem::FreeGuestOwned();
    LOGR("reboot: the title's threads ended in %lld ms, %zu KB of its memory given back",
         (long long)duration_cast<milliseconds>(steady_clock::now() - t0).count(), freed >> 10);
    g_reboot.store(false, std::memory_order_release);
    if (!RestartImage()) LOGE("reboot: the image did not start again");
}

void RequestReboot() {
    bool expected = false;
    if (g_reboot.compare_exchange_strong(expected, true)) {
        LOGR("reboot: quick reboot into the title's own image, in this process");
        logx::Flush();
        WakeForReboot();
        for (auto& t : Threads()) {
            std::lock_guard<std::mutex> l(t->apc_lock);
            t->resume_cv.notify_all();
        }
        std::thread(RebootMain).detach();
    }
    throw GuestReboot{};
}

void StartHostThread(const char* name, std::function<void(X86Context&)> fn) {
    auto t = std::make_shared<Thread>();
    t->id = g_next_id.fetch_add(4);
    SetupGuestBlocks(*t, 0x20000, 0x100);
    Bind(t, t->ethread);
    std::string n = name;
    Spawn([t, fn, n] {
        t_current = t;
        NameHostThread(n.c_str());
        X86Context ctx;
        InitContext(*t, ctx);
        fn(ctx);
        t_current.reset();
    });
}

void ExitThread(uint32_t code) {
    Thread* t = Current();
    if (t) {
        LOGR("threads: title thread %X (from %08X) exits with %08X; its last calls: %s", t->id, t->start_routine, code,
             RecentCalls(*t).c_str());
        MarkExited(*t, code);
        Unregister(t);
    }
    t_current.reset();
#ifdef _WIN32
    ::ExitThread(code);
#else
    // ROTS: unwound to Spawn's root, which marks the thread done so its stack is joined and given back
    // (pthread_exit skipped that, and on the Switch it does not unwind at all)
    throw SpawnExit{};
#endif
}

void Resume(Thread& t) {
    std::lock_guard<std::mutex> lock(t.apc_lock);
    if (t.suspend > 0) t.suspend--;
    t.resume_cv.notify_all();
}

uint32_t Suspend(Thread& t) {
    uint32_t prev = uint32_t(t.suspend++);
    if (&t == Current()) SuspendPoint();
    return prev;
}

void SuspendPoint() {
    Thread* t = Current();
    if (!t || t->suspend.load() <= 0) return;
    {
        std::unique_lock<std::mutex> lock(t->apc_lock);
        t->resume_cv.wait(lock, [&] { return t->suspend.load() <= 0 || Rebooting(); });
    }
    RebootPoint();
}

void QueueApc(Thread& t, uint32_t routine, uint32_t a1, uint32_t a2, uint32_t a3) {
    std::lock_guard<std::mutex> lock(t.apc_lock);
    t.apcs.push_back({routine, a1, a2, a3});
    SignalChanged();
}

bool RunApcs(Thread& t) {
    bool ran = false;
    for (;;) {
        std::array<uint32_t, 4> a;
        {
            std::lock_guard<std::mutex> lock(t.apc_lock);
            if (t.apcs.empty()) break;
            a = t.apcs.front();
            t.apcs.pop_front();
        }
        CallGuest(*t.ctx, a[0], {a[1], a[2], a[3]});
        ran = true;
    }
    return ran;
}

}  // namespace kernel
