#pragma once

#include <cstdint>

namespace gb::render {

struct Theme {
  std::uint16_t bg;
  std::uint16_t panel;
  std::uint16_t panel_border;
  std::uint16_t accent;
  std::uint16_t text;
  std::uint16_t text_dim;
  std::uint16_t success;
};

Theme DefaultTheme();

}  // namespace gb::render
