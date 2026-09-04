#pragma once

#include <string>

namespace gb::core {

struct RuntimeSettings {
  bool show_diagnostics = true;
  bool show_hidden_games = false;
  bool enable_bluetooth_gamepads = true;
  std::string preferred_input_device;
  std::string input_profiles;
  std::string scrape_provider = "libretro";
  bool scrape_overwrite_artwork = false;
};

bool LoadRuntimeSettings(const std::string& path,
                         RuntimeSettings& out,
                         std::string& error);
bool SaveRuntimeSettings(const std::string& path,
                         const RuntimeSettings& settings,
                         std::string& error);

}  // namespace gb::core
