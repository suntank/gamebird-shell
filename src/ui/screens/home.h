#pragma once

#include <string>
#include <vector>

#include "render/surface_240.h"
#include "render/theme.h"
#include "ui/ui_state.h"

namespace gb::ui::screens {

void DrawHome(render::Surface240& surface,
              const ui::UIState& state,
              const render::Theme& theme);
void DrawSystems(render::Surface240& surface,
                 const render::Theme& theme,
                 const std::vector<std::string>& systems,
                 int selected,
                 const std::string& status);
void DrawGameList(render::Surface240& surface,
                  const render::Theme& theme,
                  const std::string& title,
                  const std::vector<std::string>& games,
                  int selected,
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
void DrawTools(render::Surface240& surface,
               const render::Theme& theme,
               const std::vector<std::string>& rows,
               int selected,
               const std::string& status);

}  // namespace gb::ui::screens
