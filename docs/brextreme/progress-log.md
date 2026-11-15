# Progress log

One row per milestone. "Console N" is the Nth test on a real Switch (handheld, stock clocks unless noted);
"package N" is the Nth NRO sent to it.

| Date | Build | What happened |
|---|---|---|
| 2026-10-28 | — | Repository made from street-nx 0.1.0; public name brextreme-nx |
| 2026-10-29 | PC | No signatures for XDK 5233: 110 library functions named by hand, then `sdk/xdk/5233` signed from them |
| 2026-10-29 | PC | Boots to the menus after the `_initterm` function and the asynchronous reads; movies crash in the sound stream |
| 2026-10-29 | PC (Vulkan) | **First fight**, full HUD, no errors; 17 fps until draws were copied, then 60 |
| 2026-10-29 | PC | Movies shown through a real `UpdateOverlay`; the D3D12 device loss traced to a NaN LOD bias |
| 2026-10-29 | Ryujinx | First NRO: intro, title and demo fights drawn right, no errors |
| 2026-10-29 | Console 1 | Plays. Save fails (file size of an open file), arcade prologue flickers, fights at 49–55 fps |
| 2026-10-29 | Console 2–3 | Saves work, crash after a K.O. fixed (write-combined view), movies at 30 fps |
| 2026-10-29 | Console 4–5 | Video recording works; profiler on the console; yqm windows flicker in xemu too (not on the original Xbox) |
| 2026-10-29 | PC | Stage 1's sky wedges (depth clamp), D3D12's black shadows, the cathedral's flat floor |
| 2026-10-29 | Console 6 | Sky and cathedral right on the console |
| 2026-10-30 | Console 7–8 | Render thread 9.5 → 7.1 µs a draw (constants); the title thread becomes the limit |
| 2026-10-30 | Console 9–10 | No frame pointers, no errno checks, `SyncDeferred` compares whole blocks |
| 2026-11-02 | Console 11 | First PGO build: +3 fps; overclock test shows the CPU is the limit |
| 2026-11-02 | Console 13 | Package 15 profiled on stage 4: 44 fps on the jet side ([breakdown](console-15-breakdown.md)) |
| 2026-11-02 | Console 14–17 | Plain perf counters, system counter timing, compact vertex records, a lighter HLE lock: 44 → 51 fps |
| 2026-11-03 | Console 19–20 | Vertices no longer copied (`draw_copy = false`): 56.8 fps ([breakdown](console-20-breakdown.md)) |
| 2026-11-03 | Console 21 | Headers resolved on the render thread: **60 fps** on the jet side, 58.7 with smoke |
| 2026-11-03 | Console 24 | New PGO session: **60.0 with smoke too** |
| 2026-11-03 | Console 25 | Stage loads 0.45–0.6 s → 0.31–0.35 s (file stats kept); save confirmed |
| 2026-11-03 | Console 26–28 | Lone Joy-Con sticks turned for the sideways hold; saving from Options fixed |
