// ROTS: a sampling profiler for the headless harness. HARNESS_PROFILE=1 samples the CPU time of the whole process every
// millisecond (SIGPROF), keeps the program counter of the thread that took the signal, and writes the counts to
// profile.txt in the run folder every 10 s (address and count, most first). tools/harness: symbolize with addr2line.
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kRing = 1 << 20;
uint64_t g_ring[kRing];
std::atomic<uint64_t> g_head{0};

void OnProf(int, siginfo_t*, void* uc) {
    const uint64_t pc = uint64_t(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]);
    g_ring[g_head.fetch_add(1, std::memory_order_relaxed) % kRing] = pc;
}

void Dump() {
    std::unordered_map<uint64_t, uint64_t> counts;
    const uint64_t n = std::min<uint64_t>(g_head.load(), kRing);
    for (uint64_t i = 0; i < n; i++) counts[g_ring[i]]++;
    std::vector<std::pair<uint64_t, uint64_t>> v(counts.begin(), counts.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    if (FILE* f = std::fopen("profile.txt", "w")) {
        std::fprintf(f, "# %llu samples\n", (unsigned long long)n);
        for (auto& [pc, c] : v) std::fprintf(f, "%llx %llu\n", (unsigned long long)pc, (unsigned long long)c);
        std::fclose(f);
    }
}

const bool kStarted = [] {
    if (!std::getenv("HARNESS_PROFILE")) return false;
    struct sigaction sa = {};
    sa.sa_sigaction = OnProf;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    itimerval t = {{0, 1000}, {0, 1000}};
    setitimer(ITIMER_PROF, &t, nullptr);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            Dump();
        }
    }).detach();
    return true;
}();

}  // namespace
