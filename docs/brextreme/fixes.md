# Fixes

What the game needed on top of street-nx, one entry per problem: what was seen, why, what changed and how it was
checked. Most were found on the PC first (the same recompiled code, with a debugger and frame captures), some only
showed on the console.

## Crashes and hangs

### `_initterm` calls a function that was never translated

- **Symptom**: the first PC start closed at 0.17 s.
- **Cause**: the C runtime's static constructors (`_initterm`, 0xE3150) call 0xE64D0 through a pointer table, so the
  recompiler never saw it as a function.
- **Fix**: `0x000E64D0` in `[functions]` of `game/config/game.toml`.
- **Checked**: PC boot went on to the next problem.

### Black screen: the loader asks for an asynchronous read

- **Symptom**: black screen, `cmn0000.fpk` read again every frame.
- **Cause**: the game's loader (0x34C40) opens its files for overlapped I/O and treats anything but
  `ERROR_IO_PENDING` from `ReadFile` as a failure; the runtime completed every read at once and returned success.
- **Fix** (`sdk/runtime/kernel/files.cpp`): a file opened without `FILE_SYNCHRONOUS_IO_*` is marked asynchronous, and a
  read on it returns `STATUS_PENDING` with the status block and the event already complete.
- **Checked**: menus and fights on PC and console.

### The movie sound stream and the overlay

- **Symptom**: crash in the Hudson logo movie, first inside the sound stream code, then in `D3DDevice_UpdateOverlay`.
- **Cause**: the stream's methods are called through a vtable (0x14A918) and had no names, so they ran the original
  code against the Xbox's audio hardware; `UpdateOverlay` (0xF01A0) writes the NV2A's overlay registers.
- **Fix**: the six stream methods named in `game.toml`; `D3DDevice_UpdateOverlay` has its own HLE
  (`sdk/runtime/xdk/d3d8.cpp`): each YUY2 frame is converted and scaled into a surface the size of the back buffer and
  presented. While the overlay is on, the game's own `Swap` of black frames shows the overlay too (the arcade prologue
  flickered without it).
- **Checked**: every movie on PC and console at 30.0 fps; conversion 4.8 ms a frame on the console.

### D3D12 device lost in fights

- **Symptom**: `pipeline creation failed`, then `GPU readback failed`, about one fight in two.
- **Cause**: the debug layer showed `CreateSampler: MipLODBias ... NaN`. The game leaves NaN patterns in
  `D3DTSS_MIPMAPLODBIAS`, and `std::clamp` lets NaN through.
- **Fix**: a bias that is not finite reads as 0, in the D3D12, Vulkan and deko3d backends.
- **Checked**: three runs to a fight in D3D12 with no errors, one with the debug layer.

### Saving failed on the console

- **Symptom**: "failed" when saving, only on the Switch.
- **Cause**: on the Switch, `stat` by path fails for a file that is open for writing, so the file's size came back as
  0xFFFFFFFF and the game aborted the save (0x34880, code -15).
- **Fix** (`files.cpp`): the size of an open file is taken from its handle.
- **Checked**: saved and loaded on the console; a broken save from the failed attempt had to be deleted by hand.

### Crash after a K.O.

- **Symptom**: the console closed right after a K.O.
- **Cause**: `sub_00035940` locks a 640x480 surface with flag 0x40, and `Lock2DSurface` then returns the
  write-combined view `0xF0000000 | physical`, which the Switch's memory map does not have.
- **Fix**: on the Switch the `Lock2DSurface` wrapper drops flag 0x40 (the same memory at 0x8xxxxxxx). 0xF0A28 is the
  only place that forms that address.
- **Checked**: K.O.s on the console in every later test.

### Saving from the Options menu

- **Symptom**: "The software was closed because an error occurred" when saving from Options.
- **Cause**: `indirect call to 000BC3CD has no recompiled function`: another function only reached through a pointer.
- **Fix**: `0x000BC3CD` in `[functions]` of `game.toml`.
- **Checked** by the tester on the console.

### Fatal error when recording a video

- **Symptom**: holding Capture in title mode ended in a fatal error (Atmosphère report 0xFFE).
- **Cause** (likely, not confirmed): the system lets a title record and asks for recording memory the NRO never set up.
- **Fix**: `appletInitializeGamePlayRecording()` at start (`switch/main.cpp`).
- **Checked**: recording works since; the log still says recording is not available, so the cause stays unconfirmed.

## Wrong pictures

### Wedges of black in stage 1's sky

- **Symptom**: black wedges in the sky, mostly near the other fighter; also the "black triangle" over the mountain.
- **Cause**: found with a probe that lists every draw touching chosen pixels: in those pixels only the clear was
  drawn, the sky dome was missing. The game's projection has no far plane (z = 1 − 0.1/w), so the sky sits about 1e-6
  under 1 and rounding puts some vertices past it: PC and Switch GPUs clip those triangles, the Xbox did not.
- **Fix**: depth clamped instead of clipped: D3D12 `DepthClipEnable = FALSE`, Vulkan `depthClampEnable`, deko3d
  `depthClampEnable`.
- **Checked** on PC (D3D12 and Vulkan) and on the console.

### Black, hard shadows in D3D12

- **Symptom**: the fighters' shadows were solid black in D3D12; soft in Vulkan and on the Xbox.
- **Cause**: the shadows blend with `CONSTANTALPHA` and blend colour 0x90000000; D3D12 has only a per-channel blend
  factor, so RGB = 0 made them black.
- **Fix**: with constant alpha (and no constant colour) the D3D12 blend factor carries the alpha in all four channels.
- **Checked** on PC: the same soft shadows in D3D12 and Vulkan.

### Flat floor in the cathedral (stage 7)

- **Symptom**: the cathedral's floor was one flat colour on every renderer.
- **Cause**: the floor is one quad with texture coordinates 0..1 that its vertex program multiplies by 5 and 12. A
  street-nx shortcut read coordinates inside 0..1 as clamp instead of wrap (it fixes sprite edges) and looked at the
  vertices, not at what reaches the sampler.
- **Fix**: the shortcut only applies with no vertex program, no texture matrix and no generated coordinates (D3D12,
  and `switch/gfx_common.h` for Vulkan and deko3d).
- **Checked** on PC and the console; it also fixed part of the bridge stage's floor.

### First row and column left undrawn

- **Symptom**: on screens with letterbox bars, the top row and left column of the picture stayed unpainted.
- **Cause**: the game draws those passes half a pixel down and right (0.5 … 640.5). Scaled by 1.5, the first host
  pixel's centre (0.5) falls before 0.75 and is not covered.
- **Fix** (`sdk/runtime/gpu/shaders.cpp`): as the far edges already were, a screen position within 1e-3 of 0.5 is
  moved to 0, in the vertex shader epilogue and in the vertex program path. This changes every vertex shader, so the
  shader cache is rebuilt.
- **Checked**: built; not yet seen on the console.

### Open: the windows of the yqm building

The lit windows on stage 5 flicker while the camera moves. The original Xbox does not show it; xemu and another port
do. The two layers are not coplanar (half a unit apart, far from the camera) and exact depth did not change it, so
whatever the console's GPU does differently is not found yet.

## Input

### A lone Joy-Con held sideways

- **Symptom**: with a Joy-Con alone, the stick was turned a quarter: right crouched, left jumped.
- **Cause**: HID reports the stick upright even in the sideways hold.
- **Fix** (`switch/input.cpp`): the left Joy-Con's (x, y) becomes (−y, x), the right one's (y, −x).
- **Checked** by the tester, both Joy-Cons.

## Audio

### Sound streams that run dry

- **Symptom**: short drops in the intro movie's sound.
- **Cause**: the thread that decodes the movie also feeds its sound; when it falls behind, the stream plays out.
- **Fix**: a stream that ran out waits until it has 80 ms queued (or a full queue) before playing again; the log
  counts how often (`a stream ran dry N times`). Faster movie conversion halved the count.
- **Checked**: 8 drops in the whole intro, then 4; none in fights.
