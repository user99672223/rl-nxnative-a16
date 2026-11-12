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

**Found.** Upstream caps a TV's drawable at 1920 across. The game renders at most 1920x1080 docked (dynamic resolution below that).

**Changed.** Monitor output (`DM_TV_WIDTH`: 1080p, 1440p, 4K, the display's own) sets the widest drawable on the external screen; the display is asked for its 16:9 mode of that width (`front_ios.m` `tv_pick_mode`), and the present scales the game's picture to it: Standard (bilinear) or MetalFX's spatial scaler where `MTLFXSpatialScalerDescriptor supportsMetal4FX` says the GPU has it (`nvn_mtl.m` `fx_encode`, upstream's). The drawable's size is set explicitly.

**Expected, to confirm on the phone.** The upscale to 2560x1440 is one full-screen pass: about 3.7 M pixels written a frame against 2.1 M at 1080p.

## 4 to 7. Engine settings held from the host

**Found.** `main+0x2a4da08` is Unreal's system settings table: 101 rows naming each setting and the address of its value. `hle_ue3set.c` finds the table by its shape at run time and writes chosen values each frame (the game applies its own when docked or undocked and when its dynamic resolution moves). The dynamic resolution controller is `main+0x417f80` (target frame rates `UpscaleTargetFramerateDocked` / `Undocked`, floor `MinimumScreenScale`, switch `AllowDynamicResolution`); the frame limiter is `main+0xae6154`.

**Changed.** Render scale (`DM_UE3_SCALE`, 50 to 100 %) holds `ScreenPercentage` and turns the game's dynamic resolution off below 100 %; Dynamic resolution (`DM_UE3_DYNRES`) and its floor (`DM_UE3_DYNRES_MIN`); Anisotropic filtering (`DM_ANISO`) overrides the level of every trilinear sampler in the graphics layer; `DM_UE3S_<Setting>` / `DM_UE3_SET` hold any row by name (`MotionBlur`, `Bloom`, `DepthOfField`, `AmbientOcclusion`, `bAllowLightShafts`, `LensFlares`, `DynamicShadows`, `MaxShadowResolution`, ...).

**Effect.** Pending the benchmark.
