# Feasibility: Sonic Heroes on TGP482's XboxXBE-Recomp (2026-11-09)

Follows [strategy-review.md](strategy-review.md). The question: how much work it is to move Sonic Heroes onto the
toolchain behind the ports that reach 60 fps at stock clocks (Direct3D replaced at the API level, a render thread,
deko3d). Nothing here ran the game; everything is from the tools and the code.

Source used: xsaja/brextreme-nx at its current commit (the newest of that family, with the XDK 5233 work and the
stock-clock performance work), its `sdk/` (TGP482's XboxXBE-Recomp, MIT).

## 1. Their tools build and run on our default.xbe

`xbe_symbols` and `xbe_recomp` built on the cloud machine with the system clang and the repo's Zydis and toml++
(a small CMake file outside the repo). Both run on Sonic Heroes' `default.xbe` (SHA-256 5a9637...29c5c6).

## 2. The function finder (XDK signatures)

The SDK has signatures for XDK 5849 and 5233; Sonic Heroes links 5659. Pointing the finder at each set as if it
were 5659:

| Signature set | Functions found | Data found |
|---|---|---|
| 5849 | 130 of 238 | 9 of 12 |
| 5233 | 50 of 239 | 2 of 12 |

With 5849, by group (found / not found):

| Group | Found | Not found | Note |
|---|---|---|---|
| Direct3D | 87 | 15 | the gap to fill |
| DirectSound | 30 | 22 | the buffer API is all there (create, play, lock, set data, 3D, mix bins) |
| DirectSound streams | 0 | 23 | probably not used by the game (to confirm) |
| XAPI | 7 | 0 | |
| Network, Bink | 0 | 41 | Sonic Heroes has neither (no online play; movies are CRI Sofdec) |
| CRT maths | 3 | 7 | |

Spot checks against what we already know at 5659 addresses: `D3DDevice_DrawIndexedVertices` 0x003374B0 (our draw
entry in the profile), `D3DDevice_SetTexture` 0x0032BF60, `D3DDevice_Swap` 0x00338780 (our Swap hook) all agree.

Direct3D not found: BeginPush, EndPush, BlockOnFence, InsertFence, EnableOverlay, FlushVertexCache,
GetVertexShaderFunction, PersistDisplay, SetIndices, SetPalette, SetTile, SetVertexShaderConstant1,
SetVertexShaderConstant4, SwitchTexture, D3DResource_IsBusy. Some may not be linked into this XBE at all.

Cross-check from our own disassembly: 59 of the D3D section's 281 functions are called directly from outside it
(the game, XGRAPHICS, D3DX). 39 of them got a name from the 5849 signatures; 20 did not (0x0032AF30, 0x0032C110,
0x0032C1D0, 0x0032D1D0, 0x0032D780, 0x0032FA00, 0x0032FA20, 0x003311D0, 0x003312A0, 0x003312F0, 0x00335360,
0x003353F0, 0x00338930, 0x003389E0, 0x00338A30, 0x00338A60, 0x00338AB0, 0x00338AC0, 0x00338AD0, 0x00338B70;
5 to 321 bytes each). DSOUND: 33 called from outside, 30 named, 3 not. These are the functions to name by hand,
which is what brextreme-nx did for XDK 5233 (110 functions, then signed them as `sdk/xdk/5233`).

## 3. Their recompiler on our XBE

`xbe_recomp` translated the whole XBE in 2.3 seconds: 21,366 functions (ours: 22,692), 875 jump tables, one C++
exception block, and a list of instructions it does not implement (aaa, bound, daa, les and the like, 1 to 31 of
each, almost certainly data decoded as code, as in their other games).

Size of the translated code, as a stand-in for speed (it is not a measurement of speed): five of its 64 output files
compiled for the Switch's CPU (clang 18, `-O2 -mcpu=cortex-a57`, plus the `-fomit-frame-pointer -fno-math-errno`
brextreme-nx names in its docs), compared function by
function with our Switch build of the same functions (GCC, -O2):

| | Functions | Ours | Theirs | Theirs / ours |
|---|---|---|---|---|
| All matched | 1,177 | 2.40 MB | 0.71 MB | 0.30 (median per function 0.33, 10% of them under 0.19, 90% under 0.44) |
| Example: sub_002A9600 (a 4x4 matrix routine, 71 x86 instructions; 3% of our main thread) | 1 | 3,664 bytes | 800 bytes | 0.22 |

The difference has visible causes in our code: our SSE registers live in thread-local memory (every `xmm` access
goes through it), an `XMM_MEM` load is four separate checked 32-bit loads, and every guest memory access carries the
Xbox's special-address checks. Fewer instructions per x86 instruction usually means faster, and a third of the code
size also means far less pressure on the Switch CPU's instruction cache, but only a console run measures it. The
compilers differ too (clang for theirs, GCC for ours).

## 4. What their Direct3D replacement covers, against what Sonic Heroes needs

From `sdk/runtime/gpu/shaders.cpp` and `sdk/runtime/xdk/d3d8.cpp`:

| Sonic Heroes needs (what we had to build into our renderer) | Their HLE |
|---|---|
| Fixed-function lighting | yes (directional, point, spot, specular, local viewer, normalisation, material sources) |
| Texture coordinate generation (the character shine) | yes (normal, position, reflection) |
| Fog | yes (linear, exp, exp2; fog colour) |
| Vertex blending (skinning, the characters) | **no**: the render state is kept but the fixed-function shader does not blend; to add (we wrote it once in `gl_vsh.c`) |
| Vertex and pixel shaders, combiners | yes (Xbox vertex programs and pixel combiners translated) |
| Push buffers (BeginPush/EndPush) | yes, but the game's are not found yet |
| Movies (CRI Sofdec, YUY2 overlay) | yes (brextreme-nx's UpdateOverlay with YUY2 frames); EnableOverlay to find |
| DirectSound buffers (CRI ADX) | yes |

## 5. Licenses

TGP482's SDK: MIT. brextreme-nx, street-nx, mvp-nx: GPL-3. Their Switch layer comes from BLACK-NX-PORT, which
publishes no license (Mark: an upstream with no license is still fine to build on). Mark left the license to Claude.
Decision: build on brextreme-nx and make the new sheroes-nx GPL-3, as godfather-nx did from street-nx. Our own code
that comes along is MIT, which GPL-3 can include; its notices stay.

## 6. Verdict

Feasible. The finder names most of Direct3D and DirectSound on its own; the gap is about 20 Direct3D and 3
DirectSound functions to name by hand, plus a 5659 layout file (the device structure's offsets, which we partly
know). The game-specific work is vertex blending in their fixed-function shader, the movies' overlay, and our
Switch fixes (the open-file limit, the disc list) carried over. Their translated code is about a third the size of
ours, which points the right way on speed but is not proof.

## 7. Plan

1. New branch with brextreme-nx's tree and our docs; GPL-3.
2. `sdk/xdk/5659`: name the missing functions by hand from our disassembly and what we know at 5659, write
   `layout.toml`, sign them (`xbe_symbols --sign`).
3. PC build (their Windows build, Vulkan) on Mark's PC: boot, menus, movies, first stage. Add vertex blending.
4. Switch build (deko3d): first stage, then measure at stock clocks with their profiler.
5. Then their speed steps where they apply (no vertex copies, headers on the render thread, PGO).
