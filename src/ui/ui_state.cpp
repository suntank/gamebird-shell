#include "ui/ui_state.h"

namespace gb::ui {

namespace {

constexpr int kHomeItemCount = 5;

}  // namespace

void HandleButton(UIState& state, const platform::Button button) {
  switch (button) {
    case platform::Button::Start:
      state.screen = state.screen == Screen::Home ? Screen::Systems : Screen::Home;
      state.needs_redraw = true;
      return;
    case platform::Button::Select:
      state.screen = Screen::Systems;
      state.needs_redraw = true;
      return;
    default:
      break;
  }

  if (state.screen == Screen::Home) {
    if (button == platform::Button::Up) {
      state.home_selected = (state.home_selected + kHomeItemCount - 1) % kHomeItemCount;
      state.needs_redraw = true;
      return;
    }
    if (button == platform::Button::Down) {
      state.home_selected = (state.home_selected + 1) % kHomeItemCount;
      state.needs_redraw = true;
      return;
    }

    if (button == platform::Button::A) {
      switch (state.home_selected) {
        case 0:
          state.screen = Screen::GameList;
          state.list_context = "Recent";
          break;
        case 1:
          state.screen = Screen::GameList;
          state.list_context = "Favorites";
          break;
        case 2:
          state.screen = Screen::Tools;
          break;
        case 3:
          state.screen = Screen::Settings;
          break;
        case 4:
          state.screen = Screen::Systems;
          break;
        default:
          state.screen = Screen::Home;
          break;
      }
      state.needs_redraw = true;
      return;
    }
  } else {
    if (button == platform::Button::B) {
      if (state.screen != Screen::Systems) {
        state.screen = Screen::Home;
      }
      state.needs_redraw = true;
      return;
    }
    if (button == platform::Button::A && state.screen == Screen::GameList) {
      state.screen = Screen::Details;
      state.needs_redraw = true;
      return;
    }
  }

  // Exiting the shell is deliberately available from Tools, so B never
  // terminates the shell from the Start menu.
}

const char* ScreenName(const Screen screen) {
  switch (screen) {
    case Screen::Home:
      return "HOME";
    case Screen::Systems:
      return "SYSTEMS";
    case Screen::GameList:
      return "GAMES";
    case Screen::Details:
      return "DETAILS";
    case Screen::Settings:
      return "SETTINGS";
    case Screen::ScrapeProgress:
      return "SCRAPING";
    case Screen::Tools:
      return "TOOLS";
    case Screen::InputSetup:
      return "INPUT";
    case Screen::InputTest:
      return "INPUT TEST";
    case Screen::Bluetooth:
      return "BT";
    case Screen::Wifi:
      return "WIFI";
    case Screen::LaunchOptions:
      return "LAUNCH";
  }
  return "UNKNOWN";
}

}  // namespace gb::ui
