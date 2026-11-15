# Performance: 44 to 60 fps at stock clocks

The worst case measured is stage 4 facing the jet: 2550–2620 draws a frame (street-nx's matches have about 350). At
the start, handheld at stock clocks, that side ran at 44 fps. It now holds 60.0, smoke included. Every step below was
measured on the console, at stock clocks, on the same scene (Alice against Shina gives 2549 draws; other matchups give
a few more or less, so a test notes who fought).

## How a frame is made

Two threads do the work:

- **The title thread** (`guest_14` in the profiler) runs the recompiled game. Each Direct3D call is an HLE function
  that records the draw: the states that changed since the last draw, the vertex and index data, the resources it
  uses. The log calls this time `rec`.
- **The render thread** replays the recorded batches with deko3d (`rt` in the log; `acquire` is the time spent
  waiting for a swapchain image, `gpu` the GPU's own time).

The frame takes as long as the slower of the two. At the start both were saturated; the GPU never was (7–8 ms).

## Reading the log

Every 10 seconds:

```
perf: 60.0 fps, ...
perf: ms a frame: rec 3.00, wait 0.51 ..., rt 15.9, gpu 7.4, ...; 2549 draws, ...
perf: rt ms a frame: assemble ..., targets ..., shaders ..., textures ..., constants ..., upload ..., bind ..., record ...; acquire ..., present ...
```

`rec` divided by the draws gives the recording cost per draw, the number this work drove down: 2.95 µs at the start,
1.18 µs at the end. `wait` is the title thread waiting for the render thread; `wait 0.00` means the title thread is
the limit.

## The steps

| Package | Change | Jet side (fps) | rec (µs/draw) |
|---|---|---|---|
| 15 | Start of this work (PGO already in, see below) | 44.0 | 2.95 |
| 16 | The four per-draw perf counters were atomic read-modify-writes with barriers (2 % of the title thread): now plain members, published once a frame. Header caches 1024 → 4096 slots | 46.0 | 2.66 |
| 17 | Recording timed with the system counter (`cntpct_el0`) instead of `steady_clock`; vertex copies as a compact record; header copies kept inside their cache slot | 48.5 | 2.42 |
| 18 | The HLE lock on libnx's recursive mutex instead of pthreads; deferred states compared as 8 byte words; 256 commands a batch | 51.0 | 2.43 |
| 19 | New PGO session with the current code | 50.7 | 2.40 |
| 20 | **`draw_copy = false`**: vertices are no longer copied; the render thread reads them from the game's memory, as the Xbox's GPU did, and the game's own buffer locks wait for the batch that used them | 56.8 | 1.47 |
| 21 | **Headers resolved on the render thread**: the title thread sends each resource header's address and its 12–20 bytes; the render thread finds or makes the copy | 60.0 (58.7 with smoke) | 1.20 |
| 22 | Render thread: last render target and pixel shader remembered, depth copy check reordered | 58.0 (another matchup) | 1.22 |
| 23 | HLE lock as one compare-and-swap and one release store; state compares end on one overlapping 16 byte load | 59.7–60.0 | 1.35 (profile lost) |
| 24 | New PGO session | **60.0, also with smoke** | 1.18 |

Two steps did most of it: not copying vertices (package 20) and moving the header work to the render thread
(package 21). Both move work off the title thread onto the render thread, which had room: its own trims (package 22)
made sure it stayed under 16.7 ms.

## Earlier: the render thread and PGO

Before package 15 the render thread was the limit (about 10.5 µs a draw). Shader constants reused by generation
instead of compared byte by byte, only the constant ranges a shader reads uploaded, and the last draw's constants
reused when only bones or vertex buffers changed brought it to 7 µs. The recompiled code lost its frame pointers
and errno checks (`-fomit-frame-pointer -fno-math-errno`), and PGO (see [building.md](building.md)) gave about 6 %.

## What did not help

- **Pinning threads to cores** (`core_layout`): the title thread alone on core 0, the render thread on core 1. No
  faster, maybe 1 fps slower; off by default.
- **A second PGO session** on unchanged code: within noise. A new session only pays when the code changed.
- **Clocks**: a CPU overclock (1224 MHz) gave +6 fps and 1785 MHz 60 fps from the start, which showed the limit was the
  CPU. The goal was stock clocks, so overclocking was only used to tell CPU from GPU and memory.

## Known risk of `draw_copy = false`

If the game rewrote a vertex buffer without locking it while the render thread is still behind, a shape would draw
wrong for a frame. No such case has been seen. `draw_copy = true` in the settings restores the copies (and costs about
2 ms a frame on stage 4).

## Loads

Stage loads were 0.45–0.6 s of frozen picture. A profile showed 39 % of a load frame in file metadata requests
(`svcSendSyncRequest`): the case-insensitive path lookup, existence checks and the size and time of every query. The
game folder is read-only on the Xbox, so `files.cpp` now keeps its stats once taken, and drops them if anything writes
there. Loads went to 0.31–0.35 s, menus from about 0.3 s to 0.22 s.
