#include "render/theme.h"

#include "render/surface_240.h"

namespace gb::render {

Theme DefaultTheme() {
  return Theme{
      .bg = Rgb565(10, 16, 24),
      .panel = Rgb565(22, 30, 44),
      .panel_border = Rgb565(50, 64, 84),
      .accent = Rgb565(255, 170, 40),
      .text = Rgb565(233, 239, 248),
      .text_dim = Rgb565(138, 150, 170),
      .success = Rgb565(80, 201, 120),
  };
}

}  // namespace gb::render
