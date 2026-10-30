# Nest game packages, format 1

Nest games are self-contained ZIPs with `game.json` at the ZIP root. Shell
launchers, RetroPie paths, and an outer `game/` wrapper are no longer used.

## Python package

```text
drop-grid.zip
├── game.json
├── main.py
├── icon.png          (optional)
└── assets/
    ├── music.ogg
    └── sprites.png
```

Use this `game.json`:

```json
{
  "format_version": 1,
  "runtime": "python3",
  "entrypoint": "main.py",
  "args": []
}
```

`format_version`, `runtime`, and `entrypoint` are required. `args` is optional
and defaults to an empty array. Arguments are strings passed directly to the
process; spaces, quotes, braces, and shell characters remain literal. There is
no shell expansion. At most 64 arguments are allowed, each at most 4096 bytes.

Entrypoints must name an existing regular file inside the package. Subfolders
are allowed, for example `src/main.py`, but the working directory is always the
package root. Absolute paths, parent traversal (`..`), symbolic links, hard
links, and unsupported archive file types are rejected. A ZIP nested under
`game/` or a title folder is rejected because its root has no `game.json`.

GameBird runs Python packages as `python3 -u <entrypoint> <args...>`. Python 3
and any modules the game imports (such as Pygame) must already be available on
the device. Nest does not execute dependency installation scripts or run pip.

Keep the title, store version, screenshots, and download information in the
existing store API metadata; they do not need to be repeated in `game.json`.

## Assets and saves

Each game installs into `data/nest/games/<store-slug>/` by default. GameBird
sets this folder as the working directory, so `assets/music.ogg` works without
an absolute device path.

GameBird provides these environment variables:

| Variable | Meaning |
| --- | --- |
| `GAMEBIRD_GAME_DIR` | Absolute installed package directory; also the working directory. |
| `GAMEBIRD_GAME_ID` | The store slug used for this installation. |
| `GAMEBIRD_SAVE_DIR` | Absolute, writable directory for this game's persistent data. |

**Games must write saves and settings to `GAMEBIRD_SAVE_DIR`.** For example:

```python
import json
import os
from pathlib import Path

save_dir = Path(os.environ["GAMEBIRD_SAVE_DIR"])
save_file = save_dir / "progress.json"
progress = json.loads(save_file.read_text()) if save_file.exists() else {}
progress["high_score"] = 100
temporary = save_file.with_suffix(".tmp")
temporary.write_text(json.dumps(progress))
temporary.replace(save_file)
```

The save directory is created before launch and defaults to
`data/nest/saves/<store-slug>/`. Updating, uninstalling, and reinstalling the
same slug preserve it. Files written into the installed package directory are
replaced with the package and must not be used for persistent progress. Old
games' save files are not automatically moved to the new location. Keep the
store slug stable across releases to retain the same save location.

## Native executables

Use `"runtime": "native"` and set `entrypoint` to the packaged executable,
for example `bin/my-game`. Nest marks that entrypoint executable during
installation, even if the ZIP did not preserve its executable permission.
The executable and its shared libraries must match the device's CPU and OS.
The working directory, arguments, saves, and logs follow the same rules as
Python games.

## Build and publish a ZIP

1. Remove the old top-level `.sh` launcher. Put `game.json`, the entrypoint,
   and assets directly at the ZIP root. Existing source subfolders can stay
   if `entrypoint` names the correct relative file.
2. Update any hardcoded `/home/pi/...` or `/home/gamebird/...` asset paths to
   relative paths, and move persistent writes to `GAMEBIRD_SAVE_DIR`.
3. From the folder containing `game.json`, build the archive. For example:

   ```sh
   zip -r ../drop-grid-2.0.zip game.json main.py assets icon.png
   ```

   Omit `icon.png` if the package has no icon. Do not ZIP the containing folder.
4. Calculate the final ZIP's byte count and SHA-256:

   ```sh
   wc -c < ../drop-grid-2.0.zip
   sha256sum ../drop-grid-2.0.zip
   ```

5. Upload that ZIP to Cloudflare. Update the store's download path,
   `download.size_bytes`, and `download.sha256` to match the uploaded bytes.
   Bump the catalog `version` and `download.version` together so existing
   installations see the update. Prefer a versioned download path to avoid
   stale CDN copies. No Worker API schema change is required.

Nest rejects incomplete metadata, size/hash mismatches, and invalid manifests
before changing an installed game. Updates stage and validate a complete new
package, then replace that game's folder; cancellation or failure restores the
previous package and registration.

## Library and diagnostics

Nest creates a `.gbgame` data record in the configured `apps` library. It
contains the title and local package/save/log locations. GameBird reads this
record and executes the manifest entrypoint directly; it is not a shell
script and does not belong in the downloadable ZIP. Package helpers and binary
assets are outside scanned library roots, so each game appears once.

Each launch replaces `data/nest/logs/<store-slug>.log` with that run's stdout
and stderr. Missing runtimes and entrypoints are reported to the shell before
launch; nonzero exits or signals report a failure and retain the game's log.
Native game saves are controlled by the game itself; GameBird's emulator
resume-state feature does not apply to Nest packages.

All `data/nest/...` locations follow `data_dir` from the existing Nest config;
`games_dir` and `launchers_dir` can still be configured separately. Existing
script-only installations must be replaced with format 1 packages. Their
metadata can be retained for updates, but Nest no longer generates or runs
their old shell launchers. Old payload files are left untouched.
