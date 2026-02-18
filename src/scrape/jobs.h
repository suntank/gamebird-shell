#pragma once

#include <string>
#include <vector>

#include "db/db.h"

namespace gb::scrape {

struct WorkerConfig {
  std::string defaults_json_path;
  std::string systems_dir;
  std::vector<std::string> override_roots;
  bool hide_missing = false;
  int metadata_batch = 256;
};

struct WorkerStats {
  int jobs_claimed = 0;
  int jobs_ok = 0;
  int jobs_error = 0;
  int scan_files_seen = 0;
  int metadata_updates = 0;
};

void EnqueueDefaultJobs(db::Database& db,
                        bool include_scan,
                        bool include_identify,
                        bool include_scrape);
bool ProcessOneQueuedJob(db::Database& db,
                         const WorkerConfig& cfg,
                         WorkerStats& stats,
                         std::string& info);
void ProcessJobsUntilEmpty(db::Database& db,
                           const WorkerConfig& cfg,
                           WorkerStats& stats);

}  // namespace gb::scrape
