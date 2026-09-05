#include "ui/screens/home.h"

#include <algorithm>

#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

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
  widgets::DrawMenuFrame(surface, theme, finished ? "SCRAPE COMPLETE" : "SCRAPING LIBRARY");
  widgets::DrawContentText(surface, 16, 34, "Only games missing box art", theme.text_dim, 1);
  widgets::DrawContentText(surface, 16, 54,
                    std::to_string(completed) + " / " + std::to_string(total) +
                        " checked",
                    theme.text, 1);
  widgets::DrawContentText(surface, 16, 68,
                    std::to_string(std::max(0, total - completed)) + " remaining",
                    theme.text_dim, 1);

  const int percent = total > 0
      ? static_cast<int>(100LL * std::clamp(completed, 0, total) / total)
      : (finished ? 100 : 0);
  widgets::DrawProgress(surface, theme, 84, percent, finished);

  widgets::DrawContentText(surface, 16, 116,
                    "Downloaded: " + std::to_string(downloaded), theme.text, 1);
  widgets::DrawContentText(surface, 16, 130,
                    "Not found: " + std::to_string(missing), theme.text_dim, 1);
  widgets::DrawContentText(surface, 16, 144,
                    "Already had art: " + std::to_string(skipped_existing),
                    theme.text_dim, 1);
  widgets::DrawContentText(surface, 16, 166,
                    "Last checked:", theme.text_dim, 1);
  widgets::DrawContentText(surface, 16, 180, Trim(current_title, 30), theme.text, 1);
  if (!status.empty()) {
    widgets::DrawContentText(surface, 16, 196, Trim(status, 31), theme.text_dim, 1);
  }
  widgets::DrawMenuFooter(surface, theme, finished ? "A/B: BACK" : "B: CANCEL AFTER THIS GAME");
}

}  // namespace gb::ui::screens
