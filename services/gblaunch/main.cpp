#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/logging.h"
#include "db/db.h"
#include "platform/proc.h"

namespace {

struct Args {
  std::string db_path = "./data/catalog.db";
  int game_id = 0;
  bool dry_run = false;
};

Args ParseArgs(const int argc, char** argv) {
  Args out;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    if (arg == "--db" && i + 1 < argc) {
      out.db_path = argv[++i];
      continue;
    }
    if (arg == "--game-id" && i + 1 < argc) {
      out.game_id = std::atoi(argv[++i]);
      continue;
    }
    if (arg == "--dry-run") {
      out.dry_run = true;
      continue;
    }
  }

  return out;
}

std::string ReplaceAll(std::string input,
                       const std::string& from,
                       const std::string& to) {
  if (from.empty()) {
    return input;
  }

  size_t pos = 0;
  while ((pos = input.find(from, pos)) != std::string::npos) {
    input.replace(pos, from.size(), to);
    pos += to.size();
  }
  return input;
}

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
  if (out.empty()) {
    error = "launch template produced empty argv";
    return false;
  }

  return true;
}

bool BuildLaunchArgv(const gb::db::LaunchInfo& info,
                     std::vector<std::string>& out,
                     std::string& error) {
  std::string templ = info.launch_template;
  if (templ.empty()) {
    templ = "{rom_path}";
  }

  if (!TokenizeCommand(templ, out, error)) {
    return false;
  }

  const std::unordered_map<std::string, std::string> vars = {
      {"{rom_path}", info.rom_path},
      {"{system_id}", info.system_id},
      {"{title}", info.title},
      {"{game_id}", std::to_string(info.game_id)},
  };

  for (auto& token : out) {
    for (const auto& [key, value] : vars) {
      token = ReplaceAll(token, key, value);
    }
  }

  if (out.front().empty()) {
    error = "empty executable path in launch argv";
    return false;
  }

  return true;
}

bool TryGetLaunchOverride(gb::db::Database& db,
                          const std::string& scope_type,
                          const std::string& scope_id,
                          gb::db::LaunchOverride& out,
                          bool& found,
                          std::string& error) {
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
  error = db.LastError();
  return false;
}

void MergeLaunchOverride(const gb::db::LaunchOverride& src,
                         gb::db::LaunchOverride& dst) {
  if (!src.core_path.empty()) {
    dst.core_path = src.core_path;
  }
  if (src.audio_latency > 0) {
    dst.audio_latency = src.audio_latency;
  }
  if (src.video_width > 0 && src.video_height > 0) {
    dst.video_width = src.video_width;
    dst.video_height = src.video_height;
  }
}

bool ApplyCoreOverride(std::vector<std::string>& argv, const std::string& core_path) {
  if (core_path.empty() || argv.empty()) {
    return false;
  }

  for (std::size_t i = 0; i < argv.size(); ++i) {
    if ((argv[i] == "-L" || argv[i] == "--libretro") && (i + 1) < argv.size()) {
      argv[i + 1] = core_path;
      return true;
    }
    if (argv[i].rfind("-L", 0) == 0 && argv[i].size() > 2) {
      argv[i] = "-L" + core_path;
      return true;
    }
    constexpr std::string_view kLibretroPrefix = "--libretro=";
    if (argv[i].rfind(kLibretroPrefix, 0) == 0) {
      argv[i] = std::string(kLibretroPrefix) + core_path;
      return true;
    }
  }

  if (argv.size() == 1) {
    argv.push_back("-L");
    argv.push_back(core_path);
  } else {
    argv.insert(argv.begin() + 1, core_path);
    argv.insert(argv.begin() + 1, "-L");
  }
  return true;
}

bool BuildOverrideAppendConfig(const gb::db::LaunchOverride& merged_override,
                               const int game_id,
                               std::string& out_path,
                               std::string& error) {
  if (merged_override.audio_latency <= 0 &&
      (merged_override.video_width <= 0 || merged_override.video_height <= 0)) {
    return false;
  }

  std::error_code ec;
  auto dir = std::filesystem::temp_directory_path(ec);
  if (ec || dir.empty()) {
    dir = "/tmp";
  }
  const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const auto file =
      dir / ("gblaunch-override-" + std::to_string(game_id) + "-" +
             std::to_string(stamp) + ".cfg");
  std::ofstream out(file);
  if (!out) {
    error = "failed to create override config";
    return false;
  }

  if (merged_override.audio_latency > 0) {
    out << "audio_latency = \"" << merged_override.audio_latency << "\"\n";
  }
  if (merged_override.video_width > 0 && merged_override.video_height > 0) {
    out << "video_fullscreen_x = \"" << merged_override.video_width << "\"\n";
    out << "video_fullscreen_y = \"" << merged_override.video_height << "\"\n";
  }
  out.flush();
  if (!out.good()) {
    error = "failed to write override config";
    return false;
  }

  out_path = file.string();
  return true;
}

std::int64_t NowUnixSeconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);

  if (args.game_id <= 0) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "usage: gblaunch --db <path> --game-id <id> [--dry-run]");
    return 2;
  }

  gb::db::Database db;
  if (!db.Open(args.db_path) || !db.InitSchema()) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "db open/init failed: " + db.LastError());
    return 1;
  }

  gb::db::LaunchInfo info;
  if (!db.GetLaunchInfo(args.game_id, info)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "launch lookup failed: " + db.LastError());
    return 1;
  }

  std::vector<std::string> cmd_argv;
  std::string build_error;
  if (!BuildLaunchArgv(info, cmd_argv, build_error)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "launch argv build failed: " + build_error);
    return 1;
  }

  gb::db::LaunchOverride merged_override;
  if (info.launch_type == "retroarch") {
    gb::db::LaunchOverride system_override;
    gb::db::LaunchOverride game_override;
    bool has_system_override = false;
    bool has_game_override = false;
    std::string override_error;

    if (!TryGetLaunchOverride(db, "system", info.system_id, system_override,
                              has_system_override, override_error)) {
      gb::core::Log(gb::core::LogLevel::Error,
                    "system override read failed: " + override_error);
      return 1;
    }
    if (!TryGetLaunchOverride(db, "game", info.rom_path, game_override,
                              has_game_override, override_error)) {
      gb::core::Log(gb::core::LogLevel::Error,
                    "game override read failed: " + override_error);
      return 1;
    }

    if (has_system_override) {
      MergeLaunchOverride(system_override, merged_override);
    }
    if (has_game_override) {
      MergeLaunchOverride(game_override, merged_override);
    }

    ApplyCoreOverride(cmd_argv, merged_override.core_path);
  }

  std::string override_append_cfg_path;
  if (info.launch_type == "retroarch") {
    std::string append_error;
    if (!BuildOverrideAppendConfig(merged_override, info.game_id,
                                   override_append_cfg_path, append_error) &&
        !append_error.empty()) {
      gb::core::Log(gb::core::LogLevel::Error,
                    "override append config failed: " + append_error);
      return 1;
    }
    if (!override_append_cfg_path.empty()) {
      cmd_argv.push_back("--appendconfig");
      cmd_argv.push_back(override_append_cfg_path);
    }
  }

  const auto cleanup_override_append = [&]() {
    if (override_append_cfg_path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(override_append_cfg_path, ec);
  };

  if (args.dry_run) {
    std::string joined;
    for (size_t i = 0; i < cmd_argv.size(); ++i) {
      if (i > 0) {
        joined += " | ";
      }
      joined += cmd_argv[i];
    }
    gb::core::Log(gb::core::LogLevel::Info, "dry-run argv: " + joined);
    cleanup_override_append();
    return 0;
  }

  gb::core::Log(gb::core::LogLevel::Info,
                "launching game_id=" + std::to_string(info.game_id) +
                    " title=\"" + info.title + "\"");

  const auto result = gb::platform::RunProcessBlocking(cmd_argv);
  if (!result.launched) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "launch failed: " + result.error);
    cleanup_override_append();
    return 1;
  }

  if (!db.UpdateLastPlayed(info.game_id, NowUnixSeconds())) {
    gb::core::Log(gb::core::LogLevel::Warn,
                  "failed to update last_played: " + db.LastError());
  }

  if (result.signaled) {
    gb::core::Log(gb::core::LogLevel::Warn,
                  "child terminated by signal " + std::to_string(result.signal));
    cleanup_override_append();
    return result.exit_code;
  }

  gb::core::Log(gb::core::LogLevel::Info,
                "child exited code " + std::to_string(result.exit_code));
  cleanup_override_append();
  return result.exit_code;
}
