# Tools

Helper scripts can be added here for:

- library import helpers
- cache maintenance
- diagnostics export

## Local menu previews

Render representative menus at the native 240×240 resolution without the Pi:

```sh
c++ -std=c++20 -Isrc tools/ui_preview.cpp src/ui/screens/*.cpp \
  src/ui/widgets/list.cpp src/ui/widgets/text.cpp src/render/surface_240.cpp \
  src/render/theme.cpp src/render/image_cache.cpp -lpng -o /tmp/gamebird-ui-preview
/tmp/gamebird-ui-preview /tmp/gamebird-ui
```

The output contains PPM images for browsing, settings, tools, details, progress,
and all Wi-Fi views. Fixtures use sample data and do not change saved settings.

Menus share `src/ui/widgets/chrome.h`: 16-pixel content margins, a compact
header, a separated control footer, and a common progress bar. Lists use mint
selection highlights, vertically centered labels, and a scroll-position marker.

To render the new Continue, game menu, and system menu using the actual shell
router and an isolated fixture catalog, build with tests enabled and run:

```sh
./build/shell_flow_test /tmp/gamebird-play-previews
```

This also checks navigation and selection restoration; it does not launch games
or access the Pi. `play_session_test` covers save promotion and backups, while
`gblaunch_session_test` exercises the real helper with a simulated RetroArch
process (requires Python 3).
