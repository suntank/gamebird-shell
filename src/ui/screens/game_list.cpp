#include "ui/screens/home.h"

#include <algorithm>

#include "render/image_cache.h"
#include "ui/widgets/list.h"
#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

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
  widgets::DrawContentText(surface, std::max(0, (surface.Width() - width) / 2), y, text,
                    color, scale);
}

}  // namespace

void DrawGameList(render::Surface240& surface,
                  const render::Theme& theme,
                  const std::string& title,
                  const std::vector<std::string>& games,
                  const int selected,
                  const std::string& status) {
  widgets::DrawMenuFrame(surface, theme, title);
  widgets::DrawList(surface, 16, 40, 208, 150, 24, games, selected, theme);
  if (games.empty()) {
    widgets::DrawContentText(surface, 16, 80, "NO GAMES FOUND", theme.text_dim, 1);
  }
  if (!status.empty()) {
    widgets::DrawContentText(surface, 16, 196, status, theme.text_dim, 1);
  }
  widgets::DrawMenuFooter(surface, theme, "A:OPEN  B:BACK  START:MENU");
}

void DrawGameBrowser(render::Surface240& surface,
                     const render::Theme& theme,
                     const std::string& system_name,
                     const std::vector<std::string>& games,
                     const int selected,
                     const std::string& box_art_path,
                     const std::string& status) {
  widgets::DrawMenuFrame(surface, theme, system_name);

  constexpr render::Rect art_bounds{16, 34, 208, 130};
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

  widgets::DrawMenuFooter(surface, theme, "A:OPEN  B:BACK  START:MENU");
  if (games.empty()) {
    DrawCenteredText(surface, 185, "NO GAMES FOUND", theme.text_dim);
    return;
  }

  const int current = std::clamp(selected, 0, static_cast<int>(games.size()) - 1);
  const int previous =
      (current + static_cast<int>(games.size()) - 1) % games.size();
  const int next = (current + 1) % games.size();
  DrawCenteredText(surface, 169, FitText(games[previous], 34), theme.text_dim);
  surface.FillRect(16, 181, 208, 15, theme.panel_border);
  surface.FillRect(16, 181, 3, 15, theme.accent);
  DrawCenteredText(surface, 185, FitText(games[current], 34), theme.text);
  DrawCenteredText(surface, 200, FitText(games[next], 34), theme.text_dim);

  if (!status.empty()) {
    surface.FillRect(16, 198, 208, 9, theme.panel);
    DrawCenteredText(surface, 200, FitText(status, 34), theme.accent);
  }
}

}  // namespace gb::ui::screens
