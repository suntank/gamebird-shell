#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "core/logging.h"
#include "core/time.h"
#include "db/db.h"
#include "db/queries.h"
#include "scrape/jobs.h"

namespace {

struct Args {
  std::string db_path = "./data/catalog.db";
  std::string defaults_json = "./config/defaults.json";
  std::string systems_dir = "./config/systems.d";
  std::vector<std::string> roots;
  bool hide_missing = false;

  bool mode_scan_once = true;
  bool mode_jobs_once = false;
  bool mode_daemon = false;
  int poll_seconds = 2;

  bool enqueue_default = false;
  bool enqueue_scan = false;
  bool enqueue_identify = false;
  bool enqueue_scrape = false;
};

Args ParseArgs(const int argc, char** argv) {
  Args out;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];

    if (arg == "--db" && i + 1 < argc) {
      out.db_path = argv[++i];
      continue;
    }
    if (arg == "--defaults" && i + 1 < argc) {
      out.defaults_json = argv[++i];
      continue;
    }
    if (arg == "--systems-dir" && i + 1 < argc) {
      out.systems_dir = argv[++i];
      continue;
    }
    if (arg == "--root" && i + 1 < argc) {
      out.roots.emplace_back(argv[++i]);
      continue;
    }
    if (arg == "--hide-missing") {
      out.hide_missing = true;
      continue;
    }

    if (arg == "--scan-once") {
      out.mode_scan_once = true;
      out.mode_jobs_once = false;
      out.mode_daemon = false;
      continue;
    }
    if (arg == "--jobs-once") {
      out.mode_scan_once = false;
      out.mode_jobs_once = true;
      continue;
    }
    if (arg == "--daemon") {
      out.mode_scan_once = false;
      out.mode_daemon = true;
      continue;
    }
    if (arg == "--poll-seconds" && i + 1 < argc) {
      out.poll_seconds = std::atoi(argv[++i]);
      if (out.poll_seconds < 1) {
        out.poll_seconds = 1;
      }
      continue;
    }

    if (arg == "--enqueue-default") {
      out.enqueue_default = true;
      continue;
    }
    if (arg == "--enqueue-scan") {
      out.enqueue_scan = true;
      continue;
    }
    if (arg == "--enqueue-identify") {
      out.enqueue_identify = true;
      continue;
    }
    if (arg == "--enqueue-scrape") {
      out.enqueue_scrape = true;
      continue;
    }
  }

  return out;
}

int RunScanOnce(gb::db::Database& db, const Args& args) {
  gb::db::ScanConfig config;
  config.defaults_json_path = args.defaults_json;
  config.systems_dir = args.systems_dir;
  config.override_roots = args.roots;
  config.hide_missing = args.hide_missing;

  gb::db::ScanStats stats;
  const bool ok = gb::db::RunIncrementalScan(db, config, stats);

  if (!ok) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "scan failed. warnings=" +
                      std::to_string(stats.warnings.size()));
    for (const auto& warning : stats.warnings) {
      gb::core::Log(gb::core::LogLevel::Warn, warning);
    }
    return 2;
  }

  gb::core::Log(gb::core::LogLevel::Info,
                "scan complete systems=" + std::to_string(stats.systems_loaded) +
                    " files_seen=" + std::to_string(stats.files_seen) +
                    " upserted=" + std::to_string(stats.games_upserted) +
                    " total=" + std::to_string(stats.total_games) +
                    " present=" + std::to_string(stats.present_games) +
                    " missing=" + std::to_string(stats.missing_games));

  if (!stats.warnings.empty()) {
    for (const auto& warning : stats.warnings) {
      gb::core::Log(gb::core::LogLevel::Warn, warning);
    }
  }

  return 0;
}

void EnqueueRequestedJobs(gb::db::Database& db, const Args& args) {
  bool do_scan = args.enqueue_scan;
  bool do_identify = args.enqueue_identify;
  bool do_scrape = args.enqueue_scrape;

  if (args.enqueue_default) {
    do_scan = true;
    do_identify = true;
    do_scrape = true;
  }

  gb::scrape::EnqueueDefaultJobs(db, do_scan, do_identify, do_scrape);
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = ParseArgs(argc, argv);

  gb::core::Log(gb::core::LogLevel::Info,
                "gblibd start db=" + args.db_path);

  gb::db::Database db;
  if (!db.Open(args.db_path)) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "open failed: " + db.LastError());
    return 1;
  }

  if (!db.InitSchema()) {
    gb::core::Log(gb::core::LogLevel::Error,
                  "schema init failed: " + db.LastError());
    return 1;
  }

  if (args.mode_scan_once) {
    return RunScanOnce(db, args);
  }

  if (args.enqueue_default || args.enqueue_scan || args.enqueue_identify ||
      args.enqueue_scrape) {
    EnqueueRequestedJobs(db, args);
  }

  gb::scrape::WorkerConfig worker_cfg;
  worker_cfg.defaults_json_path = args.defaults_json;
  worker_cfg.systems_dir = args.systems_dir;
  worker_cfg.override_roots = args.roots;
  worker_cfg.hide_missing = args.hide_missing;

  gb::scrape::WorkerStats worker_stats;

  if (args.mode_jobs_once) {
    if (!args.enqueue_default && !args.enqueue_scan && !args.enqueue_identify &&
        !args.enqueue_scrape && db.CountJobsByStatus("queued") == 0) {
      gb::scrape::EnqueueDefaultJobs(db, true, true, false);
    }

    gb::scrape::ProcessJobsUntilEmpty(db, worker_cfg, worker_stats);

    gb::core::Log(gb::core::LogLevel::Info,
                  "jobs-once complete claimed=" +
                      std::to_string(worker_stats.jobs_claimed) +
                      " ok=" + std::to_string(worker_stats.jobs_ok) +
                      " err=" + std::to_string(worker_stats.jobs_error) +
                      " metadata_updates=" +
                      std::to_string(worker_stats.metadata_updates));
    return worker_stats.jobs_error > 0 ? 3 : 0;
  }

  if (args.mode_daemon) {
    gb::core::Log(gb::core::LogLevel::Info,
                  "daemon mode poll_seconds=" +
                      std::to_string(args.poll_seconds));

    while (true) {
      std::string info;
      if (!gb::scrape::ProcessOneQueuedJob(db, worker_cfg, worker_stats, info)) {
        gb::core::SleepMs(static_cast<std::uint32_t>(args.poll_seconds * 1000));
        continue;
      }

      if (!info.empty()) {
        gb::core::Log(gb::core::LogLevel::Info, info);
      }
    }
  }

  gb::core::Log(gb::core::LogLevel::Warn,
                "no work mode selected; exiting");
  return 0;
}
