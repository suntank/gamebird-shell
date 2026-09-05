#include "ui/screens/home.h"

#include <algorithm>

#include "render/image_cache.h"
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

void DrawFallbackConsole(render::Surface240& surface,
                         const render::Theme& theme) {
  constexpr int x = 66;
  constexpr int y = 52;
  surface.FillRect(x, y, 108, 58, theme.panel_border);
  surface.FillRect(x + 5, y + 5, 98, 48, theme.bg);
  surface.FillRect(x + 16, y + 17, 34, 22, theme.panel);
  surface.StrokeRect(x + 16, y + 17, 34, 22, theme.text_dim);
  surface.FillRect(x + 63, y + 22, 8, 8, theme.accent);
  surface.FillRect(x + 79, y + 22, 8, 8, theme.success);
  surface.FillRect(x + 6, y + 58, 18, 4, theme.panel_border);
  surface.FillRect(x + 84, y + 58, 18, 4, theme.panel_border);
}

}  // namespace

void DrawSystems(render::Surface240& surface,
                 const render::Theme& theme,
                 const std::vector<SystemCarouselItem>& systems,
                 const int selected,
                 const std::string& status) {
  widgets::DrawMenuFrame(surface, theme, "SYSTEMS");

  if (systems.empty()) {
    DrawCenteredText(surface, 102, "NO SYSTEMS FOUND", theme.text_dim);
  } else {
    const int current = std::clamp(selected, 0, static_cast<int>(systems.size()) - 1);
    const auto& item = systems[static_cast<std::size_t>(current)];
    static render::ImageCache image_cache(6);
    std::string image_error;
    const auto* icon = image_cache.LoadPng(item.icon_path, image_error);
    if (icon != nullptr) {
      render::BlitImageFit(surface, *icon, render::Rect{42, 38, 156, 88}, theme.panel);
    } else {
      DrawFallbackConsole(surface, theme);
    }

    const auto* logo = image_cache.LoadPng(item.logo_path, image_error);
    if (logo != nullptr) {
      render::BlitImageFit(surface, *logo, render::Rect{28, 132, 184, 36}, theme.panel);
    } else {
      DrawCenteredText(surface, 140, FitText(item.name, 18), theme.text, 2);
    }

    DrawCenteredText(surface, 177,
                     std::to_string(item.game_count) +
                         (item.game_count == 1 ? " GAME" : " GAMES"),
                     theme.text_dim);

    if (systems.size() > 1) {
      const int previous =
          (current + static_cast<int>(systems.size()) - 1) % systems.size();
      const int next = (current + 1) % systems.size();
      widgets::DrawContentText(surface, 16, 197,
                        "< " + FitText(systems[previous].name, 12), theme.text_dim, 1);
      const std::string next_text = FitText(systems[next].name, 12) + " >";
      widgets::DrawContentText(surface,
                        224 - widgets::MeasureTextWidth(next_text), 197, next_text,
                        theme.text_dim, 1);
    }
  }

  if (!status.empty()) {
    widgets::DrawMenuFooter(surface, theme, status);
  } else {
    widgets::DrawMenuFooter(surface, theme, "A:OPEN  B:BACK  START:MENU");
  }
}

}  // namespace gb::ui::screens
