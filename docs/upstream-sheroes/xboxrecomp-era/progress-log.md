# Progress log (newest first)

## 2026-11-03: pre-release 0.2.0

- Switch test of 3c4aa92/6d3b0f2 (disc list, lazy opens): Mark reports the black screen at boot much shorter (no
  log from that run). Mark: package a pre-release, 0.2.0 given the performance.
- Version 0.2.0 in the homebrew menu; README status and known issues updated. Release zip
  `sheroes-nx-v0.2.0-switch.zip` (same layout as 0.1.0) and `release_notes.md` in `out\release` on Mark's PC.

## 2026-11-01: stock-clock optimisation, step 2 (plain guest memory), no gain

- NRO 0b8d36b: the lifted code built with -DRECOMP_MEM_VOLATILE= (NFSU2_GEN_PLAIN_MEM), the spin hint with a
  memory clobber. 650 KB smaller NRO, no freeze in the stage. Stock clocks: 42.7 fps average in the stage (25
  readings, 30-59), against 42.3 for f42470f: no measurable change. Main-thread shares unchanged (guest .text 35%,
  D3D and libraries 24%, blocked 24%).
- Instruction-level look at the profile (pc of each main-thread sample in the 25 hottest lifted functions): the
  samples sit on loads, and the hottest are the guest data loads themselves (`ldr w, [base, w, uxtw]`), e.g.
  sub_002A9600. The address folding (cmp/and/csel/orr) is about 5% of those samples. The time is the game's own
  memory traffic, not translation overhead the compiler could remove.
- Mark asked about the long black screen at boot. From f42470f's log: 1-12 s the game lists every disc folder
  (two pauses of about 3.5 s), 12-36 s it opens all ~3,900 disc files one by one and keeps them open (300 files a
  second at first, 100 at the end; the fd recycling closes them in batches past 256), 36-45 s quiet, first picture
  at about 45 s.

- Mark pointed at bejeweled3-nx's docs/optimising-functions.md (profile-driven: changed-only register write-backs,
  inlining, hand-written natives for the hottest functions; about 1.3-1.5x there). Step 1 here: the cost of the
  thread-local registers.
- Every lifted function keeps its registers in locals and stores / reloads them around each call through the
  thread-local g_eax .. g_esp. On the Switch each thread-local address is a __aarch64_read_tp call, and GCC
  re-derived the address at every spill and reload rather than keeping it: 1,742 calls in recomp_0085.c (250
  functions). Now each function takes the addresses once and hides them from the optimiser (RECOMP_OPAQUE, an
  empty asm), so they stay in registers or on the stack: 1,050 calls in the same file, mostly one per function,
  and slightly smaller code. RECOMP_REG_TLS_PTRS=0 at regen restores the old form.
- Switch test (NRO f42470f, stock 1020 MHz forced with NFSU2_CPU_MHZ=0, profiler after 60 s): in the stage
  31-56 fps, 42.3 average over 31 readings, no measurable change from the earlier 40-48 at stock. __aarch64_read_tp
  fell from 3.7% to 2.9% of the main thread's samples, as intended, but that is too small to show in fps.
- Main thread still the limit: 92% of a core. Its samples: guest .text 37%, D3D and other libraries 26%,
  blocked 20% (7.2% in gil_lock waiting for another guest thread, 4.6% in kdisp_wait, the game's own event
  waits), runtime 3%. The renderer thread (nv2a_ack_thread) is about half idle, the APU thread mostly idle.
- Next candidate: guest memory accesses without volatile (RECOMP_MEM_VOLATILE empty for the generated code), with
  the 41 marked wait loops as the safety net; then the gil_lock waits.

## 2026-10-30: test release 0.1.0

- Mark: package an initial test release, noting it needs overclocking to perform well.
- Port menu: CPU boost (off by default): NFSU2_CPU_MHZ=1785 at the next start (switch_boot reads the saved option
  before setting the clocks; the env file still wins). 1785 MHz is the CPU clock of Nintendo's own boost mode.
- Version 0.1.0 in the homebrew menu. README rewritten (status, known issues, install, port menu, building,
  credits, legal); LICENSE names every MIT copyright holder; NOTICE.md lists the third-party licenses (xemu's code
  under the LGPL; no FFmpeg in this build).
- Release zip `sheroes-nx-v0.1.0-switch.zip`: switch/sheroes/ with the NRO, a readme, a note in game/, the license
  texts. Release notes in `release_notes.md`.

## 2026-10-30: Switch test 10, the vblank was never slow

- The timer thread's own count: 599-600 ticks every 10 s in the stage, with and without the fair dispatch lock;
  its passes are short (raise 0.00 ms average, vblank ISR 0.02-0.5, IRQs and DPCs 0.2-1.0). The [perf] line's
  "vblank 40-44 Hz" (4 Hz with the fair lock off) was a broken reading: it and the renderer's [fps] line
  (RECOMP_FPS_LOG=1) shared one baseline, each seeing only the ticks since the other. Fixed (xbox_vblank_count).
  So the fair dispatch lock, the timer thread's priority and the vblank catch-up were answers to a problem that
  was not there; they are harmless and stay (the fair lock can be turned off with RECOMP_FAIR_DISPATCH=0).
- The real limit is the CPU at stock clocks: the game thread 91-93% of a core at 42-48 frames a second; at 1785 MHz
  55-60 (test 9). Main thread profile (test 9, stock and 1785 together): guest .text 35%, the XDK D3D library
  22%, waits and locks 24%, runtime 7%.

## 2026-10-30: Switch test 9 (stock, then 1785 MHz mid-run)

- Overhead is down: the main thread's sync share fell from about 45% to about 24%, most of it now real waits
  (11.4% in KeWaitForSingleObject). svcSignalProcessWideKey 17% to 3.2%.
- Stock: 38-43 frames a second, game thread about 90-92% of a core. After Mark raised the CPU to 1785 MHz part way:
  55-60 frames a second with the game thread at 62-82%. So at stock the game is CPU bound by roughly 1.5x.
- But the vblank stayed at 40-42 Hz the whole time, even at 1785 MHz with headroom, while it is 60 Hz before the
  game renders. The game paces itself by it, so it runs at about 70% speed at any clock. The timer thread now logs
  where its passes go every 10 s ([vblank]: wait, raise to DISPATCH, vblank ISR, IRQs and DPCs), and an A/B run with
  RECOMP_FAIR_DISPATCH=0 checks whether the fair dispatch lock is part of it.

## 2026-10-30: Switch test 8, second profile

- Stage 38-45 frames a second, vblank 46-47 Hz, game thread 61-92% of a core: no visible change for Mark.
- Profile: the wake-pending change worked where aimed (the flag thread's svcArbitrateLock 28% to 0.5%; the main
  thread's svcArbitrateLock 12.8% to 3.2%), but svcSignalProcessWideKey grew to 17%: 9.5% from xbox_gil_suspend.
  gil_unlock woke the condition variable on every release, and on Horizon that is a system call even with nobody
  waiting; every kernel call and poll-loop yield releases the guest lock. Now it wakes only when a later ticket is
  held (same for the fair dispatch lock).
- Path lookups: on the Switch fix_case is skipped (FAT32 / exFAT ignore case), saving a stat and directory scans
  per open (about 2%).

## 2026-10-29: Switch tests 6-7, LTO A/B and the first Switch profile

- LTO A/B (Mark): neither felt much better. Stage 40-47 frames a second either way, game thread 80-95% of a core;
  vblank 55 Hz in the normal build's stage, 39 in the LTO run. LTO set aside.
- Mark asked whether a sound fix might matter as it did in CoD-nx (DirectSound reset the DSP and stalled 10 ms per
  frame when its heartbeat word did not read 0xCCCCCC). Not here: Sonic Heroes' DSOUND 5659 has no such check (no
  `cmp dword [eax], 0xCCCCCC` in the XBE). The APU frame thread is 86% idle; the audio lags because the game
  thread feeds it late.
- Profile (RECOMP_NX_PROFILE=1 from 60 s, normal build 6ab9cd7, tools/prof_report.py), game main thread: about 45%
  in host sync primitives. D3D's pushbuffer-space wait sub_0032FC10 (< sub_00338780 < sub_002AD860) and its spin
  hint's recomp_spin_wake / recomp_spin_yield about 16%; guest critical sections (RtlEnter/Leave, the host mutex
  and the shadow rwlock) about 8%; KeWaitForSingleObject waits about 5%; guest-lock handovers about 4%; file
  opens' path lookups (stat, readdir) about 2%. The executor thread is 52% idle, and the flag thread spends 28% in
  svcArbitrateLock on the wake lock.
- First change: recomp_spin_wake skips the lock and the broadcast while a wake is already pending.

## 2026-10-29: Switch test 5, vblank 44 Hz

- With the fair dispatch lock the stage vblank went from 0.5-7 Hz to 42-46 Hz, but menus fell from 57 to 44-45
  Hz: the game paces itself by the vblank count, so everything ran at about three quarters speed. Mark: slightly
  better at most.
- The timer thread runs at Horizon's time-sliced priority and waits up to a 10 ms slice behind a busy thread on
  its core; a tick more than half a period late was dropped. Now: the timer thread runs above the guest threads
  (xbox_nx_raise_host_thread, as the APU frame thread does), and up to four periods late the vblanks catch up
  instead of being dropped.
- The game thread is at 85-95% of a core in the stage at 1020 MHz (40-45 frames drawn a second). Next: an LTO
  build to compare.

## 2026-10-29: Switch test 4, playable but slow; a fair dispatch lock

- The descriptor recycling worked: past NOW LOADING into the first stage, none of the PC rendering problems back.
  Slow in movies, menus and the stage (stock clocks, verbose log on).
- The [perf] lines: in the stage the vblank fell to 0.5-7 Hz while the game thread ran at 93-95% of a core
  (menus: 56-58 Hz). The vblank counter only advances on the timer thread, which raises to DISPATCH (the dispatch
  lock, a plain mutex) before each tick; the game thread raises and lowers IRQL thousands of times a second and, on
  Horizon, kept taking the mutex straight back. Fix (kernel_hal.c): a ticket lock for DISPATCH, first come first
  served, on by default on the Switch (RECOMP_FAIR_DISPATCH=0 off; =1 on elsewhere).
- The verbose log (RECOMP_QUIET=0) wrote about 250 lines a second to the SD card in the stage; the test env file
  now keeps only RECOMP_FPS_LOG=1.
- Still to look at: the game thread itself near a full core at 1020 MHz (movies decode in lifted code); options are
  the LTO build (switch/build.sh LTO=1) and profiling the hottest guest functions.

## 2026-10-29: Switch test 3, the 512-file limit

- NOW LOADING again, but not the old cause. The game opens every file on the disc at boot (a file table, about
  3,900 opens from 7 s to 45 s on the SD card) and keeps the handles. Horizon allows a process about 512 open
  files: open 514 onward failed with EMFILE (logged as 0xC0000001, win32 err 0), so the sound banks
  (DVD\sound\bank*.pac) and the se_*_tbl.bin tables never opened, and the main thread spun on a critical section
  (29 million RtlEnter/LeaveCriticalSection) waiting for them. On Windows the handles cost nothing.
- Fix (win32_compat.c, kernel_file.c): descriptor recycling. At most 256 host descriptors stay open; past that the
  least recently used file handles are closed (position kept) and reopened on their next use; an EMFILE retries
  after freeing 32. The guest's handles stay valid. Checked with a small test under ulimit -n 100: 600 files open,
  read in two passes across evictions, all contents and positions right.
- Clocks: the CoD-nx defaults raised them to 1785 / 768 / 1600 MHz; now stock unless sheroes_env.txt asks.
- Also seen: the file table scan takes about 38 s on the SD card (about 10 ms per open). A later improvement.

## 2026-10-29: first Switch build since the PC fixes

- PC play test 15: the streaks, Espio and the odd sea spray are gone; about 59 fps. The log confirms the cause:
  attribute 2 (S32K, one short) at an odd address with a 3-byte stride, converted on the CPU now.
- Switch NRO built in the cloud (110 steps, 22 minutes on 2 cores) with everything above: LAHF, the lifter's
  one-arm switches, fixed-function fog, texgen, lighting, misaligned attributes. Port menu defaults 1x, 4:3, edge
  crop off; clocks stock unless sheroes_env.txt asks. Copied to Mark's out\sd\switch\sheroes\ (the bridge's
  per-file limit is 30 MB, so it went over xz-compressed and was unpacked there; SHA-256 checked).

## 2026-10-29: play test 14, the streaks are the direct vertex path

- Run B with RECOMP_GL_DIRECT=0 (every attribute converted on the CPU): the team select is clean, no streaks, Espio
  whole. So the direct attribute path is wrong for these meshes, not the program or the topology.
- Likely cause: Vulkan fetches an attribute only from an address that is a multiple of its component size, and
  AMD rounds a misaligned one down. The character vertices pack bytes (weights) and shorts (indices) between floats,
  so floats can land on 2-byte boundaries. upload_vertices now converts any attribute whose address or stride is
  not a multiple of its component size, and logs the first eight such cases ([VK] ... misaligned, converted) to
  confirm.

## 2026-10-29: play test 13, streak diagnostics

- RECOMP_VK_INSTANCE=0 (no instanced draws): the streaks and Espio are unchanged, so instancing is not the cause.
- The per-vertex check found no outlying vertex in any character draw: the CPU interpreter puts every vertex near
  the rest of its mesh. So either the streaks are long triangles between vertices that are each in a plausible place
  (strip joins), or the GPU path computes something the CPU interpreter does not (the direct attribute formats).
- Next: the trace lists each vertex-program draw's three longest triangles by topology; A/B with RECOMP_GL_DIRECT=0
  (every attribute converted on the CPU, as the interpreter reads them).

## 2026-10-29: play test 12, the streaks are not fixed-function skinning

- Play test 12 (zoomed in): Tails' face in the stage is now right; past the old freeze the stage plays on. Still
  wrong: dark streaks off Sonic's and Knuckles' heads (stage) and off Knuckles, Sonic, Shadow, Charmy, Big's belt and
  Vector (team select); Espio and part of Omega scrambled. (I also reported grey faces and flat shading from the
  screenshots; Mark checked and the faces and shading are fine. Misread on my side, struck.)
- The trace: SET_SKIN_MODE is 0 on every draw, so the fixed-function blending added earlier never runs. The
  characters are vertex-program draws (program at slot 12, five variants of 18 to 29 instructions): two-bone
  skinning from a palette of 3x4 matrices at c[8 + a0], a0 from attribute 2 (S32K shorts), weights from attribute
  1 (UB_OGL bytes), normal in attribute 3 (CMP), uv in attribute 5. The program writes screen-space oPos itself.
- Next diagnostics: the frame trace runs the CPU interpreter over every vertex of each vertex-program draw and lists
  the ones far from the rest with their raw position, weights, indices and uv. A/B run with RECOMP_VK_INSTANCE=0
  (no instanced draws) to test the instancing path.

## 2026-10-29: correction, fixed-function lighting, vertex program dump

- Correction to the entry below: the characters are NOT fixed. Mark's play test 11 screenshots still show Tails'
  odd face, streaks off several models, and Espio and Omega partly scrambled. (My notes on grey faces and flat
  shading were a misread of the screenshots; Mark says the faces and shading look fine.)
- Flat shading: the Vulkan fixed-function shader never lit anything; it passed the vertex colour stream through
  (lit_color existed only on the CPU, for the trace and the software rasteriser). The shader now does the same
  lighting on the GPU from the light registers (scene ambient + per light atten * (ambient + diffuse * N.L), colour
  material), on the blended eye position and normal. RECOMP_VK_LIGHT=0 turns it off.
- Faces and eyes: the head draws (eye texture 80CB5F00, three combiner stages) are vertex-program draws (xform 2,
  program start 12), not fixed-function. The trace now dumps each vertex program of the traced frame (raw
  instructions, non-zero constants, attribute formats) and the lighting registers, to translate offline.

## 2026-10-29: play test 11 (shine fixed), the stage freeze, vertex blending

- Texgen works: the characters and the team select models look right. The trace shows the shine passes use
  normal-map texgen on stage 0 with a texture matrix (0x8511 on S, T, R), and some draws eye-linear on stages 1-3.
- Freeze: the stage stopped while the HUD timer ran on. The log has one failed indirect call, to 0x00041FCB, from
  sub_00041F70: `movzx ecx, byte [ebx + 0x42088]; jmp [ecx*4 + 0x42084]`, a one-arm jump table behind an all-zero
  byte table (and another at 0x000420B0, and 0x000421D4 nearby). The lifter needs two arms for a switch, so the arm
  became an indirect call to a non-function and the object's update gave up. Fix (lifter.py, translator.py): accept
  one in-function arm when the dword after it is zero (the byte table). Regenerated in the cloud: only recomp_0006.c
  changes, and it compiles.
- Streaks out of the characters (team select and stage): fixed-function vertex blending (SET_SKIN_MODE) was not
  implemented, so vertices bound to model-view matrices 1-3 stayed in the bind pose. The fixed-function shader now
  blends model-view 0-3 by attribute 1's weights (xemu's vsh-ff.c), and projects the blended eye position through
  the inverse of model-view 0 and the composite matrix. RECOMP_VK_SKIN=0 turns it off. Not confirmed yet: the trace
  now prints SET_SKIN_MODE and both matrix 0s to check the inverse assumption.

## 2026-10-29: fixed-function texgen (the character shine)

- The fixed-function vertex shader now generates texture coordinates as the NV2A does (xemu's vsh-ff.c): per stage
  SET_TEXGEN_S/T/R/Q (eye linear, object linear, sphere map, normal map, reflection map) with normals taken to eye
  space by SET_INVERSE_MODEL_VIEW_MATRIX0, then SET_TEXTURE_MATRIX when SET_TEXTURE_MATRIX_ENABLE is on. Register
  addresses from nv2a_regs.h. Vulkan only (the GL back end leaves u_tg at 0 and keeps the old path).
- The frame trace prints each enabled stage's texgen modes and texture matrix, so the next trace confirms which
  modes the character passes use.
- Not yet seen running: Mark's play test checks the characters.

## 2026-10-28: textured stage (play test 10)

- The fixed-function fog fix works: Mark's screenshot shows the stage textured (grass, path, stone, water) at a good
  frame rate. Fog registers in the trace: linear, params 6 and -1/6000, plane (0,0,1,0), colour white, so fog only
  starts far away.
- Next, the characters: their second passes are additive (blend SRC_ALPHA, ONE, depth write off), colour c0 x
  texture 0 (an environment highlight, 80C87F00), and the eye and head draws add texture 1 x c0 the same way. Those
  need generated texture coordinates (SET_TEXGEN_S/T/R/Q from 0x03C0, sphere map or reflection map, and the texture
  matrices). The fixed-function vertex shader has no texgen and passes the model's own coordinates (vT0 = v9), so
  the highlight lands as a plain texture: the white patches on Sonic and Knuckles and Tails' odd head. Plan: print
  the texgen and texture-matrix registers in the trace, then add texgen to the fixed-function shader.
- Flickering lines in the level: not yet looked at (needs a description or a capture of when they show).

## 2026-10-28: why the stage geometry is white (fixed-function fog)

- The texture dump from Mark's play test shows the stage textures intact in guest memory (grass, cliffs, sea), so
  the uploads were never the problem.
- The frame trace shows the cause: the six sky, sea and far-cliff draws, which draw correctly, use the final combiner
  0000000C (R0 as is). Every draw after them uses 130C0300 (fog.a x R0 + (1 - fog.a) x fog colour), and about 150 of
  them are fixed-function (xform 1). The fixed-function vertex shader set vFog to 0, so fog.a was 0 and those draws
  came out as the fog colour: white. The earlier RECOMP_DBG_NOFOG test only touched vertex-program draws, which is
  why fog looked ruled out.
- Fix (gl_vsh.c, gl_psh.c, nv2a_vk.c): the fixed-function shader computes the fog distance as xemu's foggen does
  (spec alpha, radial, planar, abs planar, fog X) from SET_MODEL_VIEW_MATRIX0, SET_FOG_PLANE and SET_FOG_GEN_MODE,
  and the pixel shader applies the fog mode and SET_FOG_ENABLE as for vertex programs. RECOMP_VK_FFP_FOG=0 turns
  it off. The frame trace now prints the fog registers.

## 2026-10-28: diagnostic build for the white level geometry

- The frame trace now prints every combiner stage the stage draws enable (the old line showed two of three), each
  stage's two constant colours, and the final combiner's constants.
- With `RECOMP_TEXDUMP_DIR` set (run_game.ps1 sets it to `out\texdump`), a traced frame also saves the level-0 bytes of
  each draw's textures, once per address, so the stage textures can be decoded and checked offline.
- Nothing visible changes; this build only gathers the data to find why the stage draws white.

## 2026-10-28 (later): the first PC builds

- Build 1: glslang installed into a folder named `$glslangOut` (Windows PowerShell 5.1 does not expand a variable in
  an unquoted `-DNAME=$var` argument), so configure could not find it. Quoted; the stray folder is removed.
- Build 2: the APU failed under clang on Windows: qemu_shim.h redefined __builtin_ctz/clz for _MSC_VER (clang defines
  _MSC_VER too and has them built in), and M_PI/M_E needed _USE_MATH_DEFINES, set by the toolkit for MSVC only.
- tools/linux/wincheck.sh: a Windows compile and link check in the cloud (clang for x86_64-pc-windows-msvc, MinGW's
  headers in place of Microsoft's, empty system libraries). It found the rest before Mark's next build: setenv and
  unsetenv in usb_gamepad.c (now _putenv_s), clock_gettime in nv2a_pb_exec.c and sheroes_menu.c
  (platform/win32_clock.h, QueryPerformanceCounter), and xbox_guest_pin / xbox_nx_* only defined in the POSIX
  win32_compat.c (platform/win32_host.c on Windows, the one-core pin opt-in as on Linux). All 156 objects compile;
  the link leaves only Windows and C runtime functions.
- Build 3 linked (174 steps). First PC boot: kernel, APU (XAudio2), save folders, then an access violation reading
  0xFED00000 (OHCI) from XPP 0x0037F812. ohci.c protected its registers on Windows for a VEH that this project does
  not have; XPP is lifted with --mmio-sections, so the model is now registered with the MMIO accessors on Windows
  too, as on Linux (the VEH path stays behind XBOX_OHCI_VEH).
- Boot 2 ran 60 s but stalled before the first frame: NOP(9) never taken. A stall report (IRQL holders, the guest
  lock holder, their host stacks resolved with a linker map, out/pc/sheroes_recomp.map) showed the deadlock: the
  main thread held the guest lock spinning in D3D's fence wait (sub_0032FC10_gen), the timer thread waited for the
  lock at DISPATCH to run the PGRAPH ISR. recomp_types.h's poll-loop hint took the `_MSC_VER` branch under clang (a
  bare pause, never yielding the lock); now cl only. build_pc.ps1 re-lifts when that template changes.
- Boot 3: the title screen on Mark's PC (AMD Radeon, Vulkan 1.4.344, swapchain 1280x720), correct logo, sky, Press
  START and the fade on Enter; Mark reached the next menus. About 52 frames/s by the screenshot times, and slow by
  eye. Nothing called timeBeginPeriod, so every 1 ms wait (vblank clock, guest lock, executor) took a 15.6 ms tick:
  timeBeginPeriod(1) at startup. run_game.ps1 turns on RECOMP_FPS_LOG.
- Play 1 (Mark): 58.7 to 59.5 fps on the menus, sound clean. Title, save creation (text missing letters), 1 player,
  story, Team Sonic (models on team select look wrong; screenshot to come), then a crash as the story's first
  Sofdec movie started: ICALL to 0x0035C1C0, the PSFD_I section, which had no functions, nor had PSFD_P. Each is
  one integer-only function, the I and P picture handlers in the CRI decoder's table at 0x003DB1D4. Seeded both.
- Play 2: the movie got further, then ICALL to 0x0031E010, a CRI callback stored with MEM32(reg+0x12EC) = 0x31E010,
  in a 5 KB gap no function covered (one 13.9 KB function with two jump tables). Seeded. Then two sweeps so a stage
  load does not find the next one: constant code addresses written to memory (only 0x0031E010) and
  tools/table_sweep.py, pointers in .rdata/.data tables beside known functions: 0x0021A780 and 0x0023D460 (object
  factories in type descriptors, allocating 0x320 and 0x35C bytes), 0x00272110, 0x00089250, 0x0011D020. Three
  sat right after jump-table index bytes the disassembler's sweep decoded as code, so it rejected them as
  mid-instruction; seeds can now carry "verified": true (checked by hand, stated in the note), trusted like
  observed ones (tools/disasm/__main__.py).
- Play 3: the movie plays, the first stage loads and draws on the PC (no black quad: that was lavapipe), geometry
  white, and in the stage only the triggers (camera) respond. ~20,000 calls to 0: HUD number drawing
  (sub_0003AB20) calls 0x002D4B80 = jmp [obj+0x12C] with obj = [0x00558A8C] = 0, the result of the sprite-set
  lookup by name "a_num_s" (0x002D4B70). In the stage the game read font\a_num_s.met (METRICS2, pages a_num_sa.png
  and a_num_sb.png) and a_num_sa.png and never opened a_num_sb.png: PNG decode suspected (libpng 1.0.12, zlib
  1.1.3), which also fits the menu text missing letters. png_error 0x00293410 and png_warning 0x00293430 now log
  their message. The stage's DXT1 uploads hold real data ([DXT] log). tools/win/trace_frame.ps1 traces one frame.
- Play 4: every PNG failed with "Decompression error"; zlib's inflate (0x0029B4E0) returned 32511, not a zlib code.
  Reproduced on Linux in 90 s (the fonts at boot). Probes: inflate_blocks came back with esp 36 bytes off and the
  bad value; inflate_codes never returned. Cause: a seed of mine, 0x002A16B0 (the [pad] sweep, 2026-10-27, which
  took operand bytes CC for int3 padding; its note even shows the garbage decode), split inflate_codes
  (0x002A1320-0x002A1A30, switch table at 0x002A1A04) inside an instruction at 0x002A16AB. Removed; all PNGs
  decode. A scan tracing every function's control flow for starts inside another real function's instructions
  found nothing else (only harmless alias starts in sub_001D7CC0). Kept: [ZLIB] logs any failing inflate.
- Play 5 (Mark): zero PNG errors, zero null calls, the HUD (score, 00:00:00, rings) and full text drawn; still no
  control in the stage. The screenshot shows NOW LOADING for good and the timer at 0. The CRI stream keeps reading;
  the Linux attract demo loads the same files and then s01.dmo and runs. The log shows RtlEnterCriticalSection /
  RtlLeaveCriticalSection 4.3 million times (the Switch run: 38 million): a poll under a lock. Diagnostics:
  tools/win/dump_threads.ps1 (flag file) prints the guest-lock holder, IRQL holders, the hottest
  RtlEnterCriticalSection callers (counted in the kernel dispatcher) and every thread's host stack. The window's
  close button now ends the program (SDL_QUIT was ignored).
- Play 6 (thread dump): no deadlock; the main thread renders, CRI's threads idle (the self-suspending one resumed
  ~36/s). The hot lock is DirectSound's global lock (sub_0033E675). The attract demo stops at the same NOW LOADING
  (title card), so it is the stage start, not input. The stage scene (class 0x003897A0: init 0x0001A740, update
  0x0001D1D0, a 17-state machine on [this+8]) goes 2 -> 4 and stays in 4 (PC run, [SCENE] log). State 4
  (0x0001D543) waits for the screen fader at [0x0055867C] (vtable 0x0038A5D8) to finish: mode +0x28 back to 0.
  The fader update (vtable+4, 0x00038320) steps colour +0x30 toward target +0x2C by 5 and always converges, so
  either it is not called or the fade keeps restarting. [FADE] diagnostics log the update and every fade start.
- Play 7: the base fader's update never ran, but the stage's fader is the subclass (vtable 0x0038A650) whose update
  0x00038BB0 eases a float toward a target and ends the fade on `fucomip; lahf; test ah, 0x44; jp`. The lifter
  emitted LAHF after the x87 FCOMI family as a comment (its generic path has no SF expression for them), so AH was
  stale and the fade never ended: every stage waited in state 4 at NOW LOADING. lifter.py: LAHF from g_fp_cmp for
  fcomi/fcomip/fucomi/fucomip/fcompi/fucompi. 34 generated files changed. build_pc.ps1 re-lifts when the
  lifter's .py files change.
- Play 8 (Mark): the stage title card shows in full for the first time, the stage starts (scene 4 -> 5), the
  characters respond to every button, a death restarts the stage (5 -> 8 -> 4 -> 5), the timer runs. No crash, no
  failed calls, ~53 fps with -Shots. Next for playability: the stage geometry draws white.

## 2026-10-28: Switch tests, the Windows Vulkan build

- Switch test 1 (b5ec2c45): black screen, the log quiet after 7 s of loading. Switch test 2 (d957eb02,
  RECOMP_QUIET=0): boot, the attract demo's files at 23 s, the renderer up only at 46 s, then 0.0 to 0.1 fps with
  NOW LOADING showing; 38.7 million RtlEnterCriticalSection / RtlLeaveCriticalSection calls (ordinals 277/294) in
  283 s: a lock the threads spin on under Horizon's scheduler. Parked until the PC build works.
- Linux, the attract demo: the stage loads, but a black full-screen quad drawn first every frame (z 0.001, depth
  write on, LEQUAL, alpha test passes: diffuse alpha) blocks everything after it; skipping it
  (RECOMP_DBG_SKIPNEARQUAD=1) shows the sky, the sea and Team Sonic, with the stage geometry white. Ruled out: float
  or W depth (CONTROL0 0x00100001, Z24S8), stencil (off), SET_CLIP_MIN/MAX (0..16777215; clip support added
  anyway), the cull state (0x405 back, 0x901 CCW front; flipping it turns the models inside out), fog, the render
  target stages 2-3, mip chains, stale surfaces. White ground: the combiner is diffuse x texture 0; blanking stages
  0-1 turns it black, so texture 0 reads white. Diagnostics added: RECOMP_DBG_NOZ, _SKIPNEARQUAD, _FLIPFRONT,
  _NOFOG, _TEXOFF=<mask>, _DXT; the frame trace now prints formats, clip range, cull, alpha test, combiners.
- One crash in DSOUND's voice notification (sub_00341EDC, a null voice slot, on the timer thread) in 15 runs.
- Mark chose the Xbox 360 ports' loop: he builds and tests on his PC. The Windows build now uses the Switch's
  renderer: nv2a_vk on WIN32 (SDL2's VC package, glslang 15.4.0 from source, the Vulkan SDK), a console program,
  the pushbuffer executor on (RECOMP_PB_EXEC); POSIX shims for nv2a_vk (nv2a_vk_win32.h). tools/win/: env.ps1 (from
  mof-nx), check_tools.ps1, build_pc.ps1 (deps, translation on the PC, configure, compile), boot_test.ps1, play.ps1.

## 2026-10-27: the heap's top block

- With a 6 MB bigger heap the story still failed the same way, and the game's use grew by the same 6 MB, so it was
  not the budget. RECOMP_HEAP_BIG=1 (new: logs allocations of 1 MB or more) shows the 8,431,088-byte block allocated
  and freed at boot. The runtime's first-fit heap coalesced freed neighbours but never gave a free block at the top
  back to the bump pointer, so a free tail plus the untouched space above it could not serve one large request.
  xbox_HeapFree now returns such a block (and trims empty slots at the end of the table).
- The attract demo still sits on NOW LOADING on Linux with every stage file opened, the loading music streaming and
  the CRI server thread resumed and suspended about 36 times a second; the main thread is always in the renderer,
  waiting on lavapipe (about 10 to 15 fps on this 2-core machine). Not solved yet.

## 2026-10-27: self-suspending threads, the heap, the main menu on Linux

- Loading never finished (the attract demo sat on NOW LOADING). gdb: a thread looping in sub_002ED700 calling
  sub_0027B5B9, XAPI's SuspendThread (NtSuspendThread, ordinal 231) on its own handle: CRI's server thread does one
  unit of work, then suspends itself until it is resumed the next frame. The POSIX SuspendThread only counted, so the
  thread spun (2.4 million suspends in a 3-minute run) and starved everything else. Now a self-suspend blocks on the
  thread's gate with the guest lock released until ResumeThread brings the count to 0 (`w32_wait_while_suspended`,
  xbox_NtSuspendThread; Linux and the Switch, Windows was already right).
- Run 5 with that: SEGA, Sonic Team, Sofdec/ADX and Dolby logos, the real title screen, Press START, the save prompt
  (Game Data created), Main Menu, 1P Play, Story, team select (all twelve models). Known: some letters are missing in
  the message boxes ("_ease _ress _ button").
- Picking Team Sonic: `xbox_HeapAlloc: out of memory (requested 8431088, used 46362624/50855936)` loading
  event0000.scr, then a null call and the staff roll, then a crash. Thread stacks already come from the heap, so the
  8 MB stack block (main stack plus two runtime worker slices) is now 2 MB and the heap 54.5 MB (a real Xbox leaves
  about 55 MB above this 7.5 MB image).

## 2026-10-27: Linux boots to the title screen, the Switch toolchain

- Linux run 1: about 8,600 lines, then a crash after seven failed indirect calls. `tools/seed_sweep.py` (new) does
  the skill's sweeps plus two more found here: the crash log's failed targets; uncovered code right after a ret with
  no padding; library-section gaps; 16-byte-aligned code right after int3 padding (the static pass merged across the
  padding: one "function" ran 0x001F4EE0 to 0x001F6A20); pointers in .rdata/.data (vtables) to code after padding.
  103 seeds, then 57 more after run 2 (160 in all), each with its evidence.
- Run 2: the Sonic Team logo, then the attract demo loading Seaside Hill (the movies were not copied to the cloud), then
  black, with five new failed indirect calls (one 266,000 times). Run 3, with all 160 seeds: no failed indirect calls;
  the Sonic Team logo, the title screen ("Press START"), the attract demo loading Seaside Hill.
- `timeout` alone does not stop the Linux build (it ignores SIGTERM): use `timeout -s KILL`.
- Switch toolchain on this cloud machine: Mark's C:\devkitPro (devkitA64, libnx, portlibs/switch, cmake, tools;
  537 MB) through the bridge, `setup_switch_wine.sh`; NVK from ghcr.io/danfromtico/switch-dev-legacy-vulkan (layer
  with libvulkan.a; libexpat.a and libzstd.a from another layer); glslang 15.4.0 built with the Switch toolchain into
  /opt/glslang-switch.

## 2026-10-27: first regen, the XDK 5659 hooks

- First regen (tools/regen.sh, 4 min on 2 cores): 22,527 functions, 0 failed, 2.57 million lines of C in 91 files;
  230 unresolved call targets stubbed, 691 untranslated instructions (data read as code), 41 spin loops.
- Kernel: `gen_ordinal_remap.py` on the analysis: all 116 imports are identity, no remap needed.
- Linux Vulkan build (out/build-vk-linux, lavapipe) compiles cleanly, 154 steps in about 10 minutes. The CMake
  project is now `sheroes_recomp`.
- CoD-nx's hooks found again at 5659 addresses (capstone on the D3D and DSOUND sections; evidence in
  src/recomp_manual.c): BlockOnTime 0x0032FC10 (device global 0x0033BC38), DSP command post 0x0034175A, AC'97
  channel reset 0x00346F4D, vblank 0x003324D0 and PGRAPH 0x00332A40 handlers (DPC 0x00332C80, ISR 0x003322D0).
  CoD's per-frame DSP heartbeat has no counterpart: 5659's DSOUND never compares with 0xCCCCCC, and its 10 ms DSP
  stall runs only at init.

## 2026-10-27: kickoff

- Prior art: no existing recompilation or Switch port of Sonic Heroes found (searches in this and two earlier
  threads). The Xbox version is the best base (higher-quality movies, 48 kHz audio).
- Disc: Mark's dump is a trimmed XISO (the XDVDFS volume descriptor at 0x10000, not 0x18300000), so it cannot be
  checked against Redump; the XBE's SHA-256 is recorded in CLAUDE.md instead. Extracted on Mark's PC into
  `assets/game_root` with a small XDVDFS walker.
- `default.xbe`: title SONICHEROES, 5345002B, region 1, entry 0x0027972B. Every library is 1.0.5659 (CoD-nx and
  Burnout 3 are 5849), so CoD-nx's library hooks do not carry over by address. xboxrecomp knows 5659 from Crimson
  Skies (`kernel.h`, `gen_ordinal_remap.py`).
- Repo started as a copy of CoD-nx at 82c236f (music volume): renamed the app to `sheroes`, entry point, empty seed
  list; CoD's hooks taken out of `src/recomp_manual.c` with a note on what each did, to be found again here. Mark's
  OK (2026-10-27) to run xboxrecomp in the cloud and the Switch toolchain (devkitPro under Wine, NVK, glslang 15.4.0).
