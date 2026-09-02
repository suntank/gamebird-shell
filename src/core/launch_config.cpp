#include "core/launch_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <unordered_map>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace gb::core {
namespace {

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

bool TryGetLaunchOverride(db::Database& db,
                          const std::string& scope_type,
                          const std::string& scope_id,
                          db::LaunchOverride& out,
                          bool& found,
                          std::string& error) {
  if (db.GetLaunchOverride(scope_type, scope_id, out)) {
    found = true;
    return true;
  }
  found = false;
  if (db.LastError().rfind("launch override not found", 0) == 0) {
    out = db::LaunchOverride{};
    return true;
  }
  error = db.LastError();
  return false;
}

void MergeOverride(const db::LaunchOverride& src, db::LaunchOverride& dst) {
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

void ApplyCoreOverride(std::vector<std::string>& argv, const std::string& core_path) {
  if (core_path.empty() || argv.empty()) {
    return;
  }
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if ((argv[i] == "-L" || argv[i] == "--libretro") && i + 1 < argv.size()) {
      argv[i + 1] = core_path;
      return;
    }
    if (argv[i].rfind("-L", 0) == 0 && argv[i].size() > 2) {
      argv[i] = "-L" + core_path;
      return;
    }
    constexpr std::string_view kPrefix = "--libretro=";
    if (argv[i].rfind(kPrefix, 0) == 0) {
      argv[i] = std::string(kPrefix) + core_path;
      return;
    }
  }
  if (argv.size() == 1) {
    argv.push_back("-L");
    argv.push_back(core_path);
  } else {
    argv.insert(argv.begin() + 1, core_path);
    argv.insert(argv.begin() + 1, "-L");
  }
}

bool IsRegularFile(const std::string& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

bool IsExecutable(const std::string& executable) {
  if (executable.empty()) {
    return false;
  }
  if (executable.find('/') != std::string::npos) {
    if (!IsRegularFile(executable)) {
      return false;
    }
#if !defined(_WIN32)
    return ::access(executable.c_str(), X_OK) == 0;
#else
    return true;
#endif
  }

  const char* path_env = std::getenv("PATH");
  if (!path_env) {
    return false;
  }
  std::string path_value(path_env);
  std::size_t begin = 0;
  while (begin <= path_value.size()) {
    const auto end = path_value.find(':', begin);
    const std::string dir = path_value.substr(begin, end - begin);
    const auto candidate = std::filesystem::path(dir.empty() ? "." : dir) / executable;
    if (IsRegularFile(candidate.string())) {
#if !defined(_WIN32)
      if (::access(candidate.c_str(), X_OK) == 0) {
        return true;
      }
#else
      return true;
#endif
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return false;
}

void AddIssue(std::vector<LaunchIssue>& issues,
              const LaunchIssueSeverity severity,
              std::string message) {
  issues.push_back(LaunchIssue{.severity = severity, .message = std::move(message)});
}

std::vector<std::string> SplitConfigPaths(const std::string& value) {
  std::vector<std::string> out;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const auto end = value.find('|', begin);
    const std::string item = value.substr(begin, end - begin);
    if (!item.empty()) {
      out.push_back(item);
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return out;
}

}  // namespace

bool TokenizeLaunchCommand(const std::string& command,
                           std::vector<std::string>& out,
                           std::string& error) {
  out.clear();
  std::string cur;
  enum class Quote { None, Single, Double };
  Quote quote = Quote::None;
  bool escape = false;

  for (const char c : command) {
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
    } else if (c == '\'') {
      quote = Quote::Single;
    } else if (c == '"') {
      quote = Quote::Double;
    } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
    } else {
      cur.push_back(c);
    }
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

std::string ExtractRetroArchCore(const std::vector<std::string>& argv) {
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if ((argv[i] == "-L" || argv[i] == "--libretro") && i + 1 < argv.size()) {
      return argv[i + 1];
    }
    if (argv[i].rfind("-L", 0) == 0 && argv[i].size() > 2) {
      return argv[i].substr(2);
    }
    constexpr std::string_view kPrefix = "--libretro=";
    if (argv[i].rfind(kPrefix, 0) == 0 && argv[i].size() > kPrefix.size()) {
      return argv[i].substr(kPrefix.size());
    }
  }
  return {};
}

std::string ExtractRetroArchCore(const std::string& launch_template) {
  std::vector<std::string> argv;
  std::string error;
  if (!TokenizeLaunchCommand(launch_template, argv, error)) {
    return {};
  }
  return ExtractRetroArchCore(argv);
}

std::vector<std::string> ExtractAppendConfigs(const std::vector<std::string>& argv) {
  std::vector<std::string> out;
  constexpr std::string_view kPrefix = "--appendconfig=";
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--appendconfig" && i + 1 < argv.size()) {
      const auto paths = SplitConfigPaths(argv[++i]);
      out.insert(out.end(), paths.begin(), paths.end());
    } else if (argv[i].rfind(kPrefix, 0) == 0) {
      const auto paths = SplitConfigPaths(argv[i].substr(kPrefix.size()));
      out.insert(out.end(), paths.begin(), paths.end());
    }
  }
  return out;
}

bool ResolveEffectiveLaunch(db::Database& db,
                            const db::LaunchInfo& info,
                            EffectiveLaunch& out,
                            std::string& error) {
  out = EffectiveLaunch{};
  out.info = info;

  const std::string launch_template =
      info.launch_template.empty() ? "{rom_path}" : info.launch_template;
  if (!TokenizeLaunchCommand(launch_template, out.argv, error)) {
    return false;
  }

  const std::unordered_map<std::string, std::string> variables = {
      {"{rom_path}", info.rom_path},
      {"{system_id}", info.system_id},
      {"{title}", info.title},
      {"{game_id}", std::to_string(info.game_id)},
  };
  for (auto& token : out.argv) {
    for (const auto& [key, value] : variables) {
      token = ReplaceAll(token, key, value);
    }
  }
  if (out.argv.empty() || out.argv.front().empty()) {
    error = "empty executable path in launch argv";
    return false;
  }

  out.template_core = ExtractRetroArchCore(launch_template);
  out.core_source = "definition";
  if (info.launch_type == "retroarch") {
    db::LaunchOverride system_override;
    db::LaunchOverride game_override;
    if (!TryGetLaunchOverride(db, "system", info.system_id, system_override,
                              out.has_system_override, error)) {
      return false;
    }
    if (!info.rom_path.empty() &&
        !TryGetLaunchOverride(db, "game", info.rom_path, game_override,
                              out.has_game_override, error)) {
      return false;
    }
    if (out.has_system_override) {
      MergeOverride(system_override, out.merged_override);
      if (!system_override.core_path.empty()) {
        out.core_source = "system override";
      }
    }
    if (out.has_game_override) {
      MergeOverride(game_override, out.merged_override);
      if (!game_override.core_path.empty()) {
        out.core_source = "game override";
      }
    }
    ApplyCoreOverride(out.argv, out.merged_override.core_path);
  }

  out.effective_core = ExtractRetroArchCore(out.argv);
  out.append_configs = ExtractAppendConfigs(out.argv);
  return true;
}

bool ResolveEffectiveLaunch(db::Database& db,
                            const int game_id,
                            EffectiveLaunch& out,
                            std::string& error) {
  db::LaunchInfo info;
  if (!db.GetLaunchInfo(game_id, info)) {
    error = db.LastError();
    return false;
  }
  return ResolveEffectiveLaunch(db, info, out, error);
}

std::vector<LaunchIssue> ValidateEffectiveLaunch(const EffectiveLaunch& launch) {
  std::vector<LaunchIssue> issues;
  if (launch.argv.empty()) {
    AddIssue(issues, LaunchIssueSeverity::Error, "resolved command is empty");
    return issues;
  }
  if (!IsExecutable(launch.argv.front())) {
    AddIssue(issues, LaunchIssueSeverity::Error,
             "executable not found or not executable: " + launch.argv.front());
  }
  if (launch.info.game_id > 0 && !IsRegularFile(launch.info.rom_path)) {
    AddIssue(issues, LaunchIssueSeverity::Error,
             "ROM/app path is missing: " + launch.info.rom_path);
  }
  for (const auto& token : launch.argv) {
    if (token.find('{') != std::string::npos || token.find('}') != std::string::npos) {
      AddIssue(issues, LaunchIssueSeverity::Error,
               "unresolved template variable in argument: " + token);
    }
  }

  if (launch.info.launch_type == "retroarch") {
    if (launch.effective_core.empty()) {
      AddIssue(issues, LaunchIssueSeverity::Error,
               "RetroArch launch has no libretro core");
    } else if (!IsRegularFile(launch.effective_core)) {
      AddIssue(issues, LaunchIssueSeverity::Error,
               "libretro core is missing: " + launch.effective_core);
    } else if (std::filesystem::path(launch.effective_core).extension() != ".so") {
      AddIssue(issues, LaunchIssueSeverity::Warning,
               "libretro core does not use a .so extension: " + launch.effective_core);
    }
    for (const auto& config : launch.append_configs) {
      if (!IsRegularFile(config)) {
        AddIssue(issues, LaunchIssueSeverity::Error,
                 "append config is missing: " + config);
      }
    }
    if (!launch.template_core.empty() && !launch.effective_core.empty() &&
        launch.template_core != launch.effective_core) {
      AddIssue(issues, LaunchIssueSeverity::Warning,
               launch.core_source + " replaces system core " +
                   std::filesystem::path(launch.template_core).filename().string() +
                   " with " +
                   std::filesystem::path(launch.effective_core).filename().string());
    }
  }
  return issues;
}

}  // namespace gb::core
