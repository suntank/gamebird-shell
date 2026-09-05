#include "ui/widgets/list.h"

#include <algorithm>

#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

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
  if (item_h <= 0 || h < item_h || w <= 20) {
    return;
  }

  const int visible_rows = h / item_h;
  const int current = items.empty() ? -1 : std::clamp(selected, 0, static_cast<int>(items.size()) - 1);
  int start = 0;
  if (current >= visible_rows) {
    start = current - visible_rows + 1;
  }

  for (int row = 0; row < visible_rows; ++row) {
    const int item_index = start + row;
    if (item_index >= static_cast<int>(items.size())) {
      break;
    }

    const int row_y = y + row * item_h;
    const bool active = (item_index == current);

    const auto bg = active ? theme.accent : theme.panel;
    const auto fg = active ? theme.bg : theme.text;

    surface.FillRect(x, row_y, w, item_h - 2, bg);
    if (!active) surface.FillRect(x + 8, row_y + item_h - 3, w - 16, 1, theme.panel_border);
    DrawText(surface, x + 8, row_y + (item_h - 9) / 2,
             FitLabel(items[item_index], w - 28), fg, 1);
    if (active) DrawText(surface, x + w - 14, row_y + (item_h - 9) / 2, ">", fg);
  }
  if (static_cast<int>(items.size()) > visible_rows) {
    const int track = visible_rows * item_h - 2;
    const int thumb = std::max(6, track * visible_rows / static_cast<int>(items.size()));
    const int offset = (track - thumb) * start / (static_cast<int>(items.size()) - visible_rows);
    surface.FillRect(x + w + 3, y, 2, track, theme.bg);
    surface.FillRect(x + w + 3, y + offset, 2, thumb, theme.accent);
  }
}

}  // namespace gb::ui::widgets
