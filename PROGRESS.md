# PROGRESS — Rocket League (Switch 1.0) → iPhone 15 via nxnative

Read this first in every session. Newest status at the top of each section.

## Layout (decisions)

- Workspace: `/home/familybadgujarwilly/Videos/switchrl`. The git repo is `nxnative/` inside it
  (import of seedds/nxnative, `upstream` remote kept). `PROGRESS.md` at the workspace root is a
  symlink to the repo's copy.
- Game inputs stay outside the repo: NSP in `../RL/`, keys in `../prodkeys/keys/prod.keys`.
  Work folder (extraction, Ghidra, runs): `../RL/01005EE0036EC000-work/` (nxnative convention).
- `gh` is at `~/.local/bin/gh` of the real home; this agent's `HOME` differs, so every call needs
  `GH_CONFIG_DIR=/home/familybadgujarwilly/.config/gh /home/familybadgujarwilly/.local/bin/gh`.
- Output encryption: age keypair at `/home/familybadgujarwilly/.config/rl-port/age.key` (mode 600,
  never leaves the laptop). Recipient (public): see `scripts/rl/recipient.txt`.
- Input encryption: `age -p` needs a TTY, so the "symmetric key" is a second age identity whose
  secret half exists **only** as the Actions secret `RL_INPUTS_AGE_KEY` (not kept on disk); inputs
  are encrypted to its public recipient. Same security property, scriptable.
- Imported workflows: removed `pull_request`, `pull_request_target`, `issues` and `schedule`
  triggers (hard rule 2). Only `push` and `workflow_dispatch` remain.

## Decisions that deviate from CLAUDE.md's plan (owner allowed adapting, 2026-11-12) and why

1. **No picture and no GPU timing in CI.** Evidence: the runner's GPU probe (run 43149070535) reports "Apple Paravirtual
   device", `metal3 false`, `metal4 false`; `dm/gfx/nvn/nvn_mtl.m` `mtl_init` aborts an NVN game without Metal 4 (its
   whole replay layer is written against `MTL4*`; the shaders use argument buffers a paravirtual GPU lacks). All
   GitHub-hosted Macs are VMs, and so is the iOS Simulator on them. Porting the 17k-line replay layer to classic
   Metal blind (no local Mac) is not realistic. Instead: **the null GPU** (`dm/gfx/nvn/nvn_null.m`, `DM_NVN_NULL=1`):
   every Metal 4 object is a stand-in, commits report at once. CI can then boot the game, drive it, count draws /
   passes / pipelines / uploads (work metrics that carry to the A16), and exercise texture and memory paths.
   What CI cannot show: rendered frames, GPU ms. The real renderer is exercised only on the owner's iPhone.
   Shader correctness is still checked at build time (every shader translated and compiled by the Metal compiler).
2. **BC→ASTC by content substitution in the host, not by rewriting game files.** Upstream issue seedds/nxnative#570
   re-encodes 16-byte-block BC formats in place inside the RomFS; that cannot cover BC1/BC4 (8-byte blocks), and
   Rocket League's packages are Oodle-compressed chunks (no encoder available, sizes would change). Plan: a pack-time
   cache keyed by the hash of each texture level's BC bytes → ASTC blob, looked up at upload; CPU decode fallback for misses.
3. **Encrypted inputs key** is an age identity (see Layout), not an `age -p` passphrase.
4. **The game itself cannot run in the iOS Simulator** (upstream `docs/development.md`: "The iOS Simulator cannot run the
   game's code"; the host depends on device memory mapping the Simulator lacks). So Definition-of-done item 2 is met in
   two halves: the *game* runs on the Mac host with `NX_FORCE_ASTC_ONLY=1` and `NX_MEMORY_BUDGET_MB=4000` (same host
   code as the iPhone's), and the *iOS front end* (panel, export, monitor, DualSense) runs in the Simulator's XCTest harness.
5. **Native resolution before upscaling** (owner, 2026-11-12): the 1.0 game renders 720p, so the target is the engine
   itself rendering 2560x1440 (`DM_UE3_RES`), with render scale / dynamic resolution / MetalFX as the fallback.

## Status (updated 2026-11-12 ~05:55 local)

- **Phase 0**: done (facts below).
- **Phase 1** (boot on Mac in CI): the whole build works in CI from the encrypted NSP: extract, rewrite, shader split through
  the game's own Oodle decoder, **66,362 shaders translated and compiled, 0 failed** (48 min on the runner), host built
  (run 43150236700). It stops only at the pipeline prebuild, which needs Metal 4. Run 43153001707 is the first boot attempt
  on the null GPU (`scripts/rl/ci_runs/boot.sh`); result pending. The real profile is now `dm/games/01005EE0036EC000/`.
- **Phase 2** (A16): offline half done and measured (`dm/tools/ue3_astc.py` + `dm/tools/bc_astc/bc2astc.cpp`, host lookup
  `dm/gfx/tex/nvn_bcsub.c`); host upload path not wired yet (needs the boot to see how the game uploads textures).
  `NX_MEMORY_BUDGET_MB` watcher is in (`dm/host/memstat.m`).
- **Phase 3** (output, panel, input): written, compiles for Mac and iPhone SDK; Simulator tests running in CI
  (`.github/workflows/rl-ios-sim.yml`): diagnostic panel (`dm/ios/Sources/DiagPanel.swift`), performance record and export
  (`dm/host/hle_perf.c`), monitor output with `DM_TV_WIDTH` up to 2560 and display-mode pick, DualSense mapping test.
- **Phase 4** (performance): groundwork: system settings control (`dm/host/hle_ue3set.c`), anisotropy override (`DM_ANISO`),
  analysis notes below. Benchmark route not written yet.
- **Phase 5**: not started.

### Boot log (null GPU)

- Run 43153001707 (first boot): reached `nnMain`, NVN device (API 53.313, depth near=0, origin upper left), window
  1280x720 format 0x25 with present interval 1, a 520 MB memory pool, AudioOut 6 channels, HID; heap 3.13 GB. Stopped at
  `account::GetUserId` of a never-opened handle (the game asks per player slot before opening: `main+0xaa495c`) → host fixed
  to fail such a call instead of dying (`hle_account.c`).
- Run 43161239013 (run 6): past the user and save, **every package opened, real draws submitted, first presents**, memory
  1.85 GB (Metal 947 MB) under the 4000 MiB budget. Died after ~8 s: `NVN command list …: record {op 36, size 368} … 0 bytes
  into its piece of 56`. Cause (Ghidra, `main+0xa94be8` begin / `main+0xa9505c` flush): Panic Button's RHI keeps a ring of
  command buffers; one left recording with only its 3 set-up commands (56 bytes, the baseline it stores at +0xa4 and never
  submits alone) gets `AddCommandMemory` of the **same block** again when the ring's memory wraps, so the control list holds
  two pieces at one address. Hardware just replays the methods lying there. Host fix: a piece the new memory overlaps is
  kept in a host copy (`hle_nvn.c cmd_new_segment`, `piece_keep`); a piece overwritten some other way is replayed up to
  the last whole record with a red line instead of dying (`nvn_queue.inc nq_flatten`, `DM_NVN_STRICT_PIECES` for the old die).
- Frame rate (decision): the game asks for present interval 1 and ticks by real delta, physics at fixed 120 Hz inside
  (2 substeps at 60 fps, 4 at 30, limit 5 → speed correct down to 24 fps). So profile `fps_target 60` +
  `present_interval display`: upstream's Frame rate setting (30 / 60) applies as the present interval alone. "Unlocked" is
  not offered: the iPhone 15's DisplayPort output is 60 Hz, and the engine's smoothing limit is 62.
- Encrypted build cache works: 12 GB build folder (8.8 GB of it Mac shader libraries) → 1.7 GB archive in the Actions cache.

### Run 43163917332 (run 8, command-memory fix) and what followed, 2026-11-12

- **Boots and holds**: ran its whole 240 s, 5,700 frames, no crash; peak 2,406 MiB of the 4,000 budget (Metal 1,222 MiB).
  The fix's line appears twice (56-byte pieces kept); no "runs past its piece". 142 draws a frame on a screen that did not
  change with A presses (which screen is unknown: no picture on the null GPU, and the game's own log is not captured yet).
- **~26 fps with 45 % of one core in use**: the game thread sleeps in the engine's limiter. Cause: UE3's frame-rate
  smoothing (`UEngine::GetMaxTickRate`, `main+0x40f074`) limits each frame to the running average of the last 300 frame
  times, clamped to Min 22 / Max 62 (`[GEngine]+0x758` / `+0x754`, GEngine through the GOT entry `main+0x2d0ca40` (Ghidra shows it as 0x2e0ca40: its addresses are main offsets + 0x100000)); a wait that overshoots
  lowers the average for good. Fix: `DM_UE3_SMOOTH` (default `steady`) writes Min = Max, so the limit is a flat 62 and the
  display paces the frames (`hle_ue3set.c k_smooth`, keyed by build). Setting "Frame-rate smoothing" in the panel.
- **The engine settings table was never found** ("no system settings table in main"): the row named ScreenPercentage
  points into the tail of the string "UpscaleScreenPercentage" (merged by the linker), which the search excluded. Fixed and
  checked against the relocated image on the laptop (row at main+0x2a4e5d8 passes every check).
- **The 1.0 game renders 1280x720** (window made 1280x720; the 1080p docked mode came in a later update). Owner's
  target (2026-11-12): **native 1440p at 60 fps first**, upscaling only as the fallback. The engine sizes its window,
  viewport and render targets from the system settings `ResX` / `ResY` (`main+0xab43cc` → `main+0xab4f1c`), which it also
  reads from its command line (`ResX=` / `ResY=`, `main+0x42b850`). New setting **Render resolution** (`DM_UE3_RES`,
  game / 1080p / 1440p): the host holds ResX / ResY from `nvnDeviceInitialize` on, before the window is made.
  To confirm in CI: the WindowInitialize line says 2560x1440, the game's fixed pools cope, memory stays under 4,000 MiB.
- CI: `rl-build.yml` runs no longer share a concurrency group (a waiting run was being replaced by the next dispatch).
- `DM_PROBE` prints UTF-32 texts now; the game's log function is `main+0xe2b5c` (`Logf(this, format, …)`), so
  `DM_PROBE=main:0xe2b5c DM_PROBE_LIMIT=600` shows what the game logs.
- In flight: 43166521864 (1440p, A presses), 43166579317 (map URL on the command line:
  `Stadium_P?Game=TAGame.GameInfo_Soccar_TA?GameTags=BotsMedium`), iOS build 43161430713.

### Findings of the 06:15–07:00 runs (2026-11-12)

- Game log through `DM_PROBE=main:0xe2b5c` (run 43166761524): engine initialised, menu level `gameinfo_gfxmenu_sf` loaded,
  "Initial startup" at 6.5 s; Npad 0 constructed "Connected" in style 1 (the others disconnected); `SaveData/Update` and
  `Metrics/RecordMetrics` end in `OSCS_NoNetworkConnection` (offline, as wanted). Only 41 log calls in 150 s.
- The game builds its command line from `nn::os::GetHostArgv` (main thread function `main+0xab0e60`) and hands it to the
  engine's pre-init, so `ResX=` / `ResY=` and a map URL can be given with `game_args`. A map URL alone did not visibly
  load a match (run 43166579317: same memory and draw rates as the menu); not understood yet.
- 40 A presses change neither draws nor passes per second: whether the menu reacts is still unknown.
- **Menu frame, 720p** (perf export + `DM_NVN_PASSDUMP`, run 43169272803): 143 draws, 38 passes, 241 MB of attachments
  loaded + stored a frame (190 MB with `DM_NVN_TBDR=1`: stores of unwritten attachments skipped; pass count unchanged).
  The frame: 4 clear-only passes, shadow depth 512x512 ↔ scene colour+depth alternation (4 scene passes reloading
  both), 12 quarter-size bloom passes at 322x182, then **11 consecutive one-draw passes on the same 1280x720 target, each
  loading and storing it**. The pass dump's lookahead says each of those draws is alone in its command list: the game
  submits and flushes after each (`main+0xa9505c`, callers under investigation with a probe, run 43171792130).
  At 1440p those 11 passes alone are 11 x 2 x 14.7 MB = 324 MB a frame. Candidates: cut the game's flushes there, or
  keep a pass open across submissions in the host.
- The presents counted per second (25–35) vary from run to run on the CI Macs with ~45 % of one core in use, so they
  are not a measure of the game; the earlier reading "smoothing ratchets to 26 fps" is unproven (the smoothing hold
  logged nothing; diagnostics added).
- Null GPU flake: "post ring: the writes of fence N never ran" (run 43169258435): the stand-in's reports lagged 128
  commits behind. Fixed with backpressure in `nvn_null.m` (a commit waits while 16 reports are outstanding).
- ASTC pack step in CI linked the x86 astcenc library of a universal build; fixed (`astc_pack.sh`), rerun 43169181751.
  Without a pack, `NX_FORCE_ASTC_ONLY=1` ran 200 s on the CPU-decode fallback, peak 2,498 MiB.
- iOS build 43161430713 finished (success); `scripts/fetch_ipa.sh` verification running.

## CI loops (all manual-dispatch or push to main; logs with game data are encrypted)

| What | How | Time |
|---|---|---|
| Host compiles (Mac + iPhone SDK), no game data | push touching `dm/host`, `dm/gfx`, `dm/ios` → `scripts/rl/wait_compile.sh` | ~4 min |
| iOS front end in the Simulator (XCTest harness + pictures) | push touching `dm/ios` → `scripts/rl/wait_sim.sh` | ~10 min |
| Full game build / boot from the encrypted NSP | `gh workflow run rl-build.yml -f platform=mac -f run_script=boot.sh -f run_seconds=180 [-f run_env="K=V ..."]` → `scripts/rl/wait_run.sh <id>` → `../RL/ci/<id>/` | ~55 min cold; cache (encrypted, Actions cache) should cut it |

## Survey facts (Phase 0)

- NSP: one Application, title ID `01005EE0036EC000`, version 0 (base 1.0), key generation 3, no update/DLC in the folder.
- ExeFS: `main` (build `becfed979bb4204fbd0945cd450fb296`, text 0x169aa0c, ro 0x1350526, data 0x323f10, bss 0x14b90f0),
  `sdk` (build `0ac561f7b400007b97516d024c903a14…`, new to nxnative), `rtld`, `main.npdm`. No subsdk, no NRO/NRR.
- SDK 3.5.0 (nnSdk, NVN, libz, libcurl). nxnative already runs SDK 3.4.0–3.6.0 games (Xenoblade 2 is 3.5.0, other sdk build).
- Engine: Unreal Engine 3 (package version 868, licensee 20; Panic Button port), Scaleform, PhysX 2.x (Np*/Pxs*), Wwise.
  `main` exports ~46k middleware symbols (Scaleform/PhysX/Wwise); the engine itself is unnamed.
- RomFS 3.5 GB, 1,871 files, all under `tagame/cookednnx`: 1,572 `.xxx` packages (2.47 GB, Oodle-compressed chunks,
  blocks start `8C 0A`), 6 `textures*.tfc` (811 MB, same chunks), Wwise `.bnk`/`.wem` (387 MB),
  `globalshadercache-nnx.bin` (`BMSG`, 9.2 MB), `coalesced_<lang>.bin` (config, encrypted: ECB-like repeats),
  `persistentcookershaderdata.bin`, `globalpersistentcookerdata.upk`.
- Rewriter census: main 15,803 x18 uses, nothing else; sdk 6,417 x18, 106 svc, 48+2 mrs tpidr, 4 msr tpidr, 29 cntpct.
  `patch_nso.py` needed one fix (jump table whose bounds check sits 17+ instructions before `br x18`, main+0xbe78f4); both
  modules now rewrite cleanly on Linux.
- Imports: 535 (libc 241, nn::os 43, fs 30, socket 30, friends 19, ssl 18, hid 17, ldn 17, account 15, oe 14, audio 11
  (AudioOut only), nifm 8, …). Network areas (socket/ssl/ldn/nifm/friends/nsd/ec) must all report offline (hard rule 1).
- NVN: only `nvnBootstrapLoader` imported; 461 NVN names referenced, all already known to the host (`names.txt`).
- CI runner (`xcode-27`): macOS 27.0.1, Xcode 27.2, "Apple M1 (Virtual)", 3 cores, 7 GB RAM, 36 GB free disk.
  GPU "Apple Paravirtual device": BC yes, family mac2 yes, **metal3 no, metal4 no**, no counter sets.

## Analysis notes (Ghidra project `../RL/01005EE0036EC000-work/ghidra/proj`, helpers `tools/gh.sh xrefs|dec <main offsets>`)

Addresses are offsets into the 1.0 `main` (build `becfed97…`). Decompiled text stays in the work folder (never committed).

- **System settings table** (UE3 `FSystemSettings` descriptors): `main+0x2a4da08`, 101 entries of 48 bytes
  `{u32 type (2 int, 4 float, 5 bool), u32 flags, name*, variable*, range*, help*, 0}`. Every setting's global is in bss
  `main+0x2e2adb8…+0x2e2b214`. The ones that matter for performance:
  `ScreenPercentage +0x2e2b1b0 (float)`, `UseVsync +0x2e2b1ac`, `UpscaleScreenPercentage +0x2e2b1b4`,
  `UpscaleTargetFramerateDocked +0x2e2b1b8 (float)`, `UpscaleTargetFramerateUndocked +0x2e2b1bc (float)`,
  `MinimumScreenScale +0x2e2b1c0 (float)`, `AllowDynamicResolution +0x2e2b1c4`, `MaxAnisotropy +0x2e2b1a4`,
  `MaxMultiSamples +0x2e2b1e4`, `DetailMode +0x2e2adb8`, `DynamicShadows +0x2e2ae88`, `MaxShadowResolution +0x2e2ae9c`,
  `MinShadowResolution +0x2e2ae94`, `ShadowTexelsPerPixel +0x2e2aea4`, `MotionBlur +0x2e2ade8`, `DepthOfField +0x2e2adf0`,
  `AmbientOcclusion +0x2e2adf4`, `Bloom +0x2e2adf8`, `bAllowLightShafts +0x2e2adfc`, `LensFlares +0x2e2ae10`,
  `Distortion +0x2e2ae00`, `ParticleLODBias +0x2e2ae24`, `OneFrameThreadLag +0x2e2ae1c`, `AllowPerFrameSleep +0x2e2b20c`,
  `AllowPerFrameYield +0x2e2b210`, `ResX/ResY +0x2e2b1d0/+0x2e2b1d4`. Full list: work folder `survey/system_settings.tsv`.
  Plan: the host writes these globals (profile table name → offset/type), each exposed as a panel toggle.
- **Dynamic resolution controller**: `main+0x417f80`. Reads `UpscaleTargetFramerateDocked/Undocked`, compares the target
  frame time with a measured one (`main+0x2f382ec`), steps an index (`+0xaa8` of the object at `*main+0x2e0cdd0`) through a
  resolution table ("ScreenPercentageMap"), posts "Change_Resolution", and switches `nn::oe::SetPerformanceConfiguration`
  (0x20003 / 0x20004 / 0x92220008). Gated by `AllowDynamicResolution` and the object's `+0x404/+0x414` flags.
- **Frame limiter**: `main+0xae6154` (UE3 `appUpdateTimeAndHandleMaxTickRate`): calls the engine's `GetMaxTickRate`
  (vtable `+0x290` of `*main+0x2e0ca40`), then sleeps (`main+0xaa20ec`) and spins on `nn::os::GetSystemTick` until
  `1/MaxTickRate`; `AllowPerFrameSleep/Yield` are read here.
- **Physics tick**: fixed. The script defaults of `EngineShare_TA` (in `tagame.xxx`, read with the package parser) are
  `PhysicsFramerate = 120.0`, `MaxPhysicsSubsteps = 5`; its native `GetBulletFixedDeltaTime` (`main+0xba0f54`) returns
  `1 / [this+0x198]`. So physics steps at 120 Hz whatever the frame rate, up to 5 steps a frame: gameplay speed is right
  at any frame rate of 24 fps or more (5 / 120 s = 41.7 ms a frame at most), and a 30 or 60 fps cap changes only how many
  steps a frame takes (4 or 2).
- **Engine frame smoothing** (class defaults of `Engine` in `engine.xxx`; the encrypted config may override them):
  `bSmoothFrameRate = true`, `MaxSmoothedFrameRate = 62`, `MinSmoothedFrameRate = 22`: the limiter at `main+0xae6154`
  asks `GetMaxTickRate`, which with smoothing caps the tick near the measured rate within 22..62 fps. So the engine itself
  never runs past 62 fps; a 30 fps cap has to come from the present interval (the host's `present_interval display` +
  `DM_FPS_TARGET`), not from this limiter.
- **Ways into a match without menus** (leads for the benchmark; from `tagame.xxx`'s names and `main`'s strings): `main`
  imports `nn::os::GetHostArgc/Argv` and holds "open stadium_p?Listen", "GameTags=%s"; the script has game modes
  `GameInfo_Soccar_TA`, `GameInfo_BotPerfTest_TA`, `GameInfo_PerfTest_TA` and tags `BotsEasy/Medium/Hard/Unfair/None`,
  `UnlimitedTime`. The host passes a command line after `--` (`dmhost <modules> -- <args>`, or the profile's `game_args`),
  so an Unreal map URL such as `Stadium_P?Game=TAGame.GameInfo_Soccar_TA?GameTags=BotsMedium` may start a bot match
  directly: to be tried (workflow input `game_args`).
- **First-boot screens to expect** (script names): EULA / ToS / privacy (`bHasAcceptedEULA`, `ShowPrivacyModal`), a start
  menu (`GFxData_StartMenu_TA`, `EventTitleScreenClicked`), a tutorial prompt (`GFxModal_Tutorial_TA`), then the main
  menu (`GFxData_MainMenu_TA`); menus load their own packages (`gfx_startmenu_sf.xxx`, `gfx_mainmenu_sf.xxx`,
  `gfx_privatematchmenu_sf.xxx`, ...), so `DM_FS=1` shows which menu is up.
- **Stats text** "Docked %3.2f fps" / "Undocked %3.2f fps": `main+0x413e64`.
- Texture formats in the cooked data (36,710 unique mips): DXT1 0.98 GB, DXT5 1.38 GB (5,094 mips of DXT5 normal maps
  `TC_NormalmapSwizzle`: x/y in alpha/green), A8R8G8B8 0.94 GB, G8, V8U8. No BC4/5/6/7. Mips are stored pre-swizzled
  (block-linear, one level each); packages: tables AES-256-ECB, bodies Oodle chunks (first block `8C 0A`, tiny ones `8C 02`).
- Shaders: 1,738 in `globalshadercache-nnx.bin`; material shaders inline in the packages (e.g. 3,265 in `tagame.xxx`).

## Done

- Read nxnative README, AGENTS.md, docs/agents 01–03, 06.
- Cloned nxnative, sanitised workflows, extended `.gitignore`, generated age keypair.
- Repo `user99672223/rl-nxnative-a16` (public), `upstream` remote = seedds/nxnative.
- Encrypted inputs uploaded to draft release `inputs-v0` (2 NSP parts + keys + SHA256SUMS).
- Local tools in the work folder: hactool, venv (lz4, capstone), Ghidra 12.2 (needs JDK 25: `/usr/lib/jvm/java-25-openjdk-amd64`),
  flat images `elf/main.bin`, `elf/sdk.bin`, disassembler `tools/rldis.py <module> <hex> <count>`.
- CI: `.github/workflows/rl-build.yml` (manual dispatch) → `scripts/fetch_artifact.sh <run id>` decrypts results to `../RL/ci/<run>/`.

## Texture pack measurements (laptop, full game, 2026-11-12)

- `ue3_astc.py` over all 1,573 packages: 44,037 textures, 4,446 unique BC ones, 35,184 levels, 2.08 GB of BC blocks
  → pack 1.43 GB (0.69x). 16 min on 8 cores (122 CPU-minutes).
- Block size per texture = largest of 6x6 / 5x5 / 4x4 whose top level keeps the PSNR floor against the BC picture:
  colour (floor 38 dB): 3,154 at 6x6, 373 at 5x5, 296 at 4x4; DXT5 normal maps x/y in A/G (40 dB): 198 / 105 / 257;
  DXT1 normal maps (38 dB): 40 / 7 / 16. Sampled before choosing: DXT1 colour at 6x6 min 39–42 dB (3.56 bpp vs BC1's 4),
  DXT1 normal maps at 5x5 only 27 dB (so they need 4x4), one DXT5 atlas at 6x6 only 28 dB (hence per-texture choice).
- Keys verified: the pack's SHA-256 keys equal what `nvn_bcsub_key` computes; CPU decode matches a numpy reference within
  rounding (max difference 2 of 255).
- On the laptop the dev decoder (ooz) lacks the codec of tiny `8C 02` blocks (821 textures skipped); CI uses the game's own.

## Offline audit (hard rule 1), 2026-11-12

- The host opens no socket anywhere (`grep` for socket/connect/NSURLSession/CFNetwork/NWConnection over `dm/host`,
  `dm/ios`, `dm/mac`, `dm/gfx`: none). Every `nn::socket` / `ssl` / `nifm` / `ldn` / `friends` entry the game imports is cut
  to "no network" (`hle_net.c`, `hle_offline.c`; SDK 3's names added). The web view refuses any non-local URL.
- Still to do: hide upstream's opt-in iCloud Drive "Cloud saves" in this build (it is a folder the OS syncs, off by default).

## Open problems / risks

- nxnative's build (Metal shader compiler, xcodebuild) only runs on macOS → everything past the
  static survey has to run on Actions runners from the encrypted NSP.
- Upstream docs say GitHub's Macs have no Metal 4; whether the host can render there is unknown.
- Local disk: 29 GB free at start.

## Next steps (in order)

1. Read run 43153001707's `run/host.log`: how far the game boots on the null GPU; fix what stops it (symptom index first).
2. From that run's NVN call counts decide the texture upload path (pool writes vs CopyBufferToTexture) and wire
   `nvn_bcsub` into `nvn_mtl.m` behind `NX_FORCE_ASTC_ONLY=1` / no-BC devices; let `front_ios_device_check` accept A16.
3. Benchmark route (`bench.tsv`, `DM_KEYS`) into an Exhibition match; `DM_PERF_EXPORT` + `DM_NVN_TSV` → `docs/perf/`.
4. FPS cap setting (find how the game paces: present interval, `GetMaxTickRate`), dynamic-resolution table, patches.
5. iOS build in CI (unsigned IPA, encrypted), Simulator run of the real app, `scripts/fetch_ipa.sh`.
6. `docs/optimizations.md`, `REPORT.md`, final audit of repo / releases / artifacts / logs.
