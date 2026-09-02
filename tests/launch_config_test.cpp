#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "core/launch_config.h"
#include "db/db.h"

namespace {

struct TempDirectory {
  std::filesystem::path path;

  TempDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("gamebird-launch-test-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
  }

  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void WriteFile(const std::filesystem::path& path, const std::string& text = {}) {
  std::ofstream out(path);
  out << text;
}

bool HasSeverity(const std::vector<gb::core::LaunchIssue>& issues,
                 const gb::core::LaunchIssueSeverity severity) {
  for (const auto& issue : issues) {
    if (issue.severity == severity) {
      return true;
    }
  }
  return false;
}

int failures = 0;

void Expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

}  // namespace

int main() {
  TempDirectory temp;
  const auto executable = temp.path / "retroarch";
  const auto default_core = temp.path / "snes9x_libretro.so";
  const auto alternate_core = temp.path / "bsnes_libretro.so";
  const auto config = temp.path / "retroarch-gamebird.cfg";
  const auto rom = temp.path / "SuperMetroid.sfc";
  WriteFile(executable, "#!/bin/sh\nexit 0\n");
  WriteFile(default_core);
  WriteFile(alternate_core);
  WriteFile(config, "video_threaded = \"true\"\n");
  WriteFile(rom, "ROM");
  std::filesystem::permissions(
      executable, std::filesystem::perms::owner_exec |
                      std::filesystem::perms::owner_read |
                      std::filesystem::perms::owner_write);

  gb::db::Database db;
  Expect(db.Open((temp.path / "catalog.db").string()), "open test database");
  Expect(db.InitSchema(), "initialize schema");
  const std::string launch_template =
      executable.string() + " --appendconfig " + config.string() + " -L " +
      default_core.string() + " {rom_path}";
  Expect(db.UpsertSystem(gb::db::SystemRecord{
             .id = "snes",
             .name = "Super Nintendo",
             .rom_extensions = ".sfc",
             .launch_type = "retroarch",
             .launch_template = launch_template,
         }),
         "insert system definition");
  Expect(db.UpsertGame(gb::db::GameRecord{
             .system_id = "snes",
             .library_root = "",
             .path = rom.string(),
             .filename = rom.filename().string(),
             .title = "Super Metroid",
             .sort_title = "super metroid",
             .size_bytes = 3,
             .mtime = 1,
         }),
         "insert game");

  std::vector<gb::db::LaunchInfo> games;
  Expect(db.ListPresentLaunchInfos(games) && games.size() == 1,
         "list present launch targets");
  const int game_id = games.empty() ? 0 : games.front().game_id;

  gb::core::EffectiveLaunch resolved;
  std::string error;
  Expect(gb::core::ResolveEffectiveLaunch(db, game_id, resolved, error),
         "resolve definition launch: " + error);
  Expect(resolved.effective_core == default_core.string(),
         "definition selects authoritative core");
  Expect(resolved.core_source == "definition", "report definition core source");
  auto issues = gb::core::ValidateEffectiveLaunch(resolved);
  Expect(!HasSeverity(issues, gb::core::LaunchIssueSeverity::Error),
         "valid definition passes validation");

  Expect(db.UpsertLaunchOverride(gb::db::LaunchOverride{
             .scope_type = "system",
             .scope_id = "snes",
             .core_path = alternate_core.string(),
             .audio_latency = 64,
         }),
         "insert system override");
  Expect(gb::core::ResolveEffectiveLaunch(db, game_id, resolved, error),
         "resolve system override");
  Expect(resolved.effective_core == alternate_core.string(),
         "system override wins over definition");
  Expect(resolved.core_source == "system override", "report system override source");
  issues = gb::core::ValidateEffectiveLaunch(resolved);
  Expect(HasSeverity(issues, gb::core::LaunchIssueSeverity::Warning),
         "core replacement produces stale-override warning");

  Expect(db.UpsertLaunchOverride(gb::db::LaunchOverride{
             .scope_type = "game",
             .scope_id = rom.string(),
             .core_path = default_core.string(),
         }),
         "insert game override");
  Expect(gb::core::ResolveEffectiveLaunch(db, game_id, resolved, error),
         "resolve game override");
  Expect(resolved.effective_core == default_core.string(),
         "game override wins over system override");
  Expect(resolved.core_source == "game override", "report game override source");
  Expect(resolved.merged_override.audio_latency == 64,
         "game override inherits system audio setting");

  Expect(db.UpsertLaunchOverride(gb::db::LaunchOverride{
             .scope_type = "game",
             .scope_id = rom.string(),
             .core_path = (temp.path / "missing_libretro.so").string(),
         }),
         "replace game override with missing core");
  Expect(gb::core::ResolveEffectiveLaunch(db, game_id, resolved, error),
         "resolve missing-core override");
  issues = gb::core::ValidateEffectiveLaunch(resolved);
  Expect(HasSeverity(issues, gb::core::LaunchIssueSeverity::Error),
         "missing override core fails validation");

  std::vector<std::string> tokens;
  Expect(gb::core::TokenizeLaunchCommand("program \"rom with spaces.sfc\"", tokens,
                                         error) &&
             tokens.size() == 2 && tokens[1] == "rom with spaces.sfc",
         "tokenizer preserves quoted arguments");

  if (failures == 0) {
    std::cout << "launch_config_test: PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
