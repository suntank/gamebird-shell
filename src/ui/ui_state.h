#pragma once

#include <cstdint>
#include <string>

#include "platform/platform.h"

namespace gb::ui {

enum class Screen {
  Home,
  Systems,
  GameList,
  Details,
  Settings,
  ScrapeProgress,
  Tools,
  InputSetup,
  InputTest,
  Bluetooth,
  Wifi,
  LaunchOptions,
};

struct UIState {
  // The console carousel is the shell's home surface. The Home screen is
  // the compact Start menu, opened and closed with Start.
  Screen screen = Screen::Systems;
  int home_selected = 0;
  std::string list_context = "Recent";
  bool running = true;
  bool needs_redraw = true;
  bool show_diagnostics = true;
  std::uint64_t frame_count = 0;
};

void HandleButton(UIState& state, platform::Button button);
const char* ScreenName(Screen screen);

}  // namespace gb::ui
