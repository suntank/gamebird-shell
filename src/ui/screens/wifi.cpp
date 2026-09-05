#include "ui/screens/home.h"

#include <algorithm>

#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"
#include "ui/widgets/list.h"

namespace gb::ui::screens {
namespace {

std::string Fit(std::string text, const std::size_t max) {
  if (text.size() <= max) return text;
  if (max <= 3) return text.substr(0, max);
  return text.substr(0, max - 3) + "...";
}

void Signal(render::Surface240& surface, int x, int y, const int signal,
            const render::Theme& theme) {
  const int filled = std::clamp((signal + 24) / 25, 0, 4);
  for (int i = 0; i < 4; ++i) {
    const int h = 3 + i * 3;
    const auto color = i < filled ? theme.success : theme.panel_border;
    surface.FillRect(x + i * 5, y + 12 - h, 3, h, color);
  }
}

void Header(render::Surface240& surface, const render::Theme& theme,
            const std::string& title) {
  widgets::DrawMenuFrame(surface, theme, title);
}

std::string Keyboard(const int page) {
  switch ((page % 3 + 3) % 3) {
    case 0: return "abcdefghijklmnopqrstuvwxyz0123456789";
    case 1: return "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    default: return "!@#$%^&*()-_=+[]{};:,.?/\\|~`'\"     ";
  }
}

void DrawKeyboard(render::Surface240& surface, const render::Theme& theme,
                  const std::string& chars, const int selected) {
  constexpr int kColumns = 6;
  constexpr int kCellWidth = 31;
  constexpr int kCellHeight = 18;
  constexpr int kStartX = 25;
  constexpr int kStartY = 100;
  for (int i = 0; i < 36; ++i) {
    const int x = kStartX + (i % kColumns) * kCellWidth;
    const int y = kStartY + (i / kColumns) * kCellHeight;
    const bool active = i == selected;
    surface.FillRect(x, y, 28, 15, active ? theme.accent : theme.bg);
    surface.StrokeRect(x, y, 28, 15, active ? theme.text : theme.panel_border);
    std::string glyph(1, chars[static_cast<std::size_t>(i)]);
    if (glyph == " ") glyph = "_";
    widgets::DrawContentText(surface, x + 11, y + 4, glyph,
                      active ? theme.bg : theme.text, 1);
  }
}

void DrawCountryKeyboard(render::Surface240& surface, const render::Theme& theme,
                         const int selected) {
  constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  constexpr int kColumns = 7;
  constexpr int kCellWidth = 26;
  constexpr int kCellHeight = 20;
  constexpr int kStartX = 29;
  constexpr int kStartY = 106;
  for (int i = 0; i < static_cast<int>(kChars.size()); ++i) {
    const int x = kStartX + (i % kColumns) * kCellWidth;
    const int y = kStartY + (i / kColumns) * kCellHeight;
    const bool active = i == selected;
    surface.FillRect(x, y, 22, 16, active ? theme.accent : theme.bg);
    surface.StrokeRect(x, y, 22, 16, active ? theme.text : theme.panel_border);
    widgets::DrawContentText(surface, x + 8, y + 4, std::string(1, kChars[i]),
                      active ? theme.bg : theme.text, 1);
  }
}

}  // namespace

void DrawWifi(render::Surface240& surface,
              const render::Theme& theme,
              const WifiView view,
              const std::string& connected_ssid,
              const int connected_signal,
              const bool wifi_enabled,
              const std::string& country,
              const std::vector<WifiNetworkItem>& networks,
              int selected,
              const std::string& password_or_country,
              const int keyboard_page,
              const std::string& status) {
  if (view == WifiView::Overview) {
    Header(surface, theme, "WI-FI");
    surface.FillRect(16, 42, 208, 46, theme.bg);
    surface.StrokeRect(16, 42, 208, 46, theme.panel_border);
    const bool connected = !connected_ssid.empty();
    surface.FillRect(25, 56, 8, 8, connected ? theme.success : theme.accent);
    widgets::DrawContentText(surface, 42, 50, connected ? "CONNECTED" :
                      (wifi_enabled ? "NOT CONNECTED" : "WI-FI OFF"),
                      connected ? theme.success : theme.text_dim, 1);
    widgets::DrawContentText(surface, 42, 66,
                      connected ? Fit(connected_ssid, 21) : "Choose a network",
                      theme.text, 1);
    if (connected) Signal(surface, 195, 57, connected_signal, theme);

    const std::vector<std::string> rows = {
        "Browse nearby networks",
        connected ? "Disconnect from Wi-Fi" : "Disconnect (not connected)",
        "Wireless region: " + (country.empty() ? std::string("SET COUNTRY") : country),
    };
    widgets::DrawList(surface, 16, 104, 208, 84, 28, rows, selected, theme);
    if (!status.empty()) widgets::DrawContentText(surface, 16, 196, Fit(status, 34), theme.text_dim, 1);
    widgets::DrawMenuFooter(surface, theme, "A:SELECT  B:TOOLS");
    return;
  }

  if (view == WifiView::Networks) {
    Header(surface, theme, "NEARBY WI-FI");
    widgets::DrawMenuFooter(surface, theme, "A:CONNECT  X:RESCAN  B:BACK");
    if (networks.empty()) {
      widgets::DrawContentText(surface, 16, 90, "NO NETWORKS FOUND", theme.text_dim, 1);
    }
    const int first = std::max(0, selected - 4);
    for (int row = 0; row < 7 && first + row < static_cast<int>(networks.size()); ++row) {
      const int i = first + row;
      const auto& network = networks[static_cast<std::size_t>(i)];
      const int y = 44 + row * 22;
      const bool active = i == selected;
      surface.FillRect(16, y, 208, 19, active ? theme.accent : theme.bg);
      if (active) surface.FillRect(16, y, 3, 19, theme.accent);
      widgets::DrawContentText(surface, 22, y + 5, network.active ? "*" : " ",
                        active ? theme.bg : theme.success, 1);
      widgets::DrawContentText(surface, 32, y + 5, Fit(network.ssid, 22),
                        active ? theme.bg : theme.text, 1);
      if (network.secured) widgets::DrawContentText(surface, 172, y + 5, "LOCK", active ? theme.bg : theme.text_dim, 1);
      Signal(surface, 199, y + 2, network.signal, theme);
    }
    if (!status.empty()) widgets::DrawContentText(surface, 16, 198, Fit(status, 34), theme.text_dim, 1);
    return;
  }

  if (view == WifiView::Password) {
    Header(surface, theme, "CONNECT TO " + Fit(connected_ssid, 20));
    widgets::DrawContentText(surface, 16, 42, "PASSWORD", theme.text_dim, 1);
    const std::string hidden(password_or_country.size(), '*');
    surface.FillRect(16, 56, 208, 26, theme.bg);
    surface.StrokeRect(16, 56, 208, 26, theme.panel_border);
    widgets::DrawContentText(surface, 23, 65, hidden.empty() ? "(enter password)" : Fit(hidden, 31), theme.text, 1);
    widgets::DrawContentText(surface, 16, 88,
                      keyboard_page == 0 ? "lowercase" :
                      (keyboard_page == 1 ? "UPPERCASE" : "symbols"),
                      theme.text_dim, 1);
    DrawKeyboard(surface, theme, Keyboard(keyboard_page), selected);
    widgets::DrawMenuFooter(surface, theme, "A:TYPE Y:DEL X:JOIN B:BACK");
    widgets::DrawContentText(surface, 108, 88, "L/R:PAGE", theme.text_dim);
    return;
  }

  Header(surface, theme, "WIRELESS REGION");
  widgets::DrawContentText(surface, 16, 42, "CURRENT: " + (country.empty() ? std::string("NOT SET") : country), theme.text_dim, 1);
  widgets::DrawContentText(surface, 16, 60, "NEW CODE: " + (password_or_country.empty() ? std::string("__") : password_or_country), theme.text, 1);
  widgets::DrawContentText(surface, 16, 80, "Choose two country letters", theme.text_dim, 1);
  DrawCountryKeyboard(surface, theme, selected);
  if (!status.empty()) widgets::DrawContentText(surface, 16, 202, Fit(status, 34), theme.text_dim, 1);
  widgets::DrawMenuFooter(surface, theme, "A:TYPE Y:DEL X:SET B:BACK");
}

}  // namespace gb::ui::screens
