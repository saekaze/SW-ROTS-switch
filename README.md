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
| Mission 1 (Rescue over Coruscant) | Loads and plays |
| Level changes (the game reboots itself between front end and missions) | Works, done in-process (no restart of the app) |
| Sound (DirectSound at the API level) | Works |
| Movies (CRI Sofdec FMV: intro, crawl, cutscenes) | **Black for now** (sound plays; skip them with Start) |
| Switch build | Builds (deko3d). Not yet confirmed on real hardware: please report! |
| Missions past the first | Untested |

Tested so far on PC (Linux, headless): title, Start, the self-reboot, crawl, intro, mission 1 gameplay, pause,
quit mission back to the main menu, and starting a mission again (three reboots in one session).

## Install

You need a Switch with custom firmware (Atmosphere) and your own copy of the game.

1. Copy `switch/rots/` to the root of your SD card (`sdmc:/switch/rots/rots.nro` and `rots-settings.toml`).
2. Extract your disc image with [extract-xiso](https://github.com/XboxDev/extract-xiso):
   `extract-xiso -x "Star Wars Episode III.iso"`
3. Copy everything it extracted (`default.xbe`, `audio`, `cinematics`, `movies`, `pak`, ... about 2.4 GB) into
   `sdmc:/switch/rots/game/`.
4. Hold **R** while opening any installed game to get the homebrew menu with full memory (title takeover), then start
   **Star Wars Episode III (Xbox recomp)**. The album applet does not have enough memory.

The PAL release (Europe, title ID 4C410017) is the one it was made with.

### Settings

`sdmc:/switch/rots/rots-settings.toml` (comments inside): render resolution, frame cap, SMAA, anisotropic
filtering, button layout, language, CPU boost and more. **+ and -** together open the port menu in game.

### Frame rate

The game itself caps at 30 fps (`fpsLimit=30` in the game's `vars_xbox.cfg`). For 60 fps, edit
`sdmc:/switch/rots/game/vars_xbox.cfg` and set `fpsLimit=60` (experimental: report how it plays).

## Reporting problems

Open an issue with: where in the game it happened, what you saw, your Atmosphere version, and the logs from
`sdmc:/switch/rots/logs/`.

## Building

The game's code is generated from your own `default.xbe`; nothing builds without the game.

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
  `D3DDevice_Release` returning 0 (the game releases the device in a loop).
- The Switch build without a Vulkan driver (deko3d only), RotS paths and title checks.

## To do

- Movies (CRI Sofdec through the YUV overlay)
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
