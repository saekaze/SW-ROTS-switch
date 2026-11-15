# Review of the other Xbox Switch ports (2026-11-09)

Mark asked for a review of every Xbox port on robzilla10001/SwitchPorts, to find a strategy other than ours that
reaches 60 fps at stock clocks. Read: the README and docs of each repo (22 besides sheroes-nx and CoD-nx), and
CT3-nx's progress log (the same toolkit as ours, which tried the Direct3D-level route first).

## Two families

**A. xboxrecomp (sp00nznet) with the NV2A emulated: our family.** The game's own Direct3D library runs as
translated code and writes NV2A push buffer commands; a second thread reads them and draws with Vulkan or OpenGL.
burnout3-sw, nfsu1-sw, nfsu2-sw, nfsuc-sw, flatout2-sw, COLD-FEAR-NX-PORT, mcdub3-nx, defjam-sw, conker-lr-recomp,
OpenTrickyNX, doa2u-nx, doa3-nx, CoD-nx, CT3-nx, sheroes-nx. What their READMEs say:

- nfsu1-sw: 30 fps races (the game's own cap); "the CPU is what limits the port". burnout3-sw: races below 60 fps
  (a frame lock option). flatout2-sw: 20-30 fps in races. nfsu2-sw, nfsuc-sw: up to 60 uncapped, clocks opt-in, no
  figure at stock.
- defjam-sw: 60 fps in fights, higher clocks optional for busy scenes (a fighting game, few draws).
- doa3-nx: built for 1785 MHz. COLD-FEAR-NX-PORT: 22-30 fps without overclock. mcdub3-nx: 20-25 fps with maximum
  overclock.
- conker-lr-recomp: a stable 30 fps; 60 is experimental.
- OpenTrickyNX: 60 fps in races, but its default clocks are 1785 MHz CPU, 768 GPU, 1600 memory (docs/switch.md).
  Its speed option, the guest x86 registers in fixed ARM registers (GCC global register variables), is reported as
  "about three times faster than a straight translation". Our translator already keeps registers in locals inside
  each function, so most of that gain is already ours.
- CT3-nx (our toolkit, Direct3D's hottest functions rewritten natively, NV2A still emulated): 30-50 fps in a shift
  after tests 10a-11a (docs/progress-log.md there).

**B. Direct3D replaced at the API level: TGP482's XboxXBE-Recomp (MIT).** The XDK's Direct3D 8 and DirectSound
functions are found by signatures and replaced with native implementations (HLE). No NV2A command stream exists:
each Direct3D call records the draw, and a render thread replays it with deko3d (or Vulkan). BLACK-NX-PORT,
mvp-nx, street-nx, brextreme-nx, godfather-nx, ShaolinMonks-NX, scda-nx. Stated results:

- brextreme-nx (Bloody Roar Extreme): **60.0 fps at stock clocks** in its worst scene, 2550 draws a frame
  (docs/performance.md: 44 to 60 over ten measured steps).
- mvp-nx: about 58 fps median at stock clocks with deko3d, 55 or more 94% of the time.
- street-nx: 60 fps matches; "FIFA Street does not need more" than the console's clocks.
- godfather-nx: tested at stock handheld clocks, a light overclock recommended for heavy scenes.
- BLACK-NX-PORT, ShaolinMonks-NX: up to 60 / full speed (clocks not stated).
- scda-nx: tested with a light overclock; stock untested.

The ports that reach 60 at stock clocks are all in family B.

## Why B is faster for us

From the Switch profile of 0b8d36b at stock clocks (42 fps, 23.8 ms a frame), the main thread spent about
8.3 ms in the game's own code, 5.7 ms in the translated Direct3D library, 5.7 ms blocked (the guest lock,
waits) and 1.7 ms in the runtime. Family B removes the translated Direct3D library (replaced by recording, which
brextreme-nx brought to 1.2 us a draw) and the NV2A emulation behind it. Estimate, not a measurement: 8.3 ms of game
code plus 1-3 ms of recording plus the runtime is about 11-13 ms, under the 16.7 ms of a 60 fps frame. The game's
own code would then be the limit, compiled by a different recompiler whose speed on our code is not known.

## What moving to B takes

- XDK 5659 signatures and the CDevice layout. The SDK knows 5849 (and brextreme-nx added 5233 by hand: 110
  functions, then `xbe_symbols --sign`). Ours is 5659; our known D3D and DirectSound addresses are a start.
- Sonic Heroes' Direct3D use covered by the HLE: RenderWare's fixed-function pipeline with what we found needs
  emulating (vertex blending/skinning, texture coordinate generation, fog distance, lighting), vertex and pixel
  shaders, push buffers if the game uses them, the CRI Sofdec movies (YUY2 overlay; brextreme-nx did this).
- The kernel and file layer of the SDK (our Switch fixes: the open-file limit, the disc list).
- Licenses: TGP482's SDK is MIT. mvp-nx, street-nx, brextreme-nx and godfather-nx are GPL-3; building on them makes
  sheroes-nx GPL-3. BLACK-NX-PORT (the Switch layer under them) publishes no license.
- Time: brextreme-nx went from a copy of street-nx to a first fight on the PC in one day and to 60 fps at stock
  clocks on the Switch in six (its docs/progress-log.md).

## Smaller things from the same reading

- PGO (profile-guided optimisation): about 6% in brextreme-nx (+3 fps), also used by doa3-nx and OpenCE-nx.
- `-fomit-frame-pointer -fno-math-errno` for the translated code (brextreme-nx).
- File stats kept for the read-only game folder (brextreme-nx; we did the same with the disc list).
