#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/launch_config.h"

namespace gb::core {

enum class PlayMode { Fresh, Resume, Backup };

// Stored independently of settings: navigation must never implicitly save edits.
struct BrowseState {
  std::string screen = "systems";
  std::string view = "system";
  std::string system_id;
  std::map<std::string, int> selected_games;
  bool operator==(const BrowseState&) const = default;
};
bool LoadBrowseState(const std::string& db_path, BrowseState& out);
bool SaveBrowseState(const std::string& db_path, const BrowseState& state,
                     std::string& error);

std::filesystem::path ContinueDirectory(const std::string& db_path,
                                         const EffectiveLaunch& launch);
bool HasContinueSave(const std::string& db_path, const EffectiveLaunch& launch,
                     bool backup = false);
std::filesystem::path PlayResultPath(const std::string& db_path, int game_id);

// A private per-launch state path prevents failed runs from replacing good saves.
class PlaySession {
 public:
  PlaySession() = default;
  PlaySession(const PlaySession&) = delete;
  PlaySession& operator=(const PlaySession&) = delete;
  ~PlaySession();
  bool Prepare(const std::string& db_path, const EffectiveLaunch& launch,
               PlayMode mode, std::vector<std::string>& argv, std::string& error);
  bool Finish(bool clean_exit, std::string& status);
  std::filesystem::path AutoStatePath() const;
 private:
  std::filesystem::path directory_;
  std::filesystem::path staging_;
  std::filesystem::file_time_type initial_write_{};
  bool seeded_ = false;
  std::string state_name_;
};

// RetroArch accepts one --appendconfig argument containing a | separated list.
void AppendRetroArchConfig(std::vector<std::string>& argv, const std::string& path);

}  // namespace gb::core
