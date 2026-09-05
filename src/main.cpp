#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>

#include "core/logging.h"
#include "core/battery_status.h"
#include "core/launch_config.h"
#include "core/play_session.h"
#include "core/settings.h"
#include "core/retroarch_input.h"
#include "core/time.h"
#include "db/db.h"
#include "platform/input_evdev.h"
#include "platform/battery_ina219.h"
#include "platform/platform.h"
#include "platform/platform_fbdev.h"
#include "platform/platform_sdl.h"
#include "platform/proc.h"
#include "render/surface_240.h"
#include "render/theme.h"
#include "scrape/jobs.h"
#include "ui/screens/home.h"
#include "ui/screens/update.h"
#include "ui/ui_state.h"
#include "ui/widgets/list.h"
#include "ui/widgets/text.h"
#include "ui/widgets/chrome.h"

namespace {
constexpr std::uint64_t kPostLaunchInputBlockMs = 1200;
constexpr std::uint16_t kLinuxKeyEsc = 1;

struct Args {
  std::string presenter = "sdl";
  std::string db_path = "./data/catalog.db";
  std::string gblaunch = "gblaunch";
  std::string defaults_json = "./config/defaults.json";
  std::string systems_dir = "./config/systems.d";
  std::string settings_path = "./config/user_settings.json";
  std::string diagnostics_dir = "./data";
  std::string artwork_dir;
  std::string battery_i2c = "/dev/i2c-1";
  std::uint8_t battery_address = 0x43;
  bool battery_disabled = false;
  std::string fbdev = "/dev/fb1";
  std::string input_evdev = "auto";
  bool tv_mode = false;
  int scale = 3;
};

constexpr std::array<gb::platform::Button, 25> kButtonOrder = {
    gb::platform::Button::Up,     gb::platform::Button::Down,
    gb::platform::Button::Left,   gb::platform::Button::Right,
    gb::platform::Button::A,      gb::platform::Button::B,
    gb::platform::Button::X,      gb::platform::Button::Y,
    gb::platform::Button::L,      gb::platform::Button::R,
    gb::platform::Button::L2,     gb::platform::Button::R2,
    gb::platform::Button::L3,     gb::platform::Button::R3,
    gb::platform::Button::Start,  gb::platform::Button::Select,
    gb::platform::Button::Guide,
    gb::platform::Button::LeftStickUp,
    gb::platform::Button::LeftStickDown,
    gb::platform::Button::LeftStickLeft,
    gb::platform::Button::LeftStickRight,
    gb::platform::Button::RightStickUp,
    gb::platform::Button::RightStickDown,
    gb::platform::Button::RightStickLeft,
    gb::platform::Button::RightStickRight,
};

constexpr std::size_t kButtonCount = kButtonOrder.size();

std::size_t ButtonIndex(const gb::platform::Button button) {
  for (std::size_t i = 0; i < kButtonOrder.size(); ++i) {
    if (kButtonOrder[i] == button) {
      return i;
    }
  }
  return 0;
}

const char* ButtonName(const gb::platform::Button button) {
  switch (button) {
    case gb::platform::Button::Up:
      return "Up";
    case gb::platform::Button::Down:
      return "Down";
    case gb::platform::Button::Left:
      return "Left";
    case gb::platform::Button::Right:
      return "Right";
    case gb::platform::Button::A:
      return "A";
    case gb::platform::Button::B:
      return "B";
    case gb::platform::Button::X:
      return "X";
    case gb::platform::Button::Y:
      return "Y";
    case gb::platform::Button::L:
      return "L";
    case gb::platform::Button::R:
      return "R";
    case gb::platform::Button::L2:
      return "L2";
    case gb::platform::Button::R2:
      return "R2";
    case gb::platform::Button::L3:
      return "L3";
    case gb::platform::Button::R3:
      return "R3";
    case gb::platform::Button::Start:
      return "Start";
    case gb::platform::Button::Select:
      return "Select";
    case gb::platform::Button::Guide:
      return "Guide";
    case gb::platform::Button::LeftStickUp:
      return "L-Up";
    case gb::platform::Button::LeftStickDown:
      return "L-Down";
    case gb::platform::Button::LeftStickLeft:
      return "L-Left";
    case gb::platform::Button::LeftStickRight:
      return "L-Right";
    case gb::platform::Button::RightStickUp:
      return "R-Up";
    case gb::platform::Button::RightStickDown:
      return "R-Down";
    case gb::platform::Button::RightStickLeft:
      return "R-Left";
    case gb::platform::Button::RightStickRight:
      return "R-Right";
  }
  return "A";
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string Trim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

std::string ExpandHomePath(const std::string& value) {
  if (value.empty() || value[0] != '~') {
    return value;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    return value;
  }
  if (value.size() == 1) {
    return std::string(home);
  }
  if (value[1] == '/' || value[1] == '\\') {
    return (std::filesystem::path(home) / value.substr(2)).string();
  }
  // Leave "~user" unmodified for now.
  return value;
}

std::string Ellipsize(std::string s, const std::size_t max_len) {
  if (s.size() <= max_len) {
    return s;
  }
  if (max_len <= 3) {
    return s.substr(0, max_len);
  }
  s.resize(max_len - 3);
  s += "...";
  return s;
}

std::vector<std::string> Split(const std::string& text, const char delim) {
  std::vector<std::string> out;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, delim)) {
    out.push_back(item);
  }
  return out;
}

bool ParseHex16(const std::string& text, std::uint16_t& out) {
  if (text.empty()) {
    return false;
  }
  try {
    const unsigned long value = std::stoul(text, nullptr, 16);
    if (value > 0xFFFFul) {
      return false;
    }
    out = static_cast<std::uint16_t>(value);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::string Hex16(const std::uint16_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out(4, '0');
  out[0] = kHex[(value >> 12) & 0xF];
  out[1] = kHex[(value >> 8) & 0xF];
  out[2] = kHex[(value >> 4) & 0xF];
  out[3] = kHex[value & 0xF];
  return out;
}

bool ParseButtonName(std::string name, gb::platform::Button& out) {
  name = ToLower(Trim(name));
  for (const auto button : kButtonOrder) {
    if (ToLower(ButtonName(button)) == name) {
      out = button;
      return true;
    }
  }
  return false;
}

std::array<gb::platform::Button, kButtonCount> IdentityButtonMap() {
  std::array<gb::platform::Button, kButtonCount> out {};
  for (std::size_t i = 0; i < kButtonCount; ++i) {
    out[i] = kButtonOrder[i];
  }
  return out;
}

struct InputProfile {
  std::string device_match;
  std::string device_match_lower;
  std::uint16_t bus_type = 0;
  std::uint16_t vendor = 0;
  std::uint16_t product = 0;
  bool has_id = false;
  std::array<gb::platform::Button, kButtonCount> source_to_target = IdentityButtonMap();
  std::unordered_map<std::uint16_t, gb::platform::Button> keycode_to_target;
  std::array<std::string, kButtonCount> retroarch_bindings;
};

enum class BluetoothDeviceKind {
  Controller,
  Audio,
  Other,
};

struct BluetoothDevice {
  std::string address;
  std::string name;
  BluetoothDeviceKind kind = BluetoothDeviceKind::Other;
  bool paired = false;
  bool connected = false;
};

enum class PendingBluetoothAction {
  None,
  RefreshScanned,
  RefreshPaired,
  RefreshBoth,
  PairSelectedFromScanned,
  ConnectSelectedFromPaired,
  DisconnectSelectedFromPaired,
  ForgetAddressFromPaired,
  DisconnectAllPaired,
};

enum class BluetoothScanFilter {
  All,
  Controllers,
  Audio,
};

enum class BluetoothModalType {
  None,
  ConfirmForget,
  ConfirmDisconnectAll,
};

struct WifiNetwork {
  std::string ssid;
  int signal = 0;
  bool secured = false;
  bool active = false;
};

enum class PendingWifiAction {
  None,
  RefreshStatus,
  Scan,
  Connect,
  Disconnect,
  SetCountry,
};

enum class GameListView {
  System,
  Recent,
  Favorites,
};

struct UpdateStatus {
  std::string phase = "READY";
  std::string message = "Press A to check for updates";
  int progress = 0;
  int os_updates = 0;
  bool shell_update = false;
  bool reboot_required = false;
};

struct LibraryState {
  bool db_ready = false;
  std::string status;

  gb::core::RuntimeSettings settings;
  gb::core::BrowseState browse;
  gb::core::BrowseState saved_browse;
  std::uint64_t browse_changed_ms = 0;
  int continue_game_id = 0;
  gb::db::GameDetails menu_game;
  gb::ui::Screen game_menu_return = gb::ui::Screen::GameList;
  int context_selected = 0;
  bool can_resume = false;
  bool can_resume_backup = false;
  gb::core::PlayMode pending_play_mode = gb::core::PlayMode::Fresh;

  std::vector<gb::db::SystemSummary> systems;
  std::vector<gb::db::GameSummary> games;
  std::vector<gb::db::LibraryRootState> library_roots;
  std::string system_artwork_dir;
  GameListView game_list_view = GameListView::System;
  gb::db::GameDetails details;
  bool details_ready = false;
  bool battery_start_held = false;
  bool battery_start_chord_used = false;
  bool battery_hud_visible = false;
  std::uint64_t battery_hold_started_ms = 0;
  std::uint64_t battery_last_read_ms = 0;
  std::uint64_t battery_auto_hud_until_ms = 0;
  std::uint64_t battery_last_flash_ms = 0;
  bool battery_sample_seen = false;
  gb::core::BatteryStatus battery_status;
  std::string battery_error;
  int volume_percent = -1;
  std::uint64_t volume_last_read_ms = 0;
  std::uint64_t volume_last_adjust_ms = 0;
  std::string volume_error;
  UpdateStatus update_status;
  std::uint64_t update_last_read_ms = 0;
  std::uint64_t update_notice_until_ms = 0;
  bool update_notice_announced = false;
  bool tv_mode = false;
  bool tv_external_controller = false;
  std::uint64_t tv_mode_notice_until_ms = 0;

  int system_selected = 0;
  int game_selected = 0;
  int settings_selected = 0;
  int tools_selected = 0;

  bool scrape_active = false;
  gb::scrape::ScrapeSession scrape_session;
  gb::scrape::ScrapeProgress scrape_progress;

  int pending_launch_game_id = 0;
  bool pending_launch_retroarch_menu = false;
  bool tools_exit_confirm = false;

  std::string current_system_id;
  std::string current_system_name;

  gb::ui::Screen launch_options_return_screen = gb::ui::Screen::Systems;
  int launch_options_selected = 0;
  std::string launch_options_scope_type;  // "system" or "game"
  std::string launch_options_scope_id;    // system_id or rom_path
  std::string launch_options_system_id;
  std::string launch_options_title;
  int launch_options_game_id = 0;
  std::string launch_options_default_core;
  std::string launch_options_effective_core;
  std::string launch_options_effective_source;
  std::string launch_options_effective_config;
  std::string launch_options_effective_warning;
  std::vector<std::string> launch_options_core_paths;  // index 0 => inherit/default
  int launch_options_core_selected = 0;
  int launch_options_audio_selected = 0;
  int launch_options_video_selected = 0;

  std::vector<InputProfile> input_profiles;
  std::vector<gb::platform::EvdevDeviceInfo> input_devices;
  std::string last_input_device;
  std::uint16_t last_input_bus_type = 0;
  std::uint16_t last_input_vendor = 0;
  std::uint16_t last_input_product = 0;
  bool last_input_has_id = false;
  int last_input_joypad_index = -1;
  std::string last_input_summary;
  std::uint64_t last_input_ms = 0;
  std::uint16_t last_input_raw_type = 0;
  std::uint16_t last_input_raw_code = 0;
  std::string input_debug_line1;
  std::string input_debug_line2;
  std::uint64_t ignore_input_until_ms = 0;

  bool input_capture_active = false;
  int input_capture_step = 0;
  bool input_capture_arming = false;
  gb::platform::Button input_capture_arm_button = gb::platform::Button::A;
  std::uint16_t input_capture_arm_raw_type = 0;
  std::uint16_t input_capture_arm_raw_code = 0;
  std::uint64_t input_capture_arm_until_ms = 0;
  std::string input_capture_device;
  std::uint16_t input_capture_bus_type = 0;
  std::uint16_t input_capture_vendor = 0;
  std::uint16_t input_capture_product = 0;
  bool input_capture_has_id = false;
  std::array<gb::platform::Button, kButtonCount> input_capture_map = IdentityButtonMap();
  std::unordered_map<std::uint16_t, gb::platform::Button> input_capture_keycode_map;
  std::array<std::string, kButtonCount> input_capture_retroarch_bindings;

  std::array<bool, kButtonCount> input_test_seen {};
  std::string input_test_last = "none";
  int input_test_press_count = 0;

  bool bluetooth_show_paired = false;
  int bluetooth_scanned_selected = 0;
  int bluetooth_paired_selected = 0;
  std::vector<BluetoothDevice> bluetooth_scanned_devices;
  std::vector<BluetoothDevice> bluetooth_paired_devices;
  BluetoothScanFilter bluetooth_scan_filter = BluetoothScanFilter::All;
  PendingBluetoothAction pending_bluetooth_action = PendingBluetoothAction::None;
  std::string bluetooth_pending_address;
  BluetoothModalType bluetooth_modal = BluetoothModalType::None;
  std::string bluetooth_modal_address;
  std::string bluetooth_modal_name;

  gb::ui::screens::WifiView wifi_view = gb::ui::screens::WifiView::Overview;
  int wifi_selected = 0;
  int wifi_keyboard_page = 0;
  std::vector<WifiNetwork> wifi_networks;
  std::string wifi_connected_ssid;
  int wifi_connected_signal = 0;
  bool wifi_enabled = true;
  std::string wifi_country;
  std::string wifi_selected_ssid;
  std::string wifi_password;
  std::string wifi_country_entry;
  PendingWifiAction pending_wifi_action = PendingWifiAction::None;
};

std::string DefaultGblaunchPath(const char* argv0) {
  const std::filesystem::path exe =
      argv0 ? std::filesystem::path(argv0) : std::filesystem::path();
  if (exe.has_parent_path()) {
    return (exe.parent_path() / "gblaunch").string();
  }
  return "gblaunch";
}

Args ParseArgs(const int argc, char** argv) {
  Args out;
  out.gblaunch = DefaultGblaunchPath(argc > 0 ? argv[0] : nullptr);

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--presenter" && i + 1 < argc) {
      out.presenter = argv[++i];
      continue;
    }
    if (arg == "--db" && i + 1 < argc) {
      out.db_path = argv[++i];
      continue;
    }
    if (arg == "--gblaunch" && i + 1 < argc) {
      out.gblaunch = argv[++i];
      continue;
    }
    if (arg == "--defaults" && i + 1 < argc) {
      out.defaults_json = argv[++i];
      continue;
    }
    if (arg == "--systems-dir" && i + 1 < argc) {
      out.systems_dir = argv[++i];
      continue;
    }
    if (arg == "--settings" && i + 1 < argc) {
      out.settings_path = argv[++i];
      continue;
    }
    if (arg == "--diagnostics-dir" && i + 1 < argc) {
      out.diagnostics_dir = argv[++i];
      continue;
    }
    if (arg == "--artwork-dir" && i + 1 < argc) {
      out.artwork_dir = argv[++i];
      continue;
    }
    if (arg == "--battery-i2c" && i + 1 < argc) {
      out.battery_i2c = argv[++i];
      continue;
    }
    if (arg == "--battery-address" && i + 1 < argc) {
      const auto parsed = std::strtoul(argv[++i], nullptr, 0);
      if (parsed <= 0x7F) {
        out.battery_address = static_cast<std::uint8_t>(parsed);
      }
      continue;
    }
    if (arg == "--battery-disabled") {
      out.battery_disabled = true;
      continue;
    }
    if (arg == "--fbdev" && i + 1 < argc) {
      out.fbdev = argv[++i];
      continue;
    }
    if (arg == "--tv-mode") {
      out.tv_mode = true;
      continue;
    }
    if (arg == "--input-evdev" && i + 1 < argc) {
      out.input_evdev = argv[++i];
      continue;
    }
    if (arg == "--scale" && i + 1 < argc) {
      out.scale = std::atoi(argv[++i]);
      if (out.scale < 1) {
        out.scale = 1;
      }
      continue;
    }
  }
  return out;
}

std::vector<InputProfile> ParseInputProfiles(const std::string& encoded) {
  std::vector<InputProfile> out;
  if (encoded.empty()) {
    return out;
  }

  for (const auto& profile_chunk_raw : Split(encoded, ';')) {
    const std::string profile_chunk = Trim(profile_chunk_raw);
    if (profile_chunk.empty()) {
      continue;
    }

    const auto pipe_pos = profile_chunk.find('|');
    if (pipe_pos == std::string::npos || pipe_pos == 0) {
      continue;
    }

    InputProfile profile;
    std::string header = Trim(profile_chunk.substr(0, pipe_pos));
    const auto at_pos = header.find('@');
    if (at_pos != std::string::npos) {
      const std::string id_part = Trim(header.substr(0, at_pos));
      profile.device_match = Trim(header.substr(at_pos + 1));
      const auto ids = Split(id_part, ':');
      if (ids.size() == 3) {
        std::uint16_t bus = 0;
        std::uint16_t vid = 0;
        std::uint16_t pid = 0;
        if (ParseHex16(ids[0], bus) && ParseHex16(ids[1], vid) &&
            ParseHex16(ids[2], pid)) {
          profile.bus_type = bus;
          profile.vendor = vid;
          profile.product = pid;
          profile.has_id = true;
        }
      }
    } else {
      profile.device_match = header;
    }
    profile.device_match_lower = ToLower(profile.device_match);
    profile.source_to_target = IdentityButtonMap();

    const std::string mappings = profile_chunk.substr(pipe_pos + 1);
    for (const auto& pair_raw : Split(mappings, ',')) {
      const std::string pair = Trim(pair_raw);
      if (pair.empty()) {
        continue;
      }
      const auto eq_pos = pair.find('=');
      if (eq_pos == std::string::npos || eq_pos == 0) {
        continue;
      }

      const std::string left = Trim(pair.substr(0, eq_pos));
      const std::string right = Trim(pair.substr(eq_pos + 1));
      if (left.size() > 1 && (left[0] == 'R' || left[0] == 'r')) {
        gb::platform::Button target = gb::platform::Button::A;
        if (ParseButtonName(left.substr(1), target) &&
            (right.rfind("key:", 0) == 0 || right.rfind("btn:", 0) == 0 ||
             right.rfind("axis:", 0) == 0)) {
          profile.retroarch_bindings[ButtonIndex(target)] = right;
          continue;
        }
      }
      if (left.size() > 1 && (left[0] == 'K' || left[0] == 'k')) {
        try {
          const unsigned long code = std::stoul(left.substr(1));
          if (code <= 0xFFFFul) {
            gb::platform::Button dst = gb::platform::Button::A;
            if (ParseButtonName(right, dst)) {
              profile.keycode_to_target[static_cast<std::uint16_t>(code)] = dst;
            }
          }
        } catch (const std::exception&) {
        }
        continue;
      }

      gb::platform::Button src = gb::platform::Button::A;
      gb::platform::Button dst = gb::platform::Button::A;
      if (!ParseButtonName(left, src) || !ParseButtonName(right, dst)) {
        continue;
      }
      profile.source_to_target[ButtonIndex(src)] = dst;
    }

    out.push_back(profile);
  }

  return out;
}

std::string EncodeInputProfiles(const std::vector<InputProfile>& profiles) {
  std::string out;
  bool first_profile = true;

  for (const auto& profile : profiles) {
    const std::string match = Trim(profile.device_match);
    if (match.empty()) {
      continue;
    }
    if (!first_profile) {
      out.push_back(';');
    }
    first_profile = false;

    if (profile.has_id) {
      out += Hex16(profile.bus_type);
      out.push_back(':');
      out += Hex16(profile.vendor);
      out.push_back(':');
      out += Hex16(profile.product);
      out.push_back('@');
    }
    out += match;
    out.push_back('|');
    for (std::size_t i = 0; i < kButtonCount; ++i) {
      if (i > 0) {
        out.push_back(',');
      }
      out += ButtonName(kButtonOrder[i]);
      out.push_back('=');
      out += ButtonName(profile.source_to_target[i]);
    }
    if (!profile.keycode_to_target.empty()) {
      std::vector<std::pair<std::uint16_t, gb::platform::Button>> keys;
      keys.reserve(profile.keycode_to_target.size());
      for (const auto& kv : profile.keycode_to_target) {
        keys.push_back(kv);
      }
      std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
      });
      for (const auto& kv : keys) {
        out.push_back(',');
        out += "K";
        out += std::to_string(kv.first);
        out.push_back('=');
        out += ButtonName(kv.second);
      }
    }
    for (std::size_t i = 0; i < kButtonCount; ++i) {
      if (profile.retroarch_bindings[i].empty()) {
        continue;
      }
      out += ",R";
      out += ButtonName(kButtonOrder[i]);
      out.push_back('=');
      out += profile.retroarch_bindings[i];
    }
  }

  return out;
}

int FindBestProfileIndex(const std::vector<InputProfile>& profiles,
                         const std::string& device_name,
                         const std::uint16_t bus_type,
                         const std::uint16_t vendor,
                         const std::uint16_t product) {
  int best_by_name = -1;
  std::size_t best_name_len = 0;
  const std::string lower = ToLower(device_name);
  for (std::size_t i = 0; i < profiles.size(); ++i) {
    if (profiles[i].has_id && profiles[i].bus_type == bus_type &&
        profiles[i].vendor == vendor && profiles[i].product == product) {
      return static_cast<int>(i);
    }
    if (!profiles[i].device_match_lower.empty() &&
        lower.find(profiles[i].device_match_lower) != std::string::npos &&
        profiles[i].device_match_lower.size() > best_name_len) {
      best_name_len = profiles[i].device_match_lower.size();
      best_by_name = static_cast<int>(i);
    }
  }
  return best_by_name;
}

int FindKeycodeFallbackProfileIndex(const std::vector<InputProfile>& profiles,
                                    const gb::platform::InputEvent& ev) {
  if (ev.raw_code == 0) {
    return -1;
  }
  int vendor_match_idx = -1;
  int any_idx = -1;
  for (std::size_t i = 0; i < profiles.size(); ++i) {
    const auto it = profiles[i].keycode_to_target.find(ev.raw_code);
    if (it == profiles[i].keycode_to_target.end()) {
      continue;
    }
    if (any_idx < 0) {
      any_idx = static_cast<int>(i);
    }
    if (profiles[i].has_id && profiles[i].bus_type == ev.bus_type &&
        profiles[i].vendor == ev.vendor) {
      return static_cast<int>(i);
    }
    if (vendor_match_idx < 0 && profiles[i].has_id &&
        profiles[i].vendor == ev.vendor) {
      vendor_match_idx = static_cast<int>(i);
    }
  }
  if (vendor_match_idx >= 0) {
    return vendor_match_idx;
  }
  return any_idx;
}

gb::platform::Button ApplyInputMapping(const LibraryState& lib,
                                       const gb::platform::InputEvent& ev,
                                       bool* out_has_mapping = nullptr,
                                       std::string* out_debug = nullptr) {
  if (out_has_mapping != nullptr) {
    *out_has_mapping = true;
  }
  if (out_debug != nullptr) {
    *out_debug = "unknown";
  }

  const int profile_idx = FindBestProfileIndex(lib.input_profiles, ev.device_name,
                                               ev.bus_type, ev.vendor, ev.product);
  int use_idx = profile_idx;
  if (use_idx < 0) {
    use_idx = FindKeycodeFallbackProfileIndex(lib.input_profiles, ev);
    if (use_idx < 0) {
      if (!ev.mapped_button && out_has_mapping != nullptr) {
        *out_has_mapping = false;
      }
      if (out_debug != nullptr) {
        *out_debug = !ev.mapped_button ? "no-profile drop" : "no-profile passthrough";
      }
      return ev.button;
    }
  }

  const auto& profile = lib.input_profiles[static_cast<std::size_t>(use_idx)];
  const bool keyboard_like_event = (ev.is_keyboard || !ev.mapped_button) && ev.raw_code > 0;
  const bool has_explicit_keycode_map = !profile.keycode_to_target.empty();
  if (ev.raw_code > 0) {
    const auto it = profile.keycode_to_target.find(ev.raw_code);
    if (it != profile.keycode_to_target.end()) {
      if (out_debug != nullptr) {
        *out_debug = "p" + std::to_string(use_idx) + " keycode-hit";
      }
      return it->second;
    }
    if ((keyboard_like_event && has_explicit_keycode_map) || !ev.mapped_button) {
      if (out_has_mapping != nullptr) {
        *out_has_mapping = false;
      }
      if (out_debug != nullptr) {
        *out_debug = "p" + std::to_string(use_idx) + " keycode-miss drop";
      }
      return ev.button;
    }
  }

  if (!ev.mapped_button) {
    if (out_has_mapping != nullptr) {
      *out_has_mapping = false;
    }
    if (out_debug != nullptr) {
      *out_debug = "unmapped drop";
    }
    return ev.button;
  }
  if (out_debug != nullptr) {
    *out_debug = "p" + std::to_string(use_idx) + " logical-map";
  }
  return profile.source_to_target[ButtonIndex(ev.button)];
}

void UpsertInputProfile(LibraryState& lib,
                        const std::string& device_name,
                        const std::uint16_t bus_type,
                        const std::uint16_t vendor,
                        const std::uint16_t product,
                        const bool has_id,
                        const std::array<gb::platform::Button, kButtonCount>& map,
                        const std::unordered_map<std::uint16_t, gb::platform::Button>&
                            keycode_map,
                        const std::array<std::string, kButtonCount>&
                            retroarch_bindings) {
  const std::string match = Trim(device_name);
  if (match.empty()) {
    return;
  }

  const std::string lower = ToLower(match);
  for (auto& profile : lib.input_profiles) {
    if ((has_id && profile.has_id && profile.bus_type == bus_type &&
         profile.vendor == vendor && profile.product == product) ||
        (!has_id && profile.device_match_lower == lower)) {
      profile.device_match = match;
      profile.device_match_lower = lower;
      profile.bus_type = bus_type;
      profile.vendor = vendor;
      profile.product = product;
      profile.has_id = has_id;
      profile.source_to_target = map;
      profile.keycode_to_target = keycode_map;
      profile.retroarch_bindings = retroarch_bindings;
      return;
    }
  }

  InputProfile profile;
  profile.device_match = match;
  profile.device_match_lower = lower;
  profile.bus_type = bus_type;
  profile.vendor = vendor;
  profile.product = product;
  profile.has_id = has_id;
  profile.source_to_target = map;
  profile.keycode_to_target = keycode_map;
  profile.retroarch_bindings = retroarch_bindings;
  lib.input_profiles.push_back(profile);
}

void RemoveInputProfile(LibraryState& lib,
                        const std::string& device_name,
                        const std::uint16_t bus_type,
                        const std::uint16_t vendor,
                        const std::uint16_t product,
                        const bool has_id) {
  const std::string lower = ToLower(Trim(device_name));
  if (lower.empty() && !has_id) {
    return;
  }
  lib.input_profiles.erase(
      std::remove_if(lib.input_profiles.begin(), lib.input_profiles.end(),
                     [&](const InputProfile& p) {
                       if (has_id && p.has_id) {
                         return p.bus_type == bus_type && p.vendor == vendor &&
                                p.product == product;
                       }
                       return !lower.empty() && p.device_match_lower == lower;
                     }),
      lib.input_profiles.end());
}

std::string ProfilePreview(const LibraryState& lib,
                           const std::string& device_name,
                           const std::uint16_t bus_type,
                           const std::uint16_t vendor,
                           const std::uint16_t product) {
  const int profile_idx =
      FindBestProfileIndex(lib.input_profiles, device_name, bus_type, vendor, product);
  if (profile_idx < 0) {
    return "No remap profile";
  }

  const auto& map = lib.input_profiles[static_cast<std::size_t>(profile_idx)].source_to_target;
  const auto key_count =
      lib.input_profiles[static_cast<std::size_t>(profile_idx)].keycode_to_target.size();
  const auto ra_count = static_cast<int>(std::count_if(
      lib.input_profiles[static_cast<std::size_t>(profile_idx)]
          .retroarch_bindings.begin(),
      lib.input_profiles[static_cast<std::size_t>(profile_idx)]
          .retroarch_bindings.end(),
      [](const std::string& value) { return !value.empty(); }));
  std::string out = "A<-";
  out += ButtonName(map[ButtonIndex(gb::platform::Button::A)]);
  out += " B<-";
  out += ButtonName(map[ButtonIndex(gb::platform::Button::B)]);
  out += " X<-";
  out += ButtonName(map[ButtonIndex(gb::platform::Button::X)]);
  out += " Y<-";
  out += ButtonName(map[ButtonIndex(gb::platform::Button::Y)]);
  if (key_count > 0) {
    out += " K:";
    out += std::to_string(key_count);
  }
  out += " RA:";
  out += std::to_string(ra_count);
  return out;
}

constexpr std::array<gb::platform::Button, kButtonCount> kRemapCaptureOrder = {
    gb::platform::Button::Up,     gb::platform::Button::Down,
    gb::platform::Button::Left,   gb::platform::Button::Right,
    gb::platform::Button::A,      gb::platform::Button::B,
    gb::platform::Button::X,      gb::platform::Button::Y,
    gb::platform::Button::L,      gb::platform::Button::R,
    gb::platform::Button::L2,     gb::platform::Button::R2,
    gb::platform::Button::L3,     gb::platform::Button::R3,
    gb::platform::Button::Start,  gb::platform::Button::Select,
    gb::platform::Button::Guide,
    gb::platform::Button::LeftStickUp,
    gb::platform::Button::LeftStickDown,
    gb::platform::Button::LeftStickLeft,
    gb::platform::Button::LeftStickRight,
    gb::platform::Button::RightStickUp,
    gb::platform::Button::RightStickDown,
    gb::platform::Button::RightStickLeft,
    gb::platform::Button::RightStickRight,
};

constexpr std::uint64_t kRemapArmDelayMs = 250;

void ClampSelection(int& selected, const int count) {
  if (count <= 0) {
    selected = 0;
    return;
  }
  if (selected < 0) {
    selected = 0;
    return;
  }
  if (selected >= count) {
    selected = count - 1;
  }
}

void LoadSystems(gb::db::Database& db, LibraryState& lib) {
  std::string selected_system_id;
  if (lib.system_selected >= 0 &&
      lib.system_selected < static_cast<int>(lib.systems.size())) {
    selected_system_id = lib.systems[static_cast<std::size_t>(lib.system_selected)].id;
  }

  if (!lib.db_ready) {
    lib.systems.clear();
    return;
  }

  if (!db.ListSystems(lib.systems)) {
    lib.status = "DB READ ERROR";
    return;
  }

  // Apps are the console's own software library. Present it as GAMEBIRD even
  // for databases created before the system configuration was renamed.
  for (auto& system : lib.systems) {
    if (system.id == "apps") {
      system.name = "GAMEBIRD";
      break;
    }
  }

  if (!db.ListLibraryRoots(lib.library_roots)) {
    lib.status = "DB ROOT READ ERROR";
  }

  if (!selected_system_id.empty()) {
    const auto selected = std::find_if(
        lib.systems.begin(), lib.systems.end(),
        [&selected_system_id](const gb::db::SystemSummary& system) {
          return system.id == selected_system_id;
        });
    if (selected != lib.systems.end()) {
      lib.system_selected =
          static_cast<int>(std::distance(lib.systems.begin(), selected));
    }
  }
  ClampSelection(lib.system_selected, static_cast<int>(lib.systems.size()));
}

void SelectGameBirdSystem(LibraryState& lib) {
  const auto gamebird = std::find_if(
      lib.systems.begin(), lib.systems.end(),
      [](const gb::db::SystemSummary& system) { return system.id == "apps"; });
  if (gamebird != lib.systems.end()) {
    lib.system_selected =
        static_cast<int>(std::distance(lib.systems.begin(), gamebird));
  }
}

std::string BrowseKey(const LibraryState& lib) {
  if (lib.game_list_view == GameListView::Recent) return "recent";
  if (lib.game_list_view == GameListView::Favorites) return "favorites";
  return "system:" + lib.current_system_id;
}

void RememberCurrentGame(LibraryState& lib) {
  if (lib.game_selected >= 0 && lib.game_selected < static_cast<int>(lib.games.size()))
    lib.browse.selected_games[BrowseKey(lib)] = lib.games[lib.game_selected].id;
}

void RestoreGameSelection(LibraryState& lib) {
  const auto saved = lib.browse.selected_games.find(BrowseKey(lib));
  if (saved != lib.browse.selected_games.end()) {
    const auto it = std::find_if(lib.games.begin(), lib.games.end(),
        [&](const auto& game) { return game.id == saved->second; });
    if (it != lib.games.end()) lib.game_selected = static_cast<int>(it - lib.games.begin());
  }
  ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
}

void RememberBrowse(LibraryState& lib, const gb::ui::UIState& ui) {
  RememberCurrentGame(lib);
  if (ui.screen != gb::ui::Screen::Systems && ui.screen != gb::ui::Screen::GameList) return;
  lib.browse.screen = ui.screen == gb::ui::Screen::Systems ? "systems" : "games";
  lib.browse.view = lib.game_list_view == GameListView::Recent ? "recent" :
                    lib.game_list_view == GameListView::Favorites ? "favorites" : "system";
  if (!lib.systems.empty()) {
    ClampSelection(lib.system_selected, static_cast<int>(lib.systems.size()));
    lib.browse.system_id = lib.systems[lib.system_selected].id;
  }
}

void RefreshContinue(gb::db::Database& db, LibraryState& lib, gb::ui::UIState& ui) {
  std::vector<gb::db::GameSummary> recent;
  lib.continue_game_id = 0;
  ui.continue_available = false;
  ui.continue_title.clear();
  if (lib.db_ready && db.ListRecentGames(false, 1, recent) && !recent.empty()) {
    lib.continue_game_id = recent.front().id;
    ui.continue_available = true;
    ui.continue_title = recent.front().title;
  }
}

void RefreshGameMenu(gb::db::Database& db, LibraryState& lib, const Args& args) {
  lib.can_resume = lib.can_resume_backup = false;
  db.GetGameDetails(lib.menu_game.id, lib.menu_game);
  gb::core::EffectiveLaunch launch;
  std::string error;
  if (gb::core::ResolveEffectiveLaunch(db, lib.menu_game.id, launch, error)) {
    lib.can_resume = gb::core::HasContinueSave(args.db_path, launch);
    lib.can_resume_backup = gb::core::HasContinueSave(args.db_path, launch, true);
  }
}

void OpenGameMenu(gb::db::Database& db, gb::ui::UIState& ui, LibraryState& lib,
                  const Args& args, int id) {
  if (id <= 0 || !db.GetGameDetails(id, lib.menu_game)) {
    lib.status = "Game is no longer available";
    ui.needs_redraw = true;
    return;
  }
  lib.game_menu_return = ui.screen == gb::ui::Screen::Details
      ? lib.game_menu_return : ui.screen;
  lib.context_selected = 0;
  RefreshGameMenu(db, lib, args);
  lib.status.clear();
  ui.screen = gb::ui::Screen::GameMenu;
  ui.needs_redraw = true;
}

enum class GameAction { Resume, Fresh, Backup, Details, Favorite, Hide, Options, Home };
std::vector<std::pair<GameAction, std::string>> GameActions(const LibraryState& lib) {
  std::vector<std::pair<GameAction, std::string>> rows;
  if (lib.can_resume) rows.emplace_back(GameAction::Resume, "Resume");
  rows.emplace_back(GameAction::Fresh, lib.can_resume ? "Start fresh" : "Play");
  if (lib.can_resume_backup) rows.emplace_back(GameAction::Backup, "Resume previous save");
  rows.emplace_back(GameAction::Details, "Game details");
  rows.emplace_back(GameAction::Favorite, lib.menu_game.is_favorite ? "Remove favorite" : "Add to favorites");
  rows.emplace_back(GameAction::Hide, lib.menu_game.is_hidden ? "Unhide game" : "Hide game");
  rows.emplace_back(GameAction::Options, "Launch options");
  rows.emplace_back(GameAction::Home, "Main menu");
  return rows;
}

void LoadGamesForCurrentSystem(gb::db::Database& db, LibraryState& lib) {
  lib.games.clear();

  if (!lib.db_ready) {
    return;
  }
  if (lib.current_system_id.empty()) {
    return;
  }

  if (!db.ListGamesBySystem(lib.current_system_id, lib.settings.show_hidden_games,
                            lib.games)) {
    lib.status = "DB READ ERROR";
    return;
  }

  RestoreGameSelection(lib);
}

void LoadRecentGames(gb::db::Database& db, LibraryState& lib) {
  lib.games.clear();
  if (!lib.db_ready) {
    return;
  }
  if (!db.ListRecentGames(lib.settings.show_hidden_games, 30, lib.games)) {
    lib.status = "DB READ ERROR";
    return;
  }
  RestoreGameSelection(lib);
}

void LoadFavoriteGames(gb::db::Database& db, LibraryState& lib) {
  lib.games.clear();
  if (!lib.db_ready) {
    return;
  }
  if (!db.ListFavoriteGames(lib.settings.show_hidden_games, lib.games)) {
    lib.status = "DB READ ERROR";
    return;
  }
  RestoreGameSelection(lib);
}

void ReloadGameList(gb::db::Database& db, LibraryState& lib) {
  RememberCurrentGame(lib);
  switch (lib.game_list_view) {
    case GameListView::System:
      LoadGamesForCurrentSystem(db, lib);
      break;
    case GameListView::Recent:
      LoadRecentGames(db, lib);
      break;
    case GameListView::Favorites:
      LoadFavoriteGames(db, lib);
      break;
  }
}

void RefreshSelectedGameDetails(gb::db::Database& db, LibraryState& lib) {
  lib.details_ready = false;
  lib.details = gb::db::GameDetails{};
  if (lib.games.empty()) {
    return;
  }
  ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
  if (!db.GetGameDetails(lib.games[lib.game_selected].id, lib.details)) {
    lib.status = "DETAILS LOAD FAILED";
    return;
  }
  lib.details_ready = true;
}

std::string GameListTitle(const LibraryState& lib) {
  switch (lib.game_list_view) {
    case GameListView::Recent:
      return "RECENT";
    case GameListView::Favorites:
      return "FAVORITES";
    case GameListView::System:
      return lib.current_system_name.empty() ? "GAMES" : lib.current_system_name;
  }
  return "GAMES";
}

void SelectSystem(gb::db::Database& db,
                  LibraryState& lib,
                  const int selection) {
  if (lib.systems.empty()) {
    return;
  }
  RememberCurrentGame(lib);
  const int count = static_cast<int>(lib.systems.size());
  lib.system_selected = (selection % count + count) % count;
  const auto& system = lib.systems[static_cast<std::size_t>(lib.system_selected)];
  lib.current_system_id = system.id;
  lib.current_system_name = system.name;
  lib.game_list_view = GameListView::System;
  lib.game_selected = 0;
  LoadGamesForCurrentSystem(db, lib);
  RefreshSelectedGameDetails(db, lib);
}

void OpenSystem(gb::db::Database& db, gb::ui::UIState& ui, LibraryState& lib) {
  if (lib.systems.empty()) {
    return;
  }

  ClampSelection(lib.system_selected, static_cast<int>(lib.systems.size()));
  SelectSystem(db, lib, lib.system_selected);
  ui.screen = gb::ui::Screen::GameList;
  ui.needs_redraw = true;
}

std::vector<gb::ui::screens::SystemCarouselItem> BuildSystemCards(
    const LibraryState& lib) {
  std::vector<gb::ui::screens::SystemCarouselItem> out;
  out.reserve(lib.systems.size());

  for (const auto& sys : lib.systems) {
    const auto art_dir = std::filesystem::path(lib.system_artwork_dir);
    out.push_back(gb::ui::screens::SystemCarouselItem{
        .name = sys.name,
        .game_count = sys.game_count,
        .icon_path = (art_dir / (sys.id + "-icon.png")).string(),
        .logo_path = (art_dir / (sys.id + "-logo.png")).string(),
    });
  }

  return out;
}

std::vector<std::string> BuildGameRows(const LibraryState& lib) {
  std::vector<std::string> out;
  out.reserve(lib.games.size());

  for (const auto& game : lib.games) {
    std::string row;
    row += game.is_favorite ? "F " : "  ";
    row += game.title;
    out.push_back(std::move(row));
  }

  return out;
}

std::vector<std::string> BuildGameBrowserTitles(const LibraryState& lib) {
  std::vector<std::string> out;
  out.reserve(lib.games.size());
  for (const auto& game : lib.games) {
    out.push_back(game.title);
  }
  return out;
}

std::string LibraryUiSignature(const LibraryState& lib) {
  std::string signature;
  for (const auto& system : lib.systems) {
    signature += system.id;
    signature += ':';
    signature += std::to_string(system.game_count);
    signature += ';';
  }
  signature += '|';
  for (const auto& game : lib.games) {
    signature += std::to_string(game.id);
    signature += ':';
    signature += game.title;
    signature += ';';
  }
  return signature;
}

std::vector<std::string> BuildSettingsRows(const LibraryState& lib) {
  return {
      std::string("Diagnostics Overlay: ") +
          (lib.settings.show_diagnostics ? "ON" : "OFF"),
      std::string("Show Hidden Games: ") +
          (lib.settings.show_hidden_games ? "ON" : "OFF"),
      std::string("Scraper: ") +
          (lib.settings.scrape_provider == "none" ? "OFF" : "LIBRETRO"),
      "Scrape Missing Artwork",
      "Input Setup",
      "Save Settings",
  };
}

bool UpdateIsBusy(const UpdateStatus& update) {
  const std::string phase = ToLower(update.phase);
  return phase == "starting" || phase == "checking" ||
         phase == "preparing" || phase == "downloading" ||
         phase == "updating pi os" || phase == "building gamebird" ||
         phase == "installing gamebird" || phase == "finalizing" ||
         phase == "rebooting";
}

bool ReadUpdateStatus(LibraryState& lib) {
  std::ifstream input("/data/gamebird-update/status");
  if (!input) return false;

  UpdateStatus next;
  std::string line;
  while (std::getline(input, line)) {
    const auto equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, equals));
    const std::string value = Trim(line.substr(equals + 1));
    try {
      if (key == "phase") next.phase = value;
      else if (key == "message") next.message = value;
      else if (key == "progress") next.progress = std::clamp(std::stoi(value), 0, 100);
      else if (key == "os_updates") next.os_updates = std::max(0, std::stoi(value));
      else if (key == "shell_update") next.shell_update = value == "1";
      else if (key == "reboot_required") next.reboot_required = value == "1";
    } catch (...) {
      // Ignore a partially written or malformed field. The updater publishes
      // with atomic rename, so this is only defensive against manual edits.
    }
  }

  const bool changed = next.phase != lib.update_status.phase ||
                       next.message != lib.update_status.message ||
                       next.progress != lib.update_status.progress ||
                       next.os_updates != lib.update_status.os_updates ||
                       next.shell_update != lib.update_status.shell_update ||
                       next.reboot_required != lib.update_status.reboot_required;
  lib.update_status = std::move(next);

  const bool available = lib.update_status.os_updates > 0 ||
                         lib.update_status.shell_update;
  if (available && !UpdateIsBusy(lib.update_status) &&
      !lib.update_notice_announced) {
    lib.update_notice_until_ms = gb::core::NowMs() + 8000;
    lib.update_notice_announced = true;
  } else if (!available) {
    lib.update_notice_announced = false;
  }
  return changed;
}

bool StartUpdaterUnit(const std::string& unit, std::string& status) {
  const auto result = gb::platform::RunProcessBlocking(
      {"sudo", "-n", "/usr/bin/systemctl", "--no-block", "start", unit});
  if (!result.launched || result.exit_code != 0) {
    status = "Updater service unavailable";
    return false;
  }
  status = unit == "gamebird-update.service" ? "Starting update..."
                                               : "Checking for updates...";
  return true;
}

std::vector<std::string> BuildToolsRows(const LibraryState& lib) {
  int unavailable_roots = 0;
  for (const auto& root : lib.library_roots) {
    if (root.status != "ok") {
      ++unavailable_roots;
    }
  }
  std::string scan_row = "Rescan Library";
  if (unavailable_roots > 0) {
    scan_row += " [OFFLINE:" + std::to_string(unavailable_roots) + "]";
  }
  return {
      scan_row,
      "Identify Metadata",
      "Refresh Artwork",
      "Run Queued Jobs",
      "Export Diagnostics",
      "Input Setup",
      "Wi-Fi",
      std::string("System Update") +
          ((lib.update_status.os_updates > 0 || lib.update_status.shell_update)
               ? " [READY]"
               : ""),
      std::string("Bluetooth Pads: ") +
          (lib.settings.enable_bluetooth_gamepads ? "AUTO" : "OFF"),
      "RetroArch Menu",
      "Bluetooth Devices",
      lib.tools_exit_confirm ? "Exit To Console [Confirm]" : "Exit To Console",
  };
}

std::string RetroArchControlName(const gb::platform::Button button) {
  switch (button) {
    case gb::platform::Button::LeftStickUp:
      return "l_y_minus";
    case gb::platform::Button::LeftStickDown:
      return "l_y_plus";
    case gb::platform::Button::LeftStickLeft:
      return "l_x_minus";
    case gb::platform::Button::LeftStickRight:
      return "l_x_plus";
    case gb::platform::Button::RightStickUp:
      return "r_y_minus";
    case gb::platform::Button::RightStickDown:
      return "r_y_plus";
    case gb::platform::Button::RightStickLeft:
      return "r_x_minus";
    case gb::platform::Button::RightStickRight:
      return "r_x_plus";
    case gb::platform::Button::Guide:
      // Guide is a shell-level control. RetroArch has no per-player RetroPad
      // guide field; users can map it to a regular target in the wizard.
      return {};
    default:
      return ToLower(ButtonName(button));
  }
}

std::string RetroArchInputConfigPath(const Args& args) {
  const auto parent = std::filesystem::path(args.settings_path).parent_path();
  if (parent.empty()) {
    return "config/retroarch-input-gamebird.cfg";
  }
  return (parent / "retroarch-input-gamebird.cfg").string();
}

std::string RetroArchActiveInputConfigPath() {
  // tv-mode-manager.sh creates this volatile config before gbshell starts.
  // It is the saved GamePi13 mapping in handheld mode and a clean external-pad
  // config in TV mode, so a disconnected HAT can never pin player one.
  return "/run/gamebird-retroarch-input.cfg";
}

bool WriteRuntimeTextFile(const std::string& path,
                          const std::string& contents,
                          std::string& error) {
  error.clear();
  // The TV-mode manager creates this /run file and gives it to the gamebird
  // user. The /run directory itself remains root-owned, so gbshell can replace
  // the contents but cannot create and rename a sibling temporary file.
  // HDMI transitions restart gbshell before another launch, keeping this
  // direct write isolated from the manager's atomic mode switch.
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    error = "failed to open runtime config: " + path;
    return false;
  }
  out << contents;
  out.close();
  if (!out) {
    error = "failed to write runtime config: " + path;
    return false;
  }
  return true;
}

bool WriteBatteryOverlayConfig(const LibraryState& lib, std::string& error) {
  std::string image_name = "battery-na.png";
  if (lib.battery_status.available) {
    // The artwork is generated in 5% increments. Pick the nearest image from
    // the real INA219 reading once per launch; it must never change merely
    // because Start was pressed or released.
    const int rounded = std::clamp(
        ((lib.battery_status.percent + 2) / 5) * 5, 0, 100);
    image_name = "battery-" + std::to_string(rounded) + ".png";
  }

  const std::string image =
      "/home/gamebird/gamebird-shell/config/retroarch-overlays/battery/" + image_name;
  std::ostringstream config;
  config << "overlays = 2\n\n"
         << "overlay0_name = \"hidden\"\n"
         << "overlay0_full_screen = true\n"
         << "overlay0_descs = 0\n\n"
         << "overlay1_name = \"battery\"\n"
         << "overlay1_overlay = \"" << image << "\"\n"
         << "overlay1_full_screen = true\n"
         << "overlay1_descs = 0\n";
  return WriteRuntimeTextFile("/tmp/gamebird-battery-overlay.cfg", config.str(),
                              error);
}

bool IsSameInputDevice(const gb::platform::EvdevDeviceInfo& device,
                       const std::string& name,
                       const std::uint16_t bus_type,
                       const std::uint16_t vendor,
                       const std::uint16_t product) {
  if (vendor != 0 && product != 0) {
    return device.vendor == vendor && device.product == product &&
           (bus_type == 0 || device.bus_type == bus_type);
  }
  return !name.empty() && device.name == name;
}

bool WriteActiveRetroArchInputConfig(const Args& args,
                                     const LibraryState& lib,
                                     std::string& error,
                                     const bool persist_saved_profile = false) {
  std::vector<const gb::platform::EvdevDeviceInfo*> active_devices;
  for (const auto& device : lib.input_devices) {
    if (!device.is_gamepad || device.joypad_index < 0 ||
        (lib.tv_mode && device.name == "GameBird Controls") ||
        (!lib.settings.enable_bluetooth_gamepads && device.is_bluetooth)) {
      continue;
    }
    active_devices.push_back(&device);
  }
  std::sort(active_devices.begin(), active_devices.end(), [](const auto* a,
                                                              const auto* b) {
    return a->joypad_index < b->joypad_index;
  });

  auto move_to_front = [&](const auto& predicate) {
    const auto it = std::find_if(active_devices.begin(), active_devices.end(),
                                 predicate);
    if (it != active_devices.end()) {
      std::rotate(active_devices.begin(), it, it + 1);
      return true;
    }
    return false;
  };

  bool primary_selected = false;
  if (!lib.tv_mode) {
    primary_selected = move_to_front([](const auto* device) {
      return device->name == "GameBird Controls";
    });
  }
  if (!primary_selected) {
    primary_selected = move_to_front([&](const auto* device) {
      return IsSameInputDevice(*device, lib.last_input_device,
                               lib.last_input_bus_type, lib.last_input_vendor,
                               lib.last_input_product);
    });
  }
  if (!primary_selected && !lib.settings.preferred_input_device.empty()) {
    move_to_front([&](const auto* device) {
      return device->name == lib.settings.preferred_input_device;
    });
  }

  std::vector<int> active_indices;
  active_indices.reserve(active_devices.size());
  for (const auto* device : active_devices) {
    active_indices.push_back(device->joypad_index);
  }

  std::vector<std::pair<std::string, std::string>> bindings;
  std::string device_name;
  int profile_idx = -1;
  if (!active_devices.empty()) {
    const auto& primary = *active_devices.front();
    device_name = primary.name;
    profile_idx = FindBestProfileIndex(lib.input_profiles, primary.name,
                                       primary.bus_type, primary.vendor,
                                       primary.product);
  }
  if (profile_idx >= 0) {
    const auto& profile = lib.input_profiles[static_cast<std::size_t>(profile_idx)];
    for (std::size_t i = 0; i < kButtonCount; ++i) {
      if (!profile.retroarch_bindings[i].empty()) {
        const std::string control = RetroArchControlName(kButtonOrder[i]);
        if (!control.empty()) {
          bindings.emplace_back(control, profile.retroarch_bindings[i]);
        }
      }
    }
  }

  const std::string generated = gb::core::BuildRetroArchInputConfig(
      device_name, bindings, active_indices);
  const std::string saved_path = RetroArchInputConfigPath(args);
  if (lib.tv_mode) {
    const auto config_parent = std::filesystem::path(args.settings_path).parent_path();
    const auto tv_path = config_parent / "retroarch-tv-input.cfg";
    std::ifstream tv_input(tv_path);
    if (!tv_input) {
      error = "failed to read TV input config: " + tv_path.string();
      return false;
    }
    std::ostringstream combined;
    combined << tv_input.rdbuf() << '\n' << generated;
    return WriteRuntimeTextFile(RetroArchActiveInputConfigPath(), combined.str(),
                                error);
  }

  if (persist_saved_profile) {
    if (!gb::core::WriteRetroArchInputConfig(saved_path, device_name, bindings,
                                              active_indices, error)) {
      return false;
    }
  }
  return WriteRuntimeTextFile(RetroArchActiveInputConfigPath(), generated, error);
}

bool SaveInputSettings(const Args& args, LibraryState& lib, std::string& status) {
  lib.settings.preferred_input_device =
      lib.input_capture_device.empty() ? lib.last_input_device : lib.input_capture_device;
  lib.settings.input_profiles = EncodeInputProfiles(lib.input_profiles);
  std::string err;
  if (!gb::core::SaveRuntimeSettings(args.settings_path, lib.settings, err)) {
    status = "save failed";
    gb::core::Log(gb::core::LogLevel::Error, err);
    return false;
  }
  if (!WriteActiveRetroArchInputConfig(args, lib, err, true)) {
    status = "RA input config failed";
    gb::core::Log(gb::core::LogLevel::Error, err);
    return false;
  }
  return true;
}

void SeedCaptureProfileFromExisting(
    LibraryState& lib,
    const std::string& device_name,
    const std::uint16_t bus_type,
    const std::uint16_t vendor,
    const std::uint16_t product,
    std::array<gb::platform::Button, kButtonCount>& out_map,
    std::unordered_map<std::uint16_t, gb::platform::Button>& out_keycode_map,
    std::array<std::string, kButtonCount>& out_retroarch_bindings) {
  out_map = IdentityButtonMap();
  out_keycode_map.clear();
  out_retroarch_bindings.fill({});
  const int idx = FindBestProfileIndex(lib.input_profiles, device_name, bus_type, vendor,
                                       product);
  if (idx < 0) {
    return;
  }
  const auto& profile = lib.input_profiles[static_cast<std::size_t>(idx)];
  out_map = profile.source_to_target;
  out_keycode_map = profile.keycode_to_target;
  out_retroarch_bindings = profile.retroarch_bindings;
}

bool PersistCaptureProfile(const Args& args, LibraryState& lib, std::string& status) {
  if (lib.input_capture_device.empty()) {
    status = "No input device";
    return false;
  }
  UpsertInputProfile(lib, lib.input_capture_device, lib.input_capture_bus_type,
                     lib.input_capture_vendor, lib.input_capture_product,
                     lib.input_capture_has_id, lib.input_capture_map,
                     lib.input_capture_keycode_map,
                     lib.input_capture_retroarch_bindings);
  return SaveInputSettings(args, lib, status);
}

constexpr std::array<int, 8> kLaunchAudioChoices = {
    0, 32, 48, 64, 96, 128, 192, 256,
};

constexpr std::array<std::pair<int, int>, 4> kLaunchVideoChoices = {
    std::pair<int, int>{0, 0},
    std::pair<int, int>{240, 240},
    std::pair<int, int>{320, 240},
    std::pair<int, int>{640, 480},
};

constexpr int kLaunchOptionsRowCount = 6;

std::string ExtractCoreFromLaunchTemplate(const std::string& launch_template) {
  return gb::core::ExtractRetroArchCore(launch_template);
}

std::string BaseName(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  return std::filesystem::path(path).filename().string();
}

bool IsInstalledCorePath(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path p(path);
  if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
    return false;
  }
  return p.extension() == ".so";
}

std::string DetectRetroArchCoreDir() {
  std::vector<std::filesystem::path> cfg_paths;
  if (const char* home = std::getenv("HOME"); home && *home != '\0') {
    cfg_paths.emplace_back(std::filesystem::path(home) / ".config/retroarch/retroarch.cfg");
  }
  cfg_paths.emplace_back("/etc/retroarch.cfg");

  for (const auto& cfg : cfg_paths) {
    std::error_code ec;
    if (!std::filesystem::exists(cfg, ec) || !std::filesystem::is_regular_file(cfg, ec)) {
      continue;
    }
    std::ifstream in(cfg);
    if (!in) {
      continue;
    }
    std::string line;
    while (std::getline(in, line)) {
      line = Trim(line);
      if (line.empty() || line[0] == '#') {
        continue;
      }
      constexpr std::string_view kKey = "libretro_directory";
      if (line.rfind(kKey, 0) != 0) {
        continue;
      }
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      std::string value = Trim(line.substr(eq + 1));
      if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
      }
      if (value.empty()) {
        continue;
      }
      std::filesystem::path dir(ExpandHomePath(value));
      if (std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec)) {
        return dir.string();
      }
    }
  }
  return {};
}

void AddSearchRoot(std::vector<std::filesystem::path>& roots,
                   std::set<std::string>& seen_roots,
                   const std::filesystem::path& root) {
  if (root.empty()) {
    return;
  }
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(root, ec);
  const std::string key = ec ? root.lexically_normal().string() : canonical.string();
  if (seen_roots.insert(key).second) {
    roots.push_back(root);
  }
}

std::vector<std::string> DiscoverLibretroCores() {
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::vector<std::filesystem::path> roots;
  std::set<std::string> seen_roots;
  const std::string detected_core_dir = DetectRetroArchCoreDir();
  if (!detected_core_dir.empty()) {
    AddSearchRoot(roots, seen_roots, detected_core_dir);
  }
  if (const char* home = std::getenv("HOME"); home && *home != '\0') {
    AddSearchRoot(roots, seen_roots,
                  std::filesystem::path(home) / ".config/retroarch/cores");
  }
  AddSearchRoot(roots, seen_roots, "/usr/lib/libretro");
  AddSearchRoot(roots, seen_roots, "/usr/lib/arm-linux-gnueabihf/libretro");
  AddSearchRoot(roots, seen_roots, "/usr/lib/x86_64-linux-gnu/libretro");

  for (const auto& root : roots) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) ||
        !std::filesystem::is_directory(root, ec)) {
      continue;
    }
    for (const auto& entry : std::filesystem::directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      if (entry.path().extension() != ".so") {
        continue;
      }
      const std::string path = entry.path().string();
      if (seen.insert(path).second) {
        out.push_back(path);
      }
    }
  }

  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    const std::string a_base = ToLower(BaseName(a));
    const std::string b_base = ToLower(BaseName(b));
    if (a_base == b_base) {
      return a < b;
    }
    return a_base < b_base;
  });
  return out;
}

void AddCoreChoice(std::vector<std::string>& out, const std::string& path) {
  if (path.empty()) {
    return;
  }
  if (std::find(out.begin(), out.end(), path) == out.end()) {
    out.push_back(path);
  }
}

bool CoreMatchesSystem(const std::string& system_id, const std::string& core_path) {
  const std::string sid = ToLower(system_id);
  const std::string core = ToLower(BaseName(core_path));
  if (sid.empty() || core.empty()) {
    return true;
  }

  const std::unordered_map<std::string, std::vector<std::string>> hints = {
      {"snes", {"snes", "snes9x", "bsnes", "supafaust"}},
      {"nes", {"fce", "nestopia", "quicknes", "nes"}},
      {"n64", {"n64", "mupen", "parallel"}},
      {"gba", {"gba", "mgba", "vba"}},
      {"gb", {"gambatte", "sameboy", "tgb", "gameboy", "gb"}},
      {"gbc", {"gambatte", "sameboy", "gameboy", "gbc"}},
      {"genesis", {"genesis", "genplus", "picodrive", "megadrive", "md"}},
      {"megadrive", {"genesis", "genplus", "picodrive", "megadrive", "md"}},
      {"psx", {"psx", "pcsx", "duckstation", "mednafen_psx"}},
      {"ps1", {"psx", "pcsx", "duckstation", "mednafen_psx"}},
  };

  const auto it = hints.find(sid);
  if (it == hints.end()) {
    return true;
  }
  for (const auto& kw : it->second) {
    if (core.find(kw) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool TryGetLaunchOverride(gb::db::Database& db,
                          const std::string& scope_type,
                          const std::string& scope_id,
                          gb::db::LaunchOverride& out,
                          bool& found) {
  auto is_not_found = [](const std::string& err) {
    return err.rfind("launch override not found", 0) == 0;
  };
  if (db.GetLaunchOverride(scope_type, scope_id, out)) {
    found = true;
    return true;
  }
  found = false;
  if (is_not_found(db.LastError())) {
    out = gb::db::LaunchOverride{};
    return true;
  }
  return false;
}

int FindAudioChoiceIndex(const int audio_latency) {
  for (std::size_t i = 0; i < kLaunchAudioChoices.size(); ++i) {
    if (kLaunchAudioChoices[i] == audio_latency) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

int FindVideoChoiceIndex(const int width, const int height) {
  for (std::size_t i = 0; i < kLaunchVideoChoices.size(); ++i) {
    if (kLaunchVideoChoices[i].first == width &&
        kLaunchVideoChoices[i].second == height) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

std::string LaunchAudioLabel(const int audio_index) {
  const int clamped =
      std::clamp(audio_index, 0, static_cast<int>(kLaunchAudioChoices.size()) - 1);
  const int value = kLaunchAudioChoices[static_cast<std::size_t>(clamped)];
  if (value <= 0) {
    return "INHERIT";
  }
  return std::to_string(value) + " ms";
}

std::string LaunchVideoLabel(const int video_index) {
  const int clamped =
      std::clamp(video_index, 0, static_cast<int>(kLaunchVideoChoices.size()) - 1);
  const auto [w, h] = kLaunchVideoChoices[static_cast<std::size_t>(clamped)];
  if (w <= 0 || h <= 0) {
    return "INHERIT";
  }
  return std::to_string(w) + "x" + std::to_string(h);
}

std::string LaunchCoreChoiceLabel(const LibraryState& lib) {
  std::string core;
  if (!lib.launch_options_core_paths.empty()) {
    const int idx =
        std::clamp(lib.launch_options_core_selected, 0,
                   static_cast<int>(lib.launch_options_core_paths.size()) - 1);
    core = lib.launch_options_core_paths[static_cast<std::size_t>(idx)];
  }
  if (core.empty()) {
    std::string label =
        (lib.launch_options_scope_type == "game") ? "INHERIT" : "DEFAULT";
    const std::string fallback = BaseName(lib.launch_options_default_core);
    if (!fallback.empty()) {
      label += " (" + fallback + ")";
    }
    return label;
  }
  return BaseName(core).empty() ? core : BaseName(core);
}

std::vector<std::string> BuildLaunchOptionsRows(const LibraryState& lib) {
  const std::string target_prefix =
      lib.launch_options_scope_type == "game" ? "Game: " : "System: ";
  return {
      target_prefix + Ellipsize(lib.launch_options_title, 22),
      "Core: " + LaunchCoreChoiceLabel(lib),
      "Audio Latency: " + LaunchAudioLabel(lib.launch_options_audio_selected),
      "Resolution: " + LaunchVideoLabel(lib.launch_options_video_selected),
      "Save Override",
      "Clear Override",
  };
}

void FillLaunchCoreChoices(LibraryState& lib,
                           const std::string& default_core,
                           const std::string& override_core) {
  lib.launch_options_core_paths.clear();
  lib.launch_options_core_paths.push_back({});
  if (IsInstalledCorePath(default_core)) {
    AddCoreChoice(lib.launch_options_core_paths, default_core);
  }
  for (const auto& core : DiscoverLibretroCores()) {
    if (CoreMatchesSystem(lib.launch_options_system_id, core)) {
      AddCoreChoice(lib.launch_options_core_paths, core);
    }
  }
  if (IsInstalledCorePath(override_core)) {
    AddCoreChoice(lib.launch_options_core_paths, override_core);
  }

  lib.launch_options_core_selected = 0;
  if (!override_core.empty()) {
    for (std::size_t i = 0; i < lib.launch_options_core_paths.size(); ++i) {
      if (lib.launch_options_core_paths[i] == override_core) {
        lib.launch_options_core_selected = static_cast<int>(i);
        break;
      }
    }
  }
}

void SetEffectiveLaunchDisplay(LibraryState& lib,
                               const gb::core::EffectiveLaunch& launch) {
  lib.launch_options_effective_core = BaseName(launch.effective_core);
  if (lib.launch_options_effective_core.empty()) {
    lib.launch_options_effective_core = "none";
  }
  lib.launch_options_effective_source = launch.core_source;

  lib.launch_options_effective_config.clear();
  for (const auto& path : launch.append_configs) {
    if (!lib.launch_options_effective_config.empty()) {
      lib.launch_options_effective_config += "+";
    }
    lib.launch_options_effective_config += BaseName(path);
  }
  if (launch.merged_override.audio_latency > 0 ||
      (launch.merged_override.video_width > 0 &&
       launch.merged_override.video_height > 0)) {
    if (!lib.launch_options_effective_config.empty()) {
      lib.launch_options_effective_config += "+";
    }
    lib.launch_options_effective_config += "runtime override";
  }
  if (lib.launch_options_effective_config.empty()) {
    lib.launch_options_effective_config = "none";
  }

  lib.launch_options_effective_warning.clear();
  if (!launch.template_core.empty() && !launch.effective_core.empty() &&
      launch.template_core != launch.effective_core) {
    lib.launch_options_effective_warning = "WARNING: CORE OVERRIDES DEFAULT";
  }
}

bool RefreshEffectiveLaunch(gb::db::Database& db, LibraryState& lib) {
  gb::db::LaunchInfo info;
  if (lib.launch_options_scope_type == "game") {
    if (lib.launch_options_game_id <= 0 ||
        !db.GetLaunchInfo(lib.launch_options_game_id, info)) {
      return false;
    }
  } else if (!db.GetSystemLaunchInfo(lib.launch_options_system_id, info)) {
    return false;
  }

  gb::core::EffectiveLaunch launch;
  std::string error;
  if (!gb::core::ResolveEffectiveLaunch(db, info, launch, error)) {
    return false;
  }
  SetEffectiveLaunchDisplay(lib, launch);
  return true;
}

bool OpenLaunchOptionsForSystem(gb::db::Database& db,
                                gb::ui::UIState& ui,
                                LibraryState& lib) {
  if (lib.systems.empty()) {
    lib.status = "No system selected";
    return false;
  }
  ClampSelection(lib.system_selected, static_cast<int>(lib.systems.size()));
  const auto& sys = lib.systems[static_cast<std::size_t>(lib.system_selected)];

  gb::db::LaunchInfo sys_launch;
  if (!db.GetSystemLaunchInfo(sys.id, sys_launch)) {
    lib.status = "System launch config missing";
    return false;
  }

  gb::db::LaunchOverride sys_override;
  bool has_override = false;
  if (!TryGetLaunchOverride(db, "system", sys.id, sys_override, has_override)) {
    lib.status = "DB read error";
    return false;
  }

  lib.launch_options_return_screen = gb::ui::Screen::SystemMenu;
  lib.launch_options_selected = 0;
  lib.launch_options_scope_type = "system";
  lib.launch_options_scope_id = sys.id;
  lib.launch_options_system_id = sys.id;
  lib.launch_options_title = sys.name;
  lib.launch_options_game_id = 0;
  lib.launch_options_default_core =
      ExtractCoreFromLaunchTemplate(sys_launch.launch_template);
  FillLaunchCoreChoices(
      lib, lib.launch_options_default_core,
      has_override ? sys_override.core_path : std::string());
  lib.launch_options_audio_selected =
      FindAudioChoiceIndex(has_override ? sys_override.audio_latency : 0);
  lib.launch_options_video_selected = FindVideoChoiceIndex(
      has_override ? sys_override.video_width : 0,
      has_override ? sys_override.video_height : 0);
  RefreshEffectiveLaunch(db, lib);

  lib.status = lib.launch_options_effective_warning.empty()
                   ? "System launch options"
                   : lib.launch_options_effective_warning;
  ui.screen = gb::ui::Screen::LaunchOptions;
  ui.needs_redraw = true;
  return true;
}

bool OpenLaunchOptionsForGame(gb::db::Database& db,
                              gb::ui::UIState& ui,
                              LibraryState& lib) {
  const auto& game = lib.menu_game;
  if (game.id <= 0) { lib.status = "No game selected"; return false; }

  gb::db::LaunchInfo launch;
  if (!db.GetLaunchInfo(game.id, launch)) {
    lib.status = "Game launch config missing";
    return false;
  }

  gb::db::LaunchOverride sys_override;
  bool has_sys_override = false;
  if (!TryGetLaunchOverride(db, "system", launch.system_id, sys_override,
                            has_sys_override)) {
    lib.status = "DB read error";
    return false;
  }

  gb::db::LaunchOverride game_override;
  bool has_game_override = false;
  if (!TryGetLaunchOverride(db, "game", launch.rom_path, game_override,
                            has_game_override)) {
    lib.status = "DB read error";
    return false;
  }

  const std::string template_core =
      ExtractCoreFromLaunchTemplate(launch.launch_template);
  const std::string effective_default_core =
      (!has_sys_override || sys_override.core_path.empty()) ? template_core
                                                            : sys_override.core_path;

  lib.launch_options_return_screen = gb::ui::Screen::GameMenu;
  lib.launch_options_selected = 0;
  lib.launch_options_scope_type = "game";
  lib.launch_options_scope_id = launch.rom_path;
  lib.launch_options_system_id = launch.system_id;
  lib.launch_options_title = game.title;
  lib.launch_options_game_id = game.id;
  lib.launch_options_default_core = effective_default_core;
  FillLaunchCoreChoices(
      lib, lib.launch_options_default_core,
      has_game_override ? game_override.core_path : std::string());
  lib.launch_options_audio_selected =
      FindAudioChoiceIndex(has_game_override ? game_override.audio_latency : 0);
  lib.launch_options_video_selected = FindVideoChoiceIndex(
      has_game_override ? game_override.video_width : 0,
      has_game_override ? game_override.video_height : 0);
  RefreshEffectiveLaunch(db, lib);

  lib.status = lib.launch_options_effective_warning.empty()
                   ? "Game launch options"
                   : lib.launch_options_effective_warning;
  ui.screen = gb::ui::Screen::LaunchOptions;
  ui.needs_redraw = true;
  return true;
}

bool SaveLaunchOptions(gb::db::Database& db, LibraryState& lib) {
  if (lib.launch_options_scope_type.empty() || lib.launch_options_scope_id.empty()) {
    lib.status = "Invalid launch scope";
    return false;
  }

  std::string core;
  if (!lib.launch_options_core_paths.empty()) {
    const int core_idx =
        std::clamp(lib.launch_options_core_selected, 0,
                   static_cast<int>(lib.launch_options_core_paths.size()) - 1);
    core = lib.launch_options_core_paths[static_cast<std::size_t>(core_idx)];
  }
  const int audio_idx = std::clamp(
      lib.launch_options_audio_selected, 0,
      static_cast<int>(kLaunchAudioChoices.size()) - 1);
  const int audio = kLaunchAudioChoices[static_cast<std::size_t>(audio_idx)];
  const int video_idx = std::clamp(
      lib.launch_options_video_selected, 0,
      static_cast<int>(kLaunchVideoChoices.size()) - 1);
  const auto [video_width, video_height] =
      kLaunchVideoChoices[static_cast<std::size_t>(video_idx)];

  if (core.empty() && audio == 0 && video_width == 0 && video_height == 0) {
    if (!db.DeleteLaunchOverride(lib.launch_options_scope_type,
                                 lib.launch_options_scope_id)) {
      lib.status = "Override clear failed";
      return false;
    }
    lib.status = "Launch override cleared";
    RefreshEffectiveLaunch(db, lib);
    return true;
  }

  gb::db::LaunchOverride row;
  row.scope_type = lib.launch_options_scope_type;
  row.scope_id = lib.launch_options_scope_id;
  row.core_path = core;
  row.audio_latency = audio;
  row.video_width = video_width;
  row.video_height = video_height;
  if (!db.UpsertLaunchOverride(row)) {
    lib.status = "Override save failed";
    return false;
  }
  lib.status = "Launch override saved";
  RefreshEffectiveLaunch(db, lib);
  return true;
}

void ResetLaunchOptionsToInherited(LibraryState& lib) {
  lib.launch_options_core_selected = 0;
  lib.launch_options_audio_selected = 0;
  lib.launch_options_video_selected = 0;
}

gb::scrape::WorkerConfig MakeWorkerConfig(const Args& args,
                                          const LibraryState& lib) {
  gb::scrape::WorkerConfig cfg;
  cfg.defaults_json_path = args.defaults_json;
  cfg.systems_dir = args.systems_dir;
  cfg.artwork_dir = args.artwork_dir.empty()
                        ? (std::filesystem::path(args.db_path).parent_path() / "artwork").string()
                        : args.artwork_dir;
  cfg.hide_missing = false;
  cfg.provider = lib.settings.scrape_provider;
  // Batch scraping is deliberately additive. Existing user-selected artwork
  // is never replaced by an automatic match.
  cfg.overwrite_artwork = false;
  return cfg;
}

bool RunJobs(gb::db::Database& db,
             const gb::scrape::WorkerConfig& cfg,
             const bool enqueue_scan,
             const bool enqueue_identify,
             const bool enqueue_scrape,
             std::string& status) {
  gb::scrape::EnqueueDefaultJobs(db, enqueue_scan, enqueue_identify, enqueue_scrape);

  gb::scrape::WorkerStats stats;
  gb::scrape::ProcessJobsUntilEmpty(db, cfg, stats);

  status = "jobs ok=" + std::to_string(stats.jobs_ok) +
           " err=" + std::to_string(stats.jobs_error);
  if (enqueue_scan && stats.jobs_error == 0) {
    status = "scan " + std::to_string(stats.scan_roots_ok) + " ok";
    if (stats.scan_roots_unavailable > 0) {
      status += " " + std::to_string(stats.scan_roots_unavailable) + " offline";
    }
    if (stats.scan_roots_error > 0) {
      status += " " + std::to_string(stats.scan_roots_error) + " error";
    }
  }
  return stats.jobs_error == 0;
}

bool IsValidBluetoothAddress(const std::string& addr) {
  if (addr.size() != 17) {
    return false;
  }
  for (std::size_t i = 0; i < addr.size(); ++i) {
    if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
      if (addr[i] != ':') {
        return false;
      }
      continue;
    }
    if (!std::isxdigit(static_cast<unsigned char>(addr[i]))) {
      return false;
    }
  }
  return true;
}

std::string SanitizeBluetoothAddress(const std::string& addr) {
  if (!IsValidBluetoothAddress(addr)) {
    return {};
  }
  std::string out = addr;
  for (auto& ch : out) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return out;
}

std::vector<BluetoothDevice> ParseBluetoothDevices(const std::string& text) {
  std::vector<BluetoothDevice> devices;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) {
    const auto pos = line.find("Device ");
    if (pos == std::string::npos) {
      continue;
    }
    std::string rest = line.substr(pos + 7);
    if (rest.size() < 18) {
      continue;
    }
    std::string addr = SanitizeBluetoothAddress(rest.substr(0, 17));
    if (addr.empty()) {
      continue;
    }
    std::string name;
    if (rest.size() > 18) {
      name = Trim(rest.substr(18));
    }
    devices.push_back(BluetoothDevice{.address = addr, .name = name});
  }
  return devices;
}

std::set<std::string> ParseBluetoothAddressSet(const std::string& text) {
  std::set<std::string> out;
  for (const auto& d : ParseBluetoothDevices(text)) {
    out.insert(d.address);
  }
  return out;
}

BluetoothDeviceKind ClassifyBluetoothDeviceKind(std::string name) {
  name = ToLower(name);
  if (name.empty()) {
    return BluetoothDeviceKind::Other;
  }
  if (name.find("gamepad") != std::string::npos ||
      name.find("controller") != std::string::npos ||
      name.find("joystick") != std::string::npos ||
      name.find("joypad") != std::string::npos ||
      name.find("xbox") != std::string::npos ||
      name.find("8bitdo") != std::string::npos ||
      name.find("dualshock") != std::string::npos ||
      name.find("dualsense") != std::string::npos ||
      name.find("switch pro") != std::string::npos) {
    return BluetoothDeviceKind::Controller;
  }
  if (name.find("headphone") != std::string::npos ||
      name.find("headset") != std::string::npos ||
      name.find("earbud") != std::string::npos ||
      name.find("speaker") != std::string::npos ||
      name.find("soundbar") != std::string::npos ||
      name.find("audio") != std::string::npos ||
      name.find("buds") != std::string::npos) {
    return BluetoothDeviceKind::Audio;
  }
  return BluetoothDeviceKind::Other;
}

const char* BluetoothKindTag(const BluetoothDeviceKind kind) {
  switch (kind) {
    case BluetoothDeviceKind::Controller:
      return "CTL";
    case BluetoothDeviceKind::Audio:
      return "AUD";
    case BluetoothDeviceKind::Other:
      return "DEV";
  }
  return "DEV";
}

const char* BluetoothFilterTag(const BluetoothScanFilter filter) {
  switch (filter) {
    case BluetoothScanFilter::All:
      return "ALL";
    case BluetoothScanFilter::Controllers:
      return "CTL";
    case BluetoothScanFilter::Audio:
      return "AUD";
  }
  return "ALL";
}

bool BluetoothFilterMatches(const BluetoothDevice& d,
                            const BluetoothScanFilter filter) {
  switch (filter) {
    case BluetoothScanFilter::All:
      return true;
    case BluetoothScanFilter::Controllers:
      return d.kind == BluetoothDeviceKind::Controller;
    case BluetoothScanFilter::Audio:
      return d.kind == BluetoothDeviceKind::Audio;
  }
  return true;
}

void CycleBluetoothFilter(LibraryState& lib, const int delta) {
  constexpr std::array<BluetoothScanFilter, 3> kFilters = {
      BluetoothScanFilter::All,
      BluetoothScanFilter::Controllers,
      BluetoothScanFilter::Audio,
  };
  int idx = 0;
  for (int i = 0; i < static_cast<int>(kFilters.size()); ++i) {
    if (kFilters[static_cast<std::size_t>(i)] == lib.bluetooth_scan_filter) {
      idx = i;
      break;
    }
  }
  idx = (idx + delta + static_cast<int>(kFilters.size())) %
        static_cast<int>(kFilters.size());
  lib.bluetooth_scan_filter = kFilters[static_cast<std::size_t>(idx)];
}

void SortBluetoothDevices(std::vector<BluetoothDevice>& devices) {
  std::sort(devices.begin(), devices.end(),
            [](const BluetoothDevice& a, const BluetoothDevice& b) {
              if (a.connected != b.connected) {
                return a.connected > b.connected;
              }
              if (a.paired != b.paired) {
                return a.paired > b.paired;
              }
              const std::string a_name = a.name.empty() ? a.address : a.name;
              const std::string b_name = b.name.empty() ? b.address : b.name;
              return ToLower(a_name) < ToLower(b_name);
            });
}

gb::platform::ProcessCaptureResult RunShellCapture(const std::string& command) {
  return gb::platform::RunProcessCapture({"/bin/bash", "-lc", command});
}

std::string ShellQuote(const std::string& text) {
  std::string quoted = "'";
  for (const char ch : text) {
    if (ch == '\'') {
      quoted += "'\\\"'\\\"'";
    } else {
      quoted += ch;
    }
  }
  return quoted + "'";
}

std::vector<std::string> ParseNmcliTerse(const std::string& line) {
  std::vector<std::string> fields;
  std::string current;
  bool escaped = false;
  for (const char ch : line) {
    if (escaped) {
      current += ch;
      escaped = false;
    } else if (ch == '\\') {
      escaped = true;
    } else if (ch == ':') {
      fields.push_back(current);
      current.clear();
    } else {
      current += ch;
    }
  }
  if (escaped) current += '\\';
  fields.push_back(current);
  return fields;
}

bool IsValidWifiCountry(const std::string& country) {
  return country.size() == 2 &&
         std::isalpha(static_cast<unsigned char>(country[0])) &&
         std::isalpha(static_cast<unsigned char>(country[1]));
}

std::string WifiCountryFromRegdom(const std::string& text) {
  const std::string lower = ToLower(text);
  const auto marker = lower.find("country ");
  if (marker == std::string::npos || marker + 10 > lower.size()) return {};
  const std::string country = lower.substr(marker + 8, 2);
  if (!IsValidWifiCountry(country) || country == "00") return {};
  std::string out = country;
  for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return out;
}

bool RefreshWifiStatus(LibraryState& lib, std::string& status) {
  const auto radio = RunShellCapture("timeout 8s sudo -n nmcli -t -f WIFI general");
  lib.wifi_enabled = ToLower(Trim(radio.output)) == "enabled";

  const auto networks = RunShellCapture(
      "timeout 10s sudo -n nmcli -t --escape yes -f ACTIVE,SSID,SIGNAL,SECURITY "
      "device wifi list ifname wlan0");
  lib.wifi_connected_ssid.clear();
  lib.wifi_connected_signal = 0;
  if (networks.process.launched && networks.process.exit_code == 0) {
    std::istringstream input(networks.output);
    std::string line;
    while (std::getline(input, line)) {
      const auto fields = ParseNmcliTerse(line);
      if (fields.size() < 4 || fields[0] != "yes") continue;
      lib.wifi_connected_ssid = fields[1];
      try { lib.wifi_connected_signal = std::clamp(std::stoi(fields[2]), 0, 100); }
      catch (...) { lib.wifi_connected_signal = 0; }
      break;
    }
  }
  const auto regdom = RunShellCapture("timeout 5s sudo -n iw reg get");
  lib.wifi_country = WifiCountryFromRegdom(regdom.output);
  if (!lib.wifi_enabled) {
    status = "Wi-Fi is off";
  } else if (lib.wifi_connected_ssid.empty()) {
    status = "Not connected";
  } else {
    status = "Connected to " + lib.wifi_connected_ssid;
  }
  return true;
}

bool ScanWifi(LibraryState& lib, std::string& status) {
  RunShellCapture("timeout 8s sudo -n nmcli radio wifi on");
  const auto rescan = RunShellCapture(
      "timeout 18s sudo -n nmcli device wifi rescan ifname wlan0");
  if (!rescan.process.launched || rescan.process.exit_code != 0) {
    status = "Wi-Fi scan failed";
    return false;
  }
  const auto list = RunShellCapture(
      "timeout 10s sudo -n nmcli -t --escape yes -f SSID,SIGNAL,SECURITY,IN-USE "
      "device wifi list ifname wlan0");
  if (!list.process.launched || list.process.exit_code != 0) {
    status = "Could not read Wi-Fi networks";
    return false;
  }
  lib.wifi_networks.clear();
  std::set<std::string> seen_ssids;
  std::istringstream input(list.output);
  std::string line;
  while (std::getline(input, line)) {
    const auto fields = ParseNmcliTerse(line);
    if (fields.size() < 4 || fields[0].empty()) continue;
    WifiNetwork network;
    network.ssid = fields[0];
    try { network.signal = std::clamp(std::stoi(fields[1]), 0, 100); }
    catch (...) { network.signal = 0; }
    network.secured = !fields[2].empty() && fields[2] != "--";
    network.active = fields[3] == "*" || fields[3] == "yes";
    const auto existing = std::find_if(lib.wifi_networks.begin(), lib.wifi_networks.end(),
        [&network](const WifiNetwork& item) { return item.ssid == network.ssid; });
    if (existing == lib.wifi_networks.end() && seen_ssids.insert(network.ssid).second) {
      lib.wifi_networks.push_back(std::move(network));
    }
  }
  std::sort(lib.wifi_networks.begin(), lib.wifi_networks.end(),
      [](const WifiNetwork& a, const WifiNetwork& b) {
        if (a.active != b.active) return a.active > b.active;
        if (a.signal != b.signal) return a.signal > b.signal;
        return ToLower(a.ssid) < ToLower(b.ssid);
      });
  ClampSelection(lib.wifi_selected, static_cast<int>(lib.wifi_networks.size()));
  RefreshWifiStatus(lib, status);
  status = std::to_string(lib.wifi_networks.size()) + " networks found";
  return true;
}

bool ConnectWifi(LibraryState& lib, std::string& status) {
  if (lib.wifi_selected_ssid.empty()) {
    status = "No network selected";
    return false;
  }
  std::string command = "timeout 35s sudo -n nmcli device wifi connect " +
                        ShellQuote(lib.wifi_selected_ssid) + " ifname wlan0";
  if (!lib.wifi_password.empty()) command += " password " + ShellQuote(lib.wifi_password);
  const auto connected = RunShellCapture(command);
  if (!connected.process.launched || connected.process.exit_code != 0) {
    status = "Connection failed";
    return false;
  }
  RefreshWifiStatus(lib, status);
  status = "Connected to " + lib.wifi_selected_ssid;
  lib.wifi_password.clear();
  return true;
}

bool DisconnectWifi(LibraryState& lib, std::string& status) {
  const auto disconnected = RunShellCapture(
      "timeout 15s sudo -n nmcli device disconnect wlan0");
  if (!disconnected.process.launched || disconnected.process.exit_code != 0) {
    status = "Disconnect failed";
    return false;
  }
  RefreshWifiStatus(lib, status);
  status = "Wi-Fi disconnected";
  return true;
}

bool SetWifiCountry(LibraryState& lib, std::string& status) {
  std::string country = lib.wifi_country_entry;
  for (char& ch : country) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  if (!IsValidWifiCountry(country)) {
    status = "Choose a two-letter country";
    return false;
  }
  const auto set = RunShellCapture("timeout 15s sudo -n raspi-config nonint do_wifi_country " +
                                   ShellQuote(country));
  if (!set.process.launched || set.process.exit_code != 0) {
    status = "Could not set wireless region";
    return false;
  }
  lib.wifi_country = country;
  lib.wifi_country_entry.clear();
  status = "Wireless region set to " + country;
  return true;
}

bool QueueWifiAction(LibraryState& lib, const PendingWifiAction action,
                     const std::string& status) {
  if (lib.pending_wifi_action != PendingWifiAction::None) {
    lib.status = "Wi-Fi busy";
    return false;
  }
  lib.pending_wifi_action = action;
  lib.status = status;
  return true;
}

bool ExecuteWifiAction(LibraryState& lib, std::string& status) {
  switch (lib.pending_wifi_action) {
    case PendingWifiAction::None: return true;
    case PendingWifiAction::RefreshStatus: return RefreshWifiStatus(lib, status);
    case PendingWifiAction::Scan: return ScanWifi(lib, status);
    case PendingWifiAction::Connect: return ConnectWifi(lib, status);
    case PendingWifiAction::Disconnect: return DisconnectWifi(lib, status);
    case PendingWifiAction::SetCountry: return SetWifiCountry(lib, status);
  }
  return false;
}

gb::platform::ProcessCaptureResult RunBluetoothCtl(const std::string& args,
                                                   const int timeout_sec) {
  return RunShellCapture("timeout " + std::to_string(timeout_sec) +
                         "s bluetoothctl " + args);
}

bool BluetoothSetupAgent(std::string& status) {
  const auto probe = RunShellCapture("command -v bluetoothctl >/dev/null 2>&1");
  if (!probe.process.launched || probe.process.exit_code != 0) {
    status = "bluetoothctl unavailable";
    return false;
  }

  const auto before = RunBluetoothCtl("show", 6);
  const std::string before_lower = ToLower(before.output);
  if (before_lower.find("no default controller available") != std::string::npos) {
    status = "No Bluetooth controller";
    return false;
  }
  if (before_lower.find("powerstate: off-blocked") != std::string::npos) {
    status = "Bluetooth blocked (rfkill)";
    return false;
  }

  // Best-effort setup. Some commands can return non-zero transiently.
  RunBluetoothCtl("power on", 8);
  RunBluetoothCtl("pairable on", 8);
  RunBluetoothCtl("discoverable on", 8);
  RunBluetoothCtl("agent NoInputNoOutput", 8);
  RunBluetoothCtl("default-agent", 8);

  const auto after = RunBluetoothCtl("show", 8);
  const std::string after_lower = ToLower(after.output);
  if (after_lower.find("powered: yes") == std::string::npos &&
      after_lower.find("powerstate: on") == std::string::npos) {
    status = "Bluetooth not powered";
    return false;
  }
  return true;
}

bool BluetoothRefreshScanned(LibraryState& lib, std::string& status) {
  if (!BluetoothSetupAgent(status)) {
    return false;
  }

  RunBluetoothCtl("scan on", 6);
  RunShellCapture("sleep 4");
  const auto scan = RunBluetoothCtl("devices", 8);
  RunBluetoothCtl("scan off", 6);
  const auto devices = ParseBluetoothDevices(scan.output);
  const auto paired_set =
      ParseBluetoothAddressSet(RunBluetoothCtl("paired-devices", 8).output);
  const auto connected_set =
      ParseBluetoothAddressSet(RunBluetoothCtl("devices Connected", 8).output);

  lib.bluetooth_scanned_devices.clear();
  lib.bluetooth_scanned_devices.reserve(devices.size());
  for (auto d : devices) {
    d.kind = ClassifyBluetoothDeviceKind(d.name);
    d.paired = paired_set.find(d.address) != paired_set.end();
    d.connected = connected_set.find(d.address) != connected_set.end();
    lib.bluetooth_scanned_devices.push_back(std::move(d));
  }
  SortBluetoothDevices(lib.bluetooth_scanned_devices);
  ClampSelection(lib.bluetooth_scanned_selected,
                 static_cast<int>(lib.bluetooth_scanned_devices.size()));

  if (lib.bluetooth_scanned_devices.empty()) {
    status = "No BT devices found";
    return false;
  }

  status = "Scan found " + std::to_string(lib.bluetooth_scanned_devices.size()) +
           " device(s)";
  return true;
}

bool BluetoothRefreshPaired(LibraryState& lib, std::string& status) {
  if (!BluetoothSetupAgent(status)) {
    return false;
  }

  const auto paired = RunBluetoothCtl("paired-devices", 8);
  const auto devices = ParseBluetoothDevices(paired.output);
  const auto connected_set =
      ParseBluetoothAddressSet(RunBluetoothCtl("devices Connected", 8).output);

  lib.bluetooth_paired_devices.clear();
  lib.bluetooth_paired_devices.reserve(devices.size());
  for (auto d : devices) {
    d.kind = ClassifyBluetoothDeviceKind(d.name);
    d.paired = true;
    d.connected = connected_set.find(d.address) != connected_set.end();
    lib.bluetooth_paired_devices.push_back(std::move(d));
  }
  SortBluetoothDevices(lib.bluetooth_paired_devices);
  ClampSelection(lib.bluetooth_paired_selected,
                 static_cast<int>(lib.bluetooth_paired_devices.size()));

  if (lib.bluetooth_paired_devices.empty()) {
    status = "No paired BT devices";
    return false;
  }
  status = "Paired list " + std::to_string(lib.bluetooth_paired_devices.size()) +
           " device(s)";
  return true;
}

bool BluetoothRefreshBoth(LibraryState& lib, std::string& status) {
  std::string s1;
  std::string s2;
  const bool ok1 = BluetoothRefreshScanned(lib, s1);
  const bool ok2 = BluetoothRefreshPaired(lib, s2);
  if (ok1 || ok2) {
    status = s1 + " / " + s2;
    return true;
  }
  status = !s1.empty() ? s1 : s2;
  return false;
}

bool BluetoothPairAndConnect(const BluetoothDevice& d, std::string& status) {
  if (!BluetoothSetupAgent(status)) {
    return false;
  }
  const auto pair = RunBluetoothCtl("pair " + d.address, 20);
  RunBluetoothCtl("trust " + d.address, 8);
  const auto connect = RunBluetoothCtl("connect " + d.address, 12);
  const std::string lower = ToLower(pair.output + "\n" + connect.output);
  if (connect.process.exit_code == 0 ||
      lower.find("connection successful") != std::string::npos ||
      lower.find("paired: yes") != std::string::npos ||
      lower.find("successful") != std::string::npos) {
    status = "BT paired " + (d.name.empty() ? d.address : d.name);
    return true;
  }
  status = "Pair/connect failed";
  return false;
}

bool BluetoothConnectDevice(const BluetoothDevice& d, std::string& status) {
  if (!BluetoothSetupAgent(status)) {
    return false;
  }
  RunBluetoothCtl("trust " + d.address, 8);
  const auto connect = RunBluetoothCtl("connect " + d.address, 12);
  const std::string lower = ToLower(connect.output);
  if (connect.process.exit_code == 0 ||
      lower.find("connection successful") != std::string::npos ||
      lower.find("successful") != std::string::npos) {
    status = "BT connected " + (d.name.empty() ? d.address : d.name);
    return true;
  }
  status = "Connect failed";
  return false;
}

bool BluetoothDisconnectDevice(const BluetoothDevice& d, std::string& status) {
  if (!BluetoothSetupAgent(status)) {
    return false;
  }
  const auto disconnect = RunBluetoothCtl("disconnect " + d.address, 12);
  const std::string lower = ToLower(disconnect.output);
  if (disconnect.process.exit_code == 0 ||
      lower.find("successful disconnected") != std::string::npos ||
      lower.find("not connected") != std::string::npos ||
      lower.find("successful") != std::string::npos) {
    status = "BT disconnected " + (d.name.empty() ? d.address : d.name);
    return true;
  }
  status = "Disconnect failed";
  return false;
}

bool BluetoothForgetDevice(const BluetoothDevice& d, std::string& status) {
  if (!BluetoothSetupAgent(status)) {
    return false;
  }

  RunBluetoothCtl("disconnect " + d.address, 8);
  const auto remove = RunBluetoothCtl("remove " + d.address, 12);
  const std::string lower = ToLower(remove.output);
  if (remove.process.exit_code == 0 ||
      lower.find("removed") != std::string::npos ||
      lower.find("not available") != std::string::npos ||
      lower.find("not paired") != std::string::npos) {
    status = "BT forgot " + (d.name.empty() ? d.address : d.name);
    return true;
  }
  status = "Forget failed";
  return false;
}

bool BluetoothDisconnectAllPaired(std::string& status) {
  if (!BluetoothSetupAgent(status)) {
    return false;
  }

  const auto paired = ParseBluetoothDevices(RunBluetoothCtl("paired-devices", 8).output);
  const auto connected_set =
      ParseBluetoothAddressSet(RunBluetoothCtl("devices Connected", 8).output);

  int attempted = 0;
  int succeeded = 0;
  for (const auto& d : paired) {
    if (connected_set.find(d.address) == connected_set.end()) {
      continue;
    }
    ++attempted;
    const auto disconnect = RunBluetoothCtl("disconnect " + d.address, 12);
    const std::string lower = ToLower(disconnect.output);
    if (disconnect.process.exit_code == 0 ||
        lower.find("successful disconnected") != std::string::npos ||
        lower.find("not connected") != std::string::npos ||
        lower.find("successful") != std::string::npos) {
      ++succeeded;
    }
  }

  if (attempted == 0) {
    status = "No connected paired devices";
    return true;
  }
  status = "Disconnected " + std::to_string(succeeded) + "/" +
           std::to_string(attempted) + " paired device(s)";
  return succeeded == attempted;
}

std::vector<const BluetoothDevice*> BuildActiveBluetoothDeviceRefs(
    const LibraryState& lib) {
  std::vector<const BluetoothDevice*> out;
  if (lib.bluetooth_show_paired) {
    out.reserve(lib.bluetooth_paired_devices.size());
    for (const auto& d : lib.bluetooth_paired_devices) {
      out.push_back(&d);
    }
    return out;
  }

  out.reserve(lib.bluetooth_scanned_devices.size());
  for (const auto& d : lib.bluetooth_scanned_devices) {
    if (BluetoothFilterMatches(d, lib.bluetooth_scan_filter)) {
      out.push_back(&d);
    }
  }
  return out;
}

std::vector<std::string> BuildBluetoothRows(
    const std::vector<const BluetoothDevice*>& devices) {
  std::vector<std::string> rows;
  rows.reserve(devices.size());
  for (const auto* d : devices) {
    if (!d) {
      continue;
    }
    std::string row;
    row += d->connected ? "* " : "  ";
    row += d->paired ? "P " : "  ";
    row += "[";
    row += BluetoothKindTag(d->kind);
    row += "] ";
    row += d->name.empty() ? d->address : d->name;
    rows.push_back(std::move(row));
  }
  return rows;
}

int& ActiveBluetoothSelection(LibraryState& lib) {
  return lib.bluetooth_show_paired ? lib.bluetooth_paired_selected
                                   : lib.bluetooth_scanned_selected;
}

const BluetoothDevice* SelectedBluetoothDevice(const LibraryState& lib) {
  const auto devices = BuildActiveBluetoothDeviceRefs(lib);
  if (devices.empty()) {
    return nullptr;
  }
  const int idx = lib.bluetooth_show_paired ? lib.bluetooth_paired_selected
                                            : lib.bluetooth_scanned_selected;
  if (idx < 0 || idx >= static_cast<int>(devices.size())) {
    return nullptr;
  }
  return devices[static_cast<std::size_t>(idx)];
}

const BluetoothDevice* FindBluetoothByAddress(const LibraryState& lib,
                                              const std::string& address) {
  for (const auto& d : lib.bluetooth_paired_devices) {
    if (d.address == address) {
      return &d;
    }
  }
  for (const auto& d : lib.bluetooth_scanned_devices) {
    if (d.address == address) {
      return &d;
    }
  }
  return nullptr;
}

void ClearBluetoothModal(LibraryState& lib) {
  lib.bluetooth_modal = BluetoothModalType::None;
  lib.bluetooth_modal_address.clear();
  lib.bluetooth_modal_name.clear();
}

void OpenBluetoothForgetModal(LibraryState& lib, const BluetoothDevice& d) {
  lib.bluetooth_modal = BluetoothModalType::ConfirmForget;
  lib.bluetooth_modal_address = d.address;
  lib.bluetooth_modal_name = d.name.empty() ? d.address : d.name;
  lib.status = "Confirm forget?";
}

void OpenBluetoothDisconnectAllModal(LibraryState& lib) {
  lib.bluetooth_modal = BluetoothModalType::ConfirmDisconnectAll;
  lib.bluetooth_modal_address.clear();
  lib.bluetooth_modal_name.clear();
  lib.status = "Disconnect all paired?";
}

bool ExecuteBluetoothAction(LibraryState& lib, std::string& status) {
  switch (lib.pending_bluetooth_action) {
    case PendingBluetoothAction::None:
      return false;
    case PendingBluetoothAction::RefreshScanned:
      return BluetoothRefreshScanned(lib, status);
    case PendingBluetoothAction::RefreshPaired:
      return BluetoothRefreshPaired(lib, status);
    case PendingBluetoothAction::RefreshBoth:
      return BluetoothRefreshBoth(lib, status);
    case PendingBluetoothAction::PairSelectedFromScanned: {
      const auto* d = SelectedBluetoothDevice(lib);
      if (!d) {
        status = "No device selected";
        return false;
      }
      const bool ok = BluetoothPairAndConnect(*d, status);
      std::string tmp;
      BluetoothRefreshBoth(lib, tmp);
      return ok;
    }
    case PendingBluetoothAction::ConnectSelectedFromPaired: {
      const auto* d = SelectedBluetoothDevice(lib);
      if (!d) {
        status = "No device selected";
        return false;
      }
      const bool ok = BluetoothConnectDevice(*d, status);
      std::string tmp;
      BluetoothRefreshBoth(lib, tmp);
      return ok;
    }
    case PendingBluetoothAction::DisconnectSelectedFromPaired: {
      const auto* d = SelectedBluetoothDevice(lib);
      if (!d) {
        status = "No device selected";
        return false;
      }
      const bool ok = BluetoothDisconnectDevice(*d, status);
      std::string tmp;
      BluetoothRefreshBoth(lib, tmp);
      return ok;
    }
    case PendingBluetoothAction::ForgetAddressFromPaired: {
      if (lib.bluetooth_pending_address.empty()) {
        status = "No device selected";
        return false;
      }
      BluetoothDevice target;
      target.address = lib.bluetooth_pending_address;
      if (const auto* found = FindBluetoothByAddress(lib, lib.bluetooth_pending_address)) {
        target.name = found->name;
        target.kind = found->kind;
        target.paired = found->paired;
        target.connected = found->connected;
      }
      const bool ok = BluetoothForgetDevice(target, status);
      std::string tmp;
      BluetoothRefreshBoth(lib, tmp);
      lib.bluetooth_pending_address.clear();
      return ok;
    }
    case PendingBluetoothAction::DisconnectAllPaired: {
      const bool ok = BluetoothDisconnectAllPaired(status);
      std::string tmp;
      BluetoothRefreshBoth(lib, tmp);
      return ok;
    }
  }
  return false;
}

bool QueueBluetoothAction(LibraryState& lib,
                          PendingBluetoothAction action,
                          std::string status_text) {
  if (lib.pending_bluetooth_action != PendingBluetoothAction::None) {
    lib.status = "Bluetooth busy";
    return false;
  }
  if (action != PendingBluetoothAction::ForgetAddressFromPaired) {
    lib.bluetooth_pending_address.clear();
  }
  ClearBluetoothModal(lib);
  lib.pending_bluetooth_action = action;
  lib.status = std::move(status_text);
  return true;
}

bool EnsureBluetoothSelectionValid(LibraryState& lib, std::string& status) {
  auto& sel = ActiveBluetoothSelection(lib);
  const auto devices = BuildActiveBluetoothDeviceRefs(lib);
  ClampSelection(sel, static_cast<int>(devices.size()));
  if (devices.empty()) {
    if (lib.bluetooth_show_paired) {
      status = "No paired BT devices";
    } else {
      status = std::string("No scanned BT devices (") +
               BluetoothFilterTag(lib.bluetooth_scan_filter) + ")";
    }
    return false;
  }
  return true;
}

bool ExportDiagnostics(const Args& args,
                       gb::db::Database& db,
                       const LibraryState& lib,
                       std::string& status) {
  const auto ts = gb::core::NowMs();
  std::error_code ec;
  std::filesystem::create_directories(args.diagnostics_dir, ec);

  const std::filesystem::path path =
      std::filesystem::path(args.diagnostics_dir) /
      ("diagnostics-" + std::to_string(ts) + ".txt");

  std::ofstream out(path);
  if (!out) {
    status = "diag export failed";
    return false;
  }

  const auto summary = db.ReadSummary();
  out << "GameBird Diagnostics\n";
  out << "timestamp_ms=" << ts << "\n";
  out << "db_path=" << args.db_path << "\n";
  out << "systems_loaded=" << lib.systems.size() << "\n";
  out << "games_visible=" << lib.games.size() << "\n";
  out << "summary_total=" << summary.total_games << "\n";
  out << "summary_present=" << summary.present_games << "\n";
  out << "summary_missing=" << summary.missing_games << "\n";
  std::vector<gb::db::LibraryRootState> roots;
  if (db.ListLibraryRoots(roots)) {
    out << "library_roots=" << roots.size() << "\n";
    for (std::size_t i = 0; i < roots.size(); ++i) {
      out << "root_" << i << "_path=" << roots[i].root_path << "\n";
      out << "root_" << i << "_status=" << roots[i].status << "\n";
      out << "root_" << i << "_error=" << roots[i].error << "\n";
      out << "root_" << i << "_device_id=" << roots[i].device_id << "\n";
      out << "root_" << i << "_last_scan_at=" << roots[i].last_scan_at << "\n";
      out << "root_" << i << "_files_seen=" << roots[i].files_seen << "\n";
    }
  }
  out << "jobs_queued=" << db.CountJobsByStatus("queued") << "\n";
  out << "jobs_running=" << db.CountJobsByStatus("running") << "\n";
  out << "jobs_ok=" << db.CountJobsByStatus("ok") << "\n";
  out << "jobs_error=" << db.CountJobsByStatus("error") << "\n";
  out << "show_diagnostics=" << (lib.settings.show_diagnostics ? "1" : "0")
      << "\n";
  out << "show_hidden_games=" << (lib.settings.show_hidden_games ? "1" : "0")
      << "\n";
  out << "bluetooth_gamepads=" << (lib.settings.enable_bluetooth_gamepads ? "1" : "0")
      << "\n";
  out << "input_profiles=" << lib.input_profiles.size() << "\n";

  status = "diag: " + path.filename().string();
  return true;
}

void RunToolAction(const Args& args,
                   gb::db::Database& db,
                   LibraryState& lib,
                   gb::ui::UIState& ui) {
  const auto cfg = MakeWorkerConfig(args, lib);
  if (lib.tools_selected != 11) {
    lib.tools_exit_confirm = false;
  }

  switch (lib.tools_selected) {
    case 0:
      if (RunJobs(db, cfg, true, false, false, lib.status)) {
        LoadSystems(db, lib);
        ReloadGameList(db, lib);
      }
      break;

    case 1:
      if (RunJobs(db, cfg, false, true, false, lib.status)) {
        ReloadGameList(db, lib);
      }
      break;

    case 2:
      db.EnqueueJob("build_thumb", "{}");
      if (RunJobs(db, cfg, false, false, false, lib.status)) {
        lib.status = "artwork refresh complete";
      }
      break;

    case 3: {
      gb::scrape::WorkerStats stats;
      gb::scrape::ProcessJobsUntilEmpty(db, cfg, stats);
      lib.status = "ran jobs ok=" + std::to_string(stats.jobs_ok) +
                   " err=" + std::to_string(stats.jobs_error);
      break;
    }

    case 4:
      ExportDiagnostics(args, db, lib, lib.status);
      break;

    case 5:
      ui.screen = gb::ui::Screen::InputSetup;
      lib.status = "Input setup";
      break;

    case 6:
      ui.screen = gb::ui::Screen::Wifi;
      lib.wifi_view = gb::ui::screens::WifiView::Overview;
      lib.wifi_selected = 0;
      lib.wifi_networks.clear();
      lib.wifi_password.clear();
      lib.wifi_country_entry.clear();
      QueueWifiAction(lib, PendingWifiAction::RefreshStatus,
                      "Checking Wi-Fi...");
      break;

    case 7:
      ui.screen = gb::ui::Screen::Update;
      if (!UpdateIsBusy(lib.update_status) &&
          lib.update_status.os_updates == 0 &&
          !lib.update_status.shell_update) {
        StartUpdaterUnit("gamebird-update-check.service", lib.status);
        lib.update_status.phase = "CHECKING";
        lib.update_status.message = lib.status;
        lib.update_status.progress = 0;
      }
      break;

    case 8:
      lib.settings.enable_bluetooth_gamepads =
          !lib.settings.enable_bluetooth_gamepads;
      lib.status = lib.settings.enable_bluetooth_gamepads
                       ? "Bluetooth gamepads enabled"
                       : "Bluetooth gamepads disabled";
      lib.settings.input_profiles = EncodeInputProfiles(lib.input_profiles);
      {
        std::string err;
        gb::core::SaveRuntimeSettings(args.settings_path, lib.settings, err);
      }
      break;

    case 9:
      if (!lib.pending_launch_retroarch_menu && lib.pending_launch_game_id == 0) {
        lib.pending_launch_retroarch_menu = true;
        lib.status = "LAUNCHING RETROARCH...";
      } else {
        lib.status = "launch busy";
      }
      break;

    case 10:
      ui.screen = gb::ui::Screen::Bluetooth;
      lib.bluetooth_show_paired = false;
      lib.bluetooth_scanned_selected = 0;
      lib.bluetooth_paired_selected = 0;
      lib.bluetooth_scan_filter = BluetoothScanFilter::All;
      ClearBluetoothModal(lib);
      lib.bluetooth_pending_address.clear();
      QueueBluetoothAction(lib, PendingBluetoothAction::RefreshBoth,
                           "Refreshing Bluetooth...");
      break;

    case 11:
      if (!lib.tools_exit_confirm) {
        lib.tools_exit_confirm = true;
        lib.status = "Exit to console? A=yes B=no";
      } else {
        lib.status = "Exiting to console...";
        ui.running = false;
      }
      break;

    default:
      lib.status = "unknown tool";
      break;
  }

  ui.needs_redraw = true;
}

void HandleButton(gb::platform::Button button,
                  gb::ui::UIState& ui,
                  LibraryState& lib,
                  gb::db::Database& db,
                  const Args& args) {
  RememberBrowse(lib, ui);
  if (ui.screen == gb::ui::Screen::Bluetooth &&
      lib.bluetooth_modal != BluetoothModalType::None) {
    if (button == gb::platform::Button::A) {
      if (lib.bluetooth_modal == BluetoothModalType::ConfirmForget) {
        if (lib.bluetooth_modal_address.empty()) {
          lib.status = "No device selected";
          ClearBluetoothModal(lib);
        } else {
          lib.bluetooth_pending_address = lib.bluetooth_modal_address;
          QueueBluetoothAction(lib, PendingBluetoothAction::ForgetAddressFromPaired,
                               "Forgetting selected device...");
        }
      } else if (lib.bluetooth_modal == BluetoothModalType::ConfirmDisconnectAll) {
        QueueBluetoothAction(lib, PendingBluetoothAction::DisconnectAllPaired,
                             "Disconnecting all paired...");
      }
      ui.needs_redraw = true;
      return;
    }
    if (button == gb::platform::Button::B) {
      ClearBluetoothModal(lib);
      lib.status = "Cancelled";
      ui.needs_redraw = true;
      return;
    }
    return;
  }

  if (ui.screen == gb::ui::Screen::ScrapeProgress) {
    if (button == gb::platform::Button::B) {
      if (lib.scrape_active) {
        const int remaining = std::max(0, lib.scrape_progress.total -
                                              lib.scrape_progress.completed);
        lib.scrape_active = false;
        lib.status = "Scrape cancelled; " + std::to_string(remaining) + " remaining";
      }
      ui.screen = gb::ui::Screen::Settings;
      ui.needs_redraw = true;
    } else if (button == gb::platform::Button::A && !lib.scrape_active) {
      ui.screen = gb::ui::Screen::Settings;
      ui.needs_redraw = true;
    }
    return;
  }

  // Short Start opens the contextual menu; held Start remains battery/volume.
  if (button == gb::platform::Button::Start) {
    lib.tools_exit_confirm = false;
    if (ui.screen == gb::ui::Screen::GameList && !lib.games.empty()) {
      ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
      OpenGameMenu(db, ui, lib, args, lib.games[lib.game_selected].id);
    } else if (ui.screen == gb::ui::Screen::Details) {
      ui.screen = gb::ui::Screen::GameMenu;
    } else if (ui.screen == gb::ui::Screen::GameMenu) {
      ui.screen = lib.game_menu_return;
    } else if (ui.screen == gb::ui::Screen::Systems) {
      lib.context_selected = 0;
      ui.screen = gb::ui::Screen::SystemMenu;
    } else if (ui.screen == gb::ui::Screen::SystemMenu) {
      ui.screen = gb::ui::Screen::Systems;
    } else if (ui.screen == gb::ui::Screen::Home) {
      ui.screen = ui.menu_return_screen;
    } else {
      ui.menu_return_screen = ui.screen;
      ui.screen = gb::ui::Screen::Home;
      RefreshContinue(db, lib, ui);
    }
    ui.needs_redraw = true;
    return;
  }

  if (button == gb::platform::Button::Select) {
    lib.tools_exit_confirm = false;
    LoadSystems(db, lib);
    lib.status.clear();
    ui.screen = gb::ui::Screen::Systems;
    ui.needs_redraw = true;
    return;
  }

  switch (ui.screen) {
    case gb::ui::Screen::Home:
      if (button == gb::platform::Button::Up || button == gb::platform::Button::Down) {
        ui.home_selected = (ui.home_selected + (button == gb::platform::Button::Up ? 5 : 1)) % 6;
      } else if (button == gb::platform::Button::A) {
        if (ui.home_selected == 0) {
          RefreshContinue(db, lib, ui);
          if (lib.continue_game_id) OpenGameMenu(db, ui, lib, args, lib.continue_game_id);
          else lib.status = "Play a game to see it here";
        } else if (ui.home_selected == 1) {
          ui.screen = lib.browse.screen == "games" ? gb::ui::Screen::GameList : gb::ui::Screen::Systems;
          if (ui.screen == gb::ui::Screen::GameList) ReloadGameList(db, lib);
        } else if (ui.home_selected == 2 || ui.home_selected == 3) {
          lib.game_list_view = ui.home_selected == 2 ? GameListView::Recent : GameListView::Favorites;
          lib.game_selected = 0;
          if (ui.home_selected == 2) LoadRecentGames(db, lib); else LoadFavoriteGames(db, lib);
          lib.status.clear();
          ui.screen = gb::ui::Screen::GameList;
        } else if (ui.home_selected == 4) {
          lib.tools_exit_confirm = false;
          ui.screen = gb::ui::Screen::Tools;
        } else if (ui.home_selected == 5) ui.screen = gb::ui::Screen::Settings;
      } else if (button == gb::platform::Button::B) ui.screen = ui.menu_return_screen;
      ui.needs_redraw = true;
      return;

    case gb::ui::Screen::SystemMenu:
      if (button == gb::platform::Button::Up || button == gb::platform::Button::Down)
        lib.context_selected = (lib.context_selected + (button == gb::platform::Button::Up ? 2 : 1)) % 3;
      else if (button == gb::platform::Button::B) ui.screen = gb::ui::Screen::Systems;
      else if (button == gb::platform::Button::A) {
        if (lib.context_selected == 0) OpenSystem(db, ui, lib);
        else if (lib.context_selected == 1) OpenLaunchOptionsForSystem(db, ui, lib);
        else { ui.menu_return_screen = gb::ui::Screen::Systems; ui.screen = gb::ui::Screen::Home; RefreshContinue(db, lib, ui); }
      }
      ui.needs_redraw = true;
      return;

    case gb::ui::Screen::GameMenu: {
      const auto actions = GameActions(lib);
      const int count = static_cast<int>(actions.size());
      ClampSelection(lib.context_selected, count);
      if (button == gb::platform::Button::Up || button == gb::platform::Button::Down)
        lib.context_selected = (lib.context_selected + (button == gb::platform::Button::Up ? count - 1 : 1)) % count;
      else if (button == gb::platform::Button::B) ui.screen = lib.game_menu_return;
      else if (button == gb::platform::Button::A) {
        switch (actions[lib.context_selected].first) {
          case GameAction::Resume:
          case GameAction::Fresh:
          case GameAction::Backup:
            lib.pending_play_mode = actions[lib.context_selected].first == GameAction::Resume ? gb::core::PlayMode::Resume :
                actions[lib.context_selected].first == GameAction::Backup ? gb::core::PlayMode::Backup : gb::core::PlayMode::Fresh;
            lib.pending_launch_game_id = lib.menu_game.id;
            lib.status = lib.pending_play_mode == gb::core::PlayMode::Fresh ? "Starting game..." : "Resuming game...";
            break;
          case GameAction::Details:
            lib.details = lib.menu_game; lib.details_ready = true;
            ui.screen = gb::ui::Screen::Details;
            break;
          case GameAction::Favorite: {
            bool value = false;
            if (db.ToggleGameFavorite(lib.menu_game.id, value)) {
              lib.status = value ? "Added to favorites" : "Removed from favorites";
              RefreshGameMenu(db, lib, args); ReloadGameList(db, lib); LoadSystems(db, lib);
            } else lib.status = "Could not change favorite";
            break;
          }
          case GameAction::Hide: {
            bool value = false;
            if (db.ToggleGameHidden(lib.menu_game.id, value)) {
              lib.status = value ? "Game hidden" : "Game visible";
              ReloadGameList(db, lib); LoadSystems(db, lib);
              ui.screen = lib.game_menu_return;
              RefreshContinue(db, lib, ui);
            } else lib.status = "Could not change visibility";
            break;
          }
          case GameAction::Options: OpenLaunchOptionsForGame(db, ui, lib); break;
          case GameAction::Home:
            ui.menu_return_screen = gb::ui::Screen::GameMenu;
            ui.screen = gb::ui::Screen::Home;
            RefreshContinue(db, lib, ui);
            break;
        }
      }
      ui.needs_redraw = true;
      return;
    }

    case gb::ui::Screen::Systems:
      if (button == gb::platform::Button::Up ||
          button == gb::platform::Button::Left) {
        if (!lib.systems.empty()) {
          lib.system_selected =
              (lib.system_selected + static_cast<int>(lib.systems.size()) - 1) %
              static_cast<int>(lib.systems.size());
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Down ||
          button == gb::platform::Button::Right) {
        if (!lib.systems.empty()) {
          lib.system_selected =
              (lib.system_selected + 1) % static_cast<int>(lib.systems.size());
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::A) {
        OpenSystem(db, ui, lib);
        return;
      }
      if (button == gb::platform::Button::B) {
        ui.menu_return_screen = gb::ui::Screen::Systems;
        ui.screen = gb::ui::Screen::Home;
        RefreshContinue(db, lib, ui);
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::GameList:
      if (button == gb::platform::Button::Up) {
        if (!lib.games.empty()) {
          lib.game_selected =
              (lib.game_selected + static_cast<int>(lib.games.size()) - 1) %
              static_cast<int>(lib.games.size());
          if (lib.game_list_view == GameListView::System) {
            RefreshSelectedGameDetails(db, lib);
          }
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Down) {
        if (!lib.games.empty()) {
          lib.game_selected =
              (lib.game_selected + 1) % static_cast<int>(lib.games.size());
          if (lib.game_list_view == GameListView::System) {
            RefreshSelectedGameDetails(db, lib);
          }
          ui.needs_redraw = true;
        }
        return;
      }
      if (lib.game_list_view == GameListView::System &&
          (button == gb::platform::Button::Left ||
           button == gb::platform::Button::Right)) {
        SelectSystem(db, lib,
                     lib.system_selected +
                         (button == gb::platform::Button::Left ? -1 : 1));
        lib.status.clear();
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::A) {
        if (!lib.games.empty()) {
          ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
          OpenGameMenu(db, ui, lib, args, lib.games[lib.game_selected].id);
        }
        return;
      }
      if (button == gb::platform::Button::B) {
        if (lib.game_list_view == GameListView::System) {
          ui.screen = gb::ui::Screen::Systems;
          LoadSystems(db, lib);
        } else {
          ui.screen = gb::ui::Screen::Home;
        }
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::Settings:
      if (button == gb::platform::Button::Up) {
        lib.settings_selected = (lib.settings_selected + 5) % 6;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Down) {
        lib.settings_selected = (lib.settings_selected + 1) % 6;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::A) {
        if (lib.settings_selected == 0) {
          lib.settings.show_diagnostics = !lib.settings.show_diagnostics;
          ui.show_diagnostics = lib.settings.show_diagnostics;
          lib.status = lib.settings.show_diagnostics ? "diag on" : "diag off";
        } else if (lib.settings_selected == 1) {
          lib.settings.show_hidden_games = !lib.settings.show_hidden_games;
          ReloadGameList(db, lib);
          lib.status = lib.settings.show_hidden_games ? "hidden visible" : "hidden hidden";
        } else if (lib.settings_selected == 2) {
          lib.settings.scrape_provider =
              lib.settings.scrape_provider == "none" ? "libretro" : "none";
          lib.status = lib.settings.scrape_provider == "none"
                           ? "scraper disabled"
                           : "Libretro scraper selected";
        } else if (lib.settings_selected == 3) {
          if (lib.settings.scrape_provider == "none") {
            lib.status = "select a scraper first";
          } else {
            const auto cfg = MakeWorkerConfig(args, lib);
            if (lib.scrape_session.Begin(db, cfg, lib.scrape_progress)) {
              lib.scrape_active = !lib.scrape_progress.finished;
              lib.status = lib.scrape_progress.finished
                               ? "All present games already have art"
                               : "Preparing scrape...";
              ui.screen = gb::ui::Screen::ScrapeProgress;
            } else {
              lib.status = "scrape setup failed: " + lib.scrape_progress.last_error;
            }
          }
        } else if (lib.settings_selected == 4) {
          ui.screen = gb::ui::Screen::InputSetup;
          lib.status = "Input setup";
        } else {
          if (SaveInputSettings(args, lib, lib.status)) {
            lib.status = "settings saved";
          }
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::B) {
        ui.screen = gb::ui::Screen::Home;
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::ScrapeProgress:
      // Handled before the global navigation shortcuts above.
      return;

    case gb::ui::Screen::Tools:
      if (lib.tools_exit_confirm) {
        if (button == gb::platform::Button::A) {
          RunToolAction(args, db, lib, ui);
          return;
        }
        if (button == gb::platform::Button::B) {
          lib.tools_exit_confirm = false;
          lib.status = "Exit cancelled";
          ui.needs_redraw = true;
          return;
        }
      }
      if (button == gb::platform::Button::Up) {
        lib.tools_exit_confirm = false;
        lib.tools_selected = (lib.tools_selected + 11) % 12;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Down) {
        lib.tools_exit_confirm = false;
        lib.tools_selected = (lib.tools_selected + 1) % 12;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::A) {
        RunToolAction(args, db, lib, ui);
        return;
      }
      if (button == gb::platform::Button::B) {
        lib.tools_exit_confirm = false;
        ui.screen = gb::ui::Screen::Home;
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::Update: {
      const bool busy = UpdateIsBusy(lib.update_status);
      if (button == gb::platform::Button::A && !busy) {
        const bool available = lib.update_status.os_updates > 0 ||
                               lib.update_status.shell_update;
        StartUpdaterUnit(available ? "gamebird-update.service"
                                   : "gamebird-update-check.service",
                         lib.status);
        lib.update_status.phase = available ? "STARTING" : "CHECKING";
        lib.update_status.message = lib.status;
        lib.update_status.progress = 0;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::B && !busy) {
        ui.screen = gb::ui::Screen::Tools;
        ui.needs_redraw = true;
        return;
      }
      break;
    }

    case gb::ui::Screen::InputSetup:
      if (button == gb::platform::Button::A) {
        lib.input_capture_active = true;
        lib.input_capture_step = 0;
        lib.input_capture_arming = true;
        lib.input_capture_arm_button = button;
        lib.input_capture_arm_raw_type = lib.last_input_raw_type;
        lib.input_capture_arm_raw_code = lib.last_input_raw_code;
        lib.input_capture_arm_until_ms = gb::core::NowMs() + kRemapArmDelayMs;
        lib.input_capture_device = lib.last_input_device;
        lib.input_capture_bus_type = lib.last_input_bus_type;
        lib.input_capture_vendor = lib.last_input_vendor;
        lib.input_capture_product = lib.last_input_product;
        lib.input_capture_has_id = lib.last_input_has_id;
        SeedCaptureProfileFromExisting(lib, lib.input_capture_device,
                                       lib.input_capture_bus_type,
                                       lib.input_capture_vendor,
                                       lib.input_capture_product,
                                       lib.input_capture_map,
                                       lib.input_capture_keycode_map,
                                       lib.input_capture_retroarch_bindings);
        lib.status = "Release start button...";
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::X) {
        if (lib.last_input_device.empty()) {
          lib.status = "No active input device";
        } else {
          RemoveInputProfile(lib, lib.last_input_device, lib.last_input_bus_type,
                             lib.last_input_vendor, lib.last_input_product,
                             lib.last_input_has_id);
          if (SaveInputSettings(args, lib, lib.status)) {
            lib.status = "profile cleared";
          }
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Y) {
        lib.input_test_seen.fill(false);
        lib.input_test_last = "none";
        lib.input_test_press_count = 0;
        lib.status = "Press buttons; Select returns";
        ui.screen = gb::ui::Screen::InputTest;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::B) {
        lib.input_capture_active = false;
        lib.input_capture_arming = false;
        ui.screen = gb::ui::Screen::Settings;
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::InputTest:
      if (button == gb::platform::Button::Select) {
        ui.screen = gb::ui::Screen::InputSetup;
        lib.status = "Input test complete";
        ui.needs_redraw = true;
      }
      return;

    case gb::ui::Screen::Wifi: {
      using WifiView = gb::ui::screens::WifiView;
      if (lib.wifi_view == WifiView::Overview) {
        if (button == gb::platform::Button::Up) {
          lib.wifi_selected = (lib.wifi_selected + 2) % 3;
          ui.needs_redraw = true;
          return;
        }
        if (button == gb::platform::Button::Down) {
          lib.wifi_selected = (lib.wifi_selected + 1) % 3;
          ui.needs_redraw = true;
          return;
        }
        if (button == gb::platform::Button::A) {
          if (lib.wifi_selected == 0) {
            lib.wifi_view = WifiView::Networks;
            lib.wifi_selected = 0;
            QueueWifiAction(lib, PendingWifiAction::Scan, "Scanning nearby Wi-Fi...");
          } else if (lib.wifi_selected == 1) {
            if (lib.wifi_connected_ssid.empty()) {
              lib.status = "No Wi-Fi connection";
            } else {
              QueueWifiAction(lib, PendingWifiAction::Disconnect, "Disconnecting Wi-Fi...");
            }
          } else {
            lib.wifi_view = WifiView::Country;
            lib.wifi_selected = 0;
            lib.wifi_country_entry.clear();
            lib.status.clear();
          }
          ui.needs_redraw = true;
          return;
        }
        if (button == gb::platform::Button::B) {
          ui.screen = gb::ui::Screen::Tools;
          ui.needs_redraw = true;
          return;
        }
        break;
      }
      if (lib.wifi_view == WifiView::Networks) {
        if (button == gb::platform::Button::Up && !lib.wifi_networks.empty()) {
          lib.wifi_selected = (lib.wifi_selected +
                               static_cast<int>(lib.wifi_networks.size()) - 1) %
                              static_cast<int>(lib.wifi_networks.size());
          ui.needs_redraw = true;
          return;
        }
        if (button == gb::platform::Button::Down && !lib.wifi_networks.empty()) {
          lib.wifi_selected = (lib.wifi_selected + 1) %
                              static_cast<int>(lib.wifi_networks.size());
          ui.needs_redraw = true;
          return;
        }
        if (button == gb::platform::Button::X) {
          QueueWifiAction(lib, PendingWifiAction::Scan, "Scanning nearby Wi-Fi...");
          ui.needs_redraw = true;
          return;
        }
        if (button == gb::platform::Button::A && !lib.wifi_networks.empty()) {
          ClampSelection(lib.wifi_selected, static_cast<int>(lib.wifi_networks.size()));
          const auto& network =
              lib.wifi_networks[static_cast<std::size_t>(lib.wifi_selected)];
          lib.wifi_selected_ssid = network.ssid;
          lib.wifi_password.clear();
          if (network.secured && !network.active) {
            lib.wifi_view = WifiView::Password;
            lib.wifi_selected = 0;
            lib.wifi_keyboard_page = 0;
            lib.status.clear();
          } else {
            QueueWifiAction(lib, PendingWifiAction::Connect,
                            "Connecting to " + network.ssid + "...");
          }
          ui.needs_redraw = true;
          return;
        }
        if (button == gb::platform::Button::B) {
          lib.wifi_view = WifiView::Overview;
          lib.wifi_selected = 0;
          QueueWifiAction(lib, PendingWifiAction::RefreshStatus, "Checking Wi-Fi...");
          ui.needs_redraw = true;
          return;
        }
        break;
      }
      if (lib.wifi_view == WifiView::Password) {
        constexpr int kKeyboardCount = 36;
        if (button == gb::platform::Button::Left) {
          lib.wifi_selected = (lib.wifi_selected + kKeyboardCount - 1) % kKeyboardCount;
        } else if (button == gb::platform::Button::Right) {
          lib.wifi_selected = (lib.wifi_selected + 1) % kKeyboardCount;
        } else if (button == gb::platform::Button::Up) {
          lib.wifi_selected = (lib.wifi_selected + kKeyboardCount - 6) % kKeyboardCount;
        } else if (button == gb::platform::Button::Down) {
          lib.wifi_selected = (lib.wifi_selected + 6) % kKeyboardCount;
        } else if (button == gb::platform::Button::L) {
          lib.wifi_keyboard_page = (lib.wifi_keyboard_page + 2) % 3;
        } else if (button == gb::platform::Button::R) {
          lib.wifi_keyboard_page = (lib.wifi_keyboard_page + 1) % 3;
        } else if (button == gb::platform::Button::A) {
          static constexpr std::string_view kPages[] = {
              "abcdefghijklmnopqrstuvwxyz0123456789",
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
              "!@#$%^&*()-_=+[]{};:,.?/\\|~`'\"     ",
          };
          if (lib.wifi_password.size() < 63) {
            lib.wifi_password += kPages[lib.wifi_keyboard_page]
                                         [static_cast<std::size_t>(lib.wifi_selected)];
          }
        } else if (button == gb::platform::Button::Y) {
          if (!lib.wifi_password.empty()) lib.wifi_password.pop_back();
        } else if (button == gb::platform::Button::X) {
          if (lib.wifi_password.empty()) {
            lib.status = "Enter the network password";
          } else {
            lib.wifi_view = WifiView::Networks;
            QueueWifiAction(lib, PendingWifiAction::Connect,
                            "Connecting to " + lib.wifi_selected_ssid + "...");
          }
        } else if (button == gb::platform::Button::B) {
          lib.wifi_view = WifiView::Networks;
          lib.wifi_password.clear();
        } else {
          break;
        }
        ui.needs_redraw = true;
        return;
      }

      constexpr std::string_view kCountryLetters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
      constexpr int kCountryCount = 26;
      if (button == gb::platform::Button::Left) {
        lib.wifi_selected = (lib.wifi_selected + kCountryCount - 1) % kCountryCount;
      } else if (button == gb::platform::Button::Right) {
        lib.wifi_selected = (lib.wifi_selected + 1) % kCountryCount;
      } else if (button == gb::platform::Button::Up) {
        lib.wifi_selected = (lib.wifi_selected + kCountryCount - 7) % kCountryCount;
      } else if (button == gb::platform::Button::Down) {
        lib.wifi_selected = (lib.wifi_selected + 7) % kCountryCount;
      } else if (button == gb::platform::Button::A) {
        if (lib.wifi_country_entry.size() < 2) {
          lib.wifi_country_entry += kCountryLetters[static_cast<std::size_t>(lib.wifi_selected)];
        }
      } else if (button == gb::platform::Button::Y) {
        if (!lib.wifi_country_entry.empty()) lib.wifi_country_entry.pop_back();
      } else if (button == gb::platform::Button::X) {
        if (lib.wifi_country_entry.size() == 2) {
          lib.wifi_view = WifiView::Overview;
          lib.wifi_selected = 2;
          QueueWifiAction(lib, PendingWifiAction::SetCountry, "Setting wireless region...");
        } else {
          lib.status = "Choose a two-letter country";
        }
      } else if (button == gb::platform::Button::B) {
        lib.wifi_view = WifiView::Overview;
        lib.wifi_selected = 2;
      } else {
        break;
      }
      ui.needs_redraw = true;
      return;
    }

    case gb::ui::Screen::Bluetooth:
      if (button == gb::platform::Button::Left ||
          button == gb::platform::Button::Right) {
        ClearBluetoothModal(lib);
        lib.bluetooth_show_paired = !lib.bluetooth_show_paired;
        EnsureBluetoothSelectionValid(lib, lib.status);
        ui.needs_redraw = true;
        return;
      }
      if (!lib.bluetooth_show_paired && button == gb::platform::Button::L) {
        CycleBluetoothFilter(lib, -1);
        EnsureBluetoothSelectionValid(lib, lib.status);
        ui.needs_redraw = true;
        return;
      }
      if (!lib.bluetooth_show_paired && button == gb::platform::Button::R) {
        CycleBluetoothFilter(lib, 1);
        EnsureBluetoothSelectionValid(lib, lib.status);
        ui.needs_redraw = true;
        return;
      }
      if (lib.bluetooth_show_paired && button == gb::platform::Button::R) {
        OpenBluetoothDisconnectAllModal(lib);
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Up) {
        ClearBluetoothModal(lib);
        auto& sel = ActiveBluetoothSelection(lib);
        const auto devices = BuildActiveBluetoothDeviceRefs(lib);
        if (!devices.empty()) {
          sel = (sel + static_cast<int>(devices.size()) - 1) %
                static_cast<int>(devices.size());
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Down) {
        ClearBluetoothModal(lib);
        auto& sel = ActiveBluetoothSelection(lib);
        const auto devices = BuildActiveBluetoothDeviceRefs(lib);
        if (!devices.empty()) {
          sel = (sel + 1) % static_cast<int>(devices.size());
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::X) {
        if (lib.bluetooth_show_paired) {
          if (!EnsureBluetoothSelectionValid(lib, lib.status)) {
            ClearBluetoothModal(lib);
            ui.needs_redraw = true;
            return;
          }
          const auto* d = SelectedBluetoothDevice(lib);
          if (!d) {
            lib.status = "No device selected";
            ClearBluetoothModal(lib);
            ui.needs_redraw = true;
            return;
          }
          OpenBluetoothForgetModal(lib, *d);
        } else {
          ClearBluetoothModal(lib);
          QueueBluetoothAction(lib, PendingBluetoothAction::RefreshScanned,
                               "Scanning nearby devices...");
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Y) {
        ClearBluetoothModal(lib);
        if (lib.bluetooth_show_paired) {
          if (!EnsureBluetoothSelectionValid(lib, lib.status)) {
            ui.needs_redraw = true;
            return;
          }
          QueueBluetoothAction(lib,
                               PendingBluetoothAction::DisconnectSelectedFromPaired,
                               "Disconnecting selected device...");
        } else {
          QueueBluetoothAction(lib, PendingBluetoothAction::RefreshBoth,
                               "Refreshing Bluetooth...");
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::A) {
        ClearBluetoothModal(lib);
        if (!EnsureBluetoothSelectionValid(lib, lib.status)) {
          ui.needs_redraw = true;
          return;
        }
        if (lib.bluetooth_show_paired) {
          QueueBluetoothAction(lib, PendingBluetoothAction::ConnectSelectedFromPaired,
                               "Connecting selected device...");
        } else {
          QueueBluetoothAction(lib, PendingBluetoothAction::PairSelectedFromScanned,
                               "Pairing selected device...");
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::B) {
        ClearBluetoothModal(lib);
        ui.screen = gb::ui::Screen::Tools;
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::LaunchOptions:
      if (button == gb::platform::Button::Up) {
        lib.launch_options_selected =
            (lib.launch_options_selected + kLaunchOptionsRowCount - 1) %
            kLaunchOptionsRowCount;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Down) {
        lib.launch_options_selected =
            (lib.launch_options_selected + 1) % kLaunchOptionsRowCount;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Left) {
        if (lib.launch_options_selected == 1) {
          const int count = static_cast<int>(lib.launch_options_core_paths.size());
          if (count > 0) {
            lib.launch_options_core_selected =
                (lib.launch_options_core_selected + count - 1) % count;
            ui.needs_redraw = true;
          }
        } else if (lib.launch_options_selected == 2) {
          const int count = static_cast<int>(kLaunchAudioChoices.size());
          lib.launch_options_audio_selected =
              (lib.launch_options_audio_selected + count - 1) % count;
          ui.needs_redraw = true;
        } else if (lib.launch_options_selected == 3) {
          const int count = static_cast<int>(kLaunchVideoChoices.size());
          lib.launch_options_video_selected =
              (lib.launch_options_video_selected + count - 1) % count;
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Right) {
        if (lib.launch_options_selected == 1) {
          const int count = static_cast<int>(lib.launch_options_core_paths.size());
          if (count > 0) {
            lib.launch_options_core_selected =
                (lib.launch_options_core_selected + 1) % count;
            ui.needs_redraw = true;
          }
        } else if (lib.launch_options_selected == 2) {
          const int count = static_cast<int>(kLaunchAudioChoices.size());
          lib.launch_options_audio_selected =
              (lib.launch_options_audio_selected + 1) % count;
          ui.needs_redraw = true;
        } else if (lib.launch_options_selected == 3) {
          const int count = static_cast<int>(kLaunchVideoChoices.size());
          lib.launch_options_video_selected =
              (lib.launch_options_video_selected + 1) % count;
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::A) {
        if (lib.launch_options_selected == 1) {
          const int count = static_cast<int>(lib.launch_options_core_paths.size());
          if (count > 0) {
            lib.launch_options_core_selected =
                (lib.launch_options_core_selected + 1) % count;
          }
        } else if (lib.launch_options_selected == 2) {
          const int count = static_cast<int>(kLaunchAudioChoices.size());
          lib.launch_options_audio_selected =
              (lib.launch_options_audio_selected + 1) % count;
        } else if (lib.launch_options_selected == 3) {
          const int count = static_cast<int>(kLaunchVideoChoices.size());
          lib.launch_options_video_selected =
              (lib.launch_options_video_selected + 1) % count;
        } else if (lib.launch_options_selected == 4) {
          SaveLaunchOptions(db, lib);
        } else if (lib.launch_options_selected == 5) {
          if (db.DeleteLaunchOverride(lib.launch_options_scope_type,
                                      lib.launch_options_scope_id)) {
            ResetLaunchOptionsToInherited(lib);
            RefreshEffectiveLaunch(db, lib);
            lib.status = "Launch override cleared";
          } else {
            lib.status = "Override clear failed";
          }
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::X) {
        if (lib.launch_options_selected == 1) {
          const int count = static_cast<int>(lib.launch_options_core_paths.size());
          if (count > 0) {
            lib.launch_options_core_selected =
                (lib.launch_options_core_selected + count - 1) % count;
          }
        } else if (lib.launch_options_selected == 2) {
          const int count = static_cast<int>(kLaunchAudioChoices.size());
          lib.launch_options_audio_selected =
              (lib.launch_options_audio_selected + count - 1) % count;
        } else if (lib.launch_options_selected == 3) {
          const int count = static_cast<int>(kLaunchVideoChoices.size());
          lib.launch_options_video_selected =
              (lib.launch_options_video_selected + count - 1) % count;
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::B) {
        if (lib.launch_options_return_screen == gb::ui::Screen::GameMenu) RefreshGameMenu(db, lib, args);
        ui.screen = lib.launch_options_return_screen;
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::Details:
      if (button == gb::platform::Button::A || button == gb::platform::Button::B) {
        ui.screen = gb::ui::Screen::GameMenu;
        ui.needs_redraw = true;
      }
      return;
  }
}

void DrawBatteryHud(gb::render::Surface240& surface,
                    const gb::render::Theme& theme,
                    const LibraryState& lib) {
  if (!lib.battery_hud_visible) {
    return;
  }

  constexpr auto kWarningRed = gb::render::Rgb565(230, 72, 72);
  const bool low = lib.battery_status.available && !lib.battery_status.charging &&
                   lib.battery_status.percent <= 20;
  const bool critical = low && lib.battery_status.percent < 5;
  const bool flash_on = !critical || ((gb::core::NowMs() / 400) % 2 == 0);

  if (lib.battery_start_held) {
    constexpr int kVolumeX = 8;
    constexpr int kVolumeY = 10;
    surface.FillRect(kVolumeX, kVolumeY, 74, 30, gb::render::Rgb565(0, 0, 0));
    surface.StrokeRect(kVolumeX, kVolumeY, 74, 30, theme.panel_border);
    gb::ui::widgets::DrawText(surface, kVolumeX + 6, kVolumeY + 5, "VOL", theme.text_dim, 1);
    if (lib.volume_percent >= 0) {
      const int fill = std::clamp((lib.volume_percent * 42) / 100, 0, 42);
      surface.StrokeRect(kVolumeX + 25, kVolumeY + 7, 44, 9, theme.text_dim);
      surface.FillRect(kVolumeX + 26, kVolumeY + 8, fill, 7, theme.accent);
      gb::ui::widgets::DrawText(surface, kVolumeX + 6, kVolumeY + 19,
                                std::to_string(lib.volume_percent) + "%", theme.text, 1);
      gb::ui::widgets::DrawText(surface, kVolumeX + 33, kVolumeY + 19,
                                "UP/DOWN", theme.text_dim, 1);
    } else {
      gb::ui::widgets::DrawText(surface, kVolumeX + 29, kVolumeY + 10,
                                "UNAVAILABLE", theme.text_dim, 1);
    }
  }

  constexpr int kX = 174;
  constexpr int kY = 10;
  surface.FillRect(kX, kY, 58, 30, gb::render::Rgb565(0, 0, 0));
  surface.StrokeRect(kX, kY, 58, 30, low ? kWarningRed : theme.panel_border);
  if (!lib.battery_status.available) {
    gb::ui::widgets::DrawText(surface, kX + 6, kY + 10, "BAT N/A", theme.text_dim, 1);
    return;
  }

  constexpr int kBatteryX = kX + 5;
  constexpr int kBatteryY = kY + 5;
  constexpr int kBatteryW = 22;
  constexpr int kBatteryH = 11;
  const auto outline = low ? kWarningRed : theme.text;
  surface.StrokeRect(kBatteryX, kBatteryY, kBatteryW, kBatteryH, outline);
  surface.FillRect(kBatteryX + kBatteryW, kBatteryY + 3, 2, 5, outline);
  const int fill = std::clamp((lib.battery_status.percent * (kBatteryW - 4)) / 100,
                              0, kBatteryW - 4);
  const auto fill_color = low ? kWarningRed : theme.success;
  if (flash_on) {
    surface.FillRect(kBatteryX + 2, kBatteryY + 2, fill, kBatteryH - 4, fill_color);
  }
  gb::ui::widgets::DrawText(
      surface, kX + 31, kY + 5,
      std::to_string(lib.battery_status.percent) + "%", low ? kWarningRed : theme.text, 1);
  gb::ui::widgets::DrawText(
      surface, kX + 5, kY + 19,
      (lib.battery_status.charging ? "CHG " : "BAT ") +
          std::to_string(lib.battery_status.voltage).substr(0, 4) + "V",
      low ? kWarningRed : theme.text_dim, 1);
}

int ParseAlsaVolumePercent(const std::string& text) {
  const auto percent = text.find('%');
  if (percent == std::string::npos || percent == 0) return -1;
  std::size_t begin = percent;
  while (begin > 0 && std::isdigit(static_cast<unsigned char>(text[begin - 1]))) {
    --begin;
  }
  if (begin == percent) return -1;
  try {
    return std::clamp(std::stoi(text.substr(begin, percent - begin)), 0, 100);
  } catch (...) {
    return -1;
  }
}

bool RefreshVolume(LibraryState& lib) {
  const auto result = RunShellCapture("timeout 4s amixer -M -c 0 sget PCM");
  const int parsed = ParseAlsaVolumePercent(result.output);
  if (!result.process.launched || result.process.exit_code != 0 || parsed < 0) {
    lib.volume_percent = -1;
    lib.volume_error = "volume mixer unavailable";
    return false;
  }
  lib.volume_percent = parsed;
  lib.volume_error.clear();
  return true;
}

bool AdjustVolume(LibraryState& lib, const int direction) {
  const std::string amount = direction > 0 ? "5%+" : "5%-";
  const auto result = RunShellCapture("timeout 4s amixer -M -c 0 sset PCM " + amount);
  const int parsed = ParseAlsaVolumePercent(result.output);
  if (!result.process.launched || result.process.exit_code != 0 || parsed < 0) {
    lib.volume_percent = -1;
    lib.volume_error = "volume change failed";
    return false;
  }
  lib.volume_percent = parsed;
  lib.volume_error.clear();
  return true;
}

void UpdateBatteryHud(gb::platform::Ina219Battery* battery,
                      gb::ui::UIState& state,
                      LibraryState& lib) {
  const auto now = gb::core::NowMs();
  constexpr std::uint64_t kHoldThresholdMs = 850;
  constexpr std::uint64_t kMonitorIntervalMs = 5000;
  constexpr std::uint64_t kHeldReadIntervalMs = 1000;
  constexpr std::uint64_t kPlugNoticeMs = 3000;
  constexpr std::uint64_t kLowNoticeMs = 5000;
  bool changed = false;
  const bool held_long = lib.battery_start_held &&
                         now - lib.battery_hold_started_ms >= kHoldThresholdMs;

  if (battery != nullptr &&
      (lib.battery_last_read_ms == 0 ||
       now - lib.battery_last_read_ms >=
           (held_long ? kHeldReadIntervalMs : kMonitorIntervalMs))) {
    const auto previous = lib.battery_status;
    const bool had_previous_sample = lib.battery_sample_seen;
    std::string error;
    gb::core::BatteryStatus status;
    if (battery->Read(status, error)) {
      lib.battery_status = status;
      lib.battery_error.clear();
      if (had_previous_sample &&
          ((!previous.available && status.available) ||
           (!previous.charging && status.charging))) {
        lib.battery_auto_hud_until_ms = now + kPlugNoticeMs;
      }
      if (had_previous_sample && previous.percent > 20 && status.percent <= 20 &&
          !status.charging) {
        lib.battery_auto_hud_until_ms = std::max(lib.battery_auto_hud_until_ms,
                                                  now + kLowNoticeMs);
      }
      lib.battery_sample_seen = true;
    } else {
      lib.battery_status = gb::core::BatteryStatus{};
      lib.battery_error = error;
    }
    lib.battery_last_read_ms = now;
    changed = previous.available != lib.battery_status.available ||
              previous.percent != lib.battery_status.percent ||
              previous.charging != lib.battery_status.charging;
  }

  if (held_long &&
      (lib.volume_last_read_ms == 0 || now - lib.volume_last_read_ms >= 2000)) {
    const int previous_volume = lib.volume_percent;
    RefreshVolume(lib);
    lib.volume_last_read_ms = now;
    changed = changed || previous_volume != lib.volume_percent;
  }

  const bool critical = lib.battery_status.available &&
                        !lib.battery_status.charging &&
                        lib.battery_status.percent < 5;
  if (critical &&
      (lib.battery_last_flash_ms == 0 || now - lib.battery_last_flash_ms >= 400)) {
    lib.battery_last_flash_ms = now;
    changed = true;
  }
  const bool should_show = held_long || now < lib.battery_auto_hud_until_ms || critical;
  changed = changed || lib.battery_hud_visible != should_show;
  lib.battery_hud_visible = should_show;
  if (changed) {
    state.needs_redraw = true;
  }
}

void UpdateTvModeState(gb::ui::UIState& state, LibraryState& lib) {
  if (!lib.tv_mode) {
    return;
  }
  const bool external_controller = std::any_of(
      lib.input_devices.begin(), lib.input_devices.end(),
      [](const gb::platform::EvdevDeviceInfo& device) {
        const bool external_bus = device.bus == "usb" || device.is_bluetooth;
        return external_bus && device.name != "GameBird Controls" &&
               (device.is_gamepad || device.is_keyboard);
      });
  if (external_controller != lib.tv_external_controller) {
    lib.tv_external_controller = external_controller;
    state.needs_redraw = true;
  }
}

void DrawTvModeOverlay(gb::render::Surface240& surface,
                       const gb::render::Theme& theme,
                       const LibraryState& lib) {
  if (!lib.tv_mode) {
    return;
  }
  const auto now = gb::core::NowMs();
  const bool announce = now < lib.tv_mode_notice_until_ms;
  if (!announce && lib.tv_external_controller) {
    return;
  }

  surface.FillRect(20, 84, 200, lib.tv_external_controller ? 46 : 68, theme.bg);
  surface.StrokeRect(20, 84, 200, lib.tv_external_controller ? 46 : 68,
                     theme.accent);
  gb::ui::widgets::DrawText(surface, 56, 96, "TV MODE ENABLED", theme.accent, 1);
  if (!lib.tv_external_controller) {
    gb::ui::widgets::DrawText(surface, 31, 112, "PLEASE PLUG IN AN EXTERNAL", theme.text, 1);
    gb::ui::widgets::DrawText(surface, 56, 126, "USB CONTROLLER", theme.text, 1);
  }
}

void DrawUpdateNotice(gb::render::Surface240& surface,
                      const gb::render::Theme& theme,
                      const LibraryState& lib,
                      const gb::ui::Screen screen) {
  if (gb::core::NowMs() >= lib.update_notice_until_ms ||
      screen == gb::ui::Screen::Update ||
      (lib.update_status.os_updates <= 0 && !lib.update_status.shell_update)) {
    return;
  }
  surface.FillRect(18, 154, 204, 58, theme.bg);
  surface.StrokeRect(18, 154, 204, 58, theme.accent);
  gb::ui::widgets::DrawText(surface, 50, 166, "UPDATE AVAILABLE", theme.accent, 1);
  gb::ui::widgets::DrawText(surface, 34, 184, "GO TO TOOLS > SYSTEM UPDATE",
                            theme.text, 1);
}

void DrawScreen(gb::render::Surface240& surface,
                gb::ui::UIState& state,
                const gb::render::Theme& theme,
                const std::uint64_t frame_start_ms,
                const LibraryState& lib) {
  switch (state.screen) {
    case gb::ui::Screen::Home:
      gb::ui::screens::DrawHome(surface, state, theme);
      gb::ui::widgets::DrawContentText(surface, 16, 198, lib.status, theme.text_dim);
      break;
    case gb::ui::Screen::GameMenu: {
      gb::ui::widgets::DrawMenuFrame(surface, theme, "GAME MENU");
      gb::ui::widgets::DrawContentText(surface, 16, 38, lib.menu_game.title, theme.accent);
      gb::ui::widgets::DrawContentText(surface, 16, 51, lib.menu_game.system_name, theme.text_dim);
      std::vector<std::string> rows;
      for (const auto& action : GameActions(lib)) rows.push_back(action.second);
      gb::ui::widgets::DrawList(surface, 16, 68, 208, 126, 21, rows, lib.context_selected, theme);
      gb::ui::widgets::DrawContentText(surface, 16, 198, lib.status, theme.text_dim);
      gb::ui::widgets::DrawMenuFooter(surface, theme, "A:SELECT  B:BACK  START:BACK");
      break;
    }
    case gb::ui::Screen::SystemMenu: {
      gb::ui::widgets::DrawMenuFrame(surface, theme, "SYSTEM MENU");
      const std::string title = lib.systems.empty() ? "No systems" : lib.systems[lib.system_selected].name;
      gb::ui::widgets::DrawContentText(surface, 16, 40, title, theme.accent);
      gb::ui::widgets::DrawList(surface, 16, 68, 208, 90, 30,
          {"Browse games", "Launch options", "Main menu"}, lib.context_selected, theme);
      gb::ui::widgets::DrawMenuFooter(surface, theme, "A:SELECT  B:BACK  START:BACK");
      break;
    }
    case gb::ui::Screen::Systems: {
      const auto cards = BuildSystemCards(lib);
      gb::ui::screens::DrawSystems(surface, theme, cards, lib.system_selected,
                                   lib.status);
      break;
    }
    case gb::ui::Screen::GameList: {
      if (lib.game_list_view == GameListView::System) {
        const auto titles = BuildGameBrowserTitles(lib);
        gb::ui::screens::DrawGameBrowser(
            surface, theme, GameListTitle(lib), titles, lib.game_selected,
            lib.details_ready ? lib.details.box_art_path : "", lib.status);
      } else {
        const auto rows = BuildGameRows(lib);
        gb::ui::screens::DrawGameList(surface, theme, GameListTitle(lib), rows,
                                      lib.game_selected, lib.status);
      }
      break;
    }
    case gb::ui::Screen::Details: {
      const auto& details = lib.details;
      gb::ui::screens::DrawDetails(
          surface, theme,
          lib.details_ready ? Ellipsize(details.title, 31) : "NO GAME SELECTED",
          lib.details_ready ? Ellipsize(details.system_name, 31) : "",
          lib.details_ready ? Ellipsize(details.filename, 31) : "",
          lib.details_ready ? details.release_year : 0,
          lib.details_ready ? Ellipsize(details.genre, 17) : "",
          lib.details_ready ? details.players : 0,
          lib.details_ready ? Ellipsize(details.metadata_source, 24) : "",
          lib.details_ready ? details.box_art_path : "",
          lib.details_ready && details.is_favorite,
          lib.details_ready && details.is_hidden, Ellipsize(lib.status, 31));
      break;
    }
    case gb::ui::Screen::Settings: {
      const auto rows = BuildSettingsRows(lib);
      gb::ui::screens::DrawSettings(surface, theme, rows, lib.settings_selected,
                                    lib.status);
      break;
    }
    case gb::ui::Screen::ScrapeProgress: {
      const auto& progress = lib.scrape_progress;
      gb::ui::screens::DrawScrapeProgress(
          surface, theme, progress.completed, progress.total, progress.downloaded,
          progress.skipped_existing, progress.missing,
          progress.finished || !lib.scrape_active, progress.current_title,
          Ellipsize(lib.status, 31));
      break;
    }
    case gb::ui::Screen::Tools: {
      const auto rows = BuildToolsRows(lib);
      gb::ui::screens::DrawTools(surface, theme, rows, lib.tools_selected,
                                 lib.status);
      break;
    }
    case gb::ui::Screen::Update: {
      gb::ui::screens::DrawUpdate(
          surface, theme, Ellipsize(lib.update_status.phase, 28),
          lib.update_status.progress, lib.update_status.os_updates,
          lib.update_status.shell_update, UpdateIsBusy(lib.update_status),
          Ellipsize(lib.update_status.message, 30));
      break;
    }
    case gb::ui::Screen::LaunchOptions: {
      gb::ui::widgets::DrawMenuFrame(surface, theme, "LAUNCH OPTIONS");
      gb::ui::widgets::DrawText(
          surface, 16, 32,
          std::string("Scope: ") +
              (lib.launch_options_scope_type == "game" ? "GAME" : "SYSTEM"),
          theme.text_dim, 1);
      const auto rows = BuildLaunchOptionsRows(lib);
      gb::ui::widgets::DrawList(surface, 16, 50, 208, 124, 18, rows,
                                lib.launch_options_selected, theme);
      gb::ui::widgets::DrawText(
          surface, 16, 176,
          "Effective: " + Ellipsize(lib.launch_options_effective_core, 18) +
              " [" + Ellipsize(lib.launch_options_effective_source, 8) + "]",
          lib.launch_options_effective_warning.empty() ? theme.text_dim
                                                       : theme.accent,
          1);
      gb::ui::widgets::DrawText(
          surface, 16, 188,
          "Config: " + Ellipsize(lib.launch_options_effective_config, 25),
          theme.text_dim, 1);
      if (!lib.status.empty()) {
        gb::ui::widgets::DrawText(surface, 16, 200, Ellipsize(lib.status, 31),
                                  theme.text_dim, 1);
      }
      gb::ui::widgets::DrawMenuFooter(surface, theme, "L/R:ADJ A:NEXT X:PREV B:BACK");
      break;
    }
    case gb::ui::Screen::Bluetooth: {
      gb::ui::widgets::DrawMenuFrame(surface, theme, "BLUETOOTH");
      gb::ui::widgets::DrawText(
          surface, 16, 30,
          std::string("TAB: ") + (lib.bluetooth_show_paired ? "PAIRED" : "SCANNED"),
          theme.text_dim, 1);

      const auto devices = BuildActiveBluetoothDeviceRefs(lib);
      const int connected_count = static_cast<int>(std::count_if(
          devices.begin(), devices.end(),
          [](const BluetoothDevice* d) { return d != nullptr && d->connected; }));
      gb::ui::widgets::DrawText(surface, 16, 40,
                                "Devices: " + std::to_string(devices.size()) +
                                    " Connected: " + std::to_string(connected_count),
                                theme.text_dim, 1);
      if (!lib.bluetooth_show_paired) {
        gb::ui::widgets::DrawText(
            surface, 16, 48,
            std::string("Filter: ") + BluetoothFilterTag(lib.bluetooth_scan_filter),
            theme.text_dim, 1);
      }
      const auto rows = BuildBluetoothRows(devices);
      int selected = lib.bluetooth_show_paired ? lib.bluetooth_paired_selected
                                               : lib.bluetooth_scanned_selected;
      ClampSelection(selected, static_cast<int>(rows.size()));
      gb::ui::widgets::DrawList(surface, 16, 58, 208, 114, 18, rows, selected, theme);

      if (const auto* d = SelectedBluetoothDevice(lib); d != nullptr) {
        gb::ui::widgets::DrawText(surface, 16, 176, d->address, theme.text_dim, 1);
      } else {
        gb::ui::widgets::DrawText(surface, 16, 176, "(no device selected)",
                                  theme.text_dim, 1);
      }
      if (!lib.status.empty()) {
        gb::ui::widgets::DrawText(surface, 16, 188, lib.status, theme.text_dim, 1);
      }
      const std::string controls_hint =
          lib.bluetooth_modal != BluetoothModalType::None
              ? "A:YES B:NO"
              : (lib.bluetooth_show_paired
                     ? "<>:TAB R:ALL A:CON Y:DIS X:FORG B:BACK"
                     : "<>:TAB L/R:FILT A:PAIR X:SCAN Y:REF");
      gb::ui::widgets::DrawMenuFooter(surface, theme, controls_hint);

      if (lib.bluetooth_modal != BluetoothModalType::None) {
        surface.FillRect(26, 84, 188, 72, theme.bg);
        surface.StrokeRect(26, 84, 188, 72, theme.accent);
        if (lib.bluetooth_modal == BluetoothModalType::ConfirmForget) {
          gb::ui::widgets::DrawText(surface, 36, 96, "Forget device?", theme.text, 1);
          gb::ui::widgets::DrawText(
              surface, 36, 110, Ellipsize(lib.bluetooth_modal_name, 24), theme.text_dim, 1);
        } else if (lib.bluetooth_modal == BluetoothModalType::ConfirmDisconnectAll) {
          gb::ui::widgets::DrawText(surface, 36, 96, "Disconnect all", theme.text, 1);
          gb::ui::widgets::DrawText(surface, 36, 110, "paired devices?", theme.text, 1);
        }
        gb::ui::widgets::DrawText(surface, 36, 130, "A:Yes  B:No", theme.text_dim, 1);
      }
      break;
    }
    case gb::ui::Screen::Wifi: {
      std::vector<gb::ui::screens::WifiNetworkItem> networks;
      networks.reserve(lib.wifi_networks.size());
      for (const auto& network : lib.wifi_networks) {
        networks.push_back(gb::ui::screens::WifiNetworkItem{
            .ssid = network.ssid,
            .signal = network.signal,
            .secured = network.secured,
            .active = network.active,
        });
      }
      const std::string keyboard_value =
          lib.wifi_view == gb::ui::screens::WifiView::Country
              ? lib.wifi_country_entry : lib.wifi_password;
      const std::string title_ssid =
          lib.wifi_view == gb::ui::screens::WifiView::Password
              ? lib.wifi_selected_ssid : lib.wifi_connected_ssid;
      gb::ui::screens::DrawWifi(surface, theme, lib.wifi_view, title_ssid,
                                lib.wifi_connected_signal, lib.wifi_enabled,
                                lib.wifi_country, networks, lib.wifi_selected,
                                keyboard_value, lib.wifi_keyboard_page, lib.status);
      break;
    }
    case gb::ui::Screen::InputSetup: {
      gb::ui::widgets::DrawMenuFrame(surface, theme, "INPUT SETUP");
      gb::ui::widgets::DrawText(
          surface, 16, 34,
          lib.last_input_device.empty() ? "device: (none)"
                                        : ("device: " + lib.last_input_device),
          theme.text_dim, 1);
      gb::ui::widgets::DrawText(surface, 16, 48,
                                "last: " + lib.last_input_summary, theme.text_dim,
                                1);
      gb::ui::widgets::DrawText(
          surface, 16, 64,
          ProfilePreview(lib, lib.last_input_device, lib.last_input_bus_type,
                         lib.last_input_vendor, lib.last_input_product),
          theme.text_dim, 1);

      const int bt_count = static_cast<int>(std::count_if(
          lib.input_devices.begin(), lib.input_devices.end(),
          [](const gb::platform::EvdevDeviceInfo& d) {
            return d.is_bluetooth && d.is_gamepad;
          }));
      gb::ui::widgets::DrawText(surface, 16, 80,
                                "BT pads: " + std::to_string(bt_count), theme.text,
                                1);

      if (lib.input_capture_active) {
        const int step = std::clamp(lib.input_capture_step, 0,
                                    static_cast<int>(kRemapCaptureOrder.size()) - 1);
        gb::ui::widgets::DrawText(
            surface, 16, 106,
            "Remap step " + std::to_string(lib.input_capture_step + 1) + "/" +
                std::to_string(kRemapCaptureOrder.size()),
            theme.text, 1);
        gb::ui::widgets::DrawText(
            surface, 16, 120,
            std::string("Press: ") + ButtonName(kRemapCaptureOrder[step]),
            theme.accent, 1);
        gb::ui::widgets::DrawText(surface, 16, 134,
                                  "Hold 1s to skip opt", theme.text_dim, 1);
        gb::ui::widgets::DrawText(surface, 16, 146,
                                  "B:cancel capture", theme.text_dim, 1);
      } else {
        gb::ui::widgets::DrawText(surface, 16, 106,
                                  "A:start remap wizard", theme.text, 1);
        gb::ui::widgets::DrawText(surface, 16, 120,
                                  "Auto-save on complete", theme.text, 1);
        gb::ui::widgets::DrawText(surface, 16, 134,
                                  "X:clear profile", theme.text, 1);
        gb::ui::widgets::DrawText(surface, 16, 146,
                                  "Y:live input test", theme.text_dim, 1);
      }

      if (!lib.input_debug_line1.empty()) {
        gb::ui::widgets::DrawText(surface, 16, 160, lib.input_debug_line1, theme.text_dim,
                                  1);
      }
      if (!lib.input_debug_line2.empty()) {
        gb::ui::widgets::DrawText(surface, 16, 172, lib.input_debug_line2, theme.text_dim,
                                  1);
      }
      if (!lib.status.empty()) {
        gb::ui::widgets::DrawText(surface, 16, 188, lib.status, theme.text_dim, 1);
      }
      gb::ui::widgets::DrawMenuFooter(surface, theme, "B:BACK  SELECT:HOME");
      break;
    }
    case gb::ui::Screen::InputTest: {
      gb::ui::widgets::DrawMenuFrame(surface, theme, "LIVE INPUT TEST");
      gb::ui::widgets::DrawText(
          surface, 16, 34,
          "Devices: " + std::to_string(lib.input_devices.size()) + "  Presses: " +
              std::to_string(lib.input_test_press_count),
          theme.text_dim, 1);
      gb::ui::widgets::DrawText(surface, 16, 48,
                                "Last: " + Ellipsize(lib.input_test_last, 24),
                                theme.text, 1);
      for (std::size_t i = 0; i < kButtonCount; ++i) {
        const int column = static_cast<int>(i / 6);
        const int row = static_cast<int>(i % 6);
        const int x = 12 + column * 76;
        const int y = 66 + row * 20;
        const std::string label =
            std::string(lib.input_test_seen[i] ? "[X] " : "[ ] ") +
            ButtonName(kButtonOrder[i]);
        gb::ui::widgets::DrawText(surface, x, y, label,
                                  lib.input_test_seen[i] ? theme.success : theme.text_dim,
                                  1);
      }
      gb::ui::widgets::DrawText(surface, 16, 190,
                                Ellipsize(lib.input_debug_line2, 30), theme.text_dim, 1);
      gb::ui::widgets::DrawMenuFooter(surface, theme, "SELECT:BACK");
      break;
    }
  }

  if (state.show_diagnostics) {
    const auto now = gb::core::NowMs();
    const auto ms = now - frame_start_ms;
    surface.FillRect(8, 198, 224, 34, gb::render::Rgb565(0, 0, 0));
    surface.StrokeRect(8, 198, 224, 34, theme.panel_border);
    gb::ui::widgets::DrawText(surface, 12, 204,
                              std::string("SCR:") + gb::ui::ScreenName(state.screen),
                              theme.text, 1);
    gb::ui::widgets::DrawText(surface, 12, 216,
                              std::string("FRAME:") + std::to_string(state.frame_count) +
                                  " " + std::to_string(ms) + "MS",
                              theme.text_dim, 1);
  }
  DrawBatteryHud(surface, theme, lib);
  DrawTvModeOverlay(surface, theme, lib);
  DrawUpdateNotice(surface, theme, lib, state.screen);
}

int LaunchGameViaHelper(const Args& args, const int game_id, gb::core::PlayMode mode, std::string& status) {
  std::vector<std::string> launch_argv = {
      args.gblaunch,
      "--db",
      args.db_path,
      "--game-id",
      std::to_string(game_id),
  };

  launch_argv.push_back(mode == gb::core::PlayMode::Resume ? "--resume" :
                        mode == gb::core::PlayMode::Backup ? "--resume-backup" : "--fresh");
  const auto receipt_path = gb::core::PlayResultPath(args.db_path, game_id);
  std::error_code receipt_error;
  std::filesystem::remove(receipt_path, receipt_error);
  const auto result = gb::platform::RunProcessBlocking(launch_argv);
  if (!result.launched) {
    status = "LAUNCH FAILED";
    gb::core::Log(gb::core::LogLevel::Error,
                  "gblaunch spawn failed: " + result.error);
    return 1;
  }

  if (result.signaled) {
    status = "KILLED SIG " + std::to_string(result.signal);
    return result.exit_code;
  }

  status = result.exit_code == 0 ? "Returned to menu" : "Launch failed - previous save kept";
  std::ifstream receipt(receipt_path);
  std::string outcome;
  if (std::getline(receipt, outcome) && !outcome.empty()) status = outcome;
  return result.exit_code;
}

std::string RetroArchAppendConfigPath(const Args& args) {
  const std::filesystem::path systems_dir(args.systems_dir);
  const std::filesystem::path config_dir = systems_dir.parent_path();
  if (config_dir.empty()) {
    return "config/retroarch-gamebird.cfg";
  }
  return (config_dir / "retroarch-gamebird.cfg").string();
}

int LaunchRetroArchMenu(const Args& args, std::string& status) {
  std::vector<std::string> launch_argv = {"retroarch"};
  const std::string append_cfg = RetroArchAppendConfigPath(args);
  if (!append_cfg.empty() && std::filesystem::exists(append_cfg)) {
    launch_argv.push_back("--appendconfig");
    const std::string input_cfg = RetroArchActiveInputConfigPath();
    launch_argv.push_back(
        std::filesystem::exists(input_cfg) ? append_cfg + "|" + input_cfg : append_cfg);
  } else {
    gb::core::Log(gb::core::LogLevel::Warn,
                  "retroarch append config not found: " + append_cfg);
  }

  const auto result = gb::platform::RunProcessBlocking(launch_argv);
  if (!result.launched) {
    status = "RA MENU FAILED";
    gb::core::Log(gb::core::LogLevel::Error,
                  "retroarch menu launch failed: " + result.error);
    return 1;
  }

  if (result.signaled) {
    status = "RA MENU SIG " + std::to_string(result.signal);
    return result.exit_code;
  }

  status = "RA MENU EXIT " + std::to_string(result.exit_code);
  return result.exit_code;
}

bool HandleCaptureInputEvent(const gb::platform::InputEvent& ev,
                             gb::ui::UIState& state,
                             LibraryState& lib,
                             const Args& args) {
  if (state.screen != gb::ui::Screen::InputSetup || !lib.input_capture_active) {
    return false;
  }

  if (lib.input_capture_device.empty()) {
    lib.input_capture_device = ev.device_name;
    lib.input_capture_bus_type = ev.bus_type;
    lib.input_capture_vendor = ev.vendor;
    lib.input_capture_product = ev.product;
    lib.input_capture_has_id =
        (ev.bus_type != 0 || ev.vendor != 0 || ev.product != 0);
  }
  if (lib.input_capture_device != ev.device_name) {
    lib.status = "Use device: " + lib.input_capture_device;
    state.needs_redraw = true;
    return true;
  }

  if (lib.input_capture_arming) {
    const auto now = gb::core::NowMs();
    if (now < lib.input_capture_arm_until_ms) {
      return true;
    }
    bool is_arm_input = (ev.button == lib.input_capture_arm_button);
    if (lib.input_capture_arm_raw_type != 0 && lib.input_capture_arm_raw_code != 0) {
      is_arm_input = (ev.raw_type == lib.input_capture_arm_raw_type &&
                      ev.raw_code == lib.input_capture_arm_raw_code);
    }
    if (is_arm_input) {
      if (ev.raw_value <= 0) {
        lib.input_capture_arming = false;
        lib.status = std::string("Remap ") + ButtonName(kRemapCaptureOrder[0]);
        state.needs_redraw = true;
      }
      return true;
    }
    lib.input_capture_arming = false;
    lib.status = std::string("Remap ") + ButtonName(kRemapCaptureOrder[0]);
    state.needs_redraw = true;
  }

  const bool is_evdev_key =
      (ev.device_path.find("/dev/input/event") == 0 && ev.raw_type == 1);
  if (is_evdev_key && ev.raw_value > 0) {
    // Wait for release so we can detect hold-to-skip.
    return true;
  }

  if (lib.input_capture_step < 0 ||
      lib.input_capture_step >= static_cast<int>(kRemapCaptureOrder.size())) {
    lib.input_capture_active = false;
    lib.input_capture_arming = false;
    state.needs_redraw = true;
    return true;
  }

  constexpr int kMandatoryRemapCount = 6;
  constexpr int kSkipHoldMs = 900;

  if (is_evdev_key && ev.raw_value == 0 &&
      lib.input_capture_step >= kMandatoryRemapCount &&
      ev.hold_ms >= kSkipHoldMs) {
    const auto skipped =
        kRemapCaptureOrder[static_cast<std::size_t>(lib.input_capture_step)];
    ++lib.input_capture_step;
    state.needs_redraw = true;
    if (lib.input_capture_step >= static_cast<int>(kRemapCaptureOrder.size())) {
      lib.input_capture_active = false;
      lib.input_capture_arming = false;
      if (PersistCaptureProfile(args, lib, lib.status)) {
        lib.status = "Remap saved";
      }
      return true;
    }
    lib.status = std::string("Skipped ") + ButtonName(skipped) + ", now remap " +
                 ButtonName(kRemapCaptureOrder[static_cast<std::size_t>(
                     lib.input_capture_step)]);
    return true;
  }

  const auto target = kRemapCaptureOrder[static_cast<std::size_t>(lib.input_capture_step)];
  if (ev.raw_code > 0 && (ev.is_keyboard || !ev.mapped_button)) {
    if (ev.raw_code == kLinuxKeyEsc) {
      lib.status = "ESC reserved; choose another key";
      state.needs_redraw = true;
      return true;
    }
    lib.input_capture_keycode_map[ev.raw_code] = target;
  } else {
    if (!ev.mapped_button) {
      return true;
    }
    lib.input_capture_map[ButtonIndex(ev.button)] = target;
  }
  if (!ev.retroarch_binding.empty()) {
    lib.input_capture_retroarch_bindings[ButtonIndex(target)] =
        ev.retroarch_binding;
  }
  ++lib.input_capture_step;
  PersistCaptureProfile(args, lib, lib.status);
  state.needs_redraw = true;

  if (lib.input_capture_step >= static_cast<int>(kRemapCaptureOrder.size())) {
    lib.input_capture_active = false;
    lib.input_capture_arming = false;
    if (PersistCaptureProfile(args, lib, lib.status)) {
      lib.status = "Remap saved";
    }
    return true;
  }

  lib.status = std::string("Remap ") +
               ButtonName(kRemapCaptureOrder[static_cast<std::size_t>(lib.input_capture_step)]);
  return true;
}

gb::platform::Button ShellNavigationButton(const gb::platform::Button button) {
  switch (button) {
    case gb::platform::Button::LeftStickUp:
    case gb::platform::Button::RightStickUp:
      return gb::platform::Button::Up;
    case gb::platform::Button::LeftStickDown:
    case gb::platform::Button::RightStickDown:
      return gb::platform::Button::Down;
    case gb::platform::Button::LeftStickLeft:
    case gb::platform::Button::RightStickLeft:
      return gb::platform::Button::Left;
    case gb::platform::Button::LeftStickRight:
    case gb::platform::Button::RightStickRight:
      return gb::platform::Button::Right;
    default:
      return button;
  }
}

int RunUiLoop(const std::function<bool(gb::platform::InputFrame&, int)>& poll_input,
              const std::function<void(const gb::render::Surface240&)>& present,
              const std::function<int(int, std::string&)>& launch_game,
              const std::function<int(std::string&)>& launch_retroarch_menu,
              gb::platform::Ina219Battery* battery,
              gb::db::Database& db,
              LibraryState& lib,
              const Args& args) {
  gb::render::Surface240 surface;
  const gb::render::Theme theme = gb::render::DefaultTheme();
  gb::ui::UIState state;
  state.show_diagnostics = lib.settings.show_diagnostics;
  RefreshContinue(db, lib, state);
  state.home_selected = state.continue_available ? 0 : 1;
  state.menu_return_screen = lib.browse.screen == "games" ? gb::ui::Screen::GameList : gb::ui::Screen::Systems;
  if (lib.tv_mode) {
    lib.tv_mode_notice_until_ms = gb::core::NowMs() + 3000;
  }
  std::uint64_t next_library_refresh_ms = gb::core::NowMs() + 2000;

  while (state.running) {
    gb::platform::InputFrame frame;

    const int wait_ms = state.needs_redraw ? 0 : 250;
    const bool has_input = poll_input(frame, wait_ms);

    if (frame.devices_changed) {
      lib.status = "Input devices: " + std::to_string(lib.input_devices.size());
      state.needs_redraw = true;
    }

    if (frame.quit_requested) {
      state.running = false;
      break;
    }

    if (has_input) {
      std::vector<gb::platform::InputEvent> events = frame.events;
      if (events.empty()) {
        events.reserve(frame.pressed.size());
        for (const auto button : frame.pressed) {
          events.push_back(gb::platform::InputEvent{
              .button = button,
              .device_name = "unknown",
              .device_path = "unknown",
              .raw_type = 0,
              .raw_code = 0,
              .raw_value = 1,
              .retroarch_joypad_index = -1,
              .retroarch_binding = {},
          });
        }
      }

      for (const auto& ev : events) {
        if (gb::core::NowMs() < lib.ignore_input_until_ms) {
          continue;
        }

        // The hotplug service unbinds this device in TV Mode. Keep this guard
        // as a fail-safe for the short transition window before it disappears.
        if (lib.tv_mode && ev.device_name == "GameBird Controls") {
          continue;
        }

        lib.last_input_device = ev.device_name.empty() ? "unknown" : ev.device_name;
        lib.last_input_bus_type = ev.bus_type;
        lib.last_input_vendor = ev.vendor;
        lib.last_input_product = ev.product;
        lib.last_input_has_id =
            (ev.bus_type != 0 || ev.vendor != 0 || ev.product != 0);
        lib.last_input_joypad_index = ev.retroarch_joypad_index;
        lib.last_input_raw_type = ev.raw_type;
        lib.last_input_raw_code = ev.raw_code;
        lib.last_input_summary =
            (ev.mapped_button ? std::string(ButtonName(ev.button)) : std::string("KEY")) +
            " code=" + std::to_string(ev.raw_code);
        lib.last_input_ms = gb::core::NowMs();

        if (HandleCaptureInputEvent(ev, state, lib, args)) {
          continue;
        }

        if (!lib.settings.enable_bluetooth_gamepads && ev.is_bluetooth) {
          continue;
        }

        bool has_mapping = false;
        std::string mapping_debug;
        const auto mapped = ApplyInputMapping(lib, ev, &has_mapping, &mapping_debug);
        const auto shell_button = ShellNavigationButton(mapped);
        lib.input_debug_line1 = "code=" + std::to_string(ev.raw_code) +
                                " typ=" + std::to_string(ev.raw_type) +
                                " mapd=" + (ev.mapped_button ? "Y" : "N");
        lib.input_debug_line2 =
            std::string("=> ") + (has_mapping ? ButtonName(mapped) : "DROP") + " " +
            mapping_debug;
        if (state.screen == gb::ui::Screen::InputSetup) {
          state.needs_redraw = true;
        }
        if (!has_mapping) {
          continue;
        }
        if (shell_button == gb::platform::Button::Start && ev.raw_type == 1) {
          const auto now = gb::core::NowMs();
          if (ev.raw_value > 0) {
            if (!lib.battery_start_held) {
              lib.battery_start_held = true;
              lib.battery_start_chord_used = false;
              lib.battery_hold_started_ms = now;
              lib.battery_last_read_ms = 0;
              lib.volume_last_adjust_ms = 0;
            }
          } else {
            const bool held_long = now - lib.battery_hold_started_ms >= 850;
            const bool chord_used = lib.battery_start_chord_used;
            lib.battery_start_held = false;
            lib.battery_start_chord_used = false;
            if (!held_long && !chord_used) {
              HandleButton(shell_button, state, lib, db, args);
            } else {
              state.needs_redraw = true;
            }
          }
          continue;
        }
        if (lib.battery_start_held && ev.raw_value > 0 &&
            (shell_button == gb::platform::Button::Up ||
             shell_button == gb::platform::Button::Down)) {
          const auto now = gb::core::NowMs();
          constexpr std::uint64_t kVolumeRepeatMs = 140;
          if (lib.volume_last_adjust_ms == 0 ||
              now - lib.volume_last_adjust_ms >= kVolumeRepeatMs) {
            AdjustVolume(lib, shell_button == gb::platform::Button::Up ? 1 : -1);
            lib.volume_last_adjust_ms = now;
            lib.volume_last_read_ms = now;
          }
          lib.battery_start_chord_used = true;
          state.needs_redraw = true;
          continue;
        }
        // EV_ABS uses signed values: left/up are negative and are valid press
        // events, while zero is the neutral position. Treating all non-positive
        // values as releases made external controllers work only right/down.
        if ((ev.raw_type == 1 && ev.raw_value <= 0) ||
            (ev.raw_type == 3 && ev.raw_value == 0) ||
            (ev.raw_type != 1 && ev.raw_type != 3 && ev.raw_value <= 0)) {
          continue;
        }
        if (state.screen == gb::ui::Screen::InputTest) {
          const auto idx = ButtonIndex(mapped);
          lib.input_test_seen[idx] = true;
          lib.input_test_last =
              std::string(ButtonName(mapped)) + " raw=" +
              std::to_string(ev.raw_code);
          ++lib.input_test_press_count;
          state.needs_redraw = true;
          if (mapped == gb::platform::Button::Select) {
            state.screen = gb::ui::Screen::InputSetup;
            lib.status = "Input test complete";
          }
          continue;
        }
        HandleButton(shell_button, state, lib, db, args);
      }
    }

    RememberBrowse(lib, state);
    if (lib.browse != lib.saved_browse) {
      if (lib.browse_changed_ms == 0) lib.browse_changed_ms = gb::core::NowMs();
      if (gb::core::NowMs() - lib.browse_changed_ms >= 1000) {
        std::string error;
        if (gb::core::SaveBrowseState(args.db_path, lib.browse, error)) lib.saved_browse = lib.browse;
        else gb::core::Log(gb::core::LogLevel::Warn, "Browse position: " + error);
        lib.browse_changed_ms = gb::core::NowMs();
      }
    } else lib.browse_changed_ms = 0;

    UpdateBatteryHud(battery, state, lib);
    UpdateTvModeState(state, lib);
    const auto update_now_ms = gb::core::NowMs();
    if (lib.update_last_read_ms == 0 ||
        update_now_ms - lib.update_last_read_ms >= 1000) {
      if (ReadUpdateStatus(lib)) state.needs_redraw = true;
      lib.update_last_read_ms = update_now_ms;
    }

    // gblibd discovers ROMs copied over Samba in the background. Refresh the
    // visible catalog as well, so uploads appear without restarting the shell
    // or visiting the manual Rescan tool.
    const auto library_now_ms = gb::core::NowMs();
    if (library_now_ms >= next_library_refresh_ms) {
      const auto before = LibraryUiSignature(lib);
      const auto continue_before = state.continue_title;
      RefreshContinue(db, lib, state);
      if (continue_before != state.continue_title) state.needs_redraw = true;
      LoadSystems(db, lib);
      if (state.screen == gb::ui::Screen::GameList) {
        ReloadGameList(db, lib);
      }
      if (LibraryUiSignature(lib) != before) {
        state.needs_redraw = true;
      }
      next_library_refresh_ms = library_now_ms + 2000;
    }

    if (state.screen == gb::ui::Screen::ScrapeProgress && lib.scrape_active) {
      // Present the current count before doing the next network request. A
      // single request can take tens of seconds, so this screen must remain
      // visible while it is running.
      if (state.needs_redraw) {
        const auto frame_start_ms = gb::core::NowMs();
        surface.ClearDirtyRects();
        DrawScreen(surface, state, theme, frame_start_ms, lib);
        present(surface);
        state.needs_redraw = false;
        ++state.frame_count;
      }

      if (!lib.scrape_session.ProcessNext(db, lib.scrape_progress)) {
        lib.scrape_active = false;
        lib.status = "Scrape stopped: " + lib.scrape_progress.last_error;
      } else if (lib.scrape_progress.finished) {
        lib.scrape_active = false;
        lib.status = "Complete: " + std::to_string(lib.scrape_progress.downloaded) +
                     " downloaded, " + std::to_string(lib.scrape_progress.missing) +
                     " not found";
        RefreshSelectedGameDetails(db, lib);
      } else {
        lib.status = "Checking " + std::to_string(lib.scrape_progress.completed) +
                     " of " + std::to_string(lib.scrape_progress.total);
      }
      state.needs_redraw = true;
      continue;
    }

    if (lib.pending_wifi_action != PendingWifiAction::None) {
      if (state.needs_redraw) {
        const auto frame_start_ms = gb::core::NowMs();
        surface.ClearDirtyRects();
        DrawScreen(surface, state, theme, frame_start_ms, lib);
        present(surface);
        state.needs_redraw = false;
        ++state.frame_count;
      }
      std::string wifi_status;
      ExecuteWifiAction(lib, wifi_status);
      lib.pending_wifi_action = PendingWifiAction::None;
      if (!wifi_status.empty()) lib.status = wifi_status;
      state.needs_redraw = true;
      continue;
    }

    if (lib.pending_bluetooth_action != PendingBluetoothAction::None) {
      if (state.needs_redraw) {
        const auto frame_start_ms = gb::core::NowMs();
        surface.ClearDirtyRects();
        DrawScreen(surface, state, theme, frame_start_ms, lib);
        present(surface);
        state.needs_redraw = false;
        ++state.frame_count;
      }

      std::string bt_status;
      ExecuteBluetoothAction(lib, bt_status);
      lib.pending_bluetooth_action = PendingBluetoothAction::None;
      if (!bt_status.empty()) {
        lib.status = bt_status;
      }
      state.needs_redraw = true;
      continue;
    }

    if (lib.pending_launch_game_id != 0) {
      if (state.needs_redraw) {
        const auto frame_start_ms = gb::core::NowMs();
        surface.ClearDirtyRects();
        DrawScreen(surface, state, theme, frame_start_ms, lib);
        present(surface);
        state.needs_redraw = false;
        ++state.frame_count;
      }

      surface.ClearDirtyRects();
      surface.Clear(gb::render::Rgb565(0, 0, 0));
      present(surface);

      const int game_id = lib.pending_launch_game_id;
      lib.pending_launch_game_id = 0;

      std::string browse_error;
      if (gb::core::SaveBrowseState(args.db_path, lib.browse, browse_error)) lib.saved_browse = lib.browse;
      std::string launch_status;
      launch_game(game_id, launch_status);
      RefreshContinue(db, lib, state);
      RefreshGameMenu(db, lib, args);
      lib.context_selected = 0;
      state.screen = gb::ui::Screen::GameMenu;
      lib.status = launch_status;
      lib.ignore_input_until_ms = gb::core::NowMs() + kPostLaunchInputBlockMs;
      ReloadGameList(db, lib);
      LoadSystems(db, lib);
      state.needs_redraw = true;
      continue;
    }

    if (lib.pending_launch_retroarch_menu) {
      if (state.needs_redraw) {
        const auto frame_start_ms = gb::core::NowMs();
        surface.ClearDirtyRects();
        DrawScreen(surface, state, theme, frame_start_ms, lib);
        present(surface);
        state.needs_redraw = false;
        ++state.frame_count;
      }

      surface.ClearDirtyRects();
      surface.Clear(gb::render::Rgb565(0, 0, 0));
      present(surface);

      lib.pending_launch_retroarch_menu = false;

      std::string launch_status;
      launch_retroarch_menu(launch_status);
      lib.status = launch_status;
      lib.ignore_input_until_ms = gb::core::NowMs() + kPostLaunchInputBlockMs;
      state.needs_redraw = true;
      continue;
    }

    if (!state.needs_redraw) {
      continue;
    }

    const auto frame_start_ms = gb::core::NowMs();
    surface.ClearDirtyRects();
    DrawScreen(surface, state, theme, frame_start_ms, lib);
    present(surface);

    state.needs_redraw = false;
    ++state.frame_count;
  }

  RememberBrowse(lib, state);
  if (lib.browse != lib.saved_browse) {
    std::string error;
    if (!gb::core::SaveBrowseState(args.db_path, lib.browse, error))
      gb::core::Log(gb::core::LogLevel::Warn, "Browse position: " + error);
  }
  return 0;
}

int RunSdl(const Args& args, gb::db::Database& db, LibraryState& lib) {
  gb::platform::SDLPresenter presenter;
  gb::platform::PlatformOptions options;
  options.scale = args.scale;

  if (!presenter.Init(options)) {
    return 1;
  }
  lib.input_devices.clear();
  lib.input_devices.push_back(gb::platform::EvdevDeviceInfo{
      .path = "sdl://controller",
      .name = "SDL Controller",
      .bus = "sdl",
      .is_bluetooth = false,
      .is_gamepad = true,
      .is_keyboard = false,
      .joypad_index = 0,
  });
  {
    std::string input_config_error;
    if (!WriteActiveRetroArchInputConfig(args, lib, input_config_error)) {
      gb::core::Log(gb::core::LogLevel::Warn,
                    "initial RetroArch input config failed: " + input_config_error);
    }
  }
  gb::platform::Ina219Battery battery(args.battery_i2c, args.battery_address);

  const int rc = RunUiLoop(
      [&](gb::platform::InputFrame& frame, const int timeout_ms) {
        return presenter.WaitAndPoll(frame, timeout_ms);
      },
      [&](const gb::render::Surface240& surface) { presenter.Present(surface); },
      [&](const int game_id, std::string& status) {
        std::string error;
        if (!WriteActiveRetroArchInputConfig(args, lib, error)) {
          status = "INPUT CONFIG FAILED";
          gb::core::Log(gb::core::LogLevel::Error, error);
          return 1;
        }
        if (!WriteBatteryOverlayConfig(lib, error)) {
          gb::core::Log(gb::core::LogLevel::Warn,
                        "battery overlay config failed: " + error);
        }
        return LaunchGameViaHelper(args, game_id, lib.pending_play_mode, status);
      },
      [&](std::string& status) {
        std::string error;
        if (!WriteActiveRetroArchInputConfig(args, lib, error)) {
          status = "INPUT CONFIG FAILED";
          gb::core::Log(gb::core::LogLevel::Error, error);
          return 1;
        }
        if (!WriteBatteryOverlayConfig(lib, error)) {
          gb::core::Log(gb::core::LogLevel::Warn,
                        "battery overlay config failed: " + error);
        }
        return LaunchRetroArchMenu(args, status);
      },
      args.battery_disabled ? nullptr : &battery,
      db, lib, args);

  presenter.Shutdown();
  return rc;
}

int RunFbdev(const Args& args, gb::db::Database& db, LibraryState& lib) {
  gb::platform::FbdevPresenter presenter;
  if (!presenter.Init(args.fbdev)) {
    return 1;
  }

  gb::platform::EvdevInput input;
  if (!input.Init(args.input_evdev)) {
    presenter.Shutdown();
    return 1;
  }
  lib.input_devices = input.ConnectedDevices();
  {
    std::string input_config_error;
    if (!WriteActiveRetroArchInputConfig(args, lib, input_config_error)) {
      gb::core::Log(gb::core::LogLevel::Warn,
                    "initial RetroArch input config failed: " + input_config_error);
    }
  }
  const int bt_gamepads = static_cast<int>(std::count_if(
      lib.input_devices.begin(), lib.input_devices.end(),
      [](const gb::platform::EvdevDeviceInfo& d) {
        return d.is_bluetooth && d.is_gamepad;
      }));
  if (bt_gamepads > 0) {
    lib.status = "Bluetooth pads: " + std::to_string(bt_gamepads);
  }
  gb::platform::Ina219Battery battery(args.battery_i2c, args.battery_address);

  const int rc = RunUiLoop(
      [&](gb::platform::InputFrame& frame, const int timeout_ms) {
        const bool has_input = input.WaitAndPoll(frame, timeout_ms);
        if (input.ConsumeDevicesChanged()) {
          lib.input_devices = input.ConnectedDevices();
          frame.devices_changed = true;
          return true;
        }
        return has_input;
      },
      [&](const gb::render::Surface240& surface) { presenter.Present(surface); },
      [&](const int game_id, std::string& status) {
        std::string error;
        if (!WriteActiveRetroArchInputConfig(args, lib, error)) {
          status = "INPUT CONFIG FAILED";
          gb::core::Log(gb::core::LogLevel::Error, error);
          return 1;
        }
        if (!WriteBatteryOverlayConfig(lib, error)) {
          gb::core::Log(gb::core::LogLevel::Warn,
                        "battery overlay config failed: " + error);
        }
        input.ReleaseDeviceGrabs();
        const int rc = LaunchGameViaHelper(args, game_id, lib.pending_play_mode, status);
        input.AcquireDeviceGrabs();
        return rc;
      },
      [&](std::string& status) {
        std::string error;
        if (!WriteActiveRetroArchInputConfig(args, lib, error)) {
          status = "INPUT CONFIG FAILED";
          gb::core::Log(gb::core::LogLevel::Error, error);
          return 1;
        }
        if (!WriteBatteryOverlayConfig(lib, error)) {
          gb::core::Log(gb::core::LogLevel::Warn,
                        "battery overlay config failed: " + error);
        }
        input.ReleaseDeviceGrabs();
        const int rc = LaunchRetroArchMenu(args, status);
        input.AcquireDeviceGrabs();
        return rc;
      },
      args.battery_disabled ? nullptr : &battery,
      db, lib, args);

  input.Shutdown();
  presenter.Shutdown();
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);

  gb::core::Log(gb::core::LogLevel::Info,
                "Starting GameBird Shell presenter=" + args.presenter +
                    " scale=" + std::to_string(args.scale));

  gb::db::Database db;
  LibraryState lib;
  lib.tv_mode = args.tv_mode;
  {
    auto data_dir = std::filesystem::path(args.db_path).parent_path();
    if (data_dir.empty()) {
      data_dir = ".";
    }
    lib.system_artwork_dir = (data_dir / "system-art").string();
  }

  std::string settings_err;
  if (!gb::core::LoadRuntimeSettings(args.settings_path, lib.settings, settings_err)) {
    gb::core::Log(gb::core::LogLevel::Warn,
                  "settings load failed: " + settings_err);
  }
  lib.input_profiles = ParseInputProfiles(lib.settings.input_profiles);
  lib.last_input_device = lib.settings.preferred_input_device;
  lib.last_input_summary = "none";

  if (!db.Open(args.db_path) || !db.InitSchema()) {
    lib.db_ready = false;
    lib.status = "DB UNAVAILABLE";
    gb::core::Log(gb::core::LogLevel::Warn,
                  "UI running without DB data: " + db.LastError());
  } else {
    lib.db_ready = true;
    LoadSystems(db, lib);
    gb::core::LoadBrowseState(args.db_path, lib.browse);
    lib.saved_browse = lib.browse;
    const auto saved = std::find_if(lib.systems.begin(), lib.systems.end(),
        [&](const auto& system) { return system.id == lib.browse.system_id; });
    if (saved != lib.systems.end()) lib.system_selected = static_cast<int>(saved - lib.systems.begin());
    else SelectGameBirdSystem(lib);
    const auto restored_view = lib.browse.view;
    SelectSystem(db, lib, lib.system_selected);
    if (restored_view == "recent") { lib.game_list_view = GameListView::Recent; LoadRecentGames(db, lib); }
    else if (restored_view == "favorites") { lib.game_list_view = GameListView::Favorites; LoadFavoriteGames(db, lib); }
    const auto unavailable = std::count_if(
        lib.library_roots.begin(), lib.library_roots.end(),
        [](const gb::db::LibraryRootState& root) { return root.status != "ok"; });
    if (unavailable > 0) {
      lib.status = "LIBRARY STORAGE OFFLINE: " + std::to_string(unavailable);
    }
  }

  if (args.presenter == "sdl") {
    return RunSdl(args, db, lib);
  }

  if (args.presenter == "fbdev") {
    return RunFbdev(args, db, lib);
  }

  gb::core::Log(gb::core::LogLevel::Error,
                "Unsupported presenter: " + args.presenter +
                    " (expected: sdl or fbdev)");
  return 2;
}
