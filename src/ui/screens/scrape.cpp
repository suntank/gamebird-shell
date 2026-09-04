#include "ui/screens/home.h"

#include <algorithm>

#include "ui/widgets/text.h"

namespace gb::ui::screens {
namespace {

std::string Trim(const std::string& value, const std::size_t max_length) {
  if (value.size() <= max_length) return value;
  if (max_length <= 3) return value.substr(0, max_length);
  return value.substr(0, max_length - 3) + "...";
}

}  // namespace

void DrawScrapeProgress(render::Surface240& surface,
                        const render::Theme& theme,
                        const int completed,
                        const int total,
                        const int downloaded,
                        const int skipped_existing,
                        const int missing,
                        const bool finished,
                        const std::string& current_title,
                        const std::string& status) {
  surface.Clear(theme.bg);
  surface.FillRect(8, 8, 224, 224, theme.panel);
  surface.StrokeRect(8, 8, 224, 224, theme.panel_border);

  widgets::DrawText(surface, 16, 18,
                    finished ? "SCRAPE COMPLETE" : "SCRAPING LIBRARY",
                    theme.accent, 1);
  widgets::DrawText(surface, 16, 34, "Only games missing box art", theme.text_dim, 1);
  widgets::DrawText(surface, 16, 54,
                    std::to_string(completed) + " / " + std::to_string(total) +
                        " checked",
                    theme.text, 1);
  widgets::DrawText(surface, 16, 68,
                    std::to_string(std::max(0, total - completed)) + " remaining",
                    theme.text_dim, 1);

  constexpr int kBarX = 16;
  constexpr int kBarY = 84;
  constexpr int kBarWidth = 208;
  constexpr int kBarHeight = 14;
  surface.StrokeRect(kBarX, kBarY, kBarWidth, kBarHeight, theme.panel_border);
  const int filled = total > 0 ? (kBarWidth - 2) * completed / total : kBarWidth - 2;
  if (filled > 0) {
    surface.FillRect(kBarX + 1, kBarY + 1, filled, kBarHeight - 2, theme.accent);
  }

  widgets::DrawText(surface, 16, 116,
                    "Downloaded: " + std::to_string(downloaded), theme.text, 1);
  widgets::DrawText(surface, 16, 130,
                    "Not found: " + std::to_string(missing), theme.text_dim, 1);
  widgets::DrawText(surface, 16, 144,
                    "Already had art: " + std::to_string(skipped_existing),
                    theme.text_dim, 1);
  widgets::DrawText(surface, 16, 166,
                    "Last checked:", theme.text_dim, 1);
  widgets::DrawText(surface, 16, 180, Trim(current_title, 30), theme.text, 1);
  if (!status.empty()) {
    widgets::DrawText(surface, 16, 196, Trim(status, 31), theme.text_dim, 1);
  }
  widgets::DrawText(surface, 16, 216,
                    finished ? "A/B: BACK" : "B: CANCEL AFTER THIS GAME",
                    theme.text_dim, 1);
}

}  // namespace gb::ui::screens
