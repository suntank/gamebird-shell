#include <cassert>

#include "ui/ui_state.h"

int main() {
  gb::ui::UIState state;
  assert(state.screen == gb::ui::Screen::Systems);

  gb::ui::HandleButton(state, gb::platform::Button::Start);
  assert(state.screen == gb::ui::Screen::Home);

  gb::ui::HandleButton(state, gb::platform::Button::Start);
  assert(state.screen == gb::ui::Screen::Systems);

  gb::ui::HandleButton(state, gb::platform::Button::B);
  assert(state.screen == gb::ui::Screen::Systems);

  state.screen = gb::ui::Screen::Home;
  state.home_selected = 4;
  gb::ui::HandleButton(state, gb::platform::Button::A);
  assert(state.screen == gb::ui::Screen::Systems);
  return 0;
}
