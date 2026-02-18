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
- SQLite catalog with schema init + migration (`games.is_present`)
- Incremental scanner for configured roots and system definitions
- `gblibd` scan runner CLI
- Systems picker loaded from SQLite
- Per-system game list loaded from SQLite
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

Pi framebuffer mode:

```bash
sudo ./build/gbshell --presenter fbdev --fbdev /dev/fb1 --input-evdev auto
```

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

Milestone 6 browse + launch flow:

1. Run `gblibd` scan.
2. Launch `gbshell`.
3. Home -> `Systems` -> choose a system -> browse games.
4. In game list use `X` to favorite and `Y` to hide/unhide.

`gblaunch` can be called directly:

```bash
./build/gblaunch --db ./data/catalog.db --game-id 1
```

Dry-run (show expanded argv only):

```bash
./build/gblaunch --db ./data/catalog.db --game-id 1 --dry-run
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
