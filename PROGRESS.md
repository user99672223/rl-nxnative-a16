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

## Status

- Phase: 0 (survey) nearly complete; Phase 1 (boot on Mac in CI) started.

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

## Done

- Read nxnative README, AGENTS.md, docs/agents 01–03, 06.
- Cloned nxnative, sanitised workflows, extended `.gitignore`, generated age keypair.
- Repo `user99672223/rl-nxnative-a16` (public), `upstream` remote = seedds/nxnative.
- Encrypted inputs uploaded to draft release `inputs-v0` (2 NSP parts + keys + SHA256SUMS).
- Local tools in the work folder: hactool, venv (lz4, capstone), Ghidra 12.2 (needs JDK 25: `/usr/lib/jvm/java-25-openjdk-amd64`),
  flat images `elf/main.bin`, `elf/sdk.bin`, disassembler `tools/rldis.py <module> <hex> <count>`.
- CI: `.github/workflows/rl-build.yml` (manual dispatch) → `scripts/fetch_artifact.sh <run id>` decrypts results to `../RL/ci/<run>/`.

## Open problems / risks

- nxnative's build (Metal shader compiler, xcodebuild) only runs on macOS → everything past the
  static survey has to run on Actions runners from the encrypted NSP.
- Upstream docs say GitHub's Macs have no Metal 4; whether the host can render there is unknown.
- Local disk: 29 GB free at start.

## Next step

- Read the first CI build's encrypted log (survey + provisional profile from `build_ipa_from_nsp.py --platform mac`),
  move the profile into `dm/games/01005EE0036EC000/`, iterate on boot.
