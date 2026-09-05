#include "ui/screens/home.h"

#include "ui/widgets/list.h"
#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

namespace gb::ui::screens {

void DrawSettings(render::Surface240& surface,
                  const render::Theme& theme,
                  const std::vector<std::string>& rows,
                  const int selected,
                  const std::string& status) {
  widgets::DrawMenuFrame(surface, theme, "SETTINGS");
  widgets::DrawList(surface, 16, 40, 208, 150, 24, rows, selected, theme);
  if (!status.empty()) {
    widgets::DrawContentText(surface, 16, 196, status, theme.text_dim, 1);
  }
  widgets::DrawMenuFooter(surface, theme, "A:TOGGLE/SAVE B:BACK");
}

}  // namespace gb::ui::screens
