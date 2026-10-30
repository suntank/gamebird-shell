#pragma once

#include <cstdint>
#include <string>

#include "platform/platform.h"

namespace gb::ui {

enum class Screen {
  Home,
  Nest,
  Systems,
  GameList,
  GameMenu,
  SystemMenu,
  Details,
  Settings,
  ScrapeProgress,
  Tools,
  InputSetup,
  InputTest,
  Bluetooth,
  Wifi,
  Update,
  LaunchOptions,
};

enum class HomeItem { Nest, Continue, Browse, Recent, Favorites, Tools, Settings, Count };
constexpr int HomeIndex(HomeItem item) { return static_cast<int>(item); }
constexpr int kHomeItemCount = HomeIndex(HomeItem::Count);

struct UIState {
  Screen screen = Screen::Home;
  Screen menu_return_screen = Screen::Systems;
  std::string continue_title;
  bool continue_available = false;
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
