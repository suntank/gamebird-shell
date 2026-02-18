#include "ui/screens/home.h"

#include "ui/widgets/list.h"
#include "ui/widgets/text.h"

namespace gb::ui::screens {

void DrawSystems(render::Surface240& surface,
                 const render::Theme& theme,
                 const std::vector<std::string>& systems,
                 const int selected,
                 const std::string& status) {
  surface.Clear(theme.bg);
  surface.FillRect(8, 8, 224, 224, theme.panel);
  surface.StrokeRect(8, 8, 224, 224, theme.panel_border);
  widgets::DrawText(surface, 16, 18, "SYSTEMS", theme.accent, 1);
  widgets::DrawList(surface, 16, 40, 208, 154, 18, systems, selected, theme);
  if (systems.empty()) {
    widgets::DrawText(surface, 16, 80, "NO SYSTEMS FOUND", theme.text_dim, 1);
  }
  if (!status.empty()) {
    widgets::DrawText(surface, 16, 196, status, theme.text_dim, 1);
  }
  widgets::DrawText(surface, 16, 210, "A:OPEN Y:OPT B:BACK", theme.text_dim, 1);
}

}  // namespace gb::ui::screens
