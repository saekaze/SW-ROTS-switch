// The Direct3D 12 backend on a thread of its own. The title's render thread records each call with what the console
// takes at call time (state changes, resource headers, indices, rects) and goes on; vertex, texture and surface memory
// are read when the draw runs, as the console's GPU reads them after the call returns. The XDK's fences, busy checks
// and locks wait for that.
#ifdef __SWITCH__
// the Switch (ARM): NEON in place of SSE, the thread unnamed (Black-Android's src/d3d/render_thread.cpp)
#include <arm_neon.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <smmintrin.h>
#endif

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/diag.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace gpu {

namespace {

// commands handed over at once, so the render thread keeps close behind and a wait is short. BREXTREME: 256 (was 64):
// each hand-over locks, signals and often meets the render thread holding the lock, ~1 % of the title thread at ~80 a
// frame on the console; the render thread has time to spare
constexpr uint32_t kBatchCommands = 256;

// resource headers the render thread reads: common, data, lock, format, size
constexpr uint32_t kHeaderBytes = 20, kHeaderSlot = 32;
constexpr uint32_t kHeaderRing = 1u << 20;  // guest memory for header copies; a copy is renewed after half a ring
// BREXTREME: a header in a State command's bytes: its field's offset with this bit, 4 + kHeaderBytes, then the
// header's address and its bytes as the title thread read them (Snapshot); the render thread makes the copy (Header)
constexpr uint32_t kHeaderRecord = 0x80000000u;
// a group of headers is sent again after this many draws even unset, so the render thread renews each copy it reads
// before the ring comes round (256 draws make at most ~26 copies each of 32 bytes: far below half the ring)
constexpr uint64_t kGroupRefresh = 256;

enum class WaitFor : uint8_t { ReadBack, Resource, Fence, Flush, Copy, Frame, Count };

enum class Op : uint8_t { State, Draw, Clear, Copy, Present, Sync, RecordLate, Flush, Gamma, ClearReport, GetReport };

struct Cmd {
    Op op;
    uint32_t a = 0, b = 0, c = 0, d = 0;
    float f = 0;
    uint32_t data = 0, size = 0;  // bytes in Batch::bytes
    int32_t vs = -1, ps = -1;     // index in Batch::vs / ps: the shader changed
    Surface s1, s2;
};

// a draw's vertex copy of one stream: its span and where its bytes are in Batch::bytes
struct SpanAt {
    StreamSpan span;
    uint32_t at;
};

// grows without zeroing: every byte added is written at once
template <class T>
struct NoInit : std::allocator<T> {
    template <class U>
    struct rebind {
        using other = NoInit<U>;
    };
    template <class U>
    void construct(U* p) noexcept {
        ::new (static_cast<void*>(p)) U;
    }
    template <class U, class... A>
    void construct(U* p, A&&... a) {
        ::new (static_cast<void*>(p)) U(std::forward<A>(a)...);
    }
};

struct Batch {
    uint64_t id = 0;
    std::vector<Cmd> cmds;
    std::vector<uint8_t, NoInit<uint8_t>> bytes;  // Op::Draw with size != 0: its vertex copies' record at size - 1
    std::vector<std::shared_ptr<VertexShader>> vs;
    std::vector<std::shared_ptr<PixelShader>> ps;
};

// the plain data in State, around its shader pointers and the resource headers (sent as copies); of the transforms only
// view, projection, texture and world are drawn with
struct Range {
    size_t off, len;
};
size_t At(const State& s, const void* p) { return size_t(static_cast<const uint8_t*>(p) - reinterpret_cast<const uint8_t*>(&s)); }
std::vector<Range> StateRanges() {
    static const State s;
    return {
        {At(s, &s.simple), At(s, &s.textures) - At(s, &s.simple)},
        {At(s, &s.index_buffer), At(s, &s.vs) - At(s, &s.index_buffer)},
        {At(s, &s.vsc), At(s, &s.rt) - At(s, &s.vsc)},
        {At(s, &s.scissor_count), At(s, &s.transforms) - At(s, &s.scissor_count)},
        {At(s, &s.transforms[2]), 2 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[16]), 4 * sizeof(s.transforms[0])},
        {At(s, &s.transforms[256]), sizeof(s.transforms[0])},
        {At(s, &s.material), sizeof(State) - At(s, &s.material)},
    };
}

// equal bytes, 16 at a time: most of the state is compared on every draw
bool Same(const uint8_t* a, const uint8_t* b, size_t n) {
    size_t i = 0;
#ifdef __SWITCH__
    uint8x16_t diff = vdupq_n_u8(0);
    for (; i + 16 <= n; i += 16) diff = vorrq_u8(diff, veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));
    // BREXTREME: a tail behind 16 bytes or more is the last 16 bytes again, overlapping: one load each instead of the
    // word loop below (~1 % of the title thread on the console)
    if (i < n && n >= 16) {
        diff = vorrq_u8(diff, veorq_u8(vld1q_u8(a + n - 16), vld1q_u8(b + n - 16)));
        i = n;
    }
    if (vmaxvq_u8(diff)) return false;
#else
    __m128i diff = _mm_setzero_si128();
    for (; i + 16 <= n; i += 16)
        diff = _mm_or_si128(diff, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i)),
                                                _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i))));
    if (!_mm_testz_si128(diff, diff)) return false;
#endif
    // the tail inline (most pieces end off a 16 byte boundary): memcmp for a few bytes was a call, ~1.5 % of the title
    // thread on the console
    for (; i + 8 <= n; i += 8) {
        uint64_t x, y;
        std::memcpy(&x, a + i, 8);
        std::memcpy(&y, b + i, 8);
        if (x != y) return false;
    }
    for (; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

using Clock = std::chrono::steady_clock;
uint64_t Ns(Clock::time_point t) { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count()); }

// the recording's own timing. BREXTREME: on the Switch the system counter, one register read (as libnx's
// armGetSystemTick, 19.2 MHz): steady_clock::now, twice a draw, was 1.5 % of the title thread on the console
#ifdef __SWITCH__
using Stamp = uint64_t;
Stamp StampNow() {
    uint64_t t;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
}
uint64_t StampNs(Stamp t) { return (StampNow() - t) * 625 / 12; }
#else
using Stamp = Clock::time_point;
Stamp StampNow() { return Clock::now(); }
uint64_t StampNs(Stamp t) { return Ns(t); }
#endif

#ifdef __SWITCH__
extern "C" void sheroes_profile_thread(const char* name) __attribute__((weak));  // switch/profiler.cpp
extern "C" void sheroes_place_thread(int role) __attribute__((weak));  // switch/cores.cpp
#endif

class Threaded final : public Backend {
public:
    explicit Threaded(std::unique_ptr<Backend> inner) : inner_(std::move(inner)), ranges_(StateRanges()) {
        g_vsc_generation_valid = true;  // every state the inner backend sees comes through Execute
        ring_ = mem::AllocSystem(kHeaderRing, 64);
        if (std::getenv("SHEROES_TRACK_STATE")) g_track_state = true;  // off the Switch: on by the environment
        // each 64 byte block cut to the ranges it overlaps, once: SendState did this for every written block of every draw
        // (TouchAllState lights every bit of the last word too: those blocks have no pieces)
        const size_t bits = std::size(g_state_dirty) * 64;
        piece_at_.assign(bits + 1, 0);
        for (size_t b = 0; b < bits; b++) {
            for (const Range& r : ranges_) {
                size_t lo = std::max(b * 64, r.off), hi = std::min(b * 64 + 64, r.off + r.len);
                if (lo < hi) pieces_.push_back({lo, hi - lo});
            }
            piece_at_[b + 1] = uint32_t(pieces_.size());
        }
        size_t covered = 0, listed = 0;
        for (const Range& r : ranges_) covered += r.len;  // the ranges do not overlap
        for (const Range& p : pieces_) listed += p.len;
        if (covered != listed) LOGE("render thread: the state blocks cover %zu bytes, the ranges %zu", listed, covered);
        TouchAllState();
        cur_ = NewBatch();
        std::thread([this] { Run(); }).detach();
    }
    ~Threaded() override {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(m_);
        // at exit the worker may be gone mid-batch: then the inner backend is left, not freed under it
        if (!done_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] { return exited_; })) (void)inner_.release();
    }

    bool Init(uint32_t width, uint32_t height) override { return inner_->Init(width, height); }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        const Stamp t = StampNow();
        perf_draws_++;
        SendState(s);
        Cmd c{Op::Draw};
        c.a = prim;
        c.b = start;
        c.c = count;
        if (indices) {
            c.d = 1;
            c.data = Put(indices, size_t(count) * 2);
        }
        // STREET: the vertices taken now, so the title may reuse their memory at once. BREXTREME: off, the render thread
        // reads them from the title's memory when it draws, as the console's GPU did, and a lock of the buffer waits for
        // it (BlockOnResource); the copy was ~1.9 ms a frame of the title thread at ~2500 draws. SHEROES_DRAW_COPY=0/1
        static const bool env_copy = [] {
            if (const char* v = std::getenv("SHEROES_DRAW_COPY")) g_draw_copy = std::strcmp(v, "0") != 0;
            return true;
        }();
        (void)env_copy;
        const bool copy = g_draw_copy.load(std::memory_order_relaxed);
        bool copied = copy && CopyVertices(s, c, indices);
        cur_->cmds.push_back(c);
        // the batch reads these resources: once per batch for the ones the draw before in it already marked
        std::array<uint32_t, 26> used;
        for (size_t i = 0; i < 4; i++) used[i] = s.textures[i], used[4 + i] = s.palettes[i];
        for (size_t i = 0; i < 16; i++) used[8 + i] = s.streams[i].vb;
        used[24] = s.rt.header;
        used[25] = s.zs.header;
        if (cur_->id != used_batch_ || used != used_) {
            for (uint32_t h : used) Use(h);
            used_ = used;
            used_batch_ = cur_->id;
        }
        Written(s.rt);
        if (cur_->cmds.size() >= kBatchCommands) Submit();
        // Diagnostic only: distinguish reused guest vertex memory from decode/state errors.
        static const bool sync_draws = std::getenv("SHEROES_SYNC_DRAWS") != nullptr;
        // The validated STREET path is also the default for direct EXE launches.
        static const bool consume_draws = !sync_draws;
        if (consume_draws && copy && !copied) {
            // Wait for the worker to copy guest data into host upload buffers,
            // without forcing GPU completion after every draw.
            static bool reported = false;
            if (!reported) { LOGR("BREXTREME: draw-consumption diagnostic enabled (no per-draw GPU flush)"); reported = true; }
            Wait(Submit(), WaitFor::Resource, "draw consumption");
        } else if (sync_draws) {
            static bool reported = false;
            if (!reported) { LOGR("BREXTREME: synchronous draw diagnostic enabled"); reported = true; }
            Flush();
        }
        perf_capture_ns_ += StampNs(t);
    }

    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
        const Stamp t = StampNow();
        SendState(s);
        Cmd c{Op::Clear};
        c.a = count;
        c.b = flags;
        c.c = color;
        c.d = stencil;
        c.f = z;
        if (count && rects) {
            c.data = Put(rects, size_t(count) * 16);
            c.size = count * 16;
        }
        cur_->cmds.push_back(c);
        Use(s.rt.header);
        Use(s.zs.header);
        if (flags & kClearTarget) Written(s.rt);
        g_perf_capture_ns += StampNs(t);
    }

    // rects, points and the surfaces' headers are taken now; the copy itself, CPU or GPU, runs in order like the
    // console's. data: the two headers' snapshots; b, c: rects and points in the batch's bytes, 1 based (0 none)
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override {
        const Stamp t = StampNow();
        Cmd c{Op::Copy};
        c.a = count;
        c.s1 = src;
        c.s2 = dst;
        c.data = PutHeader(src.header);
        PutHeader(dst.header);
        c.b = count && src_rects ? Put(kernel::HostPtr(src_rects), count * 16) + 1 : 0;
        c.c = count && dst_points ? Put(kernel::HostPtr(dst_points), count * 8) + 1 : 0;
        cur_->cmds.push_back(c);
        Use(src.header);
        Use(dst.header);
        Written(dst);
        g_perf_capture_ns += StampNs(t);
    }

    void Present(const Surface& front) override {
#ifdef __SWITCH__
        // the thread that presents is the title thread: its own core from the first frame on
        if (!placed_title_ && sheroes_place_thread) sheroes_place_thread(0);
        placed_title_ = true;
#endif
        const Stamp t = StampNow();
        Cmd c{Op::Present};
        c.s1 = front;
        c.data = PutHeader(front.header);
        cur_->cmds.push_back(c);
        Use(front.header);
        uint64_t id = Submit();
        g_perf_capture_ns += StampNs(t);
        FlushPerf();
        // the title records one frame while the one before it draws
        Wait(last_present_, WaitFor::Frame, nullptr);
        last_present_ = id;
    }

    void Sync(uint32_t data, uint32_t) override { ReadBack(data, false); }

    void SyncLate(uint32_t data) override {
        if (!inner_->DeliverLate(data)) {
            if (diag::Perf()) LOG_FIRST(16, "render thread: no finished copy of %08X for a NOOVERWRITE lock, reading it back now", data);
            written_.insert(data);
            ReadBack(data, true);
        }
        Cmd c{Op::RecordLate};
        c.a = data;
        cur_->cmds.push_back(c);
    }

    void Flush() override {
        cur_->cmds.push_back(Cmd{Op::Flush});
        Wait(Submit(), WaitFor::Flush, "flush");
    }

    void SetGamma(const uint8_t* ramp) override {
        Cmd c{Op::Gamma};
        c.data = Put(ramp, 768);
        cur_->cmds.push_back(c);
    }

    void ClearReport() override { cur_->cmds.push_back(Cmd{Op::ClearReport}); }
    void GetReport(uint32_t report) override {
        Cmd c{Op::GetReport};
        c.a = report;
        cur_->cmds.push_back(c);
    }

    uint32_t Fence() override { return uint32_t(cur_->id); }
    bool FencePending(uint32_t fence) override { return fence > done_.load(); }
    void WaitFence(uint32_t fence) override { Wait(fence, WaitFor::Fence, "fence"); }
    bool Busy(uint32_t resource) override { return LastUse(resource) > done_.load(); }
    void WaitResource(uint32_t resource) override {
        if (uint64_t id = LastUse(resource)) Wait(id, WaitFor::Resource, "resource");
    }

private:
    Batch* NewBatch() {
        Batch* b;
        {
            std::lock_guard lock(m_);
            if (free_.empty()) {
                b = new Batch;
            } else {
                b = free_.back();
                free_.pop_back();
            }
        }
        b->id = ++next_id_;
        return b;
    }

    // the draw's vertex spans into the batch, then their record: the mask of the streams read and a SpanAt for each.
    // BREXTREME: only the streams read (most draws read one): all 16 spans were 256 bytes a draw written and read again.
    // False (nothing taken) when one is too big or not in guest memory
    bool CopyVertices(const State& s, Cmd& c, const uint8_t* indices) {
        constexpr uint32_t kMaxCopy = 1u << 20;
        std::array<StreamSpan, 16> spans;
        if (!DrawStreamSpans(s, c.a, c.b, c.c, indices, spans)) return false;
        uint64_t total = 0;
        uint32_t mask = 0;
        for (size_t i = 0; i < 16; i++) {
            const StreamSpan& sp = spans[i];
            if (!sp.bytes) continue;
            total += sp.bytes;
            if (total > kMaxCopy || uint64_t(sp.from) + sp.bytes > (1ull << 32) || !mem::IsCommitted(sp.from) ||
                !mem::IsCommitted(sp.from + sp.bytes - 1))
                return false;
            mask |= 1u << i;
        }
        uint32_t at[16];
        for (uint32_t m = mask; m; m &= m - 1) {
            const int i = std::countr_zero(m);
            at[i] = Put(kernel::HostPtr(spans[size_t(i)].from), spans[size_t(i)].bytes);
        }
        c.size = Put(&mask, sizeof(mask)) + 1;
        for (uint32_t m = mask; m; m &= m - 1) {
            const int i = std::countr_zero(m);
            const SpanAt e{spans[size_t(i)], at[i]};
            Put(&e, sizeof(e));
        }
        perf_copied_++;
        perf_copied_bytes_ += total;
        return true;
    }

    uint32_t Put(const void* p, size_t n) {
        size_t at = (cur_->bytes.size() + 3) & ~size_t(3);
        cur_->bytes.resize(at + n);
        std::memcpy(cur_->bytes.data() + at, p, n);
        return uint32_t(at);
    }

    void ReadBack(uint32_t data, bool late) {
        // only surfaces the GPU wrote since the last read-back hold anything newer than guest memory
        if (data == last_written_) last_written_ = 0;
        if (!written_.erase(data)) return;
        (late ? g_perf_sync_late : g_perf_sync_block)++;
        if (diag::Perf() && logged_.insert(uint64_t(data) << 1 | (late ? 1 : 0)).second)
            LOGI("render thread: read-back of %08X for %s", data, late ? "a NOOVERWRITE lock without a finished copy" : "a lock / BlockOnResource");
        Cmd c{Op::Sync};
        c.a = data;
        cur_->cmds.push_back(c);
        Wait(Submit(), WaitFor::ReadBack, "read-back");
    }

    // bytes the render thread reads through a guest address, in the header ring
    uint32_t Scratch(const void* p, uint32_t n) {
        uint32_t size = (n + kHeaderSlot - 1) & ~(kHeaderSlot - 1);
        if (ring_pos_ % kHeaderRing + size > kHeaderRing) ring_pos_ += kHeaderRing - ring_pos_ % kHeaderRing;
        uint32_t at = ring_ + uint32_t(ring_pos_ % kHeaderRing);
        ring_pos_ += size;
        std::memcpy(kernel::HostPtr(at), p, n);
        return at;
    }

    // A header as it is now. The console's push buffer takes a texture's format and address, and a surface's or vertex
    // buffer's address, when they are set; the title points the same header elsewhere before the GPU gets there.
    // BREXTREME: the title thread only takes its address and bytes (rec: 4 + kHeaderBytes, zero filled); finding and
    // comparing its copy (Header) was ~5 % of the title thread on the console and runs on the render thread
    void Snapshot(uint32_t header, uint8_t (&rec)[4 + kHeaderBytes]) {
        std::memset(rec, 0, sizeof(rec));
        std::memcpy(rec, &header, 4);
        if (!header) return;
        // vertex buffer, index buffer and palette headers are 12 bytes: reading 20 can run off the end of the heap
        const uint32_t type = kernel::R32(header) & 0x70000;  // 0 vertex, 0x10000 index, 0x30000 palette
        std::memcpy(rec + 4, kernel::HostPtr(header), type < 0x20000 || type == 0x30000 ? 12 : kHeaderBytes);
    }
    // a snapshot in the batch's bytes, for a command's data (Copy, Present)
    uint32_t PutHeader(uint32_t header) {
        uint8_t rec[4 + kHeaderBytes];
        Snapshot(header, rec);
        return Put(rec, sizeof(rec));
    }
    // a snapshot for a State command: the render thread writes its copy's address to that field of its State
    void HeaderAt(const void* field, uint32_t header) {
        if (!header || !ring_) return;
        uint8_t rec[4 + kHeaderBytes];
        Snapshot(header, rec);
        uint32_t head[2] = {uint32_t(At(sent_, field)) | kHeaderRecord, uint32_t(sizeof(rec))};
        size_t at = cur_->bytes.size();
        cur_->bytes.resize(at + 8 + sizeof(rec));
        std::memcpy(cur_->bytes.data() + at, head, 8);
        std::memcpy(cur_->bytes.data() + at + 8, rec, sizeof(rec));
    }

    // the render thread: the copy of a snapshot's header in the ring, its address the backend reads
    uint32_t Resolve(const uint8_t* rec) {
        uint32_t header;
        std::memcpy(&header, rec, 4);
        return Header(header, rec + 4);
    }
    uint32_t Header(uint32_t header, const uint8_t* now) {
        if (!header || !ring_) return header;
        HeaderSlot& r = recent_headers_[RecentSlot(header)];
        if (r.key != header) {
            // the slot's header goes to the map with its copy, which it finds there again (same address) when it is back
            if (r.key) headers_[r.key] = r.copy;
            auto it = headers_.find(header);
            r.copy = it != headers_.end() ? it->second : HeaderCopy{};
            r.key = header;
        }
        HeaderCopy& c = r.copy;
        if (c.made && !std::memcmp(c.bytes, now, kHeaderBytes) && ring_pos_ - c.made < kHeaderRing / 2) return c.at;
        c.at = Scratch(now, kHeaderBytes);
        c.made = ring_pos_;
        std::memcpy(c.bytes, now, kHeaderBytes);
        return c.at;
    }

    // a field of the render thread's State, when it differs from what was sent
    void Delta(const void* field, const void* value, size_t n) {
        uint8_t* sent = reinterpret_cast<uint8_t*>(&sent_) + At(sent_, field);
        if (Same(sent, static_cast<const uint8_t*>(value), n)) return;
        std::memcpy(sent, value, n);
        uint32_t head[2] = {uint32_t(At(sent_, field)), uint32_t(n)};
        size_t at = cur_->bytes.size();
        cur_->bytes.resize(at + 8 + n);
        std::memcpy(cur_->bytes.data() + at, head, 8);
        std::memcpy(cur_->bytes.data() + at + 8, value, n);
    }

    // what changed since the last command, in 64 byte pieces
    void SendState(const State& s) {
        const uint8_t* now = reinterpret_cast<const uint8_t*>(&s);
        Cmd c{Op::State};
        size_t first = cur_->bytes.size();
        // STREET: blocks of simple and vsc nobody wrote since the last send are equal to what was sent
        const bool track = g_track_state;
        const uint8_t* sent = reinterpret_cast<const uint8_t*>(&sent_);
        if (track) {
            // straight to the written blocks, each cut to the ranges it overlaps
            for (size_t w = 0; w < std::size(g_state_dirty); w++)
                for (uint64_t bits = g_state_dirty[w]; bits; bits &= bits - 1) {
                    const size_t b = w * 64 + size_t(std::countr_zero(bits));
                    for (uint32_t i = piece_at_[b]; i < piece_at_[b + 1]; i++)
                        Delta(sent + pieces_[i].off, now + pieces_[i].off, pieces_[i].len);
                }
        } else {
            for (const Range& r : ranges_)
                for (size_t off = r.off, end = r.off + r.len; off < end; off += 64)
                    Delta(sent + off, now + off, std::min<size_t>(64, end - off));
        }
        // STREET: with state tracking a group of headers is read again only when the title set it since the last send,
        // as the console takes a header's fields when it is set (and every kGroupRefresh sends, so the render thread
        // renews its copies before the ring comes round): reading each bound header from guest memory on every draw was
        // ~4.5 % of the title thread on the Switch
        sends_++;
        auto stale = [&](int g, const void* field, size_t n) {
            if (!track || StateDirty(At(s, field), n) || sends_ - groups_read_[g] >= kGroupRefresh) {
                groups_read_[g] = sends_;
                return true;
            }
            return false;
        };
        const bool read_textures = stale(0, &s.textures, sizeof(s.textures));
        const bool read_palettes = stale(1, &s.palettes, sizeof(s.palettes));
        const bool read_streams = stale(2, &s.streams, sizeof(s.streams));
        const bool read_targets = stale(3, &s.rt, sizeof(s.rt)) | stale(4, &s.zs, sizeof(s.zs));
        CleanState();
        // the title's header addresses as they are (sent_ keeps them so), then each set header's snapshot: the render
        // thread puts its copy's address over the field
        if (read_textures) {
            Delta(&sent_.textures, &s.textures, sizeof(s.textures));
            for (size_t i = 0; i < 4; i++) HeaderAt(&sent_.textures[i], s.textures[i]);
        }
        if (read_palettes) {
            Delta(&sent_.palettes, &s.palettes, sizeof(s.palettes));
            for (size_t i = 0; i < 4; i++) HeaderAt(&sent_.palettes[i], s.palettes[i]);
        }
        if (read_streams) {
            Delta(&sent_.streams, &s.streams, sizeof(s.streams));
            for (size_t i = 0; i < s.streams.size(); i++) HeaderAt(&sent_.streams[i].vb, s.streams[i].vb);
        }
        if (read_targets) {
            Delta(&sent_.rt, &s.rt, sizeof(s.rt));
            Delta(&sent_.zs, &s.zs, sizeof(s.zs));
            HeaderAt(&sent_.rt.header, s.rt.header);
            HeaderAt(&sent_.zs.header, s.zs.header);
        }
        if (s.vs.get() != sent_.vs.get()) {
            sent_.vs = s.vs;
            c.vs = int32_t(cur_->vs.size());
            cur_->vs.push_back(s.vs);
        }
        if (s.ps.get() != sent_.ps.get()) {
            sent_.ps = s.ps;
            c.ps = int32_t(cur_->ps.size());
            cur_->ps.push_back(s.ps);
        }
        if (cur_->bytes.size() == first && c.vs < 0 && c.ps < 0) return;
        c.data = uint32_t(first);
        c.size = uint32_t(cur_->bytes.size() - first);
        cur_->cmds.push_back(c);
    }

    // resources by guest header: the batch that last read them
    void Use(uint32_t header) {
        if (!header) return;
        UseSlot& r = recent_uses_[RecentSlot(header)];
        if (r.key != header) {
            if (r.key) last_use_[r.key] = r.batch;  // pushed out of its slot: kept in the map
            r.key = header;
        }
        r.batch = cur_->id;
    }
    // the batch that last read a resource, 0 none
    uint64_t LastUse(uint32_t header) const {
        const UseSlot& r = recent_uses_[RecentSlot(header)];
        if (r.key == header) return r.batch;
        auto it = last_use_.find(header);
        return it != last_use_.end() ? it->second : 0;
    }
    void Written(const Surface& s) {
        if (!s.header) return;
        uint32_t data = mem::VirtualFromPhysical(s.data);
        if (data == last_written_) return;  // draws into the same target, one after another
        written_.insert(data);
        last_written_ = data;
    }

    uint64_t Submit() {
        uint64_t id = cur_->id;
        {
            std::lock_guard lock(m_);
            queue_.push_back(cur_);
        }
        cv_.notify_one();
        cur_ = NewBatch();
        return id;
    }

    void Wait(uint64_t id, WaitFor kind, const char* why) {
        if (!id || done_.load() >= id) return;
        if (id >= cur_->id) id = Submit();
        auto t = Clock::now();
        // g_render_spin_us: poll a while first, the worker is often a few microseconds from done
        bool done = false;
        if (uint32_t spin = g_render_spin_us.load(std::memory_order_relaxed)) {
            auto until = t + std::chrono::microseconds(spin);
            while (!(done = done_.load() >= id) && Clock::now() < until) std::this_thread::yield();
        }
        if (!done) {
            std::unique_lock lock(m_);
            done_cv_.wait(lock, [&] { return done_.load() >= id; });
        }
        uint64_t ns = Ns(t);
        g_perf_wait_ns += ns;
        g_perf_wait_kind_ns[size_t(kind)] += ns;
        if (why) {
            g_perf_waits++;
            if (diag::Perf()) LOG_FIRST(32, "render thread: the title waited %.2f ms for a %s", ns / 1e6, why);
        }
    }

    void Run() {
#ifndef __SWITCH__
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto set = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))))
            set(GetCurrentThread(), L"render");
#else
        if (sheroes_profile_thread) sheroes_profile_thread("render");
        if (sheroes_place_thread) sheroes_place_thread(1);
#endif
        for (;;) {
            Batch* b;
            // g_render_spin_us: poll a while for the next batch before sleeping
            if (uint32_t spin = g_render_spin_us.load(std::memory_order_relaxed)) {
                auto until = Clock::now() + std::chrono::microseconds(spin);
                while (Clock::now() < until) {
                    {
                        std::lock_guard lock(m_);
                        if (stop_ || !queue_.empty()) break;
                    }
                    std::this_thread::yield();
                }
            }
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) break;
                b = queue_.front();
                queue_.pop_front();
            }
            auto t = Clock::now();
            for (const Cmd& c : b->cmds) Execute(*b, c);
            g_perf_render_ns += Ns(t);
            uint64_t id = b->id;
            b->cmds.clear();
            b->bytes.clear();
            b->vs.clear();
            b->ps.clear();
            {
                std::lock_guard lock(m_);
                free_.push_back(b);
                done_.store(id);
            }
            done_cv_.notify_all();
        }
        std::lock_guard lock(m_);
        exited_ = true;
        done_cv_.notify_all();
    }

    void Execute(const Batch& b, const Cmd& c) {
        const uint8_t* data = b.bytes.data() + c.data;
        switch (c.op) {
            case Op::State: {
                uint8_t* s = reinterpret_cast<uint8_t*>(&state_);
                static const size_t vsc_at = size_t(reinterpret_cast<const uint8_t*>(&state_.vsc) - s),
                                    vsc_end = vsc_at + sizeof(state_.vsc);
                static const size_t streams_at = size_t(reinterpret_cast<const uint8_t*>(&state_.streams) - s),
                                    streams_end = streams_at + sizeof(state_.streams);
                bool vsc_changed = false, other_changed = c.vs >= 0 || c.ps >= 0;
                for (const uint8_t *p = data, *next; p < data + c.size; p = next) {
                    uint32_t head[2];
                    std::memcpy(head, p, 8);
                    next = p + 8 + head[1];
                    size_t lo = head[0], hi = lo + head[1];
                    if (head[0] & kHeaderRecord) {
                        // a header's snapshot: its copy's address over the field, a change only when that differs
                        lo = head[0] & ~kHeaderRecord, hi = lo + 4;
                        const uint32_t at = Resolve(p + 8);
                        if (!std::memcmp(s + lo, &at, 4)) continue;
                        std::memcpy(s + lo, &at, 4);
                    } else {
                        std::memcpy(s + head[0], p + 8, head[1]);
                    }
                    vsc_changed |= lo < vsc_end && hi > vsc_at;
                    other_changed |= !(lo >= vsc_at && hi <= vsc_end) && !(lo >= streams_at && hi <= streams_end);
                }
                if (vsc_changed) g_vsc_generation++;
                if (other_changed) g_state_generation++;
                if (c.vs >= 0) state_.vs = b.vs[size_t(c.vs)];
                if (c.ps >= 0) state_.ps = b.ps[size_t(c.ps)];
                break;
            }
            case Op::Draw:
                if (c.size) {
                    // the vertices as they were when the draw was made (CopyVertices' record)
                    const uint8_t* p = b.bytes.data() + c.size - 1;
                    uint32_t mask;
                    std::memcpy(&mask, p, sizeof(mask));
                    p += sizeof(mask);
                    std::array<StreamCopy, 16> copies{};
                    for (uint32_t m = mask; m; m &= m - 1, p += sizeof(SpanAt)) {
                        SpanAt e;
                        std::memcpy(&e, p, sizeof(e));
                        copies[size_t(std::countr_zero(m))] = {e.span, b.bytes.data() + e.at};
                    }
                    UseStreamCopies(&copies);
                    inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                    UseStreamCopies(nullptr);
                } else {
                    inner_->Draw(state_, c.a, c.b, c.c, c.d ? data : nullptr);
                }
                break;
            case Op::Clear: inner_->Clear(state_, c.a, c.size ? data : nullptr, c.b, c.c, c.f, c.d); break;
            case Op::Copy: {
                // the headers' copies and the rects and points where the backend reads them, in the header ring
                Surface src = c.s1, dst = c.s2;
                src.header = Resolve(data);
                dst.header = Resolve(data + 4 + kHeaderBytes);
                const uint32_t rects = c.b ? Scratch(b.bytes.data() + c.b - 1, c.a * 16) : 0;
                const uint32_t points = c.c ? Scratch(b.bytes.data() + c.c - 1, c.a * 8) : 0;
                inner_->Copy(src, dst, c.a, rects, points);
                break;
            }
            case Op::Present: {
                Surface front = c.s1;
                front.header = Resolve(data);
                inner_->Present(front);
                break;
            }
            case Op::Sync: inner_->Sync(c.a, 0); break;
            case Op::RecordLate: inner_->RecordLate(c.a); break;
            case Op::Flush: inner_->Flush(); break;
            case Op::Gamma: inner_->SetGamma(data); break;
            case Op::ClearReport: inner_->ClearReport(); break;
            case Op::GetReport: inner_->GetReport(c.a); break;
        }
    }

    std::unique_ptr<Backend> inner_;
    std::vector<Range> ranges_;
    std::vector<Range> pieces_;       // the written blocks' ranges, block by block
    std::vector<uint32_t> piece_at_;  // pieces_ of block b: [piece_at_[b], piece_at_[b + 1])

    // the render thread: the header copies in the ring (Header)
    uint32_t ring_ = 0;  // set once before the thread starts, read by both
    uint64_t ring_pos_ = 0;
    struct HeaderCopy {
        uint32_t at = 0;
        uint64_t made = 0;  // ring_pos_ after it was written
        uint8_t bytes[kHeaderBytes];
    };
    std::unordered_map<uint32_t, HeaderCopy> headers_;  // the copies of headers pushed out of recent_headers_
    // STREET: the last few lookups in slots in front of the maps, by a multiplicative hash; (header >> 3) & 63 put the
    // headers of one pool in a few slots. BREXTREME: 4096 (was 1024): a stage with ~2500 draws a frame has more headers
    // live than that, and each miss was a hash map lookup
    static size_t RecentSlot(uint32_t header) { return (header * 2654435761u) >> 20; }
    // BREXTREME: the values in the slots, not pointers into the maps: following the pointer to the map's node was a
    // cache miss a lookup (headers ~1.7 %, uses ~1 % of the title thread on stage 4's ~2500 draw side)
    struct HeaderSlot {
        uint32_t key = 0;
        HeaderCopy copy;
    };
    std::array<HeaderSlot, 4096> recent_headers_{};

    // the title's thread
    struct UseSlot {
        uint32_t key = 0;
        uint64_t batch = 0;
    };
    std::array<UseSlot, 4096> recent_uses_{};
    // the title thread's per-draw perf counters, plain: four atomic adds a draw were 2.1 % of that thread on the console
    // (ldaxr/stlxr with barriers on the A57). Published to the g_perf_* atomics once a frame, at Present
    uint64_t perf_draws_ = 0, perf_capture_ns_ = 0, perf_copied_ = 0, perf_copied_bytes_ = 0;
    void FlushPerf() {
        g_perf_draws.fetch_add(perf_draws_, std::memory_order_relaxed);
        g_perf_capture_ns.fetch_add(perf_capture_ns_, std::memory_order_relaxed);
        g_perf_copied.fetch_add(perf_copied_, std::memory_order_relaxed);
        g_perf_copied_bytes.fetch_add(perf_copied_bytes_, std::memory_order_relaxed);
        perf_draws_ = perf_capture_ns_ = perf_copied_ = perf_copied_bytes_ = 0;
    }
    Batch* cur_ = nullptr;
    State sent_;  // the state as the render thread will have it, but the title's own header addresses
    uint64_t sends_ = 0;            // SendState calls
    uint64_t groups_read_[5] = {};  // sends_ when textures, palettes, streams, rt, zs headers were last read
    std::array<uint32_t, 26> used_{};  // the resources the last draw marked used, in batch used_batch_
    uint64_t used_batch_ = 0;
    std::unordered_map<uint32_t, uint64_t> last_use_;
    std::unordered_set<uint32_t> written_;  // surfaces drawn since their last read-back, by data address
    uint32_t last_written_ = 0;             // in written_ already
    std::unordered_set<uint64_t> logged_;   // perf diagnostic: read-backs named once, by address and kind
    uint64_t next_id_ = 0, last_present_ = 0;

    // the render thread
    State state_;

    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    bool placed_title_ = false;
    std::deque<Batch*> queue_;
    std::vector<Batch*> free_;
    std::atomic<uint64_t> done_{0};
    bool stop_ = false, exited_ = false;
};

}  // namespace

std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner) { return std::make_unique<Threaded>(std::move(inner)); }

}  // namespace gpu
