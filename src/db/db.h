#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "db/sqlite3_api.h"

namespace gb::db {

struct SystemRecord {
  std::string id;
  std::string name;
  std::string rom_extensions;
  std::string launch_type;
  std::string launch_template;
};

struct GameRecord {
  std::string system_id;
  std::string path;
  std::string filename;
  std::string title;
  std::string sort_title;
  std::int64_t size_bytes = 0;
  std::int64_t mtime = 0;
};

struct ScanSummary {
  int total_games = 0;
  int present_games = 0;
  int missing_games = 0;
};

struct SystemSummary {
  std::string id;
  std::string name;
  int game_count = 0;
  int favorite_count = 0;
};

struct GameSummary {
  int id = 0;
  std::string title;
  std::string filename;
  bool is_favorite = false;
  bool is_hidden = false;
};

struct LaunchInfo {
  int game_id = 0;
  std::string system_id;
  std::string title;
  std::string rom_path;
  std::string launch_type;
  std::string launch_template;
};

struct LaunchOverride {
  std::string scope_type;  // "system" or "game"
  std::string scope_id;    // system_id or rom_path
  std::string core_path;
  int audio_latency = 0;
  int video_width = 0;
  int video_height = 0;
};

struct JobRecord {
  int id = 0;
  std::string type;
  std::string status;
  std::string payload_json;
  std::string error;
  std::int64_t created_at = 0;
  std::int64_t updated_at = 0;
};

struct MetadataCandidate {
  int game_id = 0;
  std::string system_id;
  std::string title;
  std::string filename;
};

struct MetadataUpdate {
  int game_id = 0;
  int release_year = 0;
  std::string genre;
  int players = 1;
  std::string description;
  std::string source;
};

class Database {
 public:
  Database() = default;
  ~Database();

  bool Open(const std::string& db_path);
  void Close();

  bool InitSchema();
  bool BeginIncrementalScan();
  bool UpsertSystem(const SystemRecord& system);
  bool UpsertGame(const GameRecord& game);
  bool EndIncrementalScan(bool hide_missing);
  bool ListSystems(std::vector<SystemSummary>& out);
  bool ListGamesBySystem(const std::string& system_id,
                         bool include_hidden,
                         std::vector<GameSummary>& out);
  bool ToggleGameFavorite(int game_id, bool& new_value);
  bool ToggleGameHidden(int game_id, bool& new_value);
  bool GetLaunchInfo(int game_id, LaunchInfo& out);
  bool GetSystemLaunchInfo(const std::string& system_id, LaunchInfo& out);
  bool GetLaunchOverride(const std::string& scope_type,
                         const std::string& scope_id,
                         LaunchOverride& out);
  bool UpsertLaunchOverride(const LaunchOverride& override_row);
  bool DeleteLaunchOverride(const std::string& scope_type,
                            const std::string& scope_id);
  bool UpdateLastPlayed(int game_id, std::int64_t unix_seconds);
  bool EnqueueJob(const std::string& type,
                  const std::string& payload_json,
                  int* out_job_id = nullptr);
  bool ClaimNextQueuedJob(JobRecord& out);
  bool MarkJobOk(int job_id);
  bool MarkJobError(int job_id, const std::string& error);
  int CountJobsByStatus(const std::string& status);
  bool ListGamesNeedingMetadata(std::vector<MetadataCandidate>& out, int limit);
  bool UpsertGameMetadata(const MetadataUpdate& update);

  [[nodiscard]] ScanSummary ReadSummary() const;
  [[nodiscard]] bool IsOpen() const;
  [[nodiscard]] const std::string& LastError() const;

 private:
  bool Exec(const std::string& sql);
  bool EnsureGamesPresenceColumn();
  bool QuerySingleInt(const std::string& sql, int& value) const;
  void SetError(const std::string& message);

  sqlite3* db_ = nullptr;
  std::string db_path_;
  std::string last_error_;
};

}  // namespace gb::db
