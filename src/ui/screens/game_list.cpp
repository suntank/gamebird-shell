#include "ui/screens/home.h"

#include "ui/widgets/list.h"
#include "ui/widgets/text.h"

namespace gb::ui::screens {

void DrawGameList(render::Surface240& surface,
                  const render::Theme& theme,
                  const std::string& title,
                  const std::vector<std::string>& games,
                  const int selected,
                  const std::string& status) {
  surface.Clear(theme.bg);
  surface.FillRect(8, 8, 224, 224, theme.panel);
  surface.StrokeRect(8, 8, 224, 224, theme.panel_border);
  widgets::DrawText(surface, 16, 18, title, theme.accent, 1);
  widgets::DrawList(surface, 16, 40, 208, 154, 18, games, selected, theme);
  if (games.empty()) {
    widgets::DrawText(surface, 16, 80, "NO GAMES FOUND", theme.text_dim, 1);
  }
  if (!status.empty()) {
    widgets::DrawText(surface, 16, 196, status, theme.text_dim, 1);
  }
  widgets::DrawText(surface, 16, 210, "A:RUN ST:OPT X:FAV Y:HIDE", theme.text_dim, 1);
}

}  // namespace gb::ui::screens
