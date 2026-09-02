# GameBird Shell

GameBird Shell is a 240x240 launcher UI for Raspberry Pi handheld builds, with Ubuntu-first development ergonomics and Pi-oriented runtime targets.

## Current status

This repository now includes Milestone 7 foundations:

- Fixed internal `240x240` render surface (`RGB565`)
- SDL2 presenter for Ubuntu development
- FBDEV presenter for deterministic `/dev/fb*` output
- EVDEV input backend with basic auto-detection of gamepad event device
- Screen router with Home + placeholder screens
- Gamepad and keyboard navigation
- SQLite catalog with schema init + migrations (`games.is_present`, root health)
- Root-aware incremental scanner that preserves games when storage is unavailable
- `gblibd` scan runner CLI
- Systems picker loaded from SQLite
- Per-system game list loaded from SQLite
- Recent and Favorites library views backed by launch/favorite state
- Game Details screen backed by system and local metadata
- Favorite/hidden toggles persisted to SQLite (`X`/`Y`)
- `gblaunch` helper with template expansion and execve-style launch
- `A` launch from game list with return-to-UI lifecycle
- `last_played` timestamp update after launched process exits
- Background job queue in SQLite (`jobs` table + status transitions)
- `gblibd` worker modes: `--jobs-once` and `--daemon`
- Implemented job types: `scan`, `identify`, plus placeholder `scrape/download_art/build_thumb`
- Local heuristic metadata provider populating `game_metadata`
- Runtime settings persistence (`show_diagnostics`, `show_hidden_games`)
- Interactive Settings screen with save support
- Interactive Tools screen actions:
  - Rescan library
  - Identify metadata
  - Rebuild thumbnails (placeholder pipeline job)
  - Run queued jobs
  - Export diagnostics text report

## Build (Ubuntu)

```bash
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev
cmake -S . -B build -G "Unix Makefiles"
cmake --build build -j
```

## Run

```bash
./build/gbshell --presenter sdl --db ./data/catalog.db --defaults ./config/defaults.json --systems-dir ./config/systems.d --settings ./config/user_settings.json --scale 3
```

Controls:

- D-pad / Arrow keys: navigate
- `A` / `Z`: select
- `B` / `X`: back
- `A` on game list: launch selected item
- `X` / `A`: toggle favorite (in game list)
- `Y` / `S`: toggle hidden (in game list)
- `Start` / Enter: toggle diagnostics
- `Select` / Backspace: return Home
- `Esc`: quit
- Settings screen: `A` toggles/saves selected option
- Tools screen: `A` runs selected action
- Input Setup: `A` starts remapping, `Y` opens the live input test, and `X`
  clears the active profile
- Live Input Test: press each control to mark it; `Select` returns

Pi framebuffer mode:

```bash
sudo ./build/gbshell --presenter fbdev --fbdev /dev/fb1 --input-evdev auto
```

`config/retroarch-gamebird.cfg` enables threaded video intentionally. On the
GameBird's SPI-connected 240x240 panel, scanout is slower than the SNES refresh
rate; keeping presentation on a video thread prevents panel transfers from
slowing the emulation and audio run loop.

Or pin input device explicitly:

```bash
sudo ./build/gbshell --presenter fbdev --fbdev /dev/fb1 --input-evdev /dev/input/event2
```

Run a library scan:

```bash
./build/gblibd --db ./data/catalog.db --defaults ./config/defaults.json --systems-dir ./config/systems.d
```

Queue and process milestone-6 background jobs once:

```bash
./build/gblibd --jobs-once --enqueue-default --db ./data/catalog.db --defaults ./config/defaults.json --systems-dir ./config/systems.d
```

Run persistent daemon worker loop:

```bash
./build/gblibd --daemon --db ./data/catalog.db --defaults ./config/defaults.json --systems-dir ./config/systems.d
```

`config/defaults.json` is Ubuntu-dev oriented (`./roms`, `./apps`).
For Pi deployment, use `config/defaults.pi.json` or set explicit roots.

Override roots from CLI:

```bash
./build/gblibd --db ./data/catalog.db --systems-dir ./config/systems.d --root /roms --root /apps
```

Each configured root has persistent health (`ok`, `unavailable`, or `error`) and
a storage-device identity. Only a complete scan of a healthy root can mark ROMs
from that root missing. If a drive is unplugged, a mount is absent, permissions
prevent a complete traversal, or a known mount point unexpectedly becomes
empty on another device, existing catalog entries remain available and the
condition is reported as storage offline. A ROM absent from a successfully
scanned root is treated as deleted. Root health appears beside Rescan Library,
in scan logs, and in exported diagnostics.

Milestone 6 browse + launch flow:

1. Run `gblibd` scan.
2. Launch `gbshell`.
3. Home -> `Systems` -> choose a system -> browse games.
4. In game list use `X` to favorite and `Y` to hide/unhide.
5. Home -> `Recent` and `Favorites` are live filtered views. In any game list,
   press `R` for Details; Details uses `A` to launch, `X` to favorite, and `Y`
   to hide.

## Local artwork

GameBird Shell supports offline PNG box art without requiring a scraping account.
Place a cover in either of these locations, then choose **Tools → Refresh
Artwork** (or run the command below):

```text
data/artwork/<system-id>/<rom-file-stem>.png
<same-directory-as-rom>/<rom-file-stem>.png
```

For example, Super Metroid can use
`data/artwork/snes/Super Metroid (JU) [!].png`. Details will render the cover
inside its 60×60 artwork panel, aspect-fit it, and keep a small RGB565 cache so
the SPI display is not repeatedly decoding files. Missing, malformed, or very
large images safely fall back to `NO ART`.

To use a different artwork root from the command line:

```bash
./build/gblibd --jobs-once --enqueue-artwork --artwork-dir /roms/artwork \
  --db ./data/catalog.db --defaults ./config/defaults.json --systems-dir ./config/systems.d
```

`gblaunch` can be called directly:

```bash
./build/gblaunch --db ./data/catalog.db --game-id 1
```

Dry-run (show expanded argv only):

```bash
./build/gblaunch --db ./data/catalog.db --game-id 1 --dry-run
```

Show the effective core, its source, and appended configuration:

```bash
./build/gblaunch --db ./data/catalog.db --game-id 1 --show-effective --dry-run
```

Validate every present game without launching anything:

```bash
./build/gblaunch --db ./data/catalog.db --validate-all
```

Validation checks the executable, ROM/app, libretro core, appended config files,
unresolved template variables, and orphaned database overrides. A core override
that differs from the system definition is reported as a warning so an old but
otherwise valid override cannot silently change emulators again. Missing launch
files are errors and produce a non-zero exit status.
Normal launches run the same checks, log the effective configuration, and stop
before starting RetroArch when required launch files are invalid.

The launch template in each `config/systems.d/*.json` file is the authoritative
system default. In the shell, press `Y` on a system or `Start` on a game to open
Launch Options; the bottom of that screen shows the currently effective core,
whether it came from the definition/system/game override, and the active config.

Input profiles created in GameBird Shell are also exported to
`config/retroarch-input-gamebird.cfg`. RetroArch loads this generated file after
the base GameBird config, so the same keyboard, button, hat, and axis mappings
are used in games. The shell discovers the active Linux joystick index at
runtime, releases device grabs while RetroArch runs, and reacquires or
rediscovers controllers when returning. In `auto` evdev mode, USB and Bluetooth
gamepads are rescanned periodically, so disconnecting and reconnecting a pad no
longer exits the shell.

Run the automated tests with `ctest --test-dir build --output-on-failure`. On a
Linux device with `/dev/uinput`, the unplug/replug path also has a root-only
integration probe:

```bash
sudo ./build/evdev_hotplug_integration
```

Milestone 7 settings and tools:

1. Open `Settings` from Home.
2. Toggle runtime options (`show diagnostics`, `show hidden games`).
3. Save settings to `--settings` path.
4. Open `Tools` from Home to run maintenance actions.
5. Diagnostics export writes to `--diagnostics-dir` (default `./data`).

## Planned next milestones

- Network scraping provider integration and image pipeline hardening
- Thumbnail/image cache execution path in UI
