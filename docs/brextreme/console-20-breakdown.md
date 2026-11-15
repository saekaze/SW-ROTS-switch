# Console breakdown: package 20, stage 4

Package 20 (PGO build, `draw_copy = false`: vertices no longer copied), handheld, stock clocks, `profile = true`.
Stage 4 facing the jet for 220 seconds. Without the profiler the same scene ran at 56.8 fps; the profiler costs
about 2 fps.

## Frame, threads and GPU, per 10 seconds

| Time (s) | fps | Draws | rec | rt | work | GPU |
|---|---|---|---|---|---|---|
| 43 | 53.7 | 1739 | 2.66 | 16.45 | 10.87 | 6.64 |
| 53 | 54.8 | 2552 | 3.92 | 16.12 | 14.88 | 6.94 |
| 63 | 54.5 | 2590 | 3.94 | 16.33 | 15.09 | 6.92 |
| 73 | 55.1 | 2535 | 3.90 | 16.01 | 14.81 | 6.80 |
| 83 | 55.1 | 2535 | 3.89 | 15.99 | 14.80 | 6.79 |
| 93 | 55.2 | 2535 | 3.88 | 16.02 | 14.84 | 6.81 |
| 103 | 55.1 | 2535 | 3.89 | 16.08 | 14.86 | 6.90 |
| 113 | 54.8 | 2552 | 3.92 | 16.10 | 14.87 | 6.94 |
| 123 | 54.4 | 2591 | 3.96 | 16.26 | 15.05 | 6.93 |
| 133 | 55.1 | 2535 | 3.89 | 15.98 | 14.78 | 6.80 |
| 143 | 54.5 | 2535 | 3.89 | 16.01 | 14.78 | 6.80 |
| 153 | 55.1 | 2535 | 3.90 | 16.02 | 14.76 | 6.80 |
| 163 | 55.1 | 2535 | 3.90 | 16.04 | 14.86 | 6.90 |
| 173 | 54.8 | 2550 | 3.91 | 16.16 | 14.96 | 6.93 |
| 183 | 54.4 | 2591 | 3.96 | 16.24 | 15.01 | 6.93 |
| 193 | 55.2 | 2535 | 3.88 | 16.03 | 14.83 | 6.80 |
| 203 | 55.0 | 2535 | 3.90 | 16.02 | 14.85 | 6.79 |
| 213 | 55.2 | 2535 | 3.90 | 16.00 | 14.83 | 6.80 |
| 223 | 55.1 | 2535 | 3.90 | 15.96 | 14.81 | 6.89 |

Compared with [package 15](console-15-breakdown.md) on the same side: recording 7.6 → 3.9 ms a frame (with the
profiler), render thread work about 14.8 ms, so it now has room of its own (about 1.9 ms at 60 fps).

## Title thread

Self time, 50–222 s (166 s of samples):

| Share | Function | What it is |
|---|---|---|
| 10.69 % | `Threaded::SendState` | the changed states, `Delta` now inlined into it |
| 5.31 % | `Threaded::Header` | cache slot 1.56, compare 1.23, reading the game's header 0.49, the 16 stream loop 1.08 |
| 4.20 % | `sub_0001B990` | game: x87 matrix code (GCC keeps the floats in registers; only the x87 stack lives in memory) |
| 4.13 % | `sub_0001D660` | game |
| 3.71 % | `sub_0001CEF0` | game: its draw loop |
| 3.59 % | `Threaded::Draw` | recording one draw |
| 3.27 % | `sub_00032870` | game: SSE vector code |
| 2.79 % | `SyncDeferred` | applying the deferred render and texture states |
| 2.60 % | `sub_00025BF0` | game |
| 2.13 % + 1.20 % + 0.49 % | `mutexUnlock` / `mutexLock` / `rmutexLock` | the HLE lock, mostly from the draw call (0xECAD0) and 0xEC3C0 |
| 1.72 % | `x86_icall` | indirect calls between game functions |
| 1.70 % | `memmove` | index copies and the rest (was 5.8 % with vertex copies) |

The game's draw call (0xECAD0) and everything under it is 25.9 % of the thread, inclusive.

What followed: the header work moved to the render thread (package 21, `SendState` + `Header` were the two largest
entries) and the HLE lock became a single compare-and-swap (package 23).

## Render thread

Self time over the same stretch:

| Share | Function | What it is |
|---|---|---|
| 14.63 % | `DekoBackend::Draw` | one draw's deko3d commands |
| 12.72 % | `svcSendSyncRequest` | presenting and acquiring swapchain images |
| 11.52 % | `svcWaitProcessWideKeyAtomic` | waiting for the next batch |
| 9.99 % | `memmove` | vertex uploads to the GPU ring |
| 6.23 % | `gpu::Hash` | texture and state keys |
| 5.37 % | `gpu::Assemble` | building the vertex data a draw reads |
| 3.93 % | `Threaded::Execute` | replaying a batch |
| 3.53 % | render target map | `EnsureTarget` 1.8, `DepthCopy` 1.6 |
| 2.72 % | shader map | vertex 1.7, pixel 1.0 |
| 2.41 % | `gpu::Describe` | texture descriptions |

About a quarter of the render thread was waiting, the rest about 14.8 ms of work a frame. Package 22 remembered the
last render target and pixel shader (targets 0.51 → 0.22 ms, shaders 1.36 → 0.57 ms a frame), which kept the render
thread under 16.7 ms once the header work moved onto it.
