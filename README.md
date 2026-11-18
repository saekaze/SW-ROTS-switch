# SW-ROTS-switch

**Star Wars Episode III: Revenge of the Sith** (original Xbox, 2005) as a native Nintendo Switch homebrew.

It is not an emulator. The x86 code of the game's `default.xbe` is statically recompiled to C++ and built for the
Switch's ARM CPU. Direct3D 8 and DirectSound are replaced at the API level, and a render thread draws everything with
**deko3d**, the Switch's own graphics API. This is the same toolchain that brings other original Xbox games to the
Switch at 60 fps on stock clocks.

> [!IMPORTANT]
> This repository contains **no game code and no game data**. You need your own copy of the game (Xbox disc),
> and everything made from it is made on your own computer.

## Status: 0.1.0 (pre-release, testers wanted)

| What | State |
| --- | --- |
| Code translation | Done: 28,207 functions (XDK 5849) |
| Boot, legal screens, title screen | Works |
| Menus (main menu, story missions, pause) | Works |
| Mission 1 (Rescue over Coruscant) | Plays (13 minutes on PC without a crash) |
| Level changes (the game reboots itself between front end and missions) | Works, done in-process (no restart of the app) |
| Sound (DirectSound at the API level) | Works |
| Movies (CRI Sofdec FMV: intro, crawl, cutscenes) | Work (the CPU is raised to 1785 MHz while one plays, so they keep up with their sound) |
| Switch build | Runs on the console (deko3d), 60 fps cap |
| Missions past the first | Untested |

Tested so far on PC (Linux, headless): title, Start, the self-reboot, crawl, intro, mission 1 gameplay, pause,
quit mission back to the main menu, and starting a mission again (three reboots in one session).

## Install

There is no download of the game program here: like the NFS Underground ports, **you build it from your own copy of
the game**. The program is made of the game's own code, translated, so it is only ever made on your computer, from
your disc. It is one command.

### Easiest: the Windows builder

1. Download **ROTS-Switch-Builder.exe** from [Releases](https://github.com/saekaze/SW-ROTS-switch/releases) and run it
   (Windows 10 2004 or newer, or 11; about 12 GB free on C:, an internet connection).
2. Drag your disc image (`.iso`, trimmed or a full dump) or your extracted disc folder into its window, press Enter.
3. Optional: drag any picture (JPG or PNG) for the homebrew menu icon. It is cut to a square and made 256x256. Or
   press Enter for the game's own picture.
4. Wait: 20 to 40 minutes the first time (downloads and compiling), a few minutes for a new version later.
5. Copy the `switch` folder from `ROTS-for-Switch` (next to the exe) to the root of your SD card.

The first time, it sets up WSL (Microsoft's Linux for Windows) if the PC does not have it yet; that can need a
restart, then run the builder again. It builds in its own Linux (`rots-builder`, about 8 GB, kept so the next version
builds quickly; `wsl --unregister rots-builder` removes it). Windows SmartScreen may warn about an unknown publisher:
More info, Run anyway. The exe is built by this repository's workflow from `tools/windows/`, contains no game code and
uploads nothing.

### By hand (Linux or WSL2)

You need a Switch with custom firmware (Atmosphere), your own copy of the game, and a Linux PC or Windows with WSL2.

1. Extract your disc image: `python3 tools/xiso_extract.py "Star Wars Episode III.iso" disc` (after step 3's
   clone), or with [extract-xiso](https://github.com/XboxDev/extract-xiso).
2. Install the tools (Ubuntu / WSL2):
   ```sh
   sudo apt install clang cmake ninja-build python3-pil git
   ```
   and [devkitPro](https://devkitpro.org/wiki/Getting_Started), then
   `sudo dkp-pacman -S switch-dev deko3d switch-zstd switch-expat switch-zlib`
3. Build:
   ```sh
   git clone https://github.com/saekaze/SW-ROTS-switch
   cd SW-ROTS-switch
   ./build_from_xbe.sh /path/to/extracted/disc --copy-game
   ```
   The first build takes 10 to 20 minutes.
4. Copy `out/switch_sd/switch` to the root of your SD card. You get `sdmc:/switch/rots/rots.nro`,
   `rots-settings.toml` and, with `--copy-game`, the game files in `sdmc:/switch/rots/game/` (about 2.4 GB).
5. Hold **R** while opening any installed game to get the homebrew menu with full memory (title takeover), then start
   **Star Wars Episode III (Xbox recomp)**. The album applet does not have enough memory. Press **+** during a movie
   to skip it.

The PAL release (Europe, title ID 4C410017) is the one it was made with; others with the same title ID are tried.

**The icon** is made from your own `default.xbe` too (or from your picture, with the Windows builder) (the dashboard picture the game carries: Obi-Wan and Anakin
dueling), so the build needs no artwork from anywhere. Want another picture? Put any 256x256 JPEG at
`switch/icon.jpg` before building and it is used instead.

### Settings

`sdmc:/switch/rots/rots-settings.toml` (comments inside): render resolution, frame cap, the game's own frame cap
(`game_fps`), SMAA, anisotropic filtering, button layout, language, CPU boost, the movie CPU boost
(`movie_boost_mhz`), character shadows (`shadows`) and more. **+ and -** together open the port menu in game.

### Frame rate

The game runs at 30 fps, as on the Xbox. `game_fps = 60` in the settings raises the game's own cap (in its
`vars_xbox.cfg`, changed as the file is read; your disc files stay as they are), but it is experimental: some
in-game cutscenes seem to run ahead of their dialogue at 60.

## Reporting problems

Open an issue with: where in the game it happened, what you saw, your Atmosphere version, and the logs from
`sdmc:/switch/rots/logs/`.

## Building by hand (developers)

`build_from_xbe.sh` runs these steps. The game's code is generated from your own `default.xbe`; nothing builds without the game.

Linux (or WSL2), with clang, cmake, ninja and devkitPro (`switch-dev`, `deko3d`, `uam`, `switch-zstd`,
`switch-expat`, `switch-zlib`):

```sh
tools/fetch_third_party.sh                     # glslang, SPIRV-Cross, uam, imgui, toml++, Zydis, SMAA, Vulkan headers
cmake -S tools/xbe_tools -B build-tools -G Ninja && ninja -C build-tools
mkdir -p game/assets && cp /path/to/your/disc/default.xbe game/assets/
cd game
../build-tools/xbe_recomp config/game.toml --xdk ../sdk/xdk    # -> game/generated/
cd ..
JOBS=4 switch/build.sh                         # -> build-switch/rots.nro
```

`game/config/game.toml` already carries the functions named by hand for this game (marked `ROTS:`). To scan an XBE
from scratch: `build-tools/xbe_symbols game/assets/default.xbe sdk/xdk --out <file>`.

Headless test on Linux (no window, GPU or sound; the reference renderer can dump frames):

```sh
ln -s ../sdk game/sdk
sdk/harness/build.sh -r game -g game/generated/game -o build-harness -n rots_headless -e tools/harness/stubs.cpp
cd build-harness && XBE_SOFT_REBOOT=1 REF=300 ./rots_headless /path/to/extracted/disc
```

## What was done for this game

On top of the toolchain it is built on:

- **In-process soft reboot.** The game reboots into itself (XLaunchNewImage) every time it goes from the front end
  into a mission and back. A homebrew app cannot start a new process on the Switch, so the runtime does it in place:
  every game thread unwinds at its next kernel call or wait, host threads stop calling into the game, the game's
  memory, objects, timers, DPCs, D3D, DirectSound and XInput state are reset, and the image boots again with the
  launch data page (about 10 ms).
- `NtUserIoApcDispatcher` (ReadFileEx completions: every pack file read) and `FileInternalInformation` (the level
  sub-packs: without it the game showed its dirty-disc screen).
- `D3DDevice_DrawIndexedVerticesUP` (menus, HUD) and `IDirectSoundBuffer_Pause` named by hand and replaced;
  four DirectSound 3D calls on streams (cone and distance parameters) answered by the port: the library's own code
  wrote into objects the port's streams do not have and crashed a few minutes into a mission;
  `D3DDevice_Release` returning 0 (the game releases the device in a loop).
- Suspended threads keep still: a waiting thread the game had suspended for a load took the lock it waited for and
  parked holding it, and the main loop froze on that lock a few minutes into a mission.
- Dialogue plays to the end: the game's dialogue player stops a line when the stream has not reported "playing" for 40
  frames, and the port's streams never reported it, so every line was cut after 1.4 s (0.7 s at 60 fps).
- A voice line's sound file is never freed while a line still plays it (the game's release thread could free it under
  the main thread: a freeze or a crash in missions).
- A voice line that cannot get a sound stream (the game has 12) gives its slot back: the game's own failure path kept
  the slot pointing at the previous line's freed file object, and the next frame called into it (a crash in missions).
- File reads the game asks for in the background are done in the background (they stalled the main loop on the SD card).
- The movies' YUV to RGB conversion (CRI Sofdec, MMX) and inverse DCT (SSE) run natively with SIMD, bit-exact
  (checked frame by frame against the game's own code); the conversion is about 9 times faster.
- Dynamic resolution on deko3d: when a scene needs more GPU time than a frame has (the heavier cutscenes), the
  internal resolution steps down to as little as 70% of `render_percent` and back up when there is room
  (`dynamic_resolution` in the settings file).
- The movie CPU boost asks sys-clk for an override when sys-clk is running (its profiles otherwise pulled the CPU back
  every second), and clears it when the movie ends.
- deko3d clears only the colour channels the game asks for: the game clears the alpha alone after the whole scene
  to mask its heat haze, and clearing all four channels turned the screen black under the HUD.
- The Switch build without a Vulkan driver (deko3d only), RotS paths and title checks.

## To do

- Check every mission on the console
- Play through the other missions
- Measure on the console, then profile for stock clocks

## Credits

- XboxXBE-Recomp by TGP482 (MIT): the recompiler, the kernel and the XDK replacements
- [mvp-nx, street-nx, brextreme-nx](https://github.com/xsaja) by xsaja (GPL-3.0): the Switch port of that toolchain,
  the deko3d renderer and the performance work
- [BLACK-NX-PORT](https://github.com/gerisson1996/BLACK-NX-PORT) by gerisson1996: the original Switch layer
- [sheroes-nx](https://github.com/FurryLilManChild/sheroes-nx) by FurryLilManChild (GPL-3.0): the port this one
  started from
- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet, with the Switch work of antoxa2584x and
  rashevskyv: the first, low-level version of this port (used to find the game's needs)
- [xemu](https://xemu.app) and [Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) as references
- devkitPro, libnx, deko3d, Mesa (uam), glslang, SPIRV-Cross, Dear ImGui, toml++, Zydis, SMAA, font8x8

Made with the help of AI coding tools.

## Legal

No affiliation with Lucasfilm, LucasArts, The Collective, Disney, Microsoft or Nintendo. Star Wars is a trademark
of Lucasfilm Ltd. This repository contains no assets or program code from the original game; you must provide your
own legally obtained copy. Running homebrew requires custom firmware, which may violate Nintendo's terms of service.

GPL-3.0 ([LICENSE](LICENSE)); everything else the build carries keeps its own license ([LICENSES.txt](LICENSES.txt),
[THIRD_PARTY.txt](THIRD_PARTY.txt), [SDK-LICENSE.txt](SDK-LICENSE.txt)).
