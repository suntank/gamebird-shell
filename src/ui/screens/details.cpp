#include "ui/screens/home.h"

#include "render/image_cache.h"
#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

namespace gb::ui::screens {

void DrawDetails(render::Surface240& surface,
                 const render::Theme& theme,
                 const std::string& title,
                 const std::string& system,
                 const std::string& filename,
                 const int release_year,
                 const std::string& genre,
                 const int players,
                 const std::string& metadata_source,
                 const std::string& box_art_path,
                 const bool is_favorite,
                 const bool is_hidden,
                 const std::string& status) {
  widgets::DrawMenuFrame(surface, theme, "DETAILS");
  widgets::DrawContentText(surface, 16, 34, title, theme.text, 1);
  widgets::DrawContentText(surface, 16, 48, system, theme.text_dim, 1);

  static render::ImageCache image_cache;
  std::string artwork_error;
  const auto* artwork = image_cache.LoadPng(box_art_path, artwork_error);
  if (artwork != nullptr) {
    render::BlitImageFit(surface, *artwork, render::Rect{16, 62, 60, 60},
                         theme.panel_border);
  } else {
    surface.FillRect(16, 62, 60, 60, theme.panel_border);
    widgets::DrawContentText(surface, 24, 86, "NO ART", theme.text_dim, 1);
  }
  widgets::DrawContentText(surface, 88, 66,
                    "YEAR: " + (release_year > 0 ? std::to_string(release_year) : "-") ,
                    theme.text, 1);
  widgets::DrawContentText(surface, 88, 82,
                    widgets::FitLabel("GENRE: " + (genre.empty() ? "-" : genre), 136), theme.text, 1);
  widgets::DrawContentText(surface, 88, 98,
                    "PLAYERS: " + (players > 0 ? std::to_string(players) : "-"),
                    theme.text, 1);
  widgets::DrawContentText(surface, 88, 114,
                    std::string("FAV: ") + (is_favorite ? "YES" : "NO") +
                        " HIDE: " + (is_hidden ? "YES" : "NO"),
                    theme.text_dim, 1);
  surface.FillRect(16, 129, 208, 1, theme.panel_border);
  widgets::DrawContentText(surface, 16, 136, filename, theme.text_dim, 1);
  widgets::DrawContentText(surface, 16, 150,
                    "META: " + (metadata_source.empty() ? "none" : metadata_source),
                    theme.text_dim, 1);
  if (!status.empty()) {
    widgets::DrawContentText(surface, 16, 184, status, theme.text_dim, 1);
  }
  widgets::DrawMenuFooter(surface, theme, "A:MENU  B:BACK  START:MENU");
}

}  // namespace gb::ui::screens
