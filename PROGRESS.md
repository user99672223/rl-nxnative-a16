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
