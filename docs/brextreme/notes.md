# Notebook

Findings and decisions that do not fit elsewhere. Newest knowledge wins: when a later test proved an earlier guess
wrong, the guess is crossed out here rather than deleted.

## The game

- `BLOODY ROAR extreme`, title ID `48550001` (HU-001, Hudson), region 1 (NTSC-U), version 1, built April 2003.
  `default.xbe`: 10,084,352 bytes, SHA-256 `f432eb0c77554b6a28238e550f0ce2cc4502adf094956703de197e9bbd8b6307`.
- XDK **5233** for every library: XAPILIB, D3D8, XGRAPHC, DSOUND, XBOXKRNL, WMVDEC, LIBCMT. No network libraries.
- Hudson's own engine: `.fpk` packs, `.txg` textures, `.mot` and `.seq` animation, `.ptl` particles. Audio is 31 XACT
  wave banks (`.xwb`, 128 MB), movies are 18 WMV files (388 MB) played through WMVDEC and the overlay.
- 1700–2600 draws a frame in fights, many of them tiny (84 % with 30 vertices or fewer on stage 5).

## XDK 5233

street-nx only had signatures for XDK 5849. The 5233 libraries were matched by hand: the recompiler, run with a seed
`game.toml` made from the 5849 signatures, gives the disassembly, and scripts compared it with FIFA Street's (5849,
fully named) by masked bytes and by constants. 110 names in `game/config/game.toml`, each with a `# 5233 by hand:`
comment saying how it was found. From those, `xbe_symbols --sign` wrote `sdk/xdk/5233/signatures.toml` (108 of 108
functions found again from scratch, 10 of 12 data) and `sdk/xdk/5233/layout.toml`.

Traps found on the way:

- In 5233 the device method block (SetTransform … SetVertexShader) is linked **before** the render states; inside each
  block the order is 5849's.
- 0xFE587 is `DirectSoundCreateStream` (FIFA has CreateBuffer in that slot).
- 0xEB000 is a `jmp` to KickOffAndWaitForIdle (BlockUntilIdle), not BlockUntilNotBusy.
- `Lock2DSurface` starts at 0xF09E0; the recompiler opens another function at 0xF0A0E.
- 0xEDAD0 (BumpEnv) is only reached by the tail jump of SetTextureStageStateNotInline.
- `XPP_GamepadType` (0x13960C) cannot be signed: no `.XTLID` section and no library function names it. A new 5233 game
  has to set it by hand.
- `CDevice` moves a lot between 5233 and 5849 (textures at 0xB38, render target at 0x15B4, the miniport at +0x17C8);
  the miniport itself keeps 5849's layout.
- Not linked in this game: End, FlushVertexCache (the game writes 0x417FC itself), SetTile, the fence functions,
  SetGammaRamp, CopyRects, SetRenderTarget, SetScissors, PersistDisplay, pixel shader creation and constants,
  SetBackBufferScale, DirectSoundCreateBuffer.

22 library calls stay unnamed and run their original code without touching hardware (LockRect, CreateTexture,
GetSurfaceLevel, XAudioCreatePcmFormat and others). `tools/unnamed_api.py` lists them.

## Decisions

- **Public name** brextreme-nx: `brextreme.nro`, `brextreme-settings.toml`, `BREXTREME_*` variables,
  `/switch/brextreme-nx/`. The SDK's `// STREET:` comments stay: they say which port each change came from.
- **Removed from street-nx**: FIFA's `game.toml` (rebuilt from the signatures), EA's sound engine lock fix,
  `g_swap_keeps_back_buffer` (FIFA's loading bar; this game redraws whole frames).
- **`draw_copy` false on the Switch, true on PC**: on PC the render thread is far ahead, the copy costs nothing and
  hides any buffer the game might reuse without locking.
- **Two players**: a split Joy-Con pair is two real pads, not a forced second port.
- **No overclock** in the defaults: the goal was 60 fps at the console's clocks, and it was reached.

## Ruled out

- **Depth rounding** for the yqm windows (an exact-depth experiment, `BREXTREME_EXACT_DEPTH`): no change; the cause
  is still open, see [fixes.md](fixes.md).
- **Triangles behind the camera** for stage 1's wedges (`BREXTREME_NEG_W_DROP`): no change; it was far plane clipping.
- **A wrong bone index** for the same wedges: every vertex of that program uses bone 0.
- **Fog** for the same wedges: linear fog 1000–65000, almost none at that distance on the Xbox too.
- ~~invariant `gl_Position` fixes the yqm windows~~: kept (coplanar passes should match), but it did not fix them.
- **HD mode** (`xbox_720p = true`): the game still creates a 640x480 device.
- **Delta time** to fake a higher frame rate: it does not add frames, and would need the game's logic patched.

## Open

- A one-pixel line along the left column and top row during the fade to black after a fight, Switch only, since
  the near-edge fix. Same A/B test as the next item.
- A black square for an instant before a K.O. on stage 4, seen once (with the instrumented NRO), and similar earlier
  reports of a black flash in a hit. Not in the log. Next test: reach a K.O. with `draw_copy = true`.
- The shader cache could ship in the package (the `.dk2` files are the same on every Switch) to avoid the first-time
  stutters.
