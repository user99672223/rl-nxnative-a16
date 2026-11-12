# First run on the iPhone 15: what to do and what to send back

This build has never drawn a frame on a real GPU. The point of the first run is to learn what happens, not to play. Everything below takes about ten minutes.

## Install

1. `scripts/fetch_ipa.sh <run id>` on the laptop: downloads and decrypts the unsigned IPA into `../RL/ipa/`.
2. Sign and sideload it with your usual tool. Turn on **Increased Memory Limit** if the tool offers it.
3. The app needs about 6 GB of storage on the phone.

## First launch (phone only, no monitor)

Start the app, press Play, wait up to a minute, then note:

1. Does a picture appear at all? (Logos, then a title screen with "Press any button to start".)
2. Do textures look right, or are there black, white or scrambled surfaces?
3. Does a button on the DualSense (or a tap, if the touch controls are shown) get past the title screen? On the CI machines it does not, and the cause is not known yet.
4. Does it crash or freeze? If so, roughly when.

The first time each thing appears there will be stutter: the phone builds its GPU pipelines on first use and keeps them for later launches.

## Send back the log

The app writes everything it does to a text file, and keeps the previous launch's as well:

- Files app → On My iPhone → Rocket League → `log.txt` (this launch) and `log.prev.txt` (the one before).

Share both (AirDrop, mail, any way that gets them to the laptop) and put them in `../RL/device/`. If it crashed, launch once more before sharing only if you also send `log.prev.txt`, since that is then the crash's log.

## Performance log

In the game: open the menu → Diagnostics. The panel shows frame rate, frame times, memory against the limit, heat state, render size and the display's size and refresh rate.

- **Export performance log** writes a CSV and a JSON of every frame since launch with the settings in force; the share sheet opens. Send both files.
- Worth doing once at the title screen and, if you get into a match, once after a minute of play.

## With the monitor

Use a USB-C cable or adapter that carries DisplayPort (the cable in the iPhone box does not). Then:

1. Note what the panel's "Shown at" line says: the size and the refresh rate the phone drives your monitor at. This answers whether an iPhone 15 goes above 60 Hz at 1440p.
2. In the launcher's settings for the game, before Play:
   - **Monitor output**: 1440p.
   - **Render resolution**: leave "Game's own (720p)" for the first try, then 1080p, then 1440p. It takes effect at the next start of the game.
   - **Render pass merging**: "Game's own" first, then "Merged". Merged is expected to be faster and has never been seen on a GPU: say if the picture differs.
3. Export a performance log for each combination you try.

## Switches without a rebuild

A file `env.txt` in the app's folder (Files app, same place as `log.txt`), one `NAME=value` a line, is read at launch. Useful ones:

| Line | Effect |
|---|---|
| `DM_UE3_DUMP=1` | logs every engine setting and its value |
| `DM_UE3_RES=2560x1440` | renders natively at 1440p |
| `DM_UE3_KICK=merge` | render pass merging on |
| `DM_UE3_SMOOTH=off` | no engine frame-rate limit |
| `DM_NVN_FENCE_CUT=1` | the old behaviour for the UI's fences (to compare) |
| `NX_MEMORY_BUDGET_MB=4000` | ends the game with a clear log line if it passes 4,000 MiB |

Delete the file to go back to the defaults.
