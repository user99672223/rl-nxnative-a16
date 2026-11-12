# Rocket League on an iPhone 15 (A16): optimisations

What was found, the evidence, what was changed, the measured effect and the risk, one entry per item. Every item has a switch; the switches are in the launcher, the in-game menu and the diagnostic panel (see [REPORT.md](../REPORT.md) for what to set).

**How to read the measurements.** No Metal 4 GPU was available to this port: the game ran only on GitHub's virtual Macs, on the null GPU (`dm/gfx/nvn/nvn_null.h`), where the renderer does everything but execute. So the figures here are counts of work (draws, render passes, render-target megabytes loaded and stored, pipelines, texture bytes, memory), which do not depend on the machine, and CPU times on the runner's M1 cores. There are no GPU milliseconds and no pictures. Each entry says which of its claims are measured and which are expected and must be confirmed on the phone with the exported performance log.

Addresses are offsets into the 1.0 `main` (build `becfed979bb4204fbd0945cd450fb296`).

## Summary

| # | Item | Switch | Default | State |
|---|---|---|---|---|
| 1 | BC textures as ASTC from an offline pack | `NX_FORCE_ASTC_ONLY`, automatic on a GPU without BC | on where the GPU lacks BC | built, measured offline; host path compiles, run pending |
| 2 | Memory budget watcher | `NX_MEMORY_BUDGET_MB` | off (CI: 4000) | built |
| 3 | Output at 1440p with the present's upscaler | Monitor output (`DM_TV_WIDTH`), Upscaling (`DM_PRESENT_UPSCALE`) | 1080p, Standard | built, Simulator-tested |
| 4 | Render scale through the engine's own `ScreenPercentage` | Render scale (`DM_UE3_SCALE`) | 100 % | built, effect pending |
| 5 | The game's dynamic resolution | Dynamic resolution (`DM_UE3_DYNRES`, `DM_UE3_DYNRES_MIN`) | game's own | built, effect pending |
| 6 | Anisotropic filtering level | Anisotropic filtering (`DM_ANISO`) | game's own | built |
| 7 | Any engine system setting by name | `DM_UE3S_<Setting>`, `DM_UE3_SET` | game's own | built |
| 8 | Native render resolution (the engine's `ResX` / `ResY`) | Render resolution (`DM_UE3_RES`) | game's own (720p) until measured | built, run pending |
| 9 | The engine's frame-rate smoothing held flat | Frame-rate smoothing (`DM_UE3_SMOOTH`) | steady | built, run pending |
| 10 | Frame rate 30 / 60 by the present interval | Frame rate (`DM_FPS_TARGET`) | 60 | built |
| 11 | Fences no longer end the render pass | `DM_NVN_FENCE_CUT` (1 = as before) | on | measured: menu 38 → 29 passes, 241 → 138 MB |
| 12 | Unreal's mid-frame command-buffer kicks dropped | Render pass merging (`DM_UE3_KICK`) | game's own | measured: with 11, 26 passes, 112 MB |
| 13 | Render-target pool sized by the render resolution | follows Render resolution | – | needed for 8; measured: 1440p boots |

(The table grows as the benchmark gives numbers; entries below are complete only where they say "measured".)

## 1. BC textures as ASTC

**Found.** An A16's GPU has no BC texture formats (`MTLDevice.supportsBCTextureCompression` is false; upstream refuses such a device at start). The game's cooked data holds 36,710 unique texture levels: 0.98 GB of DXT1, 1.38 GB of DXT5 (5,094 levels of them normal maps with x and y in alpha and green, `TC_NormalmapSwizzle`), 0.94 GB uncompressed, no BC4 to BC7 (survey of all 1,573 packages, `dm/tools/ue3_astc.py --inventory`).

**Why not re-encode the game's files in place** (upstream issue #570's approach for 16-byte-block formats): half the data is DXT1, whose 8-byte blocks no ASTC block replaces, and the packages are Oodle-compressed chunks with their sizes recorded, with no encoder at hand.

**Changed.** Offline, `ue3_astc.py` reads every `Texture2D` of every package (tables AES-256-ECB, bodies through the game's own Oodle decoder), deswizzles each level with the host's own code and hands it to `bc2astc`, which re-encodes it with Arm's astcenc and stores it under the SHA-256 of its BC bytes. At run time the host (`nvn_mtl.m` `pkg_upload_bcs`) deswizzles each level the game uploads, computes the same key and uploads the pack's ASTC blocks; the game's files and memory are untouched. A texture the pack does not hold is decoded on the CPU to RGBA8; a single level missing gets flat-colour blocks. BC4 / BC5 are decoded to R8 / RG8.

**Block size, per texture, by measured quality**: the largest of 6x6, 5x5, 4x4 whose top level keeps a PSNR floor against the BC picture (38 dB colour, 40 dB two-channel normal maps, 38 dB DXT1 normal maps). Sampled first on real textures: DXT1 colour at 6x6 kept 39 to 42 dB at 3.56 bits per texel (BC1 is 4); DXT1 normal maps at 5x5 fell to 27 dB, so they need 4x4; one DXT5 sign atlas at 6x6 fell to 28 dB, which is why the choice is per texture and not per format. For the swizzled DXT5 normal maps red is constant and blue nearly so, so the encoder's error is weighted to green and alpha (0.2 / 1 / 0.5 / 1).

**Measured (laptop, whole game).** 4,446 unique BC textures, 35,184 levels, 2.08 GB of BC blocks → a 1.43 GB pack (0.69 of the BC size): 3,392 textures at 6x6, 485 at 5x5, 569 at 4x4. So texture memory on the phone is about 0.7 of what a BC device uses, not the 4 to 8 times of decoding. 16 minutes on 8 x86 cores. The pack's keys were checked against the host's key function, and the host's CPU decoder against an independent decoder (largest difference 2 of 255, rounding).

**Cost.** 1.43 GB more in the app. Per uploaded level one SHA-256 of its BC bytes and a CPU deswizzle (the GPU deswizzle path is not used on this route).

**Risk.** Not yet seen on a GPU. Copies between textures on the GPU (the engine's texture streaming) are not covered yet. Colour fidelity is ASTC on top of BC: a second lossy step, bounded by the floors above.

## 2. Memory budget

**Found.** A base iPhone 15 has 6 GB; with the Increased Memory Limit entitlement an app gets about 4 GB before iOS ends it. A Mac or the Simulator has no such limit.

**Changed.** `NX_MEMORY_BUDGET_MB=<MiB>` starts a watcher (`dm/host/memstat.m`) that samples `phys_footprint` (what iOS counts) four times a second, logs the peak and ends the run with exit code 86 and a `[RED]` line when it passes the budget. On the null GPU it adds Metal's allocated size, since textures there are never written and so never counted. The panel shows the same figure against the limit.

## 3. Output at 1440p

**Found.** Upstream caps a TV's drawable at 1920 across. The 1.0 game renders 1280x720 in both console modes, with a dynamic resolution below that (its 1080p mode came in a later update); see item 8 for rendering natively at the monitor's size.

**Changed.** Monitor output (`DM_TV_WIDTH`: 1080p, 1440p, 4K, the display's own) sets the widest drawable on the external screen; the display is asked for its 16:9 mode of that width (`front_ios.m` `tv_pick_mode`), and the present scales the game's picture to it: Standard (bilinear) or MetalFX's spatial scaler where `MTLFXSpatialScalerDescriptor supportsMetal4FX` says the GPU has it (`nvn_mtl.m` `fx_encode`, upstream's). The drawable's size is set explicitly.

**Expected, to confirm on the phone.** The upscale to 2560x1440 is one full-screen pass: about 3.7 M pixels written a frame against 2.1 M at 1080p.

## 4 to 7. Engine settings held from the host

**Found.** `main+0x2a4da08` is Unreal's system settings table: 101 rows naming each setting and the address of its value. `hle_ue3set.c` finds the table by its shape at run time and writes chosen values each frame (the game applies its own when docked or undocked and when its dynamic resolution moves). The dynamic resolution controller is `main+0x417f80` (target frame rates `UpscaleTargetFramerateDocked` / `Undocked`, floor `MinimumScreenScale`, switch `AllowDynamicResolution`); the frame limiter is `main+0xae6154`.

**Changed.** Render scale (`DM_UE3_SCALE`, 50 to 100 %) holds `ScreenPercentage` and turns the game's dynamic resolution off below 100 %; Dynamic resolution (`DM_UE3_DYNRES`) and its floor (`DM_UE3_DYNRES_MIN`); Anisotropic filtering (`DM_ANISO`) overrides the level of every trilinear sampler in the graphics layer; `DM_UE3S_<Setting>` / `DM_UE3_SET` hold any row by name (`MotionBlur`, `Bloom`, `DepthOfField`, `AmbientOcclusion`, `bAllowLightShafts`, `LensFlares`, `DynamicShadows`, `MaxShadowResolution`, ...).

**Effect.** Pending the benchmark.

## 8. Native render resolution

**Found.** The engine sizes its window, its viewport and every scene render target from two system settings, `ResX` and `ResY` (RHI start `main+0xab43cc` → window `main+0xab4f1c`; the game engine's start also reads `ResX=` / `ResY=` from the command line, `main+0x42b850`). The 1.0 game's config gives 1280x720. So the picture a 1440p monitor gets is a 720p picture scaled 2x each way unless the engine itself is told a larger size.

**Changed.** Render resolution (`DM_UE3_RES`: the game's own, 1920x1080, 2560x1440) holds `ResX` / `ResY` from the moment the game makes its NVN device, before the window exists (`hle_ue3set.c` `dm_ue3set_early`, called from `nvnDeviceInitialize`). Nothing in the game's code is patched. It takes a restart, as the engine reads the size once.

**Cost, expected.** 4x the pixels of 720p in every full-screen pass, and render targets 4x the size in the game's fixed texture pool. Measured numbers follow below once the benchmark has run at each size.

## 9. Frame-rate smoothing

**Found.** With `bSmoothFrameRate` (on in this game's config) Unreal's `GetMaxTickRate` (`main+0x40f074`) returns the running average of the last 300 frame times as the frame-rate limit, kept between `MinSmoothedFrameRate` 22 and `MaxSmoothedFrameRate` 62, and the frame limiter (`main+0xae6154`) sleeps each frame out to it. Since every frame is held to the average, no frame can be shorter than the average: a wait that overshoots, or one slow stretch, lowers the limit and nothing raises it again. **Measured:** on the CI Mac the game settled at 25.9 fps with the game thread asleep in the limiter and 45 % of one core in use (run 43163917332; thread dump: game thread in `main+0xae7768`, last call a sleep).

**Changed.** Frame-rate smoothing = Steady (`DM_UE3_SMOOTH`, the default) writes the minimum to the maximum each frame, so the limit is a flat 62 fps, the game's own ceiling, and the display's vsync sets the pace. "Game's own" puts the minimum back. The offsets are of this build (`hle_ue3set.c` `k_smooth`).

**Risk.** None seen for game speed: the engine steps by measured time and the physics by its own 120 Hz steps.

## 10. Frame rate 30 / 60

**Found.** The game asks NVN for present interval 1 and steps by the time that passed; the physics runs fixed 120 Hz steps inside each tick (`PhysicsFramerate=120`, `MaxPhysicsSubsteps=5`), so the game's speed is right at any frame rate down to 24 fps.

**Changed.** Profile keys `fps_target 60` and `present_interval display`: the player's Frame rate (30 or 60) is applied as the present interval alone. No "unlocked": an iPhone 15's DisplayPort output is 60 Hz, and the engine's ceiling is 62.

## 11. Fences that ended the render pass

**Found.** A menu frame was 38 render passes for 143 draws; its last 11 draws, the UI, were 11 passes on the same 1280x720 target, each loading and storing all of it. Per frame the game calls `nvnCommandBufferFenceSync` 17 times: Scaleform's render sync puts a fence after each batch it draws, to know when its vertex memory is free again (`main+0xa94988`, behind the thunks at `main+0xa2c408`), and the occlusion queries fence as well (`main+0xa988fc`). The host's queue layer made each fence a step of its own between two Metal command buffers, and a command buffer's end is a render pass's end. Evidence: the op-by-op pass dump of run 43183630282 (each UI draw followed by the list's end and nothing else), the call counts of a frame.

**Changed.** `dm/gfx/nvn/nvn_queue.inc`: a fence among commands stays inside their command buffer; its sync is published with that command buffer's fence when it is committed. The sync never passes earlier than the game asked, only at the end of the commands it sits among. `DM_NVN_FENCE_CUT=1` restores the cut. This is general host behaviour, not a patch of the game.

**Measured (null GPU, menu, 720p).** 38 → 29 passes, 241 → 138 MB of attachments loaded and stored a frame.

**Risk.** A game that fences in the middle of a list, submits it and then waits for the sync gets it when the whole list's command buffer is done instead of part-way: later, never wrong.

## 12. Unreal's mid-frame kicks

**Found.** `RHIKickCommandBuffer` (`main+0xa95968`) ends the command list, submits it and flushes the queue without waiting; the scene renderer calls it between phases and after post-process steps (probe run 43171792130: 14 submissions a frame). Each ends the open pass.

**Changed.** A rewriter stub (`dm/games/01005EE0036EC000/patches.py` `kick_gate`) returns from the kick at once while a flag is set; the flag is a system setting no code reads, held by the host. Setting Render pass merging (`DM_UE3_KICK`: Game's own / Merged).

**Measured.** With item 11: 29 → 26 passes, 138 → 112 MB. One submission a frame instead of 14. At 2560x1440 with both: 36 → 25 passes, 966 → 424 MB a frame (run 43187087202).

**Risk.** The GPU starts on the frame when the frame's list is complete rather than during it.

**On the iPhone 15 (2026-11-12, build 97a844ad, 1080p, a Free Play match).** Switched on in the in-game menu mid-match: 87 → 76 passes and 2,693 → 2,264 MB of attachments a frame, 38 → 51 fps over the next 35 s, no change in the picture reported. It is now the default; "Game's own" stays in the settings.

## 13. Render-target pool

**Found.** The RHI makes its render targets in a fixed pool of 286 MB (`main+0xab4698`), enough for 1280x720. At 2560x1440 the pool runs out and the game faults on the null storage (`main+0xa9ca88`, run 43172097281).

**Changed.** Stub `target_pool`: the pool is 286 MB at the game's size, 656 MB above 1280 across, 1,152 MB above 1920 across, read from the held `ResX` when the pool is made.

**Measured.** 2560x1440 boots and runs; peak memory 2,511 MiB against 2,354 MiB at 720p (the pool is address space until targets are made in it).

## 14. The replay thread ran at a third of the clock

**Found (iPhone 15, first device log, `../RL/device/drop1/log.txt`).** In a Free Play match at 1080p the GPU took 12 to 14 ms a frame, inside 16.7 ms, and the frame rate was still 35 to 55. The time went to the host thread that replays the game's NVN commands into Metal (`nq_worker`, `dm/gfx/nvn/nvn_queue.inc`). Its work did not grow: 22 M cycles a frame at the menu and 20 to 25 M in the match. Its clock fell: a mean 3.35 GHz at the menu (6.7 ms of CPU a frame), 1.1 to 1.9 GHz in the match (13 to 20 ms), with the CPU drawing 0.6 W against 1.0 W at the menu. The thread had a user-interactive QoS and no deadline; the thread that acquires the frame declares one (`set_time_constraint_comp`, `hle_nvn.c`) and iOS sets the clock from such declarations.

**Changed.** The replay thread declares a real-time deadline of its own: period one frame, computation the larger of 1.25 x the p90 and 1.1 x the peak of its CPU time a frame over 120 frames, capped at 0.9 x the period (`nq_rt_tick`). `DM_NQ_RT=0` turns it off.

**Expected, not measured yet.** At 3.35 GHz the match's 20 to 25 M cycles are 6 to 7.5 ms, which leaves the GPU's 12 to 14 ms as the limit: 60 fps at 1080p. The next device log's "translation detail" lines (mean clock, CPU ms) say whether it holds.

**Risk.** A real-time thread that runs past its declaration is demoted for tens of milliseconds; the declaration follows the measured peak to avoid it. Two real-time threads and the game's main thread now compete for the A16's two performance cores.

