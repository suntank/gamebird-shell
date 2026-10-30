#pragma once

#include <string>
#include <cstdint>

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
                const std::string& message,
                const std::string& running_commit = {},
                const std::string& installed_commit = {},
                const std::string& latest_commit = {},
                std::uint64_t animation_ms = 0,
                bool reboot_required = false);

}  // namespace gb::ui::screens
