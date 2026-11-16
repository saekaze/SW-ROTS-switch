// ROTS-Switch-Builder.exe: builds the Switch version of Star Wars Episode III: Revenge of the Sith (Xbox) from the
// user's own disc image, on their own PC, in one go. It
//   1. asks for the disc image (.iso) or the extracted disc folder, and an optional picture for the icon,
//   2. makes sure WSL is there (Windows' own Linux layer), and makes its own distribution for the build
//      ("rots-builder", Ubuntu 24.04 from cloud-images.ubuntu.com), so nothing else on the PC is touched,
//   3. runs builder.sh in it (compiled into this exe; the same file is in tools/windows/ of the repository): it
//      installs the build tools and devkitPro, gets the port's source code at this exe's version, extracts the disc
//      into the SD card folder and builds rots.nro,
//   4. leaves ROTS-for-Switch\switch\ next to the exe, ready to copy to the root of the SD card.
// Nothing of the game is uploaded anywhere. Built by tools/windows/build_exe.sh (MinGW).
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "builder_script.h"  // kBuilderScript: tools/windows/builder.sh

#ifndef ROTS_REF
#define ROTS_REF "main"
#endif

static const wchar_t* kDistro = L"rots-builder";
static const wchar_t* kRootfsUrl =
    L"https://cloud-images.ubuntu.com/wsl/releases/24.04/current/ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz";

static void Say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void Utf8(const wchar_t* w, char* out, int n) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL); }

static void PauseExit(int code) {
    Say("\nPress Enter to close this window.");
    getchar();
    exit(code);
}

// a line typed or dragged in: trimmed, its quotes gone (Explorer adds them around paths with spaces)
static void ReadPath(wchar_t* out, int n) {
    out[0] = 0;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode, got = 0;
    if (GetConsoleMode(h, &mode)) {
        // the console's own UTF-16: paths with any letters (a Cyrillic user name) arrive whole
        if (!ReadConsoleW(h, out, (DWORD)n - 1, &got, NULL)) got = 0;
        out[got] = 0;
    } else if (!fgetws(out, n, stdin)) {
        return;
    }
    size_t len = wcslen(out);
    while (len && (out[len - 1] == L'\n' || out[len - 1] == L'\r' || out[len - 1] == L' ')) out[--len] = 0;
    wchar_t* s = out;
    while (*s == L' ') s++;
    if (*s == L'"') {
        s++;
        wchar_t* q = wcschr(s, L'"');
        if (q) *q = 0;
    }
    memmove(out, s, (wcslen(s) + 1) * sizeof(wchar_t));
}

// runs a command line in this console and waits; the exit code, or -1 when it could not start
static int Run(const wchar_t* cmdline) {
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    wchar_t* buf = _wcsdup(cmdline);
    BOOL ok = CreateProcessW(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    free(buf);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// runs a command line with its output captured (not shown); the exit code
static int Capture(const wchar_t* cmdline, char* out, DWORD n) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    wchar_t* buf = _wcsdup(cmdline);
    BOOL ok = CreateProcessW(NULL, buf, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(buf);
    CloseHandle(wr);
    DWORD got = 0, r;
    if (ok)
        while (got + 1 < n && ReadFile(rd, out + got, n - 1 - got, &r, NULL) && r) got += r;
    out[got] = 0;
    CloseHandle(rd);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// wsl.exe -l -q writes UTF-16: is our distribution in it
static int HasDistro(void) {
    static char out[65536];
    if (Capture(L"wsl.exe -l -q", out, sizeof out) != 0) return 0;
    // compare ignoring the zero bytes of UTF-16
    char ascii[32768];
    int j = 0;
    for (int i = 0; out[i] || out[i + 1]; i++)
        if (out[i] && j < (int)sizeof ascii - 1) ascii[j++] = out[i];
    ascii[j] = 0;
    for (char* line = strtok(ascii, "\r\n"); line; line = strtok(NULL, "\r\n"))
        if (!strcmp(line, "rots-builder")) return 1;
    return 0;
}

static int Elevated(const wchar_t* exe, const wchar_t* args) {
    SHELLEXECUTEINFOW se = {sizeof se};
    se.fMask = SEE_MASK_NOCLOSEPROCESS;
    se.lpVerb = L"runas";
    se.lpFile = exe;
    se.lpParameters = args;
    se.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&se) || !se.hProcess) return -1;
    WaitForSingleObject(se.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(se.hProcess, &code);
    CloseHandle(se.hProcess);
    return (int)code;
}

static int WriteFileUtf8(const wchar_t* path, const char* text, size_t len) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return 0;
    size_t w = fwrite(text, 1, len, f);
    fclose(f);
    return w == len;
}

// C:\a\b -> /mnt/c/a/b (WSL's default mount); only for this exe's own folder in %LOCALAPPDATA%
static void MntPath(const wchar_t* win, wchar_t* out, int n) {
    _snwprintf(out, n, L"/mnt/%c/%ls", towlower(win[0]), win + 3);
    out[n - 1] = 0;
    for (wchar_t* p = out; *p; p++)
        if (*p == L'\\') *p = L'/';
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Revenge of the Sith for Switch - builder");
    Say("Star Wars Episode III: Revenge of the Sith (Xbox) for Nintendo Switch - builder %s\n", ROTS_REF);
    Say("Builds the Switch version from YOUR OWN copy of the game, on this PC. Nothing is uploaded.\n");
    Say("Needs: Windows 10 (2004 or newer) or 11, about 12 GB free on C:, an internet connection.\n");
    Say("Takes 20 to 40 minutes the first time (downloads and compiling), less after.\n\n");

    // the disc: dragged onto the exe, or asked for
    wchar_t in[MAX_PATH * 2] = {0};
    if (argc > 1) {
        wcsncpy(in, argv[1], MAX_PATH * 2 - 1);
    } else {
        Say("1. Drag your disc image (.iso) into this window, or the folder of your extracted disc\n");
        Say("   (the one with default.xbe in it), then press Enter:\n> ");
        ReadPath(in, MAX_PATH * 2);
    }
    DWORD attr = GetFileAttributesW(in);
    if (!in[0] || attr == INVALID_FILE_ATTRIBUTES) {
        Say("\nNot found: %ls\n", in);
        PauseExit(1);
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        wchar_t xbe[MAX_PATH * 2];
        _snwprintf(xbe, MAX_PATH * 2, L"%ls\\default.xbe", in);
        if (GetFileAttributesW(xbe) == INVALID_FILE_ATTRIBUTES) {
            Say("\nThat folder has no default.xbe. Pick the folder default.xbe is in, or the .iso itself.\n");
            PauseExit(1);
        }
    }

    // the icon: any picture, or the game's own
    wchar_t icon[MAX_PATH * 2] = {0};
    Say("\n2. Optional: drag a picture (JPG or PNG) here for the icon in the homebrew menu. It is cut to a square\n");
    Say("   and made 256x256. Or just press Enter for the game's own picture:\n> ");
    ReadPath(icon, MAX_PATH * 2);
    if (icon[0] && GetFileAttributesW(icon) == INVALID_FILE_ATTRIBUTES) {
        Say("   Not found, using the game's own picture.\n");
        icon[0] = 0;
    }

    // the output: next to the exe
    wchar_t exe[MAX_PATH], outdir[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy(outdir, exe);
    wchar_t* slash = wcsrchr(outdir, L'\\');
    if (slash) *slash = 0;
    wcsncat(outdir, L"\\ROTS-for-Switch", MAX_PATH - wcslen(outdir) - 1);
    CreateDirectoryW(outdir, NULL);

    // the builder's own Linux, in %LOCALAPPDATA%\rots-builder: imported from Ubuntu's WSL image
    wchar_t base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) PauseExit(1);
    wcsncat(base, L"\\rots-builder", MAX_PATH - wcslen(base) - 1);
    CreateDirectoryW(base, NULL);
    Say("\n== checking WSL (Windows Subsystem for Linux)\n");
    if (!HasDistro()) {
        wchar_t tar[MAX_PATH], disk[MAX_PATH], cmd[4096];
        _snwprintf(tar, MAX_PATH, L"%ls\\ubuntu.tar.gz", base);
        _snwprintf(disk, MAX_PATH, L"%ls\\disk", base);
        if (GetFileAttributesW(tar) == INVALID_FILE_ATTRIBUTES) {
            Say("making the builder's own Linux: downloading Ubuntu 24.04 (about 350 MB)\n");
            wchar_t part[MAX_PATH];
            _snwprintf(part, MAX_PATH, L"%ls.part", tar);
            _snwprintf(cmd, 4096, L"curl.exe -L --fail --retry 3 -o \"%ls\" \"%ls\"", part, kRootfsUrl);
            if (Run(cmd) != 0 || !MoveFileExW(part, tar, MOVEFILE_REPLACE_EXISTING)) {
                Say("\nThe download failed. Check the internet connection and run the builder again.\n");
                PauseExit(1);
            }
        }
        _snwprintf(cmd, 4096, L"wsl.exe --import %ls \"%ls\" \"%ls\" --version 2", kDistro, disk, tar);
        int r = Run(cmd);
        if (r != 0) {
            char out[8192];
            if (Capture(L"wsl.exe --version", out, sizeof out) != 0) {
                // no WSL at all (or the old one built into Windows): Microsoft's installer, then a restart
                Say("\nWSL is not set up on this PC. It is Microsoft's own and free; the build runs in it.\n");
                Say("Install it now? Windows asks for permission, then the PC needs a restart. (Y/n) ");
                wchar_t yn[16];
                ReadPath(yn, 16);
                if (yn[0] == L'n' || yn[0] == L'N') PauseExit(1);
                int e = Elevated(L"wsl.exe", L"--install --no-distribution");
                if (e != 0) e = Elevated(L"wsl.exe", L"--install");
                if (e != 0) {
                    Say("\nThe WSL install did not finish (code %d). Open PowerShell as administrator, run\n", e);
                    Say("    wsl --install\nthen restart the PC and run this builder again.\n");
                    PauseExit(1);
                }
                Say("\nWSL is installed. RESTART THE PC, then run this builder again (it carries on from here).\n");
                PauseExit(0);
            }
            Say("\nupdating WSL and trying again (Windows may ask for permission)\n");
            Elevated(L"wsl.exe", L"--update");
            r = Run(cmd);
        }
        if (r != 0) {
            Say("\nWSL could not set up the builder's Linux. Usually virtualization is off: turn on \"SVM\" (AMD)\n");
            Say("or \"Intel VT-x\" in the PC's BIOS, and \"Virtual Machine Platform\" in Windows features, then\n");
            Say("restart and run the builder again.\n");
            PauseExit(1);
        }
        DeleteFileW(tar);
    }
    Say("ready\n");

    // the script and its parameters
    wchar_t script[MAX_PATH], params[MAX_PATH], mnt[MAX_PATH * 2], cmd[4096];
    _snwprintf(script, MAX_PATH, L"%ls\\builder.sh", base);
    _snwprintf(params, MAX_PATH, L"%ls\\params.txt", base);
    if (!WriteFileUtf8(script, kBuilderScript, strlen(kBuilderScript))) {
        Say("\nCannot write %ls\n", script);
        PauseExit(1);
    }
    char p[8192], a[MAX_PATH * 6], b[MAX_PATH * 6], c[MAX_PATH * 6];
    Utf8(in, a, sizeof a);
    Utf8(outdir, b, sizeof b);
    Utf8(icon, c, sizeof c);
    snprintf(p, sizeof p, "%s\n%s\n%s\n%s\n", a, b, ROTS_REF, c);
    WriteFileUtf8(params, p, strlen(p));
    MntPath(script, mnt, MAX_PATH * 2);
    _snwprintf(cmd, 4096, L"wsl.exe -d %ls -u root --exec bash \"%ls\"", kDistro, mnt);

    Say("\n== building (the window shows what it does; leave it open)\n");
    int r = Run(cmd);
    if (r != 0) {
        Say("\nThe build stopped (code %d). The messages above say why. Run the builder again to retry: it keeps\n", r);
        Say("what it already did. If it keeps failing, report the last lines on the project's issue page.\n");
        PauseExit(1);
    }

    Say("\nAll done. Your SD card folder is ready:\n    %ls\\switch\n\n", outdir);
    Say("1. Copy that \"switch\" folder to the root of your SD card (merge it with the switch folder there).\n");
    Say("2. On the Switch: hold R while starting any game to get the homebrew menu with full memory, then pick\n");
    Say("   \"Star Wars Episode III (Xbox recomp)\".\n\n");
    Say("The builder keeps its Linux (about 8 GB) so a new version builds quickly. To remove it, open PowerShell\n");
    Say("and run:   wsl --unregister rots-builder\n");
    ShellExecuteW(NULL, L"open", outdir, NULL, NULL, SW_SHOWNORMAL);
    PauseExit(0);
    return 0;
}
