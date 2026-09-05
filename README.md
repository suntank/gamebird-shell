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
- Implemented job types: `scan`, `identify`, `scrape`, `download_art`, and `build_thumb`
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
sudo apt-get install -y build-essential cmake pkg-config libsdl2-dev libpng-dev
cmake -S . -B build -G "Unix Makefiles"
cmake --build build -j
```

## Run

```bash
./build/gbshell --presenter sdl --db ./data/catalog.db --defaults ./config/defaults.json --systems-dir ./config/systems.d --settings ./config/user_settings.json --scale 3
```

Controls:

- D-pad / Arrow keys: navigate
- `A` / `Z`: open the selected item or choose a menu action
- `B` / `X`: return to the previous screen
- `Start` / Enter: open or close the contextual menu (game menu while browsing
  games, system menu on the console carousel, main menu from settings/tools)
- `Select` / Backspace: shortcut to the console carousel
- On the handheld, hold physical Start for about one second to show
  battery/volume status, then use D-pad Up/Down for volume
- `Esc`: quit
- Settings screen: `A` toggles/saves the selected option
- Tools screen: `A` runs the selected action
- Input Setup: `A` starts remapping, `Y` opens the live input test, and `X`
  clears the active profile
- Live Input Test: press each control to mark it; `Select` returns (B is tested
  as an input here)

### Continue playing

The shell opens on Home, with the last visible, present game featured above
**Continue playing**. Press A to open its game menu, then A again to Resume
when a matching Continue save exists. First-time games offer **Play**. Games
with a Continue save also offer **Start fresh**, which boots normally without
loading the automatic state; ordinary in-game battery/memory-card saves are
not erased.

On any game, A or Start opens the same menu: Play/Resume, Start fresh,
Resume previous save (when available), Game details, Add/remove favorite,
Hide/unhide game, Launch options, and Main menu. B returns to the screen that
opened the menu. The old X/Y/R shortcuts for game actions are replaced by
these labeled actions.

System and game selection, plus per-system/Recent/Favorites game positions,
are stored by catalog ID in `browse-state` next to the catalog database.
**Browse games** returns to the last browsing view. Background rescans retain
selection when titles are inserted or reordered. Navigation persistence is
separate from settings, so it does not save uncommitted settings edits.

RetroArch launches use a private Continue state location under
`continue/` next to the catalog database. On a normal emulator exit, supported
cores write an automatic state. The helper promotes a newly written, nonempty
state only after a successful exit and retains the previous state as a backup.
The game menu confirms **Progress saved - ready to resume** after promotion;
unsupported cores or unsuccessful saves report that no new Resume save was
created. A failed launch or crash preserves the existing Continue states.

Continue states are tied to the ROM and core file identities; replacing a ROM
or updating/changing a core makes old Continue states unavailable rather than
loading them into a potentially incompatible core. Existing states are kept
on disk. The session does not set `savefile_directory` or erase SRAM/memory-card saves.
It owns the automatic-state directory and disables RetroArch's additional
per-core/game `.cfg` auto-overrides so they cannot redirect these states;
use the shell's **Launch options** for core and configuration overrides.
Numbered manual state slots created during managed sessions are retained
separately under `manual/`; pre-existing states in other RetroArch directories
are not imported. This provides
save-and-resume on deliberate exit, not suspend-to-RAM or protection against
cutting power during gameplay.

For helper use: `gblaunch --db <catalog> --game-id <id> --resume`,
`--resume-backup`, or `--fresh` (the default). `--dry-run` validates and shows
the base launch command without creating a play session or changing saves.

Pi framebuffer mode:

```bash
sudo ./build/gbshell --presenter fbdev --fbdev /dev/fb1 --input-evdev auto
```

`config/retroarch-gamebird.cfg` enables threaded video intentionally. On the
GameBird's SPI-connected 240x240 panel, scanout is slower than the SNES refresh
rate; keeping presentation on a video thread prevents panel transfers from
slowing the emulation and audio run loop.

While a RetroArch game is running, hold **Select** and press **Start** to exit
back to GameBird Shell and create a Continue save when the core supports it.

Or pin input device explicitly:

```bash
sudo ./build/gbshell --presenter fbdev --fbdev /dev/fb1 --input-evdev /dev/input/event2
```

## Waveshare GamePi13 controls

The handheld's twelve active-low GPIO switches are described by
`hardware/gamepi13/gamebird-controls-overlay.dts`. The overlay uses Linux's
in-tree `gpio-keys` driver and exposes one input device named
`GameBird Controls`; no `mk_arcade_joystick_rpi` module or background polling
service is required. Its BCM pin map follows the GamePi13 board specification:

```text
Up 5    Down 6    Left 16   Right 13
A 21    B 20      X 15      Y 12
L 23    R 14      Start 26  Select 19
```

GPIO 18 remains dedicated to PWM audio, GPIO 25/27 to display DC/reset, and
SPI0 to the display. Install the overlay on the Pi with:

```bash
sudo ./scripts/install-gamepi13-controls.sh
sudo reboot
```

The installer temporarily remounts the protected firmware partition writable,
compiles and validates the overlay, enables it in `config.txt`, installs the
matching RetroArch controller profile, syncs the changes, and restores the
read-only firmware mount before returning.

The SPI panel is a permanent appliance display rather than a desktop seat
device. Install the matching access rule so opening and closing SSH sessions
cannot cause logind to revoke RetroArch's access to the panel DRM device:

```bash
sudo ./scripts/install-panel-access-rule.sh
```

## Battery status (INA219)

The framebuffer shell reads the existing INA219 battery monitor directly over
I²C; no Python overlay process is required. Hold the physical `Start` button
for about 0.85 seconds to show voltage, charge/discharge state, a battery
gauge, and the current volume. While holding Start, use D-pad Up/Down to
change the ALSA PCM volume in 5% steps. Releasing Start hides the held panel.
When charging begins, the battery gauge appears briefly; at 20% or lower it is
red, and below 5% it flashes until charging resumes. The default is the
original monitor address `0x43` on `/dev/i2c-1`:

```bash
./build/gbshell --presenter fbdev --battery-i2c /dev/i2c-1 --battery-address 0x43
```

When the monitor or battery is disconnected, the hold panel displays `BAT N/A`
and the launcher continues normally. It performs no automatic shutdown or
battery-management write. Use `--battery-disabled` to turn the probe off
entirely.

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

Browse and launch flow:

1. Run `gblibd` to scan the library.
2. Open **Browse games** from Home and select a system.
3. Choose a game with A, then select Play or Resume.
4. Use the game menu for details, favorites, hiding, or launch options.
5. Recent and Favorites use the same game menu and navigation as system browsing.

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

The Systems screen is a left/right console carousel. Press `A` to enter its
cover browser, use up/down to move through games, and use left/right to switch
consoles without leaving the cover view. The selected game's cover fills the
upper portion of the display, with the previous/current/next titles below it.
Optional console artwork uses:

```text
data/system-art/<system-id>-icon.png
data/system-art/<system-id>-logo.png
```

Both are aspect-fitted PNGs. If either file is absent, the shell draws a built-in
console icon or the system name, so console browsing remains fully usable
offline.

### Scraping box art and metadata

Open **Settings**, leave **Scraper: LIBRETRO** selected, and choose **Scrape
Library Now**. The account-free provider searches Libretro's thumbnail catalog,
normalizes common dump tags such as `(JU) [!]`, and downloads only confident
matches. It also imports available release year, genre, developer, publisher,
and player-count fields from the matching Libretro database record. Downloads
are written to the persistent `data/artwork/<system-id>/` tree and indexed in
the catalog. Existing artwork is preserved unless **Replace Existing Art** is
enabled.

The first pass supports SNES, NES, Game Boy, Game Boy Color, Game Boy Advance,
Mega Drive/Genesis, Master System, Game Gear, Nintendo 64, PlayStation, and
Atari 2600 system IDs. Unsupported systems and executable apps are skipped.

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
system default. In the shell, press Start on a system or game and select
**Launch options**; the bottom of that screen shows the currently effective core,
whether it came from the definition/system/game override, and the active config.

Input profiles created in GameBird Shell are also exported to
`config/retroarch-input-gamebird.cfg`. RetroArch loads this generated file after
the base GameBird config, so the same keyboard, button, hat, and axis mappings
are used in games. The shell discovers the active Linux joystick index at
runtime, releases device grabs while RetroArch runs, and reacquires or
rediscovers controllers when returning. In `auto` evdev mode, USB and Bluetooth
gamepads are rescanned periodically, so disconnecting and reconnecting a pad no
longer exits the shell.

The built-in GamePi13 controller also has a RetroArch auto-configuration profile
at `config/retroarch-autoconfig/linuxraw/GameBird Controls.cfg`. Install it in
`~/.config/retroarch/autoconfig/linuxraw/` on the handheld so RetroArch recognizes
the pad and does not fall back to an unconfigured-device warning.

Run the automated tests with `ctest --test-dir build --output-on-failure`. On a
Linux device with `/dev/uinput`, the unplug/replug path also has a root-only
integration probe:

```bash
sudo ./build/evdev_hotplug_integration
```

## System updater

Install the protected updater on the console with:

```bash
sudo ./scripts/install-updater.sh
```

The **Tools -> System Update** screen checks Raspberry Pi OS packages and the
`main` branch of `suntank/gamebird-shell`, shows separate availability and a
progress bar, and installs only after the player presses `A`. The checker runs
shortly after boot and every six hours. When it finds an update, the shell
briefly directs the player to the updater screen.

On an OverlayFS console, installation uses `overlayroot-chroot` to update the
protected lower root and binds the protected firmware and persistent `/data`
mounts into that environment. The live read-only arrangement is restored and
the console reboots after a successful install. Package lists, progress state,
and logs are kept under `/data/gamebird-update`; they are outside the anonymous
ROM/config Samba share. Keep power connected throughout an installation.

The GitHub repository must be anonymously readable for retail consoles to
discover and download shell builds. Do not ship a reusable private-repository
token in an image: a purchaser can recover it. A private project should instead
publish signed release artifacts through a public, read-only update channel.

Milestone 7 settings and tools:

1. Open `Settings` from Home.
2. Toggle runtime options (`show diagnostics`, `show hidden games`).
3. Save settings to `--settings` path.
4. Open `Tools` from Home to run maintenance actions.
5. Diagnostics export writes to `--diagnostics-dir` (default `./data`).

## Planned next milestones

- Additional account-backed scraping providers and manual match selection
- Thumbnail/image cache execution path in UI
