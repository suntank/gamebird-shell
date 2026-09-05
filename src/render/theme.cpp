#include "render/theme.h"

#include "render/surface_240.h"

namespace gb::render {

Theme DefaultTheme() {
  return Theme{
      .bg = Rgb565(9, 16, 23),
      .panel = Rgb565(19, 30, 39),
      .panel_border = Rgb565(42, 60, 70),
      .accent = Rgb565(110, 224, 195),
      .text = Rgb565(237, 244, 242),
      .text_dim = Rgb565(153, 174, 180),
      .success = Rgb565(80, 201, 120),
  };
}

}  // namespace gb::render
