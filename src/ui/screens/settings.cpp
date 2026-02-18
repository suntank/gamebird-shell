#include "ui/screens/home.h"

#include "ui/widgets/list.h"
#include "ui/widgets/text.h"

namespace gb::ui::screens {

void DrawSettings(render::Surface240& surface,
                  const render::Theme& theme,
                  const std::vector<std::string>& rows,
                  const int selected,
                  const std::string& status) {
  surface.Clear(theme.bg);
  surface.FillRect(8, 8, 224, 224, theme.panel);
  surface.StrokeRect(8, 8, 224, 224, theme.panel_border);

  widgets::DrawText(surface, 16, 18, "SETTINGS", theme.accent, 1);
  widgets::DrawList(surface, 16, 40, 208, 154, 18, rows, selected, theme);
  if (!status.empty()) {
    widgets::DrawText(surface, 16, 196, status, theme.text_dim, 1);
  }
  widgets::DrawText(surface, 16, 210, "A:TOGGLE/SAVE B:BACK", theme.text_dim, 1);
}

}  // namespace gb::ui::screens
