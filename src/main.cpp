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
#include "core/settings.h"
#include "core/time.h"
#include "db/db.h"
#include "platform/input_evdev.h"
#include "platform/platform.h"
#include "platform/platform_fbdev.h"
#include "platform/platform_sdl.h"
#include "platform/proc.h"
#include "render/surface_240.h"
#include "render/theme.h"
#include "scrape/jobs.h"
#include "ui/screens/home.h"
#include "ui/ui_state.h"
#include "ui/widgets/list.h"
#include "ui/widgets/text.h"

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
  std::string fbdev = "/dev/fb1";
  std::string input_evdev = "auto";
  int scale = 3;
};

constexpr std::array<gb::platform::Button, 12> kButtonOrder = {
    gb::platform::Button::Up,     gb::platform::Button::Down,
    gb::platform::Button::Left,   gb::platform::Button::Right,
    gb::platform::Button::A,      gb::platform::Button::B,
    gb::platform::Button::X,      gb::platform::Button::Y,
    gb::platform::Button::L,      gb::platform::Button::R,
    gb::platform::Button::Start,  gb::platform::Button::Select,
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
    case gb::platform::Button::Start:
      return "Start";
    case gb::platform::Button::Select:
      return "Select";
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

struct LibraryState {
  bool db_ready = false;
  std::string status;

  gb::core::RuntimeSettings settings;

  std::vector<gb::db::SystemSummary> systems;
  std::vector<gb::db::GameSummary> games;

  int system_selected = 0;
  int game_selected = 0;
  int settings_selected = 0;
  int tools_selected = 0;

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
  std::string launch_options_default_core;
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
    if (arg == "--fbdev" && i + 1 < argc) {
      out.fbdev = argv[++i];
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
                            keycode_map) {
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
  return out;
}

constexpr std::array<gb::platform::Button, kButtonCount> kRemapCaptureOrder = {
    gb::platform::Button::Up,     gb::platform::Button::Down,
    gb::platform::Button::Left,   gb::platform::Button::Right,
    gb::platform::Button::A,      gb::platform::Button::B,
    gb::platform::Button::X,      gb::platform::Button::Y,
    gb::platform::Button::L,      gb::platform::Button::R,
    gb::platform::Button::Start,  gb::platform::Button::Select,
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
  if (!lib.db_ready) {
    lib.systems.clear();
    return;
  }

  if (!db.ListSystems(lib.systems)) {
    lib.status = "DB READ ERROR";
    return;
  }

  ClampSelection(lib.system_selected, static_cast<int>(lib.systems.size()));
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

  ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
}

void OpenSystem(gb::db::Database& db, gb::ui::UIState& ui, LibraryState& lib) {
  if (lib.systems.empty()) {
    return;
  }

  ClampSelection(lib.system_selected, static_cast<int>(lib.systems.size()));
  lib.current_system_id = lib.systems[lib.system_selected].id;
  lib.current_system_name = lib.systems[lib.system_selected].name;
  lib.game_selected = 0;
  LoadGamesForCurrentSystem(db, lib);
  ui.screen = gb::ui::Screen::GameList;
  ui.needs_redraw = true;
}

std::vector<std::string> BuildSystemRows(const LibraryState& lib) {
  std::vector<std::string> out;
  out.reserve(lib.systems.size());

  for (const auto& sys : lib.systems) {
    out.push_back(sys.name + " - " + std::to_string(sys.game_count));
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

std::vector<std::string> BuildSettingsRows(const LibraryState& lib) {
  return {
      std::string("Diagnostics Overlay: ") +
          (lib.settings.show_diagnostics ? "ON" : "OFF"),
      std::string("Show Hidden Games: ") +
          (lib.settings.show_hidden_games ? "ON" : "OFF"),
      "Input Setup",
      "Save Settings",
  };
}

std::vector<std::string> BuildToolsRows(const LibraryState& lib) {
  return {
      "Rescan Library",
      "Identify Metadata",
      "Rebuild Thumbnails",
      "Run Queued Jobs",
      "Export Diagnostics",
      "Input Setup",
      std::string("Bluetooth Pads: ") +
          (lib.settings.enable_bluetooth_gamepads ? "AUTO" : "OFF"),
      "RetroArch Menu",
      "Bluetooth Devices",
      lib.tools_exit_confirm ? "Exit To Console [Confirm]" : "Exit To Console",
  };
}

bool SaveInputSettings(const Args& args, LibraryState& lib, std::string& status) {
  lib.settings.preferred_input_device =
      lib.input_capture_device.empty() ? lib.last_input_device : lib.input_capture_device;
  lib.settings.input_profiles = EncodeInputProfiles(lib.input_profiles);
  std::string err;
  if (gb::core::SaveRuntimeSettings(args.settings_path, lib.settings, err)) {
    return true;
  }
  status = "save failed";
  gb::core::Log(gb::core::LogLevel::Error, err);
  return false;
}

void SeedCaptureProfileFromExisting(
    LibraryState& lib,
    const std::string& device_name,
    const std::uint16_t bus_type,
    const std::uint16_t vendor,
    const std::uint16_t product,
    std::array<gb::platform::Button, kButtonCount>& out_map,
    std::unordered_map<std::uint16_t, gb::platform::Button>& out_keycode_map) {
  out_map = IdentityButtonMap();
  out_keycode_map.clear();
  const int idx = FindBestProfileIndex(lib.input_profiles, device_name, bus_type, vendor,
                                       product);
  if (idx < 0) {
    return;
  }
  const auto& profile = lib.input_profiles[static_cast<std::size_t>(idx)];
  out_map = profile.source_to_target;
  out_keycode_map = profile.keycode_to_target;
}

bool PersistCaptureProfile(const Args& args, LibraryState& lib, std::string& status) {
  if (lib.input_capture_device.empty()) {
    status = "No input device";
    return false;
  }
  UpsertInputProfile(lib, lib.input_capture_device, lib.input_capture_bus_type,
                     lib.input_capture_vendor, lib.input_capture_product,
                     lib.input_capture_has_id, lib.input_capture_map,
                     lib.input_capture_keycode_map);
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

bool TokenizeCommand(const std::string& command,
                     std::vector<std::string>& out,
                     std::string& error) {
  out.clear();
  std::string cur;

  enum class Quote { None, Single, Double };
  Quote quote = Quote::None;
  bool escape = false;

  for (char c : command) {
    if (escape) {
      cur.push_back(c);
      escape = false;
      continue;
    }

    if (quote == Quote::Single) {
      if (c == '\'') {
        quote = Quote::None;
      } else {
        cur.push_back(c);
      }
      continue;
    }

    if (quote == Quote::Double) {
      if (c == '"') {
        quote = Quote::None;
      } else if (c == '\\') {
        escape = true;
      } else {
        cur.push_back(c);
      }
      continue;
    }

    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '\'') {
      quote = Quote::Single;
      continue;
    }
    if (c == '"') {
      quote = Quote::Double;
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(c);
  }

  if (escape) {
    error = "trailing escape in launch template";
    return false;
  }
  if (quote != Quote::None) {
    error = "unclosed quote in launch template";
    return false;
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return true;
}

std::string ExtractCoreFromLaunchTemplate(const std::string& launch_template) {
  std::vector<std::string> tokens;
  std::string error;
  if (!TokenizeCommand(launch_template, tokens, error)) {
    return {};
  }
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if ((tokens[i] == "-L" || tokens[i] == "--libretro") &&
        (i + 1) < tokens.size()) {
      return tokens[i + 1];
    }
    if (tokens[i].rfind("-L", 0) == 0 && tokens[i].size() > 2) {
      return tokens[i].substr(2);
    }
    constexpr std::string_view kLibretroPrefix = "--libretro=";
    if (tokens[i].rfind(kLibretroPrefix, 0) == 0 &&
        tokens[i].size() > kLibretroPrefix.size()) {
      return tokens[i].substr(kLibretroPrefix.size());
    }
  }
  return {};
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

  lib.launch_options_return_screen = gb::ui::Screen::Systems;
  lib.launch_options_selected = 0;
  lib.launch_options_scope_type = "system";
  lib.launch_options_scope_id = sys.id;
  lib.launch_options_system_id = sys.id;
  lib.launch_options_title = sys.name;
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

  lib.status = "System launch options";
  ui.screen = gb::ui::Screen::LaunchOptions;
  ui.needs_redraw = true;
  return true;
}

bool OpenLaunchOptionsForGame(gb::db::Database& db,
                              gb::ui::UIState& ui,
                              LibraryState& lib) {
  if (lib.games.empty()) {
    lib.status = "No game selected";
    return false;
  }
  ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
  const auto& game = lib.games[static_cast<std::size_t>(lib.game_selected)];

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

  lib.launch_options_return_screen = gb::ui::Screen::GameList;
  lib.launch_options_selected = 0;
  lib.launch_options_scope_type = "game";
  lib.launch_options_scope_id = launch.rom_path;
  lib.launch_options_system_id = launch.system_id;
  lib.launch_options_title = game.title;
  lib.launch_options_default_core = effective_default_core;
  FillLaunchCoreChoices(
      lib, lib.launch_options_default_core,
      has_game_override ? game_override.core_path : std::string());
  lib.launch_options_audio_selected =
      FindAudioChoiceIndex(has_game_override ? game_override.audio_latency : 0);
  lib.launch_options_video_selected = FindVideoChoiceIndex(
      has_game_override ? game_override.video_width : 0,
      has_game_override ? game_override.video_height : 0);

  lib.status = "Game launch options";
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
  return true;
}

void ResetLaunchOptionsToInherited(LibraryState& lib) {
  lib.launch_options_core_selected = 0;
  lib.launch_options_audio_selected = 0;
  lib.launch_options_video_selected = 0;
}

gb::scrape::WorkerConfig MakeWorkerConfig(const Args& args) {
  gb::scrape::WorkerConfig cfg;
  cfg.defaults_json_path = args.defaults_json;
  cfg.systems_dir = args.systems_dir;
  cfg.hide_missing = false;
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
  const auto cfg = MakeWorkerConfig(args);
  if (lib.tools_selected != 9) {
    lib.tools_exit_confirm = false;
  }

  switch (lib.tools_selected) {
    case 0:
      if (RunJobs(db, cfg, true, false, false, lib.status)) {
        LoadSystems(db, lib);
        LoadGamesForCurrentSystem(db, lib);
      }
      break;

    case 1:
      if (RunJobs(db, cfg, false, true, false, lib.status)) {
        LoadGamesForCurrentSystem(db, lib);
      }
      break;

    case 2:
      db.EnqueueJob("build_thumb", "{}");
      if (RunJobs(db, cfg, false, false, false, lib.status)) {
        lib.status = "thumbs rebuilt";
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

    case 7:
      if (!lib.pending_launch_retroarch_menu && lib.pending_launch_game_id == 0) {
        lib.pending_launch_retroarch_menu = true;
        lib.status = "LAUNCHING RETROARCH...";
      } else {
        lib.status = "launch busy";
      }
      break;

    case 8:
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

    case 9:
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

  if (button == gb::platform::Button::Select) {
    lib.tools_exit_confirm = false;
    ui.screen = gb::ui::Screen::Home;
    ui.needs_redraw = true;
    return;
  }

  switch (ui.screen) {
    case gb::ui::Screen::Home:
      if (button == gb::platform::Button::Up) {
        ui.home_selected = (ui.home_selected + 4) % 5;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Down) {
        ui.home_selected = (ui.home_selected + 1) % 5;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::A) {
        if (ui.home_selected == 2 || ui.home_selected == 1 || ui.home_selected == 0) {
          LoadSystems(db, lib);
          if (ui.home_selected == 0) {
            lib.status = "RECENT VIEW COMING SOON";
          } else if (ui.home_selected == 1) {
            lib.status = "FAVORITES VIEW COMING SOON";
          } else {
            lib.status.clear();
          }
          ui.screen = gb::ui::Screen::Systems;
        } else if (ui.home_selected == 3) {
          lib.tools_exit_confirm = false;
          ui.screen = gb::ui::Screen::Tools;
        } else if (ui.home_selected == 4) {
          ui.screen = gb::ui::Screen::Settings;
        }
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::B) {
        lib.status = "Use Tools -> Exit To Console";
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::Systems:
      if (button == gb::platform::Button::Up) {
        if (!lib.systems.empty()) {
          lib.system_selected =
              (lib.system_selected + static_cast<int>(lib.systems.size()) - 1) %
              static_cast<int>(lib.systems.size());
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Down) {
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
      if (button == gb::platform::Button::Y) {
        OpenLaunchOptionsForSystem(db, ui, lib);
        return;
      }
      if (button == gb::platform::Button::B) {
        ui.screen = gb::ui::Screen::Home;
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
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Down) {
        if (!lib.games.empty()) {
          lib.game_selected =
              (lib.game_selected + 1) % static_cast<int>(lib.games.size());
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::A) {
        if (!lib.games.empty() && lib.pending_launch_game_id == 0) {
          ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
          lib.pending_launch_game_id = lib.games[lib.game_selected].id;
          lib.status = "LAUNCHING...";
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::X) {
        if (!lib.games.empty()) {
          ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
          const auto game_id = lib.games[lib.game_selected].id;
          bool now_favorite = false;
          if (db.ToggleGameFavorite(game_id, now_favorite)) {
            lib.status = now_favorite ? "FAVORITE ON" : "FAVORITE OFF";
            LoadGamesForCurrentSystem(db, lib);
            LoadSystems(db, lib);
          } else {
            lib.status = "FAVORITE TOGGLE FAILED";
          }
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Y) {
        if (!lib.games.empty()) {
          ClampSelection(lib.game_selected, static_cast<int>(lib.games.size()));
          const auto game_id = lib.games[lib.game_selected].id;
          bool now_hidden = false;
          if (db.ToggleGameHidden(game_id, now_hidden)) {
            lib.status = now_hidden ? "HIDDEN" : "UNHIDDEN";
            LoadGamesForCurrentSystem(db, lib);
            LoadSystems(db, lib);
          } else {
            lib.status = "HIDE TOGGLE FAILED";
          }
          ui.needs_redraw = true;
        }
        return;
      }
      if (button == gb::platform::Button::Start) {
        OpenLaunchOptionsForGame(db, ui, lib);
        return;
      }
      if (button == gb::platform::Button::B) {
        ui.screen = gb::ui::Screen::Systems;
        LoadSystems(db, lib);
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::Settings:
      if (button == gb::platform::Button::Up) {
        lib.settings_selected = (lib.settings_selected + 3) % 4;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Down) {
        lib.settings_selected = (lib.settings_selected + 1) % 4;
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
          LoadGamesForCurrentSystem(db, lib);
          lib.status = lib.settings.show_hidden_games ? "hidden visible" : "hidden hidden";
        } else if (lib.settings_selected == 2) {
          ui.screen = gb::ui::Screen::InputSetup;
          lib.status = "Input setup";
        } else {
          lib.settings.input_profiles = EncodeInputProfiles(lib.input_profiles);
          lib.settings.preferred_input_device = lib.last_input_device;
          std::string err;
          if (gb::core::SaveRuntimeSettings(args.settings_path, lib.settings, err)) {
            lib.status = "settings saved";
          } else {
            lib.status = "save failed";
            gb::core::Log(gb::core::LogLevel::Error, err);
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
        lib.tools_selected = (lib.tools_selected + 9) % 10;
        ui.needs_redraw = true;
        return;
      }
      if (button == gb::platform::Button::Down) {
        lib.tools_exit_confirm = false;
        lib.tools_selected = (lib.tools_selected + 1) % 10;
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
                                       lib.input_capture_keycode_map);
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
      if (button == gb::platform::Button::B) {
        lib.input_capture_active = false;
        lib.input_capture_arming = false;
        ui.screen = gb::ui::Screen::Settings;
        ui.needs_redraw = true;
        return;
      }
      break;

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
        ui.screen = lib.launch_options_return_screen;
        ui.needs_redraw = true;
        return;
      }
      break;

    case gb::ui::Screen::Details:
      if (button == gb::platform::Button::B) {
        ui.screen = gb::ui::Screen::Home;
        ui.needs_redraw = true;
      }
      return;
  }
}

void DrawScreen(gb::render::Surface240& surface,
                gb::ui::UIState& state,
                const gb::render::Theme& theme,
                const std::uint64_t frame_start_ms,
                const LibraryState& lib) {
  switch (state.screen) {
    case gb::ui::Screen::Home:
      gb::ui::screens::DrawHome(surface, state, theme);
      break;
    case gb::ui::Screen::Systems: {
      const auto rows = BuildSystemRows(lib);
      gb::ui::screens::DrawSystems(surface, theme, rows, lib.system_selected,
                                   lib.status);
      break;
    }
    case gb::ui::Screen::GameList: {
      const auto rows = BuildGameRows(lib);
      const std::string title =
          lib.current_system_name.empty() ? "GAMES" : lib.current_system_name;
      gb::ui::screens::DrawGameList(surface, theme, title, rows, lib.game_selected,
                                    lib.status);
      break;
    }
    case gb::ui::Screen::Details:
      gb::ui::screens::DrawDetails(surface, theme);
      break;
    case gb::ui::Screen::Settings: {
      const auto rows = BuildSettingsRows(lib);
      gb::ui::screens::DrawSettings(surface, theme, rows, lib.settings_selected,
                                    lib.status);
      break;
    }
    case gb::ui::Screen::Tools: {
      const auto rows = BuildToolsRows(lib);
      gb::ui::screens::DrawTools(surface, theme, rows, lib.tools_selected,
                                 lib.status);
      break;
    }
    case gb::ui::Screen::LaunchOptions: {
      surface.Clear(theme.bg);
      surface.FillRect(8, 8, 224, 224, theme.panel);
      surface.StrokeRect(8, 8, 224, 224, theme.panel_border);

      gb::ui::widgets::DrawText(surface, 16, 18, "LAUNCH OPTIONS", theme.accent, 1);
      gb::ui::widgets::DrawText(
          surface, 16, 32,
          std::string("Scope: ") +
              (lib.launch_options_scope_type == "game" ? "GAME" : "SYSTEM"),
          theme.text_dim, 1);
      const auto rows = BuildLaunchOptionsRows(lib);
      gb::ui::widgets::DrawList(surface, 16, 50, 208, 124, 18, rows,
                                lib.launch_options_selected, theme);
      if (!lib.launch_options_core_paths.empty() &&
          lib.launch_options_core_selected > 0 &&
          lib.launch_options_core_selected <
              static_cast<int>(lib.launch_options_core_paths.size())) {
        gb::ui::widgets::DrawText(
            surface, 16, 178,
            Ellipsize(lib.launch_options_core_paths[static_cast<std::size_t>(
                          lib.launch_options_core_selected)],
                      30),
            theme.text_dim, 1);
      } else {
        gb::ui::widgets::DrawText(
            surface, 16, 178,
            std::string("Default core: ") +
                Ellipsize(BaseName(lib.launch_options_default_core), 18),
            theme.text_dim, 1);
      }
      if (!lib.status.empty()) {
        gb::ui::widgets::DrawText(surface, 16, 192, lib.status, theme.text_dim, 1);
      }
      gb::ui::widgets::DrawText(surface, 16, 210,
                                "L/R:ADJ A:NEXT X:PREV B:BACK", theme.text_dim, 1);
      break;
    }
    case gb::ui::Screen::Bluetooth: {
      surface.Clear(theme.bg);
      surface.FillRect(8, 8, 224, 224, theme.panel);
      surface.StrokeRect(8, 8, 224, 224, theme.panel_border);
      gb::ui::widgets::DrawText(surface, 16, 18, "BLUETOOTH", theme.accent, 1);
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
      gb::ui::widgets::DrawText(surface, 16, 210, controls_hint, theme.text_dim, 1);

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
    case gb::ui::Screen::InputSetup: {
      surface.Clear(theme.bg);
      surface.FillRect(8, 8, 224, 224, theme.panel);
      surface.StrokeRect(8, 8, 224, 224, theme.panel_border);
      gb::ui::widgets::DrawText(surface, 16, 18, "INPUT SETUP", theme.accent, 1);
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
                                  "Keyboard: any key (ESC reserved)", theme.text_dim, 1);
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
      gb::ui::widgets::DrawText(surface, 16, 210,
                                "B:BACK  SELECT:HOME", theme.text_dim, 1);
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
}

int LaunchGameViaHelper(const Args& args, const int game_id, std::string& status) {
  std::vector<std::string> launch_argv = {
      args.gblaunch,
      "--db",
      args.db_path,
      "--game-id",
      std::to_string(game_id),
  };

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

  status = "EXIT " + std::to_string(result.exit_code);
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
    launch_argv.push_back(append_cfg);
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

int RunUiLoop(const std::function<bool(gb::platform::InputFrame&, int)>& poll_input,
              const std::function<void(const gb::render::Surface240&)>& present,
              const std::function<int(int, std::string&)>& launch_game,
              const std::function<int(std::string&)>& launch_retroarch_menu,
              gb::db::Database& db,
              LibraryState& lib,
              const Args& args) {
  gb::render::Surface240 surface;
  const gb::render::Theme theme = gb::render::DefaultTheme();
  gb::ui::UIState state;
  state.show_diagnostics = lib.settings.show_diagnostics;

  while (state.running) {
    gb::platform::InputFrame frame;

    const int wait_ms = state.needs_redraw ? 0 : 250;
    const bool has_input = poll_input(frame, wait_ms);

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
          });
        }
      }

      for (const auto& ev : events) {
        if (gb::core::NowMs() < lib.ignore_input_until_ms) {
          continue;
        }

        lib.last_input_device = ev.device_name.empty() ? "unknown" : ev.device_name;
        lib.last_input_bus_type = ev.bus_type;
        lib.last_input_vendor = ev.vendor;
        lib.last_input_product = ev.product;
        lib.last_input_has_id =
            (ev.bus_type != 0 || ev.vendor != 0 || ev.product != 0);
        lib.last_input_raw_type = ev.raw_type;
        lib.last_input_raw_code = ev.raw_code;
        lib.last_input_summary =
            (ev.mapped_button ? std::string(ButtonName(ev.button)) : std::string("KEY")) +
            " code=" + std::to_string(ev.raw_code);
        lib.last_input_ms = gb::core::NowMs();

        if (HandleCaptureInputEvent(ev, state, lib, args)) {
          continue;
        }

        if (ev.raw_value <= 0) {
          continue;
        }

        if (!ev.mapped_button) {
          continue;
        }

        if (!lib.settings.enable_bluetooth_gamepads && ev.is_bluetooth) {
          continue;
        }

        bool has_mapping = false;
        std::string mapping_debug;
        const auto mapped = ApplyInputMapping(lib, ev, &has_mapping, &mapping_debug);
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
        HandleButton(mapped, state, lib, db, args);
      }
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

      std::string launch_status;
      launch_game(game_id, launch_status);
      lib.status = launch_status;
      lib.ignore_input_until_ms = gb::core::NowMs() + kPostLaunchInputBlockMs;
      LoadGamesForCurrentSystem(db, lib);
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
  });

  const int rc = RunUiLoop(
      [&](gb::platform::InputFrame& frame, const int timeout_ms) {
        return presenter.WaitAndPoll(frame, timeout_ms);
      },
      [&](const gb::render::Surface240& surface) { presenter.Present(surface); },
      [&](const int game_id, std::string& status) {
        return LaunchGameViaHelper(args, game_id, status);
      },
      [&](std::string& status) { return LaunchRetroArchMenu(args, status); },
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
  const int bt_gamepads = static_cast<int>(std::count_if(
      lib.input_devices.begin(), lib.input_devices.end(),
      [](const gb::platform::EvdevDeviceInfo& d) {
        return d.is_bluetooth && d.is_gamepad;
      }));
  if (bt_gamepads > 0) {
    lib.status = "Bluetooth pads: " + std::to_string(bt_gamepads);
  }

  const int rc = RunUiLoop(
      [&](gb::platform::InputFrame& frame, const int timeout_ms) {
        return input.WaitAndPoll(frame, timeout_ms);
      },
      [&](const gb::render::Surface240& surface) { presenter.Present(surface); },
      [&](const int game_id, std::string& status) {
        input.ReleaseDeviceGrabs();
        const int rc = LaunchGameViaHelper(args, game_id, status);
        input.AcquireDeviceGrabs();
        return rc;
      },
      [&](std::string& status) {
        input.ReleaseDeviceGrabs();
        const int rc = LaunchRetroArchMenu(args, status);
        input.AcquireDeviceGrabs();
        return rc;
      },
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
