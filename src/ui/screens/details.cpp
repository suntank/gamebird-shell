#include "ui/screens/home.h"

#include "ui/widgets/text.h"

namespace gb::ui::screens {

void DrawDetails(render::Surface240& surface, const render::Theme& theme) {
  surface.Clear(theme.bg);
  surface.FillRect(8, 8, 224, 224, theme.panel);
  surface.StrokeRect(8, 8, 224, 224, theme.panel_border);

  surface.FillRect(16, 28, 64, 64, theme.panel_border);
  widgets::DrawText(surface, 16, 18, "DETAILS", theme.accent, 1);
  widgets::DrawText(surface, 92, 34, "YEAR: 1994", theme.text, 1);
  widgets::DrawText(surface, 92, 50, "GENRE: ACTION", theme.text, 1);
  widgets::DrawText(surface, 92, 66, "PLAYERS: 1", theme.text, 1);
  widgets::DrawText(surface, 16, 108, "LAUNCH", theme.success, 1);
  widgets::DrawText(surface, 16, 124, "FAVORITE", theme.text, 1);
  widgets::DrawText(surface, 16, 140, "HIDE", theme.text, 1);
  widgets::DrawText(surface, 16, 210, "B:BACK", theme.text_dim, 1);
}

}  // namespace gb::ui::screens
