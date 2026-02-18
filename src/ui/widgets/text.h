#pragma once

#include <string_view>

#include "render/surface_240.h"

namespace gb::ui::widgets {

int MeasureTextWidth(std::string_view text, int scale = 1);
void DrawText(render::Surface240& surface,
              int x,
              int y,
              std::string_view text,
              std::uint16_t color,
              int scale = 1);

}  // namespace gb::ui::widgets
