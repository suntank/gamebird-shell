#pragma once

#include <string>
#include <vector>

#include "db/db.h"
#include "scrape/providers/provider_libretro.h"

namespace gb::scrape {

struct WorkerConfig {
  std::string defaults_json_path;
  std::string systems_dir;
  std::vector<std::string> override_roots;
  std::string artwork_dir = "./data/artwork";
  std::string provider = "libretro";
  bool overwrite_artwork = false;
  bool hide_missing = false;
  int metadata_batch = 256;
};

struct WorkerStats {
  int jobs_claimed = 0;
  int jobs_ok = 0;
  int jobs_error = 0;
  int scan_files_seen = 0;
  int scan_roots_ok = 0;
  int scan_roots_unavailable = 0;
  int scan_roots_error = 0;
  int metadata_updates = 0;
  int artwork_indexed = 0;
  int artwork_missing = 0;
  int scrape_matched = 0;
  int scrape_downloaded = 0;
  int scrape_skipped = 0;
};

// A scrape is intentionally stepped one game at a time by the shell. This
// keeps the handheld UI responsive and lets it report useful progress while
// network requests are in flight.
struct ScrapeProgress {
  int total = 0;
  int completed = 0;
  int matched = 0;
  int downloaded = 0;
  int skipped_existing = 0;
  int missing = 0;
  std::string current_title;
  std::string last_error;
  bool finished = false;
};

class ScrapeSession {
 public:
  bool Begin(db::Database& db, const WorkerConfig& cfg, ScrapeProgress& progress);
  bool ProcessNext(db::Database& db, ScrapeProgress& progress);

 private:
  WorkerConfig cfg_;
  std::vector<db::AssetCandidate> games_;
  providers::LibretroProvider provider_;
  std::size_t next_game_ = 0;
  bool active_ = false;
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
