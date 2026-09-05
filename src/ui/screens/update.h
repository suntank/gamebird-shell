#pragma once

#include <string>

#include "render/surface_240.h"
#include "render/theme.h"

namespace gb::ui::screens {

void DrawUpdate(render::Surface240& surface,
                const render::Theme& theme,
                const std::string& phase,
                int progress,
                int os_updates,
                bool shell_update,
                bool busy,
                const std::string& message);

}  // namespace gb::ui::screens
