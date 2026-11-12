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

- Phase: 0 (survey) — in progress.

## Done

- Read nxnative README, AGENTS.md, docs/agents 01–03, 06.
- Cloned nxnative, sanitised workflows, extended `.gitignore`, generated age keypair.

## Open problems / risks

- nxnative's build (Metal shader compiler, xcodebuild) only runs on macOS → everything past the
  static survey has to run on Actions runners from the encrypted NSP.
- Upstream docs say GitHub's Macs have no Metal 4; whether the host can render there is unknown.
- Local disk: 29 GB free at start.

## Next step

- Create the GitHub repo, push; extract the NSP locally; write the survey.
