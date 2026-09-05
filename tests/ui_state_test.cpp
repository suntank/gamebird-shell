#include <cassert>
#include "ui/ui_state.h"

int main() {
  using gb::ui::Screen;
  using gb::platform::Button;
  gb::ui::UIState state;
  assert(state.screen == Screen::Home);
  state.home_selected = 0;
  gb::ui::HandleButton(state, Button::A);
  assert(state.screen == Screen::Home);
  state.continue_available = true;
  gb::ui::HandleButton(state, Button::A);
  assert(state.screen == Screen::GameMenu);
  state.screen = Screen::Systems;
  gb::ui::HandleButton(state, Button::Start);
  assert(state.screen == Screen::SystemMenu);
  gb::ui::HandleButton(state, Button::B);
  assert(state.screen == Screen::Systems);
  state.screen = Screen::GameList;
  gb::ui::HandleButton(state, Button::A);
  assert(state.screen == Screen::GameMenu);
  gb::ui::HandleButton(state, Button::B);
  assert(state.screen == Screen::GameList);
  state.screen = Screen::Settings;
  gb::ui::HandleButton(state, Button::Start);
  assert(state.screen == Screen::Home);
  gb::ui::HandleButton(state, Button::B);
  assert(state.screen == Screen::Settings);
}
