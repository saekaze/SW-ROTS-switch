# brextreme-nx documentation

| Page | What it covers |
|---|---|
| [building.md](building.md) | The PC and Switch builds, the PGO builds, the package, the profiler |
| [fixes.md](fixes.md) | Every crash and wrong picture the game showed, each with its symptom, cause, fix and how it was checked |
| [performance.md](performance.md) | How the heaviest fight went from 44 to 60 fps at stock clocks, and how to measure it |
| [notes.md](notes.md) | The notebook: XDK 5233, the game's engine, decisions and things ruled out |
| [progress-log.md](progress-log.md) | The timeline, one row per build tried on the console |
| [console-15-breakdown.md](console-15-breakdown.md) | Profile of package 15 on stage 4 (44 fps on the jet side) |
| [console-20-breakdown.md](console-20-breakdown.md) | Profile of package 20 on stage 4 (55 fps on the jet side, vertices no longer copied) |

The numbers come from the console's own log (`logs/brextreme_NNN.log`): every 10 seconds it writes a `perf:` block
with the frame rate, the frame pacing, the milliseconds each thread spends a frame and the draws a frame. All
measurements are handheld, at the console's own clocks (CPU 1020 MHz, GPU 307 MHz, memory 1331 MHz) unless a row says
otherwise.
