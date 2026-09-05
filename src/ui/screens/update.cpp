#include "ui/screens/update.h"

#include <algorithm>
#include <utility>

#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

namespace gb::ui::screens {
namespace {

std::pair<std::string, std::string> WrapMessage(const std::string& message) {
  constexpr std::size_t kMaxCharacters = 34;
  if (message.size() <= kMaxCharacters) return {message, {}};

  std::size_t split = message.rfind(' ', kMaxCharacters);
  if (split == std::string::npos) split = kMaxCharacters;
  std::string second = message.substr(split + (message[split] == ' ' ? 1 : 0));
  if (second.size() > kMaxCharacters) {
    second.resize(kMaxCharacters - 3);
    second += "...";
  }
  return {message.substr(0, split), std::move(second)};
}

}  // namespace

void DrawUpdate(render::Surface240& surface,
                const render::Theme& theme,
                const std::string& phase,
                const int progress,
                const int os_updates,
                const bool shell_update,
                const bool busy,
                const std::string& message) {
  widgets::DrawMenuFrame(surface, theme, "SYSTEM UPDATE");

  widgets::DrawContentText(surface, 16, 42, "STATUS", theme.text_dim, 1);
  widgets::DrawContentText(surface, 16, 56, phase.empty() ? "READY" : phase,
                    busy ? theme.accent : theme.text, 1);

  const int clamped = std::clamp(progress, 0, 100);
  widgets::DrawProgress(surface, theme, 78, clamped, !busy);
  widgets::DrawContentText(surface, 16, 98, std::to_string(clamped) + "%",
                    theme.text_dim, 1);

  widgets::DrawContentText(surface, 16, 120,
                    "PI OS: " + std::to_string(std::max(0, os_updates)) +
                        " PACKAGES",
                    theme.text, 1);
  widgets::DrawContentText(surface, 16, 136,
                    std::string("GAMEBIRD: ") +
                        (shell_update ? "UPDATE READY" : "CURRENT"),
                    shell_update ? theme.accent : theme.text, 1);
  const auto [message_line1, message_line2] = WrapMessage(message);
  widgets::DrawContentText(surface, 16, 158, message_line1, theme.text_dim, 1);
  if (!message_line2.empty()) {
    widgets::DrawContentText(surface, 16, 172, message_line2, theme.text_dim, 1);
  }

  if (busy) {
    widgets::DrawMenuFooter(surface, theme, "PLEASE KEEP POWER CONNECTED");
  } else {
    widgets::DrawContentText(surface, 16, 196,
                      (os_updates > 0 || shell_update) ? "A:INSTALL" : "A:CHECK",
                      theme.text, 1);
    widgets::DrawMenuFooter(surface, theme, "B:BACK");
  }
}

}  // namespace gb::ui::screens
