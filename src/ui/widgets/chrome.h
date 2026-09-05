#pragma once

#include <algorithm>
#include <string>
#include <string_view>

#include "render/theme.h"
#include "ui/widgets/text.h"

namespace gb::ui::widgets {

inline std::string FitLabel(std::string_view text, int width, int scale = 1) {
  const int count = std::max(0, width / (6 * std::max(1, scale)));
  if (static_cast<int>(text.size()) <= count) return std::string(text);
  if (count <= 3) return std::string(text.substr(0, count));
  return std::string(text.substr(0, count - 3)) + "...";
}

inline void DrawContentText(render::Surface240& surface, int x, int y,
                            std::string_view text, std::uint16_t color,
                            int scale = 1) {
  DrawText(surface, x, y, FitLabel(text, 224 - x, scale), color, scale);
}

inline void DrawMenuFrame(render::Surface240& surface, const render::Theme& theme,
                          std::string_view title) {
  surface.Clear(theme.bg);
  surface.FillRect(8, 8, 224, 224, theme.panel);
  surface.FillRect(8, 8, 224, 20, theme.bg);
  surface.FillRect(16, 16, 3, 8, theme.accent);
  DrawText(surface, 25, 17, FitLabel(title, 198), theme.text);
  surface.FillRect(16, 28, 208, 1, theme.panel_border);
}

inline void DrawMenuFooter(render::Surface240& surface, const render::Theme& theme,
                           std::string_view controls) {
  surface.FillRect(16, 208, 208, 1, theme.panel_border);
  if (controls.size() <= 34) {
    DrawText(surface, 16, 218, controls, theme.text_dim);
  } else {
    auto split = controls.rfind(' ', 34);
    if (split == std::string_view::npos) split = 34;
    DrawText(surface, 16, 212, controls.substr(0, split), theme.text_dim);
    const auto next = split + (controls[split] == ' ' ? 1 : 0);
    DrawText(surface, 16, 224, FitLabel(controls.substr(next), 208), theme.text_dim);
  }
}

inline void DrawProgress(render::Surface240& surface, const render::Theme& theme,
                         int y, int percent, bool finished = false) {
  surface.FillRect(16, y, 208, 12, theme.bg);
  const int width = 204 * std::clamp(percent, 0, 100) / 100;
  surface.FillRect(18, y + 2, width, 8, finished ? theme.success : theme.accent);
}

}  // namespace gb::ui::widgets
