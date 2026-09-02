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
  Tools,
  InputSetup,
  InputTest,
  Bluetooth,
  LaunchOptions,
};

struct UIState {
  Screen screen = Screen::Home;
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
