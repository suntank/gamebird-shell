#pragma once

#include <string>
#include <vector>

#include "db/db.h"

namespace gb::core {

enum class LaunchIssueSeverity {
  Warning,
  Error,
};

struct LaunchIssue {
  LaunchIssueSeverity severity = LaunchIssueSeverity::Error;
  std::string message;
};

struct EffectiveLaunch {
  db::LaunchInfo info;
  std::vector<std::string> argv;
  db::LaunchOverride merged_override;
  bool has_system_override = false;
  bool has_game_override = false;
  std::string template_core;
  std::string effective_core;
  std::string core_source;  // "definition", "system override", or "game override"
  std::vector<std::string> append_configs;
};

bool TokenizeLaunchCommand(const std::string& command,
                           std::vector<std::string>& out,
                           std::string& error);
std::string ExtractRetroArchCore(const std::vector<std::string>& argv);
std::string ExtractRetroArchCore(const std::string& launch_template);
std::vector<std::string> ExtractAppendConfigs(const std::vector<std::string>& argv);

bool ResolveEffectiveLaunch(db::Database& db,
                            const db::LaunchInfo& info,
                            EffectiveLaunch& out,
                            std::string& error);
bool ResolveEffectiveLaunch(db::Database& db,
                            int game_id,
                            EffectiveLaunch& out,
                            std::string& error);

std::vector<LaunchIssue> ValidateEffectiveLaunch(const EffectiveLaunch& launch);

}  // namespace gb::core
