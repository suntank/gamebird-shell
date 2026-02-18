#include "ui/screens/home.h"

#include <vector>

#include "ui/widgets/list.h"
#include "ui/widgets/text.h"

namespace gb::ui::screens {

void DrawHome(render::Surface240& surface,
              const ui::UIState& state,
              const render::Theme& theme) {
  static const std::vector<std::string> items = {
      "Recent",
      "Favorites",
      "Systems",
      "Tools",
      "Settings",
  };

  surface.Clear(theme.bg);
  surface.FillRect(8, 8, 224, 26, theme.panel);
  surface.StrokeRect(8, 8, 224, 26, theme.panel_border);
  widgets::DrawText(surface, 14, 16, "GAMEBIRD SHELL", theme.text, 1);

  widgets::DrawList(surface, 14, 44, 212, 166, 32, items, state.home_selected, theme);
  widgets::DrawText(surface, 12, 218, "A:SELECT", theme.text_dim, 1);
}

}  // namespace gb::ui::screens
