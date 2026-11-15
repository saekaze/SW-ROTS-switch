<p align="center"><img src="docs/images/banner.jpg" alt="Bloody Roar Extreme" width="320"></p>

<h1 align="center">brextreme-nx</h1>

<p align="center">
  <b>Bloody Roar Extreme</b> (Xbox, 2003) on the <b>Nintendo Switch</b>,<br>
  statically recompiled from the original Xbox executable — no emulator.
</p>

The game's x86 code is translated to C++ and built as a native Switch program. A runtime stands in for the Xbox
kernel, its Direct3D 8 (drawn with **deko3d**, the Switch's own graphics API, or with Vulkan/NVK), its audio and its
WMV movie player. brextreme-nx is built on **[street-nx](https://github.com/xsaja/street-nx)** (the FIFA Street port),
itself built on **[mvp-nx](https://github.com/xsaja/mvp-nx)** (MVP Baseball 2005), which starts from TGP482's
**[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp)**, maddog847's
**[MVP05 XBE Recomp](https://github.com/maddog847/MVP-Baseball-2005-XBERecomp)** and the Switch layer of
**[BLACK-NX-PORT](https://github.com/gerisson1996/BLACK-NX-PORT)**. Bloody Roar Extreme is the first game on it built
with the older XDK 5233 libraries (see [docs/notes.md](docs/notes.md)).

> [!NOTE]
> You need your own copy of *Bloody Roar Extreme* for the original Xbox, **USA** release (title ID `48550001`): the
> disc image (`.iso`) or the extracted disc (the folder with `default.xbe`). No game files are in this repository,
> and none are distributed.

## What works

Tested on a Switch (Atmosphère, title mode), handheld and docked, at the console's own clocks:

- Boot, the Hudson logo and the intro movie, the menus, Arcade and Training, the arcade prologue movies, saving and loading
  (also from the Options menu)
- Fights at **60 fps** with sound and music: handheld at 1280x720 the heaviest scene measured (stage 4, facing the jet,
  with its smoke) holds 60.0; docked at 1920x1080 fights average 59.6
- Two players on one console: a pair of Joy-Cons split into two pads held sideways, or any two controllers
- Stage loads of about a third of a second
- Leaving from sphaira without errors

Known: the windows of the yqm building on stage 5 flicker while
the camera moves. The original Xbox does not do this; xemu and another port do it too, and the cause is not found yet.

A black square shows up on screen for an instant right before a K.O. It has happened only about twice in 30 test sessions, so it hasn’t been captured yet and the cause is still unknown. It may be related to the shader cache.

## Installation

> [!TIP]
> **Installer page: [xsaja.github.io/brextreme-nx](https://xsaja.github.io/brextreme-nx/)** builds `brextreme-nx.zip` from your disc image or game folder; extract it into
> `sdmc:/switch/`. Everything runs in your browser: your game files are never uploaded.

By hand:

1. Copy the `brextreme-nx` folder of the package to `SD:/switch/brextreme-nx/` (`brextreme.nro`,
   `brextreme-settings.toml`, `LICENSES.txt`, `shadercache/`).
2. Copy the files of your disc to `SD:/switch/brextreme-nx/game/`: `default.xbe` and the `chr`, `cmn`, `fpack`,
   `game`, `media`, `movie` and `stg` folders (about 740 MB).
3. Start it in **title mode** (hold R while starting a game and pick it in hbmenu, or use sphaira or a forwarder).
   Applet mode (the album) does not have enough memory and says so.

The package carries the deko3d shaders a console made in play (`shadercache/`). One it does not have is compiled the
first time it is seen (about a quarter of a second, a short stutter) and kept there.

## Controls

| Switch | Xbox |
|---|---|
| B / A / Y / X | A / B / X / Y (by position: the button the game shows sits where you press) |
| L / R | left / right trigger |
| ZL / ZR | white / black |
| + / − | Start / Back |
| sticks, stick clicks | sticks, stick clicks |

`buttons_by_name = true` in the settings maps A/B/X/Y by letter instead.

**A lone Joy-Con** held sideways is a pad of its own: the four face buttons are A/B/X/Y by position, SL/SR are
white/black, the shoulder buttons are the triggers, + or − is Start and the stick click is Back. Pressing **+ and −
together** on the first pad opens the system's controller screen, to split a pair (SL+SR on each) or join it again.

## Settings

`brextreme-settings.toml` beside the NRO, commented. The ones a player may want:

- `language`: `"auto"` (the Switch's) or `"en"`, `"es"`...
- `anisotropic_filtering`: 2 by default; 4 or 8 sharpen the ground further.
- `smaa`: SMAA anti-aliasing, off by default (about 5 ms of GPU a frame).
- `buttons_by_name`: see Controls.
- `cpu_mhz`, `gpu_mhz`, `mem_mhz`: 0 = the console's clocks. Not needed. A system overclock tool may set its own
  clocks over these; the log writes the clocks in use whenever they change.
- `renderer`: `"deko3d"` (default) or `"vulkan"`.

Logs go to `SD:/switch/brextreme-nx/logs/`.

## Building

Windows, with Visual Studio 2026 (its Clang), CMake and Python for the PC builds, and devkitPro (MSYS2, `switch-dev`,
deko3d, uam) for the Switch. Put the disc's files in `game/assets` (or a junction to them). The details, PGO included,
are in [docs/building.md](docs/building.md).

```
tools\build_pc.cmd                      # BREXTREME-vk.exe, BREXTREME-d3d12.exe; recompiles default.xbe into game/generated
switch/build.sh                         # in MSYS2: build-switch/brextreme.nro
```

## Documentation

[docs/](docs/README.md): building, the fixes the game needed (each with its symptom and cause), how the fights got from
44 to 60 fps at stock clocks, a progress log and two profiles taken on the console.

## Credits

- **TGP482**: XboxXBE-Recomp, the recompiler and the runtime
- **maddog847**: MVP05 XBE Recomp, whose SDK fixes mvp-nx, street-nx and brextreme-nx carry
- **gerisson1996**: BLACK-NX-PORT, the Switch layer
- **xsaja**: mvp-nx and street-nx
- **ChanseyIsTheBest**: the deko3d guide of sa2-nx, uam as a library, the Ryujinx fork used for testing
- **fincs**: deko3d and uam; **danfromtico**, **NaGaa95**: mesa-switch and its 26.2.4 build
- **R-drg**: defjam-sw, whose profiler the Switch profiler is adapted from
- **Jorge Jimenez et al.**: SMAA
- Banner, installer art and NRO icon: cut from the game's cover

Every license is in `LICENSES.txt`.

## Legal

Bloody Roar Extreme is (c) 2003 Hudson Soft, whose games now belong to Konami. This project has no relation to Hudson
Soft, Konami, Microsoft or Nintendo. It contains no game data; `brextreme.nro` contains code translated from the
player's own `default.xbe`. The code of brextreme-nx is licensed under the GNU General Public License version 3
(`LICENSE`), as street-nx's and mvp-nx's are.
