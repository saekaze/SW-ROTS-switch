# Console breakdown: package 15, stage 4

Package 15 (PGO build), handheld, stock clocks (CPU 1020, GPU 307 MHz), `profile = true`, `core_layout = false`.
Stage 4 for 249 seconds: first facing the jet (the heavy side), then turned the other way (the light side). The
console's own overlay read CPU 98–99 %, GPU 40–54 %: CPU bound.

## Frame, threads and GPU, per 10 seconds

`rec` = title thread recording draws, `rt` = render thread, `work` = render thread minus waiting for the swapchain,
all in ms a frame.

| Time (s) | fps | Draws | rec | rt | work | GPU |
|---|---|---|---|---|---|---|
| 63 | 53.1 | 1115 | 3.26 | 16.88 | 8.41 | 6.43 |
| 73 | 43.8 | 2576 | 7.61 | 18.23 | 16.70 | 7.50 |
| 83 | 43.1 | 2657 | 7.79 | 18.56 | 17.00 | 7.76 |
| 93 | 44.2 | 2574 | 7.59 | 17.73 | 16.49 | 7.43 |
| 103 | 44.2 | 2565 | 7.58 | 17.80 | 16.51 | 7.63 |
| 113 | 44.0 | 2582 | 7.61 | 17.78 | 16.48 | 7.17 |
| 123 | 44.0 | 2573 | 7.59 | 17.77 | 16.44 | 7.71 |
| 133 | 43.7 | 2608 | 7.67 | 18.07 | 16.72 | 7.31 |
| 143 | 44.1 | 2573 | 7.61 | 17.79 | 16.51 | 7.76 |
| 153 | 43.9 | 2589 | 7.64 | 17.82 | 16.57 | 7.41 |
| 163 | 43.4 | 2659 | 7.76 | 18.16 | 16.85 | 7.41 |
| 173 | 44.2 | 2569 | 7.58 | 17.72 | 16.44 | 7.43 |
| 183 | 52.6 | 2024 | 6.09 | 15.06 | 13.62 | 7.51 |
| 193 | 53.6 | 1999 | 5.96 | 14.54 | 13.32 | 6.79 |
| 203 | 54.2 | 1975 | 5.89 | 14.43 | 13.20 | 6.68 |
| 213 | 53.7 | 2002 | 5.97 | 14.85 | 13.42 | 6.91 |
| 223 | 49.4 | 2258 | 6.65 | 16.10 | 14.77 | 7.30 |
| 233 | 51.1 | 2115 | 6.28 | 15.22 | 13.96 | 6.84 |
| 243 | 54.6 | 1994 | 5.91 | 14.56 | 13.23 | 6.57 |

Heavy side about 44 fps with about 2575 draws, light side 53.5 with about 2000. The title thread never waited for the
render thread (`wait 0.00`), so it set the pace: 22.7 ms a frame on the heavy side.

## Where the title thread's time went

Profiler, title thread (`guest_14`), self time, heavy side (76–172 s, 93.6 s of samples):

| Share | Function | What it is |
|---|---|---|
| 8.70 % | `Threaded::Draw` | recording one draw |
| 5.82 % | `memmove` | mostly the vertex copies |
| 5.53 % | `Threaded::Header` | finding the copy of a resource header |
| 4.23 % | `Threaded::SendState` | sending the changed states |
| 4.19 % | `Threaded::Delta` | comparing a state block with the last one sent |
| 3.45 % | `sub_0001B990` | game: x87 matrix code |
| 3.05 % | `sub_0001D660` | game |
| 2.85 % | `sub_0001CEF0` | game: its draw loop |
| 2.83 % | `DrawStreamSpans` | which vertex ranges a draw reads |
| 2.66 % | `sub_00032870` | game: SSE vector code |
| 2.46 % | `memcmp` | state compares |
| 1.92 % | `sub_00025BF0` | game |
| 1.50 % | `x86_icall` | indirect calls between game functions |
| 1.36 % + 1.03 % | `mutexUnlock` / `mutexLock` | the HLE lock |
| 0.89 % | `steady_clock::now` | timing the recording |

The recorded draw path was about 42 % of the title thread, the game's own functions spread flat (none above 3.5 %).

## Heavy minus light

Subtracting the light side's profile from the heavy side's, cost scaled by frame time: **+4.3 ms for about 575 more
draws, about 7 µs a draw**, with no single hot spot. About 1.7 ms of it was the recording (Draw 0.41, Header 0.31,
memmove 0.23, SendState 0.19, Delta 0.18, memcmp 0.12, DrawStreamSpans 0.11, lock 0.08), the rest the game's own
work per object.

By source line, 2.1 % of the thread sat on two lines of `atomic_base.h`: the four per-draw perf counters, each an
exclusive load/store pair with barriers. That was the first fix (package 16), followed by the rest of
[performance.md](performance.md).
