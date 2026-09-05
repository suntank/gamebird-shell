#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/launch_config.h"
#include "core/play_session.h"
#include "core/logging.h"
#include "db/db.h"
#include "platform/proc.h"

namespace {

struct Args {
  std::string db_path = "./data/catalog.db";
  int game_id = 0;
  bool dry_run = false;
  bool show_effective = false;
  bool validate_all = false;
  gb::core::PlayMode play_mode = gb::core::PlayMode::Fresh;
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
    if (arg == "--resume") { out.play_mode = gb::core::PlayMode::Resume; continue; }
    if (arg == "--resume-backup") { out.play_mode = gb::core::PlayMode::Backup; continue; }
    if (arg == "--fresh") { out.play_mode = gb::core::PlayMode::Fresh; continue; }
    if (arg == "--dry-run") {
      out.dry_run = true;
      continue;
    }
    if (arg == "--show-effective") {
      out.show_effective = true;
      continue;
    }
    if (arg == "--validate-all") {
      out.validate_all = true;
      continue;
    }
  }

  return out;
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

std::string BaseName(const std::string& path) {
  return path.empty() ? std::string() : std::filesystem::path(path).filename().string();
}

std::string JoinConfigNames(const gb::core::EffectiveLaunch& launch) {
  if (launch.append_configs.empty()) {
    return "none";
  }
  std::string out;
  for (const auto& config : launch.append_configs) {
    if (!out.empty()) {
      out += ",";
    }
    out += BaseName(config);
  }
  return out;
}

void LogEffectiveLaunch(const gb::core::EffectiveLaunch& launch) {
  gb::core::Log(gb::core::LogLevel::Info,
                "effective game_id=" + std::to_string(launch.info.game_id) +
                    " system=" + launch.info.system_id +
                    " core=" +
                    (launch.effective_core.empty() ? "none" : launch.effective_core) +
                    " core_source=\"" + launch.core_source + "\"" +
                    " append_config=" + JoinConfigNames(launch));
}

bool LogLaunchIssues(const gb::core::EffectiveLaunch& launch) {
  bool has_error = false;
  for (const auto& issue : gb::core::ValidateEffectiveLaunch(launch)) {
    const bool is_error = issue.severity == gb::core::LaunchIssueSeverity::Error;
    has_error |= is_error;
    gb::core::Log(is_error ? gb::core::LogLevel::Error : gb::core::LogLevel::Warn,
                  "game_id=" + std::to_string(launch.info.game_id) + " " +
                      issue.message);
  }
  return !has_error;
}

int ValidateAllLaunches(gb::db::Database& db) {
  std::vector<gb::db::LaunchInfo> games;
  if (!db.ListPresentLaunchInfos(games)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "validation query failed: " + db.LastError());
    return 1;
  }

  std::set<std::string> system_ids;
  std::set<std::string> game_paths;
  std::vector<gb::db::SystemSummary> systems;
  if (!db.ListSystems(systems)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "system query failed: " + db.LastError());
    return 1;
  }
  for (const auto& system : systems) {
    system_ids.insert(system.id);
  }
  for (const auto& game : games) {
    game_paths.insert(game.rom_path);
  }

  int error_count = 0;
  int warning_count = 0;
  for (const auto& game : games) {
    gb::core::EffectiveLaunch launch;
    std::string error;
    if (!gb::core::ResolveEffectiveLaunch(db, game, launch, error)) {
      ++error_count;
      gb::core::Log(gb::core::LogLevel::Error,
                    "game_id=" + std::to_string(game.game_id) + " " + error);
      continue;
    }

    const auto issues = gb::core::ValidateEffectiveLaunch(launch);
    bool has_error = false;
    for (const auto& issue : issues) {
      const bool is_error = issue.severity == gb::core::LaunchIssueSeverity::Error;
      has_error |= is_error;
      if (is_error) {
        ++error_count;
      } else {
        ++warning_count;
      }
      gb::core::Log(is_error ? gb::core::LogLevel::Error : gb::core::LogLevel::Warn,
                    "game_id=" + std::to_string(game.game_id) + " " + issue.message);
    }
    if (!has_error) {
      gb::core::Log(gb::core::LogLevel::Info,
                    "OK game_id=" + std::to_string(game.game_id) +
                        " system=" + game.system_id + " core=" +
                        (launch.effective_core.empty() ? "none"
                                                       : BaseName(launch.effective_core)) +
                        " config=" + JoinConfigNames(launch));
    }
  }

  std::vector<gb::db::LaunchOverride> overrides;
  if (!db.ListLaunchOverrides(overrides)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "override query failed: " + db.LastError());
    return 1;
  }
  for (const auto& row : overrides) {
    bool orphaned = false;
    if (row.scope_type == "system") {
      orphaned = !system_ids.count(row.scope_id);
    } else if (row.scope_type == "game") {
      orphaned = !game_paths.count(row.scope_id);
    } else {
      ++error_count;
      gb::core::Log(gb::core::LogLevel::Error,
                    "invalid override scope: " + row.scope_type + ":" + row.scope_id);
      continue;
    }
    if (orphaned) {
      ++warning_count;
      gb::core::Log(gb::core::LogLevel::Warn,
                    "orphaned override: " + row.scope_type + ":" + row.scope_id);
    }
  }

  gb::core::Log(error_count == 0 ? gb::core::LogLevel::Info
                                 : gb::core::LogLevel::Error,
                "validation summary games=" + std::to_string(games.size()) +
                    " errors=" + std::to_string(error_count) +
                    " warnings=" + std::to_string(warning_count));
  return error_count == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);

  if (args.game_id <= 0 && !args.validate_all) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "usage: gblaunch --db <path> (--game-id <id> [--dry-run] "
                  "[--show-effective] [--fresh|--resume|--resume-backup] | --validate-all)");
    return 2;
  }

  gb::db::Database db;
  if (!db.Open(args.db_path) || !db.InitSchema()) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "db open/init failed: " + db.LastError());
    return 1;
  }

  if (args.validate_all) {
    return ValidateAllLaunches(db);
  }

  gb::core::EffectiveLaunch effective;
  std::string build_error;
  if (!gb::core::ResolveEffectiveLaunch(db, args.game_id, effective, build_error)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "launch resolution failed: " + build_error);
    return 1;
  }
  std::vector<std::string> cmd_argv = effective.argv;
  if (!LogLaunchIssues(effective)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "launch blocked by invalid effective configuration");
    return 1;
  }
  LogEffectiveLaunch(effective);

  std::string override_append_cfg_path;
  if (effective.info.launch_type == "retroarch") {
    std::string append_error;
    if (!BuildOverrideAppendConfig(effective.merged_override, effective.info.game_id,
                                   override_append_cfg_path, append_error) &&
        !append_error.empty()) {
      gb::core::Log(gb::core::LogLevel::Error,
                    "override append config failed: " + append_error);
      return 1;
    }
    if (!override_append_cfg_path.empty()) {
      gb::core::AppendRetroArchConfig(cmd_argv, override_append_cfg_path);
    }
  }

  const auto cleanup_override_append = [&]() {
    if (override_append_cfg_path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(override_append_cfg_path, ec);
  };

  gb::core::PlaySession session;
  if (!args.dry_run && !session.Prepare(args.db_path, effective, args.play_mode, cmd_argv, build_error)) {
    gb::core::Log(gb::core::LogLevel::Error, "session preparation failed: " + build_error);
    cleanup_override_append();
    return 1;
  }

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
                "launching game_id=" + std::to_string(effective.info.game_id) +
                    " title=\"" + effective.info.title + "\"");

  const auto result = gb::platform::RunProcessBlocking(cmd_argv);
  if (!result.launched) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "launch failed: " + result.error);
    cleanup_override_append();
    return 1;
  }

  std::string session_status;
  session.Finish(result.exited_normally && result.exit_code == 0, session_status);
  {
    const auto path = gb::core::PlayResultPath(args.db_path, effective.info.game_id);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream receipt(path);
    receipt << session_status << '\n';
  }
  gb::core::Log(gb::core::LogLevel::Info, session_status);

  if (result.exited_normally && result.exit_code == 0 &&
      !db.UpdateLastPlayed(effective.info.game_id, NowUnixSeconds())) {
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
