#include "ui/screens/home.h"

#include <vector>

#include "ui/widgets/list.h"
#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

namespace gb::ui::screens {

void DrawHome(render::Surface240& surface,
              const ui::UIState& state,
              const render::Theme& theme) {
  const std::vector<std::string> items = {
      state.continue_available ? "Continue playing" : "Continue (no recent game)",
      "Browse games", "Recent", "Favorites", "Tools", "Settings",
  };
  widgets::DrawMenuFrame(surface, theme, "GAMEBIRD");
  widgets::DrawContentText(surface, 16, 40,
      state.continue_available ? state.continue_title : "Your next game starts here",
      state.continue_available ? theme.accent : theme.text_dim);
  widgets::DrawList(surface, 16, 64, 208, 132, 22, items, state.home_selected, theme);
  widgets::DrawMenuFooter(surface, theme, "A:OPEN  B:BACK  START:BACK");
}

}  // namespace gb::ui::screens
