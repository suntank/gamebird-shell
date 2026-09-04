#pragma once

#include <string>
#include <vector>

#include "render/surface_240.h"
#include "render/theme.h"
#include "ui/ui_state.h"

namespace gb::ui::screens {

struct SystemCarouselItem {
  std::string name;
  int game_count = 0;
  std::string icon_path;
  std::string logo_path;
};

struct WifiNetworkItem {
  std::string ssid;
  int signal = 0;
  bool secured = false;
  bool active = false;
};

enum class WifiView {
  Overview,
  Networks,
  Password,
  Country,
};

void DrawHome(render::Surface240& surface,
              const ui::UIState& state,
              const render::Theme& theme);
void DrawSystems(render::Surface240& surface,
                 const render::Theme& theme,
                 const std::vector<SystemCarouselItem>& systems,
                 int selected,
                 const std::string& status);
void DrawGameList(render::Surface240& surface,
                  const render::Theme& theme,
                  const std::string& title,
                  const std::vector<std::string>& games,
                  int selected,
                  const std::string& status);
void DrawGameBrowser(render::Surface240& surface,
                     const render::Theme& theme,
                     const std::string& system_name,
                     const std::vector<std::string>& games,
                     int selected,
                     const std::string& box_art_path,
                     const std::string& status);
void DrawDetails(render::Surface240& surface,
                 const render::Theme& theme,
                 const std::string& title,
                 const std::string& system,
                 const std::string& filename,
                 int release_year,
                 const std::string& genre,
                 int players,
                 const std::string& metadata_source,
                 const std::string& box_art_path,
                 bool is_favorite,
                 bool is_hidden,
                 const std::string& status);
void DrawSettings(render::Surface240& surface,
                  const render::Theme& theme,
                  const std::vector<std::string>& rows,
                  int selected,
                  const std::string& status);
void DrawScrapeProgress(render::Surface240& surface,
                        const render::Theme& theme,
                        int completed,
                        int total,
                        int downloaded,
                        int skipped_existing,
                        int missing,
                        bool finished,
                        const std::string& current_title,
                        const std::string& status);
void DrawTools(render::Surface240& surface,
               const render::Theme& theme,
               const std::vector<std::string>& rows,
               int selected,
               const std::string& status);
void DrawWifi(render::Surface240& surface,
              const render::Theme& theme,
              WifiView view,
              const std::string& connected_ssid,
              int connected_signal,
              bool wifi_enabled,
              const std::string& country,
              const std::vector<WifiNetworkItem>& networks,
              int selected,
              const std::string& password_or_country,
              int keyboard_page,
              const std::string& status);

}  // namespace gb::ui::screens
