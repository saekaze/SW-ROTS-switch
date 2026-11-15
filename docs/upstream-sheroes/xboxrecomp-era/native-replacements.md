# Native replacements for the hottest D3D functions: plan

Status: proposed (2026-11-01). Nothing here is built yet.

## Why

At stock clocks (1020 MHz) the game's main thread is the limit (92% of a core). In the Switch profile of NRO 0b8d36b,
25% of the main thread's samples are in the D3D library the game links statically (lifted like the rest of the XBE,
from the D3D section at 0x0032AE8C and up). Two compiler-level changes (cheaper register spills, plain guest memory)
gave no measurable fps. A function written by hand in C does the same job with native floats, native pointers and no
x86 flag bookkeeping, which the lifted code cannot shed.

## The targets (Switch profile of 0b8d36b, main thread, stage)

The busy part is one call chain: every draw call flushes the render state that changed since the last one.

| Function | Self | With callees | What it is |
|---|---|---|---|
| sub_003374B0 | 1.7% | 15.3% | draw call entry (called from game code, e.g. sub_002AF0E0) |
| sub_00334FF0 | 1.4% | 13.3% | the per-draw state flush: reads the dirty flags at 0x0033B798 and calls the handlers |
| sub_00334F70 | 0.4% | 11.9% | the dirty-flag dispatcher: one test and call per state group |
| sub_00336C50 | 1.6% | 1.9% | a state handler (texture stages; 57 reads, 53 writes, a 26-way switch) |
| sub_00334D70 | 0.8% | 5.2% | transform handler: calls the three matrix routines below |
| sub_0032F0D0 | 1.8% | 2.1% | x87 matrix math (774 float/double conversions in its Switch code) |
| sub_0032EE20 | 1.5% | 1.8% | SSE 4x4 matrix multiply (16 mulps/addps) |
| sub_0032F670 | 0.9% | 1.0% | 4x4 matrix transpose copy (16 loads, 16 stores) |
| sub_00333EA0, sub_00334A20, sub_00334270 | 1.3%, 1.0%, 0.8% | | other state handlers (00334270 is x87 heavy: lighting) |

"Self" is time in the function itself; "with callees" includes what it calls. The roles come from reading the lifted
code; the names of the XDK's own functions are not in the XBE.

## How a replacement goes in

1. Read the function's lifted C (`out/gen/`) and its x86 (`xboxrecomp/tools/disasm`), and write down exactly what it
   reads and writes: arguments, the D3D globals (dirty flags at 0x0033B798, the device at 0x0033BC38), the push buffer
   it writes NV2A commands into.
2. Write a C version in a new file, `src/d3d_native.c`, through the same guest memory accessors, so its results land
   in exactly the same guest memory. Matrix math uses `float` in the same order of operations with fused
   multiply-add off (`-ffp-contract=off`), which rounds like the Xbox's x87 at 24-bit precision and like its SSE.
3. Register it as an override in `recomp_lookup_manual` (`src/recomp_manual.c`) with its evidence: address, callers,
   profile share, what it does.
4. Each replacement has its own switch in `sheroes_env.txt` (`SHEROES_NATIVE_<ADDR>=0` puts the lifted version
   back), so a bad one can be turned off without a new build.

## How each one is checked before it counts

A check mode, `SHEROES_NATIVE_CHECK=1`: every call runs both versions on the same input and compares what they
wrote.

- Pure functions (the matrix routines): run the native version into a scratch buffer, then the lifted one, and
  compare the output bytes.
- State handlers: save the D3D globals and the push buffer range they may write, run the lifted version, swap in
  the saved copy, run the native one, compare the two results, then keep the lifted result.

The log gets one line per function every 10 s: calls, mismatches, and the first mismatch in full (inputs, both
outputs). Zero mismatches over a few minutes of play in two or three stages is the bar. The check mode is slow; it is
for correctness, never for fps.

## Order of work

- Batch 1, the three matrix routines (sub_0032EE20, sub_0032F670, sub_0032F0D0): about 4% of the main thread,
  pure functions, the easiest to check. Expect maybe 2-3% fps: a test of the method more than a visible gain.
- Batch 2, the dispatcher and the transform handler (sub_00334F70, sub_00334D70).
- Batch 3, the per-draw flush and the other state handlers (sub_00334FF0, sub_00336C50, sub_00333EA0,
  sub_00334A20, sub_00334270).
- Batch 4 if it pays: the draw call entry (sub_003374B0).

Each batch is two Switch tests: a check run (`SHEROES_NATIVE_CHECK=1`, any stage, a few minutes, send the log), then
a speed run (the usual stock-clock env with the profiler, same stage as before, send log and prof.bin). Only
src/ changes between builds, not the generated code, so a cloud Switch build takes minutes, not 40.

## Expectations

If the replacements run two to three times faster than the lifted code, the whole D3D share (25%) drops to about
10%, and the main thread saves about 15%: roughly 42 fps to 48 at stock clocks, after all four batches. Each batch on
its own is a few percent. These are estimates from the profile; each speed run measures the real number.

## Risks

- A replacement that differs from the original breaks rendering: wrong transforms, missing textures, a crash. The
  check mode exists to catch this before a speed run, and each function can be turned off from the env file.
- The flush functions are long (300-700 lines of lifted code with switches). They are the bulk of the gain and of
  the work.
