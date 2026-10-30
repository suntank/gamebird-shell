#include "ui/ui_state.h"

namespace gb::ui {

void HandleButton(UIState& state, const platform::Button button) {
  switch (button) {
    case platform::Button::Start:
      if (state.screen == Screen::GameList || state.screen == Screen::Details)
        state.screen = Screen::GameMenu;
      else if (state.screen == Screen::GameMenu) state.screen = Screen::GameList;
      else if (state.screen == Screen::Systems) state.screen = Screen::SystemMenu;
      else if (state.screen == Screen::SystemMenu) state.screen = Screen::Systems;
      else if (state.screen == Screen::Home) state.screen = state.menu_return_screen;
      else { state.menu_return_screen = state.screen; state.screen = Screen::Home; }
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
        case HomeIndex(HomeItem::Nest): state.screen = Screen::Nest; break;
        case HomeIndex(HomeItem::Continue): if (state.continue_available) state.screen = Screen::GameMenu; break;
        case HomeIndex(HomeItem::Browse): state.screen = Screen::Systems; break;
        case HomeIndex(HomeItem::Recent): state.screen = Screen::GameList; state.list_context = "Recent"; break;
        case HomeIndex(HomeItem::Favorites): state.screen = Screen::GameList; state.list_context = "Favorites"; break;
        case HomeIndex(HomeItem::Tools): state.screen = Screen::Tools; break;
        case HomeIndex(HomeItem::Settings): state.screen = Screen::Settings; break;
      }
      state.needs_redraw = true;
      return;
    }
    if (button == platform::Button::B) {
      state.screen = state.menu_return_screen;
      state.needs_redraw = true;
      return;
    }
  } else {
    if (button == platform::Button::B) {
      if (state.screen == Screen::GameMenu) state.screen = Screen::GameList;
      else if (state.screen == Screen::SystemMenu || state.screen == Screen::GameList) state.screen = Screen::Systems;
      else if (state.screen == Screen::Details) state.screen = Screen::GameMenu;
      else state.screen = Screen::Home;
      state.needs_redraw = true;
      return;
    }
    if (button == platform::Button::A && state.screen == Screen::GameList) {
      state.screen = Screen::GameMenu;
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
    case Screen::Nest:
      return "NEST";
    case Screen::Systems:
      return "SYSTEMS";
    case Screen::GameList:
      return "GAMES";
    case Screen::GameMenu: return "GAME MENU";
    case Screen::SystemMenu: return "SYSTEM MENU";
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
    case Screen::Update:
      return "UPDATE";
    case Screen::LaunchOptions:
      return "LAUNCH";
  }
  return "UNKNOWN";
}

}  // namespace gb::ui
