# sheroes-nx

**Sonic Heroes** (original Xbox, USA, 2004) as a native Nintendo Switch homebrew program, statically recompiled from
the original Xbox executable. Not an emulator: the game's x86 code is translated to C++ and compiled for the Switch,
and Direct3D 8 and DirectSound are replaced by the Switch's own graphics (deko3d) and sound.

You need your own copy of the game. No game files are in this repository or its releases.

## Status

**Version 0.5.0 is a pre-release for the Switch: the game runs at 60 frames a second at the Switch's stock clocks.**
The menus, the movies and the first stage have been played with the graphics and sound right. The rest of the game
has not been played through yet.

| | |
| --- | --- |
| Switch | 60 fps at stock clocks (CPU 1020 MHz) in the stages played; port menu; deko3d renderer |
| PC (Windows) | For development: builds and plays, no release |

Session-by-session notes: [docs/progress-log.md](docs/progress-log.md).

### Known issues

- Only the first stage has been played. Anything past it is untested; please report what you find.
- From the extracted disc folder, a few short hitches when new areas load (files are opened as the game reads
  them). The disc image has fewer.
- Start the game with full memory: hold **R** while opening any game, then pick Sonic Heroes from the homebrew menu.
  The album applet does not have enough memory.

## Download and install (Switch)

You need a Switch with custom firmware (Atmosphere) and your own dump of the USA Xbox disc.

1. Download `sheroes-nx-v0.5.0-switch.zip` from this repository's **Releases** page.
2. Extract it to the root of your SD card. It makes `switch/sheroes/` with `sheroes.nro` and
   `sheroes-settings.toml` in it.
3. Put your game in `switch/sheroes/game/` on the SD card, either way:
   - **Recommended: the disc image.** Copy your `.iso` into `switch/sheroes/game/`. It must be a trimmed Xbox image
     (about 3.8 GB for this game): a FAT32 card cannot hold a file over 4 GB, and an untrimmed dump is bigger.
     [extract-xiso](https://github.com/XboxDev/extract-xiso) can rewrite an image (`extract-xiso -r "Sonic Heroes.iso"`);
     if the result is still over 4 GB, use the extracted disc instead.
   - **Or the extracted disc.** Extract the image with extract-xiso (`extract-xiso -x "Sonic Heroes.iso"`) and copy
     everything in the folder it makes (`default.xbe`, `DVD` and the rest) into `switch/sheroes/game/`.

   If both are there, the image is used.
4. Hold **R** while opening any game to get the homebrew menu with full memory, and start
   **Sonic Heroes (Xbox recomp)**. The first start compiles the graphics shaders, a few seconds.

The supported `default.xbe` (title 5345002B) has this SHA-256:
`5a9637976b147d22d2ea845c178379f97a3df9040ee41efe670a37298129c5c6`.

Saves go to `switch/sheroes/hdd/`, apart from the game folder.

### Reporting problems

Open an issue with where in the game it happened, what you saw, and the files in `sdmc:/switch/sheroes/logs/`.

## The port menu

Press **+ and -** together to open it (the game gets no input while it is open). Up and down choose a row, left and
right change it (A too), B saves and closes. Settings are saved to `sheroes-settings.toml` next to the program.

| Setting | Choices |
| --- | --- |
| Button layout | Xbox positions (default), Switch labels, or any Switch button for each Xbox button |
| Invert camera | left/right and up/down, separately |
| Steering response | Linear (default), Gentle, Gentler: finer slight left and right turns on the left stick |
| Controllers screen | the system's screen to pair or split Joy-Cons |
| Resolution | 50%, 67%, 75%, 100% (default) of the screen's height |
| Texture filtering | off, 2x (default), 4x, 8x, 16x |
| Smoothing (SMAA) | off (default) or on |
| FPS counter | off (default) or on |
| Music volume, effects volume | 0% to 300% (default 100%) |
| CPU boost | off (default) or on (1785 MHz) |

More settings, with notes, are in `sheroes-settings.toml`.

## Building

The game's code is generated from your own `default.xbe`, so there is nothing to build without the game. The Switch
build is `switch/build.sh` (devkitPro); the PC build is `tools\win\build_pc.ps1`. [docs/brextreme/](docs/brextreme/)
describes how the toolchain works.

## How it was made

The releases up to 0.2.0 used xboxrecomp (the `main` branch) and needed a CPU boost. 0.5.0 is rebuilt on TGP482's
[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by way of
[brextreme-nx](https://github.com/xsaja/brextreme-nx) (Bloody Roar Extreme), which replaces the graphics and sound at
the API level and draws on a render thread of its own. Why:
[docs/xboxrecomp-era/strategy-review.md](docs/xboxrecomp-era/strategy-review.md) and
[docs/xboxrecomp-era/feasibility-xbe-recomp.md](docs/xboxrecomp-era/feasibility-xbe-recomp.md).

## License and credits

GPL-3 ([LICENSE](LICENSE)); the components and their licenses are listed in [LICENSES.txt](LICENSES.txt).

Credits: TGP482 (XboxXBE-Recomp), xsaja (brextreme-nx, street-nx, mvp-nx), maddog847 (MVP05 XBE Recomp),
gerisson1996 (BLACK-NX-PORT), Daniel Hepper (font8x8), and the projects listed in LICENSES.txt.

No affiliation with SEGA, Sonic Team, Microsoft or Nintendo.
