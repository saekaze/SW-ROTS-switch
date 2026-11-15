# Building

## What is needed

- **The game**: the files of a USA disc of Bloody Roar Extreme in `game/assets/` (a junction to an extracted disc is
  enough). The recompiler reads `game/assets/default.xbe`; the PC builds run the game from that folder.
- **PC**: Windows, Visual Studio 2026 (its Clang), CMake, Python 3.
- **Switch**: devkitPro in MSYS2 (`switch-dev`, deko3d, uam, the portlibs zstd, expat and zlib), and the Mesa 26.2.4
  Horizon build (NVK) in `nvk_sdk/` for the Vulkan renderer.

## PC

```
tools\build_pc.cmd
```

It builds `xbe_symbols` and `xbe_recomp`, writes `game/config/game.toml` the first time (XDK functions found by their
signatures in `sdk/xdk/5233`), recompiles `default.xbe` into `game/generated/` and builds `build-pc\BREXTREME-vk.exe`
and `build-pc\BREXTREME-d3d12.exe`. Any change to `game/config/game.toml` (a function added by hand, say) needs this
script run again, also before a Switch build: the Switch build compiles the same `game/generated/`.

Useful environment variables for the PC builds: `BREXTREME_DRAW_COPY=0` (draw from the game's memory, as the Switch
does), `BREXTREME_REUSE_CONSTANTS=1`, `XBE_D3D12_DEBUG=1` (the D3D12 debug layer), `XBE_CAPTURE_DIR` with the `CAPTURE`
pad script command (a frame capture: every draw, its states and textures).

## Switch

From PowerShell, in the MSYS2 shell (Git Bash's paths break the CMake cache):

```
$env:MSYSTEM='MSYS'; $env:CHERE_INVOKING='1'; & 'D:\msys64\usr\bin\bash.exe' -lc 'JOBS=6 switch/build.sh'
```

The result is `build-switch/brextreme.nro` (and its `.elf`, which the profiler needs). `VERBOSE=ON` builds the
verbose log; `JOBS` lowers the parallel jobs if GCC runs out of memory (the recompiled code is large).
`switch/icon.jpg` becomes the NRO's icon when it exists (checked when CMake configures).

### PGO

The recompiled game code is built with profile-guided optimisation. The profile is only valid for the exact source it
was taken with: a changed function loses its profile (GCC warns with `coverage-mismatch`), so the profile is renewed
after any change to the hot paths.

1. `PGO=generate switch/build.sh` builds the instrumented NRO in `build-switch-pgo/`. `switch/pgo.cpp` writes its
   counters every 60 seconds and on exit to `sdmc:/switch/brextreme-nx/pgo/session-NN.stream`.
2. Play it on the console for 10 minutes or more, through every stage.
3. `tools\pgo_merge.ps1 <folder with the session files>` merges them into `build-switch`'s `.gcda` files and deletes
   the objects (ninja does not see a `.gcda` change: without that the next build would only link).
4. `PGO=use switch/build.sh` builds the final NRO. `-fprofile-correction` is needed: the counters are not atomic and
   the threads race on them.

### Package

```
tools\package_switch.ps1
```

writes `switch_sd\switch\brextreme-nx\` beside the repo: the NRO, the default `brextreme-settings.toml`,
`LICENSES.txt` and an empty `game\` folder. `-Build build-switch-pgo` packages the instrumented NRO. When `installer/release/shadercache/` exists (the `shadercache` folder a console made in play, listed in `installer/release/manifest.json`), it goes in too, and the installer page adds it to its zip. It only replaces
its own files: logs a tester copied there stay. Run `python tools/make_licenses.py` first when a license changes.

## The console profiler

`profile = true` in the settings makes the NRO sample every thread at 1 kHz into `sdmc:/switch/brextreme-nx/prof.bin`.
Keep the `.elf` of that exact NRO, then:

```
python -I tools/prof_report.py prof.bin brextreme.elf brextreme.log --thread guest_14 --from 76 --to 172
```

gives the self time by function for the title thread (`guest_14`, the one that runs the game and records its draws)
or `render`, the callers of the hottest runtime functions and an inclusive view of the game's functions. The
`--from`/`--to` seconds come from the log's `perf:` blocks: pick a stretch of one scene with steady draws.
