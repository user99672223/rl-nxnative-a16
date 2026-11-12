# Rocket League (`01005EE0036EC000`)

Base game 1.0.0 (v0), no update. SDK 3.5.0. Psyonix's game on Panic Button's Unreal Engine 3 port (package version 868, licensee 20) on NVN, with Scaleform for the menus, PhysX 2 and Wwise. Modules `main` (build `becfed979bb4204fbd0945cd450fb296`) and `sdk` (`0ac561f7b400007b97516d024c903a14`). **Bring-up in progress**: the port's state, decisions and measurements are in [PROGRESS.md](../../../PROGRESS.md) at the repository root; this note holds what is settled.

This profile was brought up without a Mac at hand: every build and run happened on GitHub's virtual Macs, whose GPU has no Metal 4, so the game has only run on the null GPU there ([`nvn_null.h`](../../gfx/nvn/nvn_null.h)): booted, driven and counted, never drawn. Nothing below claims a picture.

## What it needs

| Setting | Where | Why |
|---|---|---|
| The rewriter's offset-table form with the check further back | `dm/tools/patch_nso.py` `jump_offsets(back=32)` | `main+0xbe78f4`: eight loads between the table load and the `br x18`, so the bounds check is 17 to 19 instructions back |
| The shader record form of SDK 3 and chunks from the header size | `dm/gfx/tools/shader_split.py` `ue3_shaders`, `ue3_body`; `dm/games/profile_guess.py` | a record is code length, code area, control length, control section; the packages' chunks start at the summary's header size, past the first 4 KB |
| Offline cuts under SDK 3's names | `dm/host/hle_net.c`, `hle_offline.c` | `nn::socket::GetHostByName`, `Ioctl`, `nn::ssl::Connection::Write(char const*, int*, unsigned)`, `nn::nifm::HandleNetworkRequestResult`, `nn::ldn::GetNetworkInfo` with the update array and the rest of this SDK's network imports |
| `ue3_settings on` | [`game.tsv`](game.tsv), `dm/host/hle_ue3set.c` | the engine's system settings (render scale, dynamic resolution, post-process switches) held from the host; see below |

## Files and shaders

- RomFS: 1,871 files, 3.5 GB, all under `tagame/cookednnx`: 1,572 cooked packages (`*.xxx`, 2.47 GB), six texture caches (`textures*.tfc`, 811 MB), Wwise banks and streams (387 MB), the global shader cache, the config (`coalesced_<language>.bin`, encrypted).
- A package: the summary, then the name, import and export tables AES-256-ECB up to the header size, then Oodle chunks (first block `8C 0A`, a few tiny ones `8C 02`); after the tables a chunk table {uncompressed offset, uncompressed size, compressed offset, compressed size}.
- 66,362 shaders (32,646 vertex, 33,716 pixel), all translated and compiled for the Mac with no failure (GitHub's runner, 48 minutes on 3 cores): 1,738 in `globalshadercache-nnx.bin`, the rest inline in the packages.
- Textures: 36,710 unique levels: DXT1 0.98 GB, DXT5 1.38 GB (5,094 levels of DXT5 normal maps with x and y in alpha and green), A8R8G8B8 0.94 GB, a little G8 and V8U8; no BC4 to BC7. A level is stored block-linear as a one-level texture of its size.

## The engine's settings

`main+0x2a4da08` holds Unreal's system settings table: 101 rows of {type, flags, name, address of the variable, range, help}, the texts UTF-32. `hle_ue3set.c` finds it by its shape in any build and holds chosen values (`DM_UE3_SET`, the launcher's Render scale and Dynamic resolution). This port's own rows: `UpscaleScreenPercentage`, `UpscaleTargetFramerateDocked` / `Undocked`, `MinimumScreenScale`, `AllowDynamicResolution`.

- The dynamic resolution controller is `main+0x417f80`: it compares the target frame time with a measured one, steps an index through a table of sizes and switches `nn::oe::SetPerformanceConfiguration`.
- The frame limiter is Unreal's `appUpdateTimeAndHandleMaxTickRate` at `main+0xae6154`: `GetMaxTickRate` (vtable `+0x290` of the engine object), then a sleep and a spin on the system tick.

## Open

- Never drawn: no Metal 4 GPU has run it yet.
- Memory keys are the packing script's guesses.
