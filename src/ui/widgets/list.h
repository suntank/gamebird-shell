#pragma once

#include <string>
#include <vector>

#include "render/surface_240.h"
#include "render/theme.h"

namespace gb::ui::widgets {

void DrawList(render::Surface240& surface,
              int x,
              int y,
              int w,
              int h,
              int item_h,
              const std::vector<std::string>& items,
              int selected,
              const render::Theme& theme);

}  // namespace gb::ui::widgets
