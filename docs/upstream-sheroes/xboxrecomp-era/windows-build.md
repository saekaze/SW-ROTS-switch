# Building and testing on Windows

The PC build uses the same renderer as the Switch (Vulkan, xboxrecomp's nv2a_vk, through the pushbuffer executor),
so what works on the PC works the same way on the console's GPU. The game code is translated on your PC from your
own disc; nothing made from the game is downloaded or committed.

## One-time setup

The Xbox 360 ports' tools (Git with GCM, Python, LLVM 20, CMake, Ninja, Visual Studio 2022 Build Tools) are all
used here. Add the Vulkan SDK, in PowerShell:

```
winget install --id KhronosGroup.VulkanSDK -e
```

Close PowerShell and open a new one (so it sees VULKAN_SDK), then put the repository into the folder that already
holds the disc:

```
cd C:\sheroes-nx
git init
git remote add origin https://github.com/FurryLilManChild/sheroes-nx.git
git fetch origin
git checkout -t origin/main
```

`assets\game_root` (the extracted disc) stays where it is; `.gitignore` keeps it out of the repository.

## Every build

From `C:\sheroes-nx`:

| Command | What it does | Log |
| --- | --- | --- |
| `git pull` | Get the latest changes | |
| `powershell -ExecutionPolicy Bypass -File tools\win\check_tools.ps1` | Checks every tool, the Vulkan SDK, the game files and disk space | `out\logs\check_tools.log` |
| `powershell -ExecutionPolicy Bypass -File tools\win\build_pc.ps1` | Libraries (first time), translates the game code when needed, compiles | `out\logs\deps_*.log`, `pc_*.log` |
| `powershell -ExecutionPolicy Bypass -File tools\win\boot_test.ps1` | Runs the game for 60 s and keeps the log | `out\logs\boot_latest.log` |
| `powershell -ExecutionPolicy Bypass -File tools\win\play.ps1` | Runs the game to play, no time limit | `out\logs\play_latest.log` |

The first build takes the longest: SDL2 and glslang (a few minutes), the translation (a few minutes), then about
150 compile steps. Later builds only redo what changed.

Options: `build_pc.ps1 -Jobs 4` (fewer parallel jobs if the PC runs out of memory), `-Clean` (empty build folder),
`-Regen` (translate again). `boot_test.ps1 -Seconds 120`, `-Shots` (a picture every 60 frames in `out\shots\`),
`-Env "KEY=VALUE"` (a test switch Claude asks for).

Note: devkitPro's MSYS2 tools on the PATH shadow Windows' cmd, cmake and git; `tools\win\env.ps1` leaves them out
of the build session only.
