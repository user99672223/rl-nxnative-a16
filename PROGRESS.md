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

### Findings of the 07:00–07:30 runs (2026-11-12)

- **Ghidra addresses are main offsets + 0x100000** for everything read off a decompile (`PTR_DAT_02e0ca40` is
  `main+0x2d0ca40`); function names `FUN_00b9505c` = `main+0xa9505c`. The settings variables listed below were taken from
  the file and are true main offsets. (The smoothing hold first read the engine pointer 1 MB off.)
- Settings table rows end in a run-time flag (1 after the config is read), not 0: fixed; **101 settings found**, the dump
  works (`DM_UE3_DUMP=1`). As shipped: ResX/ResY 1280x720, ScreenPercentage 100, AllowDynamicResolution 1,
  MinimumScreenScale 100, UpscaleTargetFramerate 60/60, MaxAnisotropy 4, MaxShadowResolution 512, MotionBlur 1,
  DepthOfField 1, Bloom 1, AmbientOcclusion 0, bAllowLightShafts 1, DynamicShadows 1, OneFrameThreadLag 1.
- **Native 1440p takes** (`DM_UE3_RES=2560x1440`, run 43172097281): "WindowInitialize … first 2560x1440". The game then
  faulted at `main+0xa9ca88`: a render target's storage came back null from the RHI's fixed "Targets" pool
  (`main+0xab4698: mov w1, #0x11e10000`, 286 MB; the other pools: Textures 520 MB, Buffers 128 MB, Shaders 64 MB,
  CommandBuffers 48 MB, Queries 2 MB). Fix: a rewriter stub sizes that pool by the held ResX (286 / 656 / 1,152 MB),
  `dm/games/01005EE0036EC000/patches.py target_pool`. Rerun 43175097487.
- **The per-draw submissions are Unreal's `RHIKickCommandBuffer`** (`main+0xa95968` → `main+0xab5ed4(dev, 1, 0)`), called
  by the scene renderer between phases and after each post-process step (probe run 43171792130: callers
  `main+0x7f0940`, `main+0x7f10e4`, …). Stub `kick_gate` returns at once while a flag is set; the flag is the system
  setting `SpeedTreeLeaves` (`main+0x2e2adc0`, no code reads it), held by the host: `DM_UE3_KICK=merge|game`, setting
  "Render pass merging", default game. A/B runs 43173025823 (game) / 43173040846 (merge).
- Menu input: 30 A presses → no file opened, no log line (run 43169511083). The focus handler (`main+0xaa6a0`) only
  notifies the online subsystem. HLE call counts with / without presses: runs 43172452198 / 43172468085
  (`boot.sh` now ends a run with SIGTERM so the host writes `hlecount.tsv` and its exports).
- ASTC pack in CI: 1.61 GB, 33,696 levels, 13 m 55 s; the boot with it died on the null GPU's post-ring flake (before
  the backpressure fix) at 1,326 MiB. To rerun.

### `scripts/fetch_ipa.sh` verified end to end (2026-11-12 08:32 local)

- Run 43161430713 (platform=ios): artifact `rl-enc-ios-7`, 4.88 GB encrypted in parts → downloaded, joined, decrypted
  with the laptop key, **checksum matches the build's**; `../RL/ipa/RocketLeague-run43161430713.ipa`, 4.6 GB, 52 files,
  `Payload/DmIOS.app`. That build predates the ASTC pack fix, the settings fixes and both stubs: it has **no texture
  pack inside**, so it is a test of the pipeline only. The final IPA is a later `platform=ios run_script=astc_pack.sh` run.
- Input reaches the game: with 12 A presses `fs::MountSaveData` / `EnsureSaveData` are called once more (HLE counts of
  runs 43172452198 / 43172468085). `nn::hid::GetNpadState` is called once a tick: 2,183 calls in 95 s, so the game
  ticks at about 25 a second on the CI Mac. Each tick also resubmits a network request (`main+0xaca218`, the online
  subsystem's reconnect loop; instant in the host). At boot `main+0xab7f90` sleeps 1 s while the request is "on hold".

### Native 1440p boots; menu measurements (2026-11-12 ~09:00 local)

| run | settings | window | passes / frame | draws | attachment load+store / frame | peak memory |
|---|---|---|---|---|---|---|
| 43173025823 | as shipped | 1280x720 | 38 | 143 | 241 MB | 2,354 MiB |
| 43169272803 | `DM_NVN_TBDR=1` | 1280x720 | 38 | 143 | 190 MB | – |
| 43173040846 | `DM_UE3_KICK=merge` | 1280x720 | 35 | 142 | 215 MB | – |
| 43175097487 | `DM_UE3_RES=2560x1440` | **2560x1440** | 36 | 143 | **966 MB** | 2,511 MiB |
| 43175174078 | `NX_FORCE_ASTC_ONLY=1` + pack | 1280x720 | – | – | – | 2,199 MiB |

- 1440p: the pool stub works (no fault), 120 s, under the 4,000 MiB budget. The traffic is 4x: at 60 fps 58 GB/s of
  attachment loads and stores for a *menu* frame, more than an A16 can move. Native 1440p at 60 needs the passes merged.
- `DM_UE3_KICK=merge` removes the kicks but only 3 passes: the UI's draws are each followed by the render command
  "BlockUntilGPUIdle" (`main+0x27ae74` FlushRenderingCommands → `main+0xa9597c` → flush + `nvnQueueFinish`), about ten
  times a frame: a CPU–GPU round trip each, which also ends the pass. Probe run 43177541723 names the callers.
- Smoothing hold applies (min 22 → 62) and does not change the CI tick rate (25–33 a second): not the cause.
- ASTC-only at the menu: 200 s, clean exit, 12 textures not in the pack (CPU-decoded: 64x64 ones, one 640x640, two
  1024x1024 with 11 levels): to look at.

### What split the UI into one pass a draw, and the fix (2026-11-12 ~11:30 local)

- Not a per-frame "BlockUntilGPUIdle" (probe of `main+0x27ae74`: 0 hits in normal frames; that reading was wrong).
- The op trace (`DM_NVN_PASSDUMP_ALL`, run 43183630282) shows each UI draw followed by the end of its command buffer with
  no record between. Per frame the game calls `nvnCommandBufferFenceSync` 17 times: Scaleform's render sync (thunks at
  `main+0xa2c408..0xa2c438`: kick, is-fence-done, wait-fence `main+0xa94a00`, new-fence `main+0xa94988`) fences after
  every batch it draws; the occlusion queries fence too (`main+0xa988fc`, `main+0xa98a9c`). `SyncWait` 16 a frame are
  polls with timeout 0. `nvn_queue.inc` made every fence a node of its own = a Metal command buffer = a pass end.
- **Fix (host, general):** a fence among commands stays in their node and is published at its commit
  (`nq_record_one`, `struct nq_fence`); `DM_NVN_FENCE_CUT=1` cuts as before.

| run | menu frame, 720p | passes | load+store MB |
|---|---|---|---|
| 43173025823 | before | 38 | 241 |
| 43187034962 | fences inline | 29 | 138 |
| 43187053178 | fences inline + `DM_UE3_KICK=merge` | 26 | 112 |
| 43187070052 | the same + `DM_NVN_TBDR=1` | 29 | 127 |
| 43175097487 | 2560x1440, before | 36 | 966 |
| 43187087202 | 2560x1440, fences inline + `DM_UE3_KICK=merge` | 25 | 424 |

  (`DM_NVN_TBDR=1` turns the waiting-clear path off, `lc_on`, which is why it has more passes here.)
- Input: the game does read the presses (probe of its pad object, run 43189985782: buttons 0x1 / 0x2 / 0x400 while A /
  B / Plus are held, on the one connected pad), and still shows nothing new after "PressAnyButtonToStart". Open.

### Disk clean-up on the laptop (owner asked, 2026-11-12)

Deleted, all regenerable: the test IPA (`../RL/ipa/`, re-fetch with `scripts/fetch_ipa.sh`), the local ASTC pack and its
trial packs (`pack/`, `astc_eval/`; CI builds its own), the extracted RomFS (`data/v0/RomFS`; re-extract from the NSP with
hactool when a local package read is needed; `data/v0/ExeFS` kept for the rewriter), decompressed packages (`dec/`), CI
results older than the last 12 runs, pip's cache. Kept: the NSP and keys, Ghidra and its project, `elf/`, `survey/`.

## First run on the owner's iPhone 15 (2026-11-12, build 97a844ad, iOS 27.2.1; files in `../RL/device/drop1/`)

The owner's account: first Play crashed, the second reached the menu, Free Play loaded almost at once and played well; setting the monitor output to 1080p or 1440p really switches the monitor's mode; the monitor stays at 60 Hz (its mode list has no rate above 60 at any size); after changing settings during play the picture ended up as a zoomed part of the scene on both screens.

What the two logs and the performance export show:

| Finding | Evidence | Done |
|---|---|---|
| The game runs on the A16: ASTC pack in use, menu 60 fps, Free Play playable | `log.txt`, 8,970 frames, peak footprint 2,259 MiB of about 3,970 | - |
| Crash 27 s into the menu: `FATAL post ring: the writes of fence 42217 never ran` | `log.prev.txt`. About 26 submissions a frame, the phone about 3 frames ahead of the display, one 35 ms frame put more than 128 in flight | ring 1024, a full ring waits for the GPU (`mtl_sub_commit`) |
| The first-launch crash | its log was overwritten by later launches; the same ring is the likely cause (first launch builds every pipeline, long frames) and is not proven | keep three logs next time if it recurs |
| Misfit picture | `DM_UE3_RES` changed in the in-game menu at 87 s (to game) and 150 s (to 2560x1440): ResX / ResY were rewritten under targets made at 1920x1080 | the setting applies at the next start only |
| Matches at 35 to 55 fps with the GPU at 12 to 14 ms | the replay thread's clock: 3.35 GHz at the menu, 1.1 to 1.9 GHz in the match, same cycles a frame (optimizations.md item 14) | real-time declaration for the replay thread (`DM_NQ_RT`) |
| Pass merging works on a real GPU | switched on at 99 s: 87 → 76 passes, 38 → 51 fps | now the default |
| A match frame at 1080p: 300 to 440 draws, 65 to 116 passes, 1.8 to 3.4 GB of attachments loaded and stored | perf CSV; this build is from before the inline fences (item 11) | the next build has item 11 |
| Performance export had 0 MiB and 0 x 0 in every row | the iOS front end never started the memory watcher; the recorder called its own weak fallbacks | watcher started in `main.m`; weak references in `hle_perf.c` |
| 12 tiny mip levels (2x2, 1x1) of BC textures not in the ASTC pack | `[RED] BC texture level not in the ASTC pack`, drawn as flat blocks | harmless at that size; open |
| Monitor: 60 Hz only | display modes list, `phone_refresh_hz 60` | the 120 fps path is not needed for this monitor over this cable |

Not measured on the device yet: the inline fences (item 11), the replay thread's deadline (item 14), native 1440p from the start of the game.

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
