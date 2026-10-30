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
                const std::string& message,
                const std::string& running_commit,
                const std::string& installed_commit,
                const std::string& latest_commit,
                const std::uint64_t animation_ms,
                const bool reboot_required) {
  widgets::DrawMenuFrame(surface, theme, "SYSTEM UPDATE");

  widgets::DrawContentText(surface, 16, 42, phase.empty() ? "READY" : phase,
                    theme.text, 1);

  const int clamped = std::clamp(progress, 0, 100);
  widgets::DrawProgress(surface, theme, 62, clamped, !busy && phase == "CURRENT");
  if (busy) {
    // A moving marker confirms the shell is alive during downloads/builds,
    // whose actual percentage may remain unchanged for several minutes.
    const int marker = static_cast<int>((animation_ms / 100) % 25) * 7;
    surface.FillRect(18 + marker, 64, 28, 8, theme.text);
  }
  widgets::DrawContentText(surface, 16, 82, std::to_string(clamped) + "%" +
                    (busy ? "  WORKING..." : ""),
                    theme.text_dim, 1);
  const auto commit = [](const std::string& value) {
    return value.empty() ? std::string("UNKNOWN") : value.substr(0, 10);
  };
  widgets::DrawContentText(surface, 16, 104, "RUNNING:   " + commit(running_commit), theme.text);
  widgets::DrawContentText(surface, 16, 116, "INSTALLED: " + commit(installed_commit), theme.text);
  widgets::DrawContentText(surface, 16, 128, "LATEST:    " + commit(latest_commit), theme.text);
  widgets::DrawContentText(surface, 16, 148,
                    "PI OS: " + std::to_string(std::max(0, os_updates)) +
                        (shell_update ? "  SHELL: UPDATE" : ""), theme.text, 1);
  const auto [message_line1, message_line2] = WrapMessage(message);
  widgets::DrawContentText(surface, 16, 164, message_line1, theme.text_dim, 1);
  if (!message_line2.empty()) {
    widgets::DrawContentText(surface, 16, 176, message_line2, theme.text_dim, 1);
  }

  if (busy) {
    widgets::DrawMenuFooter(surface, theme, "PLEASE KEEP POWER CONNECTED");
  } else {
    widgets::DrawContentText(surface, 16, 196,
                      reboot_required ? "A:RESTART" :
                          ((os_updates > 0 || shell_update) ? "A:INSTALL" : "A:CHECK"),
                      theme.text, 1);
    widgets::DrawMenuFooter(surface, theme, "B:BACK");
  }
}

}  // namespace gb::ui::screens
