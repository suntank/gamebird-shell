#include "ui/screens/home.h"

#include <algorithm>

#include "render/image_cache.h"
#include "ui/widgets/list.h"
#include "ui/widgets/text.h"

namespace gb::ui::screens {
namespace {

std::string FitText(std::string text, const std::size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars <= 3) {
    return text.substr(0, max_chars);
  }
  text.resize(max_chars - 3);
  return text + "...";
}

void DrawCenteredText(render::Surface240& surface,
                      const int y,
                      const std::string& text,
                      const std::uint16_t color,
                      const int scale = 1) {
  const int width = widgets::MeasureTextWidth(text, scale);
  widgets::DrawText(surface, std::max(0, (surface.Width() - width) / 2), y, text,
                    color, scale);
}

}  // namespace

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
  widgets::DrawText(surface, 16, 210, "A:RUN R:INFO X:FAV Y:OPTIONS", theme.text_dim, 1);
}

void DrawGameBrowser(render::Surface240& surface,
                     const render::Theme& theme,
                     const std::string& system_name,
                     const std::vector<std::string>& games,
                     const int selected,
                     const std::string& box_art_path,
                     const std::string& status) {
  surface.Clear(theme.bg);
  surface.FillRect(4, 4, 232, 232, theme.panel);
  surface.StrokeRect(4, 4, 232, 232, theme.panel_border);

  constexpr render::Rect art_bounds{6, 6, 228, 182};
  static render::ImageCache image_cache(10);
  std::string artwork_error;
  const auto* artwork = image_cache.LoadPng(box_art_path, artwork_error);
  if (artwork != nullptr) {
    render::BlitImageFit(surface, *artwork, art_bounds, theme.bg);
  } else {
    surface.FillRect(art_bounds.x, art_bounds.y, art_bounds.w, art_bounds.h,
                     theme.bg);
    surface.StrokeRect(art_bounds.x, art_bounds.y, art_bounds.w, art_bounds.h,
                       theme.panel_border);
    DrawCenteredText(surface, 96, "NO ART", theme.text_dim, 2);
  }

  surface.FillRect(7, 7, 226, 15, theme.panel);
  const std::string heading = "< " + FitText(system_name, 29) + " >";
  DrawCenteredText(surface, 11, heading, theme.accent);

  surface.FillRect(5, 190, 230, 45, theme.bg);
  if (games.empty()) {
    DrawCenteredText(surface, 207, "NO GAMES FOUND", theme.text_dim);
    return;
  }

  const int current = std::clamp(selected, 0, static_cast<int>(games.size()) - 1);
  const int previous =
      (current + static_cast<int>(games.size()) - 1) % games.size();
  const int next = (current + 1) % games.size();
  DrawCenteredText(surface, 192, FitText(games[previous], 34), theme.text_dim);
  surface.FillRect(8, 203, 224, 15, theme.panel_border);
  DrawCenteredText(surface, 207, FitText(games[current], 34), theme.text);
  DrawCenteredText(surface, 223, FitText(games[next], 34), theme.text_dim);

  if (!status.empty()) {
    surface.FillRect(8, 222, 224, 11, theme.panel);
    DrawCenteredText(surface, 224, FitText(status, 34), theme.accent);
  }
}

}  // namespace gb::ui::screens
