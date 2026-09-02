#pragma once

#include <string>
#include <vector>

#include "db/db.h"

namespace gb::db {

struct SystemDefinition {
  std::string id;
  std::string name;
  std::vector<std::string> rom_extensions;
  std::string launch_type;
  std::string launch_template;
};

struct ScanConfig {
  std::string defaults_json_path;
  std::string systems_dir;
  std::vector<std::string> override_roots;
  bool hide_missing = false;
};

struct ScanStats {
  int systems_loaded = 0;
  int roots_ok = 0;
  int roots_unavailable = 0;
  int roots_error = 0;
  int files_seen = 0;
  int games_upserted = 0;
  int total_games = 0;
  int present_games = 0;
  int missing_games = 0;
  std::vector<LibraryRootState> roots;
  std::vector<std::string> warnings;
};

bool LoadLibraryRootsFromDefaults(const std::string& path,
                                  std::vector<std::string>& roots,
                                  std::string& error);
bool LoadSystemsFromDirectory(const std::string& directory,
                              std::vector<SystemDefinition>& systems,
                              std::string& error);
bool RunIncrementalScan(Database& db, const ScanConfig& config, ScanStats& stats);

}  // namespace gb::db
