#include "ui/widgets/list.h"

#include <algorithm>

#include "ui/widgets/text.h"

namespace gb::ui::widgets {

void DrawList(render::Surface240& surface,
              const int x,
              const int y,
              const int w,
              const int h,
              const int item_h,
              const std::vector<std::string>& items,
              const int selected,
              const render::Theme& theme) {
  if (item_h <= 0 || h <= 0) {
    return;
  }

  const int visible_rows = std::max(1, h / item_h);
  int start = 0;
  if (selected >= visible_rows) {
    start = selected - visible_rows + 1;
  }

  for (int row = 0; row < visible_rows; ++row) {
    const int item_index = start + row;
    if (item_index >= static_cast<int>(items.size())) {
      break;
    }

    const int row_y = y + row * item_h;
    const bool active = (item_index == selected);

    const auto bg = active ? theme.accent : theme.panel;
    const auto fg = active ? theme.bg : theme.text;

    surface.FillRect(x, row_y, w, item_h - 2, bg);
    surface.StrokeRect(x, row_y, w, item_h - 2, theme.panel_border);
    DrawText(surface, x + 6, row_y + 4, items[item_index], fg, 1);
  }
}

}  // namespace gb::ui::widgets
