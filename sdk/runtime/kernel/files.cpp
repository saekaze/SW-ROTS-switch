#include <functional>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <list>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#ifdef __SWITCH__
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "kernel/disc.h"
#include "app/app.h"
#include "kernel/kernel.h"
#include "kernel/exports.h"

namespace fs = std::filesystem;

namespace kernel {

namespace {

std::mutex g_links_lock;
std::map<std::string, std::string> g_links;    // upper case "\??\D:" -> "\Device\CdRom0"
std::map<std::string, fs::path> g_devices;     // upper case device -> host directory
constexpr char kCdRom[] = "\\DEVICE\\CDROM0";

std::string Upper(std::string s) {
    for (char& c : s) c = char(std::toupper(uint8_t(c)));
    return s;
}

std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string out;
    for (char c : p) {
        if (c == '\\' && !out.empty() && out.back() == '\\') continue;
        out += c;
    }
    while (out.size() > 1 && out.back() == '\\') out.pop_back();
    return out;
}

uint64_t ToFileTime(fs::file_time_type t);

// a host path's kind, size and time (FILETIME), as a stat gives them
struct HostStat {
    bool exists = false, dir = false;
    uint64_t size = 0, time = 116444736000000000ull;
};

HostStat StatUncached(const std::string& p) {
#ifdef __SWITCH__
    struct stat st;
    if (::stat(p.c_str(), &st)) return {};
    const bool dir = S_ISDIR(st.st_mode);
    return {true, dir, dir ? 0 : uint64_t(st.st_size), uint64_t(st.st_mtime) * 10000000ull + 116444736000000000ull};
#else
    std::error_code ec;
    const fs::file_status s = fs::status(p, ec);
    if (ec || !fs::exists(s)) return {};
    HostStat h{true, fs::is_directory(s)};
    if (!h.dir) h.size = fs::file_size(p, ec);
    h.time = ToFileTime(fs::last_write_time(p, ec));
    return h;
#endif
}

// BREXTREME: the game's folder stands for the console's DVD, which nothing writes: its stats are taken once. On the
// Switch every stat by path is several requests to the file system service (entry type, time stamp, the time zone's
// conversion), and the title opens and queries each packed file again for every piece it reads: ~35 % of a stage
// load's pause on the console. Anything that writes there drops them all (ForgetGameStats)
std::string g_game_root;  // set once by MountDefaults, before the title runs
std::mutex g_stat_lock;
std::unordered_map<std::string, HostStat> g_game_stats;

bool InGame(const std::string& p) {
    const size_t n = g_game_root.size();
    return n && p.compare(0, n, g_game_root) == 0 && (p.size() == n || p[n] == '/' || p[n] == '\\');
}

HostStat Stat(const std::string& p) {
    if (!InGame(p)) return StatUncached(p);
    std::lock_guard<std::mutex> lock(g_stat_lock);
    auto [it, fresh] = g_game_stats.try_emplace(p);
    if (fresh) it->second = StatUncached(p);
    return it->second;
}

void ForgetGameStats(const std::string& p) {
    if (!InGame(p)) return;
    std::lock_guard<std::mutex> lock(g_stat_lock);
    g_game_stats.clear();
}

// host lookup that ignores case on file systems that do not
fs::path MatchCase(const fs::path& dir, const std::vector<std::string>& parts) {
    fs::path cur = dir;
    for (auto& part : parts) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            cur = cur.parent_path();
            continue;
        }
        fs::path direct = cur / part;
        std::error_code ec;
        if (Stat(direct.string()).exists) {
            cur = direct;
            continue;
        }
        bool found = false;
        std::string want = Upper(part);
        for (auto& e : fs::directory_iterator(cur, ec)) {
            if (Upper(e.path().filename().string()) == want) {
                cur = e.path();
                found = true;
                break;
            }
        }
        if (!found) cur = direct;
    }
    return cur;
}

std::atomic<uint64_t> g_movie_read_ms{0};

bool IsMovie(const std::string& x) {
    if (x.size() <= 4) return false;
    std::string ext = x.substr(x.size() - 4);
    for (char& c : ext) c = char(std::tolower(uint8_t(c)));
    return ext == ".sfd" || ext == ".xmv" || ext == ".bik";
}

struct File;
void ForgetLazy(File* f);
// ROTS: the movie file last read, for SkipMovie
std::mutex g_movie_lock;
std::weak_ptr<File> g_movie_file;

struct File : Object {
    std::atomic<bool> skipped{false};  // ROTS: a movie the player skipped: its reads answer end of file
    fs::path host;
    std::string xbox;
    FILE* fp = nullptr;
    // sheroes-nx: a game file opened to read only, its FILE opened at its first read (OpenLazy) and closed again when
    // too many are open. Sonic Heroes opens all ~3,900 disc files at boot and keeps them open: from the card's folder
    // that froze the title 44 s at boot and 10 s in the menus, and opens failed later on (a null object at team
    // select). Switch, 2026-11-13; main's lazy opens did the same for the xboxrecomp build
    bool lazy = false;
    const Disc::Entry* entry = nullptr;  // on the disc image instead of a host path
    bool dir = false;
    uint64_t pos = 0;
    std::vector<fs::directory_entry> entries;
    std::vector<const Disc::Entry*> disc_entries;
    size_t next = 0;
    bool listed = false;
    bool delete_on_close = false;
    bool async = false;  // opened without FILE_SYNCHRONOUS_IO_ALERT/NONALERT: transfers report STATUS_PENDING
    std::mutex io;  // threads sharing a handle: seek and transfer stay together
    File() : Object(ObjType::File) {}
    ~File() override {
        if (lazy) ForgetLazy(this);
        if (fp) std::fclose(fp);
        if (delete_on_close) {
            std::error_code ec;
            fs::remove(host, ec);
            ForgetGameStats(host.string());
        }
    }
};

// the lazy files with a FILE open, most recently read first; past kLazyOpen the oldest idle one is closed
constexpr size_t kLazyOpen = 48;
std::mutex g_lazy_lock;
std::list<File*> g_lazy_open;

void ForgetLazy(File* f) {
    std::lock_guard<std::mutex> lock(g_lazy_lock);
    g_lazy_open.remove(f);
}

// call with f->io held: makes sure the FILE is open and marks it most recently used
bool OpenLazy(File& f) {
    if (!f.lazy) return f.fp != nullptr;
    std::lock_guard<std::mutex> lock(g_lazy_lock);
    if (f.fp) {
        g_lazy_open.remove(&f);
        g_lazy_open.push_front(&f);
        return true;
    }
    // close the oldest idle ones (a file another thread is reading keeps its FILE)
    for (auto it = g_lazy_open.end(); g_lazy_open.size() >= kLazyOpen && it != g_lazy_open.begin();) {
        --it;
        File* old = *it;
        if (!old->io.try_lock()) continue;
        std::fclose(old->fp);
        old->fp = nullptr;
        old->io.unlock();
        it = g_lazy_open.erase(it);
    }
    f.fp = std::fopen(f.host.string().c_str(), "rb");
    if (!f.fp) {
        LOGW("files: cannot open %s", f.host.string().c_str());
        return false;
    }
    g_lazy_open.push_front(&f);
    return true;
}

struct Link : Object {
    std::string target;
    Link() : Object(ObjType::SymLink) {}
};

uint64_t ToFileTime(fs::file_time_type t) {
    using namespace std::chrono;
    auto sys = time_point_cast<nanoseconds>(t - fs::file_time_type::clock::now() + system_clock::now());
    return uint64_t(sys.time_since_epoch().count() / 100 + 116444736000000000ll);
}

#ifdef __SWITCH__
// The card's file system will not open a file a second time while a handle to it can write, and stat (and
// libstdc++'s file_size) opens it: a file the title holds open is asked through its own handle (fstat)
static uint64_t OpenSize(FILE* fp, std::error_code& ec) {
    struct stat st;
    if (::fstat(fileno(fp), &st)) {
        ec = std::error_code(errno, std::generic_category());
        return uint64_t(-1);
    }
    ec.clear();
    return uint64_t(st.st_size);
}
#endif
static uint64_t HostSize(const fs::path& p, std::error_code& ec) {
    const HostStat s = Stat(p.string());
    if (!s.exists || s.dir) {
        ec = std::make_error_code(s.exists ? std::errc::is_a_directory : std::errc::no_such_file_or_directory);
        return uint64_t(-1);
    }
    ec.clear();
    return s.size;
}

static uint64_t HostTime(const fs::path& p) { return Stat(p.string()).time; }

void FillNetworkInfo(uint32_t out, uint64_t t, uint64_t size, bool dir) {
    for (int i = 0; i < 4; i++) W64(out + i * 8, t);
    W64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
    W64(out + 0x28, size);
    W32(out + 0x30, dir ? 0x10 : 0x80);  // FILE_ATTRIBUTE_DIRECTORY or NORMAL
}

void FillNetworkInfo(uint32_t out, const fs::path& p) {
    const HostStat s = Stat(p.string());
    FillNetworkInfo(out, s.time, s.dir ? 0 : s.size, s.dir);
}

void FillNetworkInfo(uint32_t out, const Disc::Entry& e) {
    FillNetworkInfo(out, MountedDisc()->Time(), e.dir ? 0 : e.size, e.dir);
}

bool WildMatch(const std::string& pat, const std::string& s) {
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || std::toupper(uint8_t(pat[p])) == std::toupper(uint8_t(s[i])))) {
            p++;
            i++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

// past 2 GB a long offset wraps on Windows
void Seek(FILE* fp, uint64_t pos) {
#ifdef _WIN32
    _fseeki64(fp, int64_t(pos), SEEK_SET);
#else
    fseeko(fp, off_t(pos), SEEK_SET);
#endif
}

void IoStatus(uint32_t iosb, uint32_t status, uint32_t info) {
    if (!iosb) return;
    W32(iosb, status);
    W32(iosb + 4, info);
}

std::shared_ptr<File> FileFromHandle(uint32_t h) {
    ObjectRef o = FromHandle(h);
    if (!o || o->type != ObjType::File) return nullptr;
    return std::static_pointer_cast<File>(o);
}

std::string ObjectName(uint32_t attrs, uint32_t* root = nullptr) {
    if (root) *root = R32(attrs);
    return ReadAnsiString(R32(attrs + 4));
}

void Complete(uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    if (event)
        if (ObjectRef e = FromHandle(event)) SetEvent(*e);
    if (apc && Current()) QueueApc(*Current(), apc, apc_ctx, iosb, 0);
}

}  // namespace

std::string ReadAnsiString(uint32_t str) {
    if (!str) return {};
    uint16_t len = R16(str);
    uint32_t buf = R32(str + 4);
    if (!buf) return {};
    return std::string(reinterpret_cast<const char*>(HostPtr(buf)), len);
}

std::string ReadCString(uint32_t va, size_t max) {
    std::string s;
    if (!va) return s;
    for (size_t i = 0; i < max; i++) {
        char c = char(G<uint8_t>(va + uint32_t(i)));
        if (!c) break;
        s += c;
    }
    return s;
}

void MountDefaults(const std::string& game_dir, const std::string& data_dir) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    fs::path d = data_dir;
    g_devices[kCdRom] = game_dir;
    g_game_root = fs::path(game_dir).string();
    while (g_game_root.size() > 1 && (g_game_root.back() == '/' || g_game_root.back() == '\\')) g_game_root.pop_back();
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION1"] = d / "E";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION2"] = d / "C";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION3"] = d / "X";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION4"] = d / "Y";
    g_devices["\\DEVICE\\HARDDISK0\\PARTITION5"] = d / "Z";
    for (auto& [k, v] : g_devices) {
        std::error_code ec;
        fs::create_directories(v, ec);
    }
    g_links["\\??\\D:"] = "\\Device\\CdRom0";
    const Disc* disc = MountedDisc();
    LOGI("files: D: is %s, hard disk under %s", disc ? disc->Path().string().c_str() : game_dir.c_str(), data_dir.c_str());
}

bool CreateSymbolicLink(const std::string& link, const std::string& target) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    g_links[Upper(Normalize(link))] = Normalize(target);
    LOGI("files: link %s -> %s", link.c_str(), target.c_str());
    return true;
}

bool DeleteSymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    return g_links.erase(Upper(Normalize(link))) != 0;
}

std::string QuerySymbolicLink(const std::string& link) {
    std::lock_guard<std::mutex> lock(g_links_lock);
    auto it = g_links.find(Upper(Normalize(link)));
    return it == g_links.end() ? std::string() : it->second;
}

namespace {

struct Target {
    std::string host;                    // empty on the disc image or unmapped
    bool disc = false;                   // on the mounted disc image
    const Disc::Entry* entry = nullptr;  // there: the file or folder, null when missing
};

Target Resolve(const std::string& xbox_path, uint32_t root_handle) {
    std::string p = Normalize(xbox_path);
    if (root_handle) {
        if (auto f = FileFromHandle(root_handle)) p = Normalize(f->xbox + "\\" + p);
    }
    if (p.size() >= 2 && p[1] == ':') p = "\\??\\" + p;
    std::lock_guard<std::mutex> lock(g_links_lock);
    for (int depth = 0; depth < 8; depth++) {
        std::string up = Upper(p);
        bool changed = false;
        for (auto& [k, v] : g_links) {
            if (up.compare(0, k.size(), k) == 0 && (up.size() == k.size() || up[k.size()] == '\\')) {
                p = v + p.substr(k.size());
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    std::string up = Upper(p);
    const std::pair<const std::string, fs::path>* best = nullptr;
    for (auto& e : g_devices)
        if (up.compare(0, e.first.size(), e.first) == 0 && (up.size() == e.first.size() || up[e.first.size()] == '\\'))
            if (!best || e.first.size() > best->first.size()) best = &e;
    if (!best) return {};
    std::vector<std::string> parts;
    std::string rest = p.substr(best->first.size());
    size_t s = 0;
    while (s < rest.size()) {
        size_t e = rest.find('\\', s);
        if (e == std::string::npos) e = rest.size();
        if (e > s) parts.push_back(rest.substr(s, e - s));
        s = e + 1;
    }
    // no Xbox name holds ':', and fs::path would take "D:" as a host drive
    for (auto& part : parts)
        if (part.find(':') != std::string::npos) return {};
    if (best->first == kCdRom)
        if (const Disc* disc = MountedDisc()) return {{}, true, disc->Find(parts)};
    return {MatchCase(best->second, parts).string()};
}

// the disc image is read only: nothing on it is created, replaced or written
uint32_t OpenOnDisc(const Disc::Entry* e, const std::string& name, uint32_t handle_out, uint32_t iosb, uint32_t disposition,
                    uint32_t options) {
    if (!e && (disposition == 1 || disposition == 4)) {  // FILE_OPEN or FILE_OVERWRITE need an existing file
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return kObjectNameNotFound;
    }
    if (!e || (disposition != 1 && disposition != 3)) {
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    if (e->dir && !(options & 1) && (options & 0x40)) {  // options 1 FILE_DIRECTORY_FILE, 0x40 FILE_NON_DIRECTORY_FILE
        IoStatus(iosb, kFileIsADirectory, 0);
        return kFileIsADirectory;
    }
    auto f = std::make_shared<File>();
    f->xbox = Normalize(name);
    f->entry = e;
    f->dir = e->dir;
    f->async = !(options & 0x30);
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

}  // namespace

std::string ResolvePath(const std::string& xbox_path, uint32_t root_handle) { return Resolve(xbox_path, root_handle).host; }

bool ReadGameFile(const std::string& xbox_path, std::vector<uint8_t>& out) {
    Target t = Resolve(xbox_path, 0);
    if (t.disc) {
        if (!t.entry || t.entry->dir) return false;
        out.resize(t.entry->size);
        return MountedDisc()->Read(*t.entry, 0, out.data(), t.entry->size) == t.entry->size;
    }
    std::ifstream f(t.host, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// exports

KERNEL_EXPORT(190, NtCreateFile, 9) {
    uint32_t handle_out = a[0], access = a[1], attrs = a[2], iosb = a[3], disposition = a[7], options = a[8];
    (void)access;
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, disposition, options);
    const std::string& host = t.host;
    if (host.empty()) {
        LOGW("NtCreateFile: no mapping for %s", name.c_str());
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
    std::error_code ec;
    const HostStat hs = Stat(host);
    bool exists = hs.exists;
    bool is_dir = hs.dir;
    bool want_dir = options & 1;
    uint32_t info = 1;  // FILE_OPENED
    if (want_dir || is_dir) {
        if (!exists) {
            if (disposition == 1 || disposition == 4) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            fs::create_directories(host, ec);
            ForgetGameStats(host);
            info = 2;
        } else if (disposition == 2) {
            IoStatus(iosb, kObjectNameCollision, 4);
            return kObjectNameCollision;
        }
        if (!want_dir && (options & 0x40)) {
            IoStatus(iosb, kFileIsADirectory, 0);
            return kFileIsADirectory;
        }
        auto f = std::make_shared<File>();
        f->host = host;
        f->xbox = Normalize(name);
        f->dir = true;
        W32(handle_out, NewHandle(f));
        IoStatus(iosb, kSuccess, info);
        return kSuccess;
    }
    const char* mode = nullptr;
    switch (disposition) {
        case 0:  // supersede
        case 5:  // overwrite if
            mode = "w+b";
            info = exists ? 3 : 2;
            break;
        case 1:  // open
            if (!exists) {
                LOG_FIRST(200, "missing %s", name.c_str());
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "r+b";
            break;
        case 2:  // create
            if (exists) {
                IoStatus(iosb, kObjectNameCollision, 4);
                return kObjectNameCollision;
            }
            mode = "w+b";
            info = 2;
            break;
        case 3:  // open if
            mode = exists ? "r+b" : "w+b";
            info = exists ? 1 : 2;
            break;
        case 4:  // overwrite
            if (!exists) {
                IoStatus(iosb, kObjectNameNotFound, 5);
                return kObjectNameNotFound;
            }
            mode = "w+b";
            info = 3;
            break;
        default:
            return kInvalidParameter;
    }
    // a file in a missing folder fails as on the console: XAPI probes saves with OPEN_ALWAYS
    if (!exists && !Stat(fs::path(host).parent_path().string()).dir) {
        IoStatus(iosb, kObjectPathNotFound, 0);
        return kObjectPathNotFound;
    }
#ifdef __SWITCH__
    // a handle that can write locks the file on the card against any other open: one that only reads stays "rb"
    // (GENERIC_WRITE, GENERIC_ALL, MAXIMUM_ALLOWED, WRITE_DAC/OWNER, DELETE, FILE_WRITE_DATA/APPEND/EA/ATTRIBUTES)
    if (std::string(mode) == "r+b" && !(access & 0x520D0116u)) mode = "rb";
#endif
    const bool lazy = std::string(mode) == "rb" && InGame(host);
    FILE* fp = lazy ? nullptr : std::fopen(host.c_str(), mode);
    if (!fp && !lazy && std::string(mode) == "r+b") fp = std::fopen(host.c_str(), "rb");  // read only media
    if (!fp && !lazy) {
        LOGW("NtCreateFile: cannot open %s", host.c_str());
        IoStatus(iosb, kAccessDenied, 0);
        return kAccessDenied;
    }
    if (mode[0] == 'w') ForgetGameStats(host);  // made or emptied
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->fp = fp;
    f->lazy = lazy;
    f->delete_on_close = options & 0x1000;  // FILE_DELETE_ON_CLOSE
    f->async = !(options & 0x30);
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, info);
    LOG_FIRST(400, "open %s", name.c_str());
    return kSuccess;
}

KERNEL_EXPORT(202, NtOpenFile, 6) {
    // same as create with FILE_OPEN
    uint32_t handle_out = a[0], attrs = a[2], iosb = a[3], options = a[5];
    uint32_t root;
    std::string name = ObjectName(attrs, &root);
    Target t = Resolve(name, root);
    if (t.disc) return OpenOnDisc(t.entry, name, handle_out, iosb, 1, options);
    const std::string& host = t.host;
    const HostStat hs = host.empty() ? HostStat{} : Stat(host);
    if (!hs.exists) {
        LOG_FIRST(200, "missing %s", name.c_str());
        IoStatus(iosb, kObjectNameNotFound, 5);
        return host.empty() ? kObjectPathNotFound : kObjectNameNotFound;
    }
    auto f = std::make_shared<File>();
    f->host = host;
    f->xbox = Normalize(name);
    f->dir = hs.dir;
    if (!f->dir) {
        if (!(a[1] & 0x520D0116u) && InGame(host)) {
            f->lazy = true;  // opened at its first read
        } else {
#ifdef __SWITCH__
            // as in NtCreateFile: only a handle that writes may lock the file on the card
            if (a[1] & 0x520D0116u) f->fp = std::fopen(host.c_str(), "r+b");
#else
            f->fp = std::fopen(host.c_str(), "r+b");
#endif
            if (!f->fp) f->fp = std::fopen(host.c_str(), "rb");
            if (!f->fp) return kAccessDenied;
        }
    }
    f->async = !(options & 0x30);
    W32(handle_out, NewHandle(f));
    IoStatus(iosb, kSuccess, 1);
    return kSuccess;
}

// I/O completes before the call returns: the status block is written, then the file object, the event and the
// APC are signalled (GetOverlappedResult without an event waits on the file handle)
// BREXTREME: a transfer that succeeds on a handle opened for asynchronous I/O returns STATUS_PENDING, as the Xbox's
// disc and hard disk drivers do; the status block already holds the result. Bloody Roar Extreme's loader (0x34C40)
// issues an overlapped ReadFile and takes anything but ERROR_IO_PENDING as a failure: it read cmn0000.fpk again on
// every frame, on a black screen.
static uint32_t Finish(File& f, uint32_t status, uint32_t n, uint32_t event, uint32_t apc, uint32_t apc_ctx, uint32_t iosb) {
    IoStatus(iosb, status, n);
    SetEvent(f);
    Complete(event, apc, apc_ctx, iosb);
    return f.async && status == kSuccess ? kPending : status;
}

static uint64_t Offset(File& f, uint32_t off) {
    int64_t o = off ? int64_t(R64(off)) : -1;
    return o >= 0 ? uint64_t(o) : f.pos;
}

// STREET: the game disc's files of 2 MB or less, whole in memory after their first read. In a match the game reads the
// same few KB of data\datafile.big again and again; from the SD card each read queued on the loading thread behind
// the audio streams' 64 KB reads, and the game thread waited 50-90 ms for it (a hitch every 20 s or so). Null for any
// other file (remembered, so a big one costs its size check once).
constexpr uint64_t kSmallFile = 2ull << 20, kSmallFilesBudget = 48ull << 20;
using FileBytes = std::shared_ptr<const std::vector<uint8_t>>;

static FileBytes SmallFile(File& f) {
    const std::string& x = f.xbox;
    bool game_disc = f.entry || (x.size() > 2 && std::tolower(uint8_t(x[0])) == 'd' && x[1] == ':') ||
                     x.rfind("\\Device\\CdRom0", 0) == 0;
    if (!game_disc || f.dir) return nullptr;
    static std::mutex lock;
    static std::unordered_map<std::string, FileBytes> cache;
    static uint64_t used = 0;
    std::string key = f.entry ? "disc:" + std::to_string(reinterpret_cast<uintptr_t>(f.entry)) : f.host.string();
    std::lock_guard<std::mutex> guard(lock);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    FileBytes& slot = cache[key];  // null unless filled below
    std::error_code ec;
#ifdef __SWITCH__
    uint64_t size = f.entry ? f.entry->size : f.lazy ? HostSize(f.host, ec) : f.fp ? OpenSize(f.fp, ec) : uint64_t(-1);
#else
    uint64_t size = f.entry ? f.entry->size : HostSize(f.host, ec);
#endif
    if (ec || !size || size > kSmallFile || used + size > kSmallFilesBudget) return nullptr;
    auto bytes = std::make_shared<std::vector<uint8_t>>(size_t(size));
    size_t got = 0;
    if (f.entry) {
        got = MountedDisc()->Read(*f.entry, 0, bytes->data(), size_t(size));
    } else if (OpenLazy(f)) {
        Seek(f.fp, 0);
        got = std::fread(bytes->data(), 1, size_t(size), f.fp);
    }
    if (got != size) return nullptr;
    if (app::kGame.patch_file) app::kGame.patch_file(x, *bytes);
    used += size;
    LOGI("files: %s kept in memory (%llu KB, %llu KB of small files)", x.c_str(), (unsigned long long)(size >> 10),
         (unsigned long long)(used >> 10));
    slot = std::move(bytes);
    return slot;
}

// the transfer itself, with f->io held: the bytes and the file position; the status and count for Finish
struct Moved {
    uint32_t status, n;
};
static Moved Move(const std::shared_ptr<File>& f, bool write, uint64_t pos, uint32_t buf, uint32_t len, uint32_t pages,
                  FileBytes cached) {
    // diagnostic: SHEROES_SLOW_READ_MS stretches every read, so a PC loads as slowly as an SD card
    static const int slow_ms = [] {
        const char* v = std::getenv("SHEROES_SLOW_READ_MS");
        return v ? std::atoi(v) : 0;
    }();
    if (slow_ms > 0 && !write && !cached) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
    auto t0 = std::chrono::steady_clock::now();
    if (!cached && f->lazy && !OpenLazy(*f)) return {kAccessDenied, 0};
    if (f->fp && !cached) Seek(f->fp, pos);
    uint32_t n = 0;
    while (cached && n < len && pos + n < cached->size()) {
        uint32_t at = pages ? R32(pages + n / 0x1000 * 8) + n % 0x1000 : buf + n;
        uint32_t chunk = pages ? std::min(len - n, 0x1000 - n % 0x1000) : len - n;
        chunk = uint32_t(std::min<uint64_t>(chunk, cached->size() - (pos + n)));
        std::memcpy(HostPtr(at), cached->data() + pos + n, chunk);
        n += chunk;
    }
    while (!cached && n < len) {
        uint32_t at = pages ? R32(pages + n / 0x1000 * 8) + n % 0x1000 : buf + n;
        // what is left: the Switch's bounce below cuts it into 1 MB pieces, which must not run past the buffer
        uint32_t chunk = pages ? std::min(len - n, 0x1000 - n % 0x1000) : len - n;
#ifdef __SWITCH__
        // guest memory is code memory here, which the file system's IPC refuses as a buffer: through the heap
        static thread_local std::vector<uint8_t> bounce;
        chunk = std::min<uint32_t>(chunk, 1u << 20);
        bounce.resize(chunk);
        if (write) std::memcpy(bounce.data(), HostPtr(at), chunk);
        size_t done = f->entry ? MountedDisc()->Read(*f->entry, pos + n, bounce.data(), chunk)
                      : write  ? std::fwrite(bounce.data(), 1, chunk, f->fp)
                               : std::fread(bounce.data(), 1, chunk, f->fp);
        if (!write) std::memcpy(HostPtr(at), bounce.data(), done);
#else
        size_t done = f->entry ? MountedDisc()->Read(*f->entry, pos + n, HostPtr(at), chunk)
                      : write  ? std::fwrite(HostPtr(at), 1, chunk, f->fp)
                               : std::fread(HostPtr(at), 1, chunk, f->fp);
#endif
        n += uint32_t(done);
        if (done < chunk) break;
    }
    f->pos = pos + n;
    // ROTS: a movie being streamed (CRI Sofdec .sfd, XMV, Bink): the Switch host raises the CPU clock while it plays
    if (!write && IsMovie(f->xbox)) {
        {
            std::lock_guard<std::mutex> ml(g_movie_lock);
            g_movie_file = f;
        }
        g_movie_read_ms.store(uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count()));
    }
    if (logx::kVerbose) {
        // STREET: every transfer with a hash of its bytes, to compare a run with another platform's line by line
        uint32_t hash = 2166136261u;
        if (!pages)
            for (uint32_t i = 0; i < n; i++) hash = (hash ^ static_cast<const uint8_t*>(HostPtr(buf))[i]) * 16777619u;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        LOGD("%s %s at %llX: %u of %u to %08X, fnv %08X, %.1f ms%s", write ? "write" : "read", f->xbox.c_str(),
             (unsigned long long)pos, n, len, buf, hash, ms, cached ? " (memory)" : "");
    }
    if (write && n < len) return {0xC000007F, n};  // disk full
    if (!write && len && !n) return {kEndOfFile, 0};
    return {kSuccess, n};
}

// ROTS: reads on a handle opened for asynchronous I/O, with their offset given, that need the card are done in the
// background, as the Xbox's drivers do: the call returns STATUS_PENDING at once and a worker reads, in the order asked,
// then writes the status block, signals the file and the event and queues the APC to the thread that asked. Done on
// the asking thread before, each read stopped the title's main loop (Revenge of the Sith reads its streamed dialogue
// and level data, gen_dialogue.rpx and others, from the main thread with overlapped reads it polls each frame: 915 of
// 1049 reads in a harness run were the main thread's), a stall the length of the read on the SD card.
// ROTS_SYNC_READS=1 goes back to reading on the asking thread.
struct AsyncRead {
    std::shared_ptr<File> f;
    uint64_t pos;
    uint32_t event, apc, apc_ctx, iosb, buf, len, pages;
    ThreadRef issuer;
    uint64_t epoch;
};
std::mutex g_aio_lock;
std::condition_variable g_aio_cv;
std::deque<AsyncRead> g_aio;
std::atomic<uint64_t> g_aio_epoch{0};  // a soft reboot drops the reads queued before it

static void AsyncWorker() {
    for (;;) {
        AsyncRead r;
        {
            std::unique_lock<std::mutex> lock(g_aio_lock);
            g_aio_cv.wait(lock, [] { return !g_aio.empty(); });
            r = std::move(g_aio.front());
            g_aio.pop_front();
        }
        if (r.epoch != g_aio_epoch.load() || !HostGuestEnter()) continue;
        Moved m;
        {
            std::lock_guard<std::mutex> lock(r.f->io);
            m = Move(r.f, false, r.pos, r.buf, r.len, r.pages, nullptr);
        }
        // the count before the status: a title polling the status block sees both
        if (r.iosb) {
            W32(r.iosb + 4, m.n);
            std::atomic_thread_fence(std::memory_order_release);
            W32(r.iosb, m.status);
        }
        SetEvent(*r.f);
        if (r.event)
            if (ObjectRef e = FromHandle(r.event)) SetEvent(*e);
        if (r.apc && r.issuer) QueueApc(*r.issuer, r.apc, r.apc_ctx, r.iosb, 0);
        HostGuestLeave();
    }
}

static const bool kAioRebootHook = (kernel::OnReboot([] {
    std::lock_guard<std::mutex> lock(g_aio_lock);
    g_aio.clear();
    g_aio_epoch.fetch_add(1);
}), true);

static bool AsyncReads() {
    static const bool on = [] {
        const char* e = std::getenv("ROTS_SYNC_READS");
        if (e && *e == '1') return false;
        std::thread(AsyncWorker).detach();
        return true;
    }();
    return on;
}

// pages is the FILE_SEGMENT_ELEMENT array for scatter and gather, 0 for one buffer
static uint32_t Transfer(const Args& a, bool write, uint32_t pages) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], buf = a[5], len = a[6], off = a[7];
    auto f = FileFromHandle(h);
    if (!f || (!f->fp && !f->entry && !f->lazy)) return kInvalidHandle;
    if (write && (f->entry || f->lazy)) return kAccessDenied;
    ResetEvent(*f);
    std::unique_lock<std::mutex> lock(f->io);
    uint64_t pos = Offset(*f, off);
    // ROTS: a skipped movie reads as ended: the title's own player finishes it as if it had played out
    if (!write && f->skipped.load()) {
        f->pos = pos;
        return Finish(*f, kEndOfFile, 0, event, apc, apc_ctx, iosb);
    }
    FileBytes cached = write ? nullptr : SmallFile(*f);
    if (!write && !cached && f->async && off && R64(off) < (1ull << 62) && AsyncReads()) {
        if (event)
            if (ObjectRef e = FromHandle(event)) ResetEvent(*e);
        IoStatus(iosb, kPending, 0);
        lock.unlock();
        std::lock_guard<std::mutex> q(g_aio_lock);
        g_aio.push_back({f, pos, event, apc, apc_ctx, iosb, buf, len, pages, CurrentRef(), g_aio_epoch.load()});
        g_aio_cv.notify_one();
        return kPending;
    }
    Moved m = Move(f, write, pos, buf, len, pages, cached);
    return Finish(*f, m.status, m.n, event, apc, apc_ctx, iosb);
}

KERNEL_EXPORT(219, NtReadFile, 8) { return Transfer(a, false, 0); }
KERNEL_EXPORT(236, NtWriteFile, 8) { return Transfer(a, true, 0); }
// the buffer argument is an array of one page per element
KERNEL_EXPORT(220, NtReadFileScatter, 8) { return Transfer(a, false, a[5]); }
KERNEL_EXPORT(237, NtWriteFileGather, 8) { return Transfer(a, true, a[5]); }

KERNEL_EXPORT(211, NtQueryInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], out = a[2], len = a[3], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    std::error_code ec;
    uint64_t size = f->dir ? 0 : f->entry ? f->entry->size : HostSize(f->host, ec);
    // the game's own files are not written: their size is the one taken once (Stat), not asked through the handle
    if (f->fp && !InGame(f->host.string())) {
        std::fflush(f->fp);
#ifdef __SWITCH__
        size = OpenSize(f->fp, ec);
#else
        size = HostSize(f->host, ec);
#endif
    }
    if (ec) LOG_FIRST(8, "NtQueryInformationFile: no size for %s (%s)", f->host.string().c_str(), ec.message().c_str());
    LOGD("query %s class %u: size %llu", f->xbox.c_str(), cls, (unsigned long long)size);
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        case 4: {  // FileBasicInformation
            uint64_t t = f->entry ? MountedDisc()->Time() : HostTime(f->host);
            for (int i = 0; i < 4; i++) W64(out + i * 8, t);
            W32(out + 0x20, f->dir ? 0x10 : 0x80);
            IoStatus(iosb, kSuccess, 0x28);
            return kSuccess;
        }
        case 5:  // FileStandardInformation
            W64(out, (size + 0xFFF) & ~0xFFFull);
            W64(out + 8, size);
            W32(out + 0x10, 1);  // NumberOfLinks
            G<uint8_t>(out + 0x15) = f->dir;  // Directory, after DeletePending
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 14:  // FilePositionInformation
            W64(out, f->pos);
            IoStatus(iosb, kSuccess, 8);
            return kSuccess;
        case 34:  // FileNetworkOpenInformation
            // BREXTREME: the size found above (through the handle when the file is open): on the Switch a stat by
            // path fails on a file the title holds open for writing, which gave GetFileSize 0xFFFFFFFF and made
            // Bloody Roar Extreme's new save fail ("failed") right after creating its data file
            if (f->entry) FillNetworkInfo(out, *f->entry);
            else FillNetworkInfo(out, HostTime(f->host), f->dir || ec ? 0 : size, f->dir);
            IoStatus(iosb, kSuccess, 0x38);
            return kSuccess;
        case 6: {  // FileInternalInformation: a file id, which XAPI's GetFileInformationByHandle needs (ROTS sizes
                   // every level sub-pack through it; refused, mission 1 stopped on the dirty-disc screen)
            uint64_t id = std::hash<std::string>{}(f->xbox);
            W64(out, id ? id : 1);
            IoStatus(iosb, kSuccess, 8);
            return kSuccess;
        }
        case 22:  // FileModeInformation
        case 16:
            IoStatus(iosb, kSuccess, 4);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(226, NtSetInformationFile, 5) {
    uint32_t h = a[0], iosb = a[1], in = a[2], cls = a[4];
    auto f = FileFromHandle(h);
    if (!f) return kInvalidHandle;
    if (f->entry && cls != 14 && cls != 4) return kAccessDenied;
    std::error_code ec;
    switch (cls) {
        case 14:  // position
            f->pos = R64(in);
            break;
        case 19:  // allocation: only a size below the end of file cuts it
        case 20:  // end of file
            if (f->fp) {
                std::lock_guard<std::mutex> lock(f->io);
                std::fflush(f->fp);
#ifdef __SWITCH__
                uint64_t size = OpenSize(f->fp, ec);
#else
                uint64_t size = HostSize(f->host, ec);
#endif
                if (cls == 20 || R64(in) < size) {
#ifdef __SWITCH__
                    if (::truncate(f->host.c_str(), off_t(R64(in)))) ec = std::error_code(errno, std::generic_category());
#else
                    fs::resize_file(f->host, R64(in), ec);
#endif
                    ForgetGameStats(f->host.string());
                }
            }
            break;
        case 13:  // disposition
            f->delete_on_close = G<uint8_t>(in) != 0;
            break;
        case 10: {  // rename: ReplaceIfExists, RootDirectory, FileName (ANSI_STRING inline)
            uint32_t root = R32(in + 4);
            std::string target = ReadAnsiString(in + 8);
            std::string host = ResolvePath(target, root);
            if (host.empty()) return kObjectPathNotFound;
            if (f->fp) std::fflush(f->fp);
            fs::rename(f->host, host, ec);
            ForgetGameStats(f->host.string());
            ForgetGameStats(host);
            if (ec) return kAccessDenied;
            f->host = host;
            break;
        }
        case 4:  // basic, times and attributes are not kept
            break;
        default:
            LOG_FIRST(8, "NtSetInformationFile: class %u", cls);
            return kInvalidParameter;
    }
    IoStatus(iosb, kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(218, NtQueryVolumeInformationFile, 5) {
    uint32_t iosb = a[1], out = a[2], len = a[3], cls = a[4];
    std::memset(HostPtr(out), 0, len);
    switch (cls) {
        // ROTS: 1.875 GiB total, 1.5 GiB free, in 16 KiB units. Was 16 GiB and 8 GiB: exact multiples of 4 GiB, which a
        // title that counts bytes in 32 bits reads as 0 free (Ice Age 2's Switch port hit just that on its save screen,
        // "not enough free blocks"); both now stay under 2 GiB, so signed 32-bit sums hold them too. More than any
        // Xbox save needs (the dashboard counts 50,000 blocks, 0.8 GB, as full)
        case 3:  // FileFsSizeInformation
            W64(out, 0x1E000);
            W64(out + 8, 0x18000);
            W32(out + 0x10, 32);
            W32(out + 0x14, 512);
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 7:  // FileFsFullSizeInformation, the same
            W64(out, 0x1E000);
            W64(out + 8, 0x18000);
            W64(out + 0x10, 0x18000);
            W32(out + 0x18, 32);
            W32(out + 0x1C, 512);
            IoStatus(iosb, kSuccess, 0x20);
            return kSuccess;
        case 1:  // FileFsVolumeInformation
            W32(out + 8, 0x12345678);  // VolumeSerialNumber
            IoStatus(iosb, kSuccess, 0x18);
            return kSuccess;
        case 5:  // FileFsAttributeInformation: attributes, max name, name length, "FATX"
            W32(out, 0);
            W32(out + 4, 42);
            W32(out + 8, 4);
            std::memcpy(HostPtr(out + 12), "FATX", 4);
            IoStatus(iosb, kSuccess, 16);
            return kSuccess;
        default:
            LOG_FIRST(8, "NtQueryVolumeInformationFile: class %u", cls);
            return kInvalidParameter;
    }
}

KERNEL_EXPORT(207, NtQueryDirectoryFile, 10) {
    uint32_t h = a[0], event = a[1], apc = a[2], apc_ctx = a[3], iosb = a[4], out = a[5], len = a[6], cls = a[7],
             mask = a[8], restart = a[9];
    auto f = FileFromHandle(h);
    if (!f || !f->dir) return kInvalidHandle;
    if (cls != 1) {
        LOG_FIRST(8, "NtQueryDirectoryFile: class %u", cls);
        return kInvalidParameter;
    }
    std::string pat = mask ? ReadAnsiString(mask) : "*";
    if (!f->listed || (restart & 0xFF)) {
        f->entries.clear();
        f->disc_entries.clear();
        if (f->entry) {
            for (const Disc::Entry& e : f->entry->children)
                if (WildMatch(pat, e.name)) f->disc_entries.push_back(&e);
        } else {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(f->host, ec))
                if (WildMatch(pat, e.path().filename().string())) f->entries.push_back(e);
            std::sort(f->entries.begin(), f->entries.end());
        }
        f->next = 0;
        f->listed = true;
    }
    if (f->next >= (f->entry ? f->disc_entries.size() : f->entries.size())) {
        IoStatus(iosb, kNoMoreFiles, 0);
        return f->next == 0 ? kNoSuchFile : kNoMoreFiles;
    }
    size_t i = f->next++;
    std::string fname = f->entry ? f->disc_entries[i]->name : f->entries[i].path().filename().string();
    uint32_t need = 0x40 + uint32_t(fname.size());
    if (len < need) return kBufferTooSmall;
    std::memset(HostPtr(out), 0, need);
    // times, sizes and attributes sit at the same place
    if (f->entry) FillNetworkInfo(out + 8, *f->disc_entries[i]);
    else FillNetworkInfo(out + 8, f->entries[i].path());
    // FILE_DIRECTORY_INFORMATION has EndOfFile before AllocationSize, the reverse of the network info
    W32(out + 0x38, R32(out + 8 + 0x30));
    uint64_t alloc = R64(out + 8 + 0x20), eof = R64(out + 8 + 0x28);
    W64(out + 0x28, eof);
    W64(out + 0x30, alloc);
    W32(out + 0x3C, uint32_t(fname.size()));
    std::memcpy(HostPtr(out + 0x40), fname.data(), fname.size());
    IoStatus(iosb, kSuccess, need);
    Complete(event, apc, apc_ctx, iosb);
    return kSuccess;
}

KERNEL_EXPORT(210, NtQueryFullAttributesFile, 2) {
    std::string name = ReadAnsiString(R32(a[0] + 4));
    Target t = Resolve(name, R32(a[0]));
    if (t.disc ? !t.entry : t.host.empty() || !Stat(t.host).exists) {
        LOG_FIRST(200, "missing %s", name.c_str());
        return kObjectNameNotFound;
    }
    if (t.disc) FillNetworkInfo(a[1], *t.entry);
    else FillNetworkInfo(a[1], t.host);
    return kSuccess;
}

KERNEL_EXPORT(195, NtDeleteFile, 1) {
    std::string host = ResolvePath(ReadAnsiString(R32(a[0] + 4)), R32(a[0]));
    std::error_code ec;
    if (host.empty() || !fs::remove(host, ec)) return kObjectNameNotFound;
    ForgetGameStats(host);
    return kSuccess;
}

KERNEL_EXPORT(198, NtFlushBuffersFile, 2) {
    if (auto f = FileFromHandle(a[0]))
        if (f->fp) std::fflush(f->fp);
    IoStatus(a[1], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(200, NtFsControlFile, 10) {
    IoStatus(a[4], kSuccess, 0);
    return kSuccess;
}

KERNEL_EXPORT(196, NtDeviceIoControlFile, 10) {
    uint32_t iosb = a[4], code = a[5], out = a[8], out_len = a[9];
    LOG_FIRST(16, "NtDeviceIoControlFile: code %08X", code);
    if (out && out_len) std::memset(HostPtr(out), 0, out_len);
    if (code == 0x70000 && out_len >= 0x18) {  // IOCTL_DISK_GET_DRIVE_GEOMETRY
        W64(out, 1000);
        W32(out + 8, 12);
        W32(out + 12, 255);
        W32(out + 16, 63);
        W32(out + 20, 512);
    }
    IoStatus(iosb, kSuccess, out_len);
    return kSuccess;
}

KERNEL_EXPORT(67, IoCreateSymbolicLink, 2) {
    CreateSymbolicLink(ReadAnsiString(a[0]), ReadAnsiString(a[1]));
    return kSuccess;
}

KERNEL_EXPORT(69, IoDeleteSymbolicLink, 1) {
    return DeleteSymbolicLink(ReadAnsiString(a[0])) ? kSuccess : kObjectNameNotFound;
}

KERNEL_EXPORT(203, NtOpenSymbolicLinkObject, 2) {
    std::string name = ObjectName(a[1]);
    std::string target = QuerySymbolicLink(name);
    if (target.empty()) return kObjectNameNotFound;
    auto l = std::make_shared<Link>();
    l->target = target;
    W32(a[0], NewHandle(l));
    return kSuccess;
}

KERNEL_EXPORT(215, NtQuerySymbolicLinkObject, 3) {
    ObjectRef o = FromHandle(a[0]);
    if (!o || o->type != ObjType::SymLink) return kInvalidHandle;
    auto& t = static_cast<Link&>(*o).target;
    uint32_t str = a[1];
    uint16_t maxlen = R16(str + 2);
    if (a[2]) W32(a[2], uint32_t(t.size()));
    if (t.size() > maxlen) return kBufferTooSmall;
    std::memcpy(HostPtr(R32(str + 4)), t.data(), t.size());
    W16(str, uint16_t(t.size()));
    return kSuccess;
}

// ROTS: when a movie file was last read, steady clock ms (0: never)
uint64_t MovieReadMs() { return g_movie_read_ms.load(); }

// ROTS: the movie playing now ends: its file reads as ended from here on
bool SkipMovie() {
    std::shared_ptr<File> m;
    {
        std::lock_guard<std::mutex> ml(g_movie_lock);
        m = g_movie_file.lock();
    }
    if (!m || m->skipped.exchange(true)) return false;
    LOGI("files: movie %s skipped", m->xbox.c_str());
    return true;
}

}  // namespace kernel
