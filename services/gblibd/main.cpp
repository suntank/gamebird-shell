#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
  std::string artwork_dir;
  std::vector<std::string> roots;
  bool hide_missing = false;

  bool mode_scan_once = true;
  bool mode_jobs_once = false;
  bool mode_daemon = false;
  int poll_seconds = 2;
  int rescan_seconds = 10;

  bool enqueue_default = false;
  bool enqueue_scan = false;
  bool enqueue_identify = false;
  bool enqueue_scrape = false;
  bool enqueue_artwork = false;
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
    if (arg == "--artwork-dir" && i + 1 < argc) {
      out.artwork_dir = argv[++i];
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
    if (arg == "--rescan-seconds" && i + 1 < argc) {
      out.rescan_seconds = std::atoi(argv[++i]);
      if (out.rescan_seconds < 2) {
        out.rescan_seconds = 2;
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
    if (arg == "--enqueue-artwork") {
      out.enqueue_artwork = true;
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
                    " roots_ok=" + std::to_string(stats.roots_ok) +
                    " roots_unavailable=" +
                    std::to_string(stats.roots_unavailable) +
                    " roots_error=" + std::to_string(stats.roots_error) +
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

std::vector<std::string> LibrarySnapshot(const Args& args) {
  std::vector<std::string> roots = args.roots;
  if (roots.empty()) {
    std::string error;
    gb::db::LoadLibraryRootsFromDefaults(args.defaults_json, roots, error);
  }

  std::vector<std::string> snapshot;
  for (const auto& root : roots) {
    std::error_code ec;
    const auto status = std::filesystem::status(root, ec);
    if (ec || !std::filesystem::is_directory(status)) {
      snapshot.push_back(root + "\n<unavailable>");
      continue;
    }

    std::filesystem::recursive_directory_iterator it(root, ec), end;
    if (ec) {
      snapshot.push_back(root + "\n<unreadable>");
      continue;
    }
    while (it != end) {
      const auto& entry = *it;
      std::error_code entry_ec;
      std::string item = entry.path().lexically_normal().string();
      item += '\n';
      item += std::to_string(entry.last_write_time(entry_ec).time_since_epoch().count());
      if (entry_ec) {
        item += "\n<stat-error>";
      } else if (entry.is_regular_file(entry_ec) && !entry_ec) {
        item += '\n';
        item += std::to_string(entry.file_size(entry_ec));
      }
      snapshot.push_back(std::move(item));
      it.increment(ec);
      if (ec) {
        snapshot.push_back(root + "\n<traversal-error>");
        break;
      }
    }
  }
  std::sort(snapshot.begin(), snapshot.end());
  return snapshot;
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
  if (args.enqueue_artwork) {
    db.EnqueueJob("build_thumb", "{}");
  }
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
      args.enqueue_scrape || args.enqueue_artwork) {
    EnqueueRequestedJobs(db, args);
  }

  gb::scrape::WorkerConfig worker_cfg;
  worker_cfg.defaults_json_path = args.defaults_json;
  worker_cfg.systems_dir = args.systems_dir;
  worker_cfg.artwork_dir = args.artwork_dir.empty()
                               ? (std::filesystem::path(args.db_path).parent_path() / "artwork").string()
                               : args.artwork_dir;
  worker_cfg.override_roots = args.roots;
  worker_cfg.hide_missing = args.hide_missing;

  gb::scrape::WorkerStats worker_stats;

  if (args.mode_jobs_once) {
    if (!args.enqueue_default && !args.enqueue_scan && !args.enqueue_identify &&
        !args.enqueue_scrape && !args.enqueue_artwork &&
        db.CountJobsByStatus("queued") == 0) {
      gb::scrape::EnqueueDefaultJobs(db, true, true, false);
    }

    gb::scrape::ProcessJobsUntilEmpty(db, worker_cfg, worker_stats);

    gb::core::Log(gb::core::LogLevel::Info,
                  "jobs-once complete claimed=" +
                      std::to_string(worker_stats.jobs_claimed) +
                      " ok=" + std::to_string(worker_stats.jobs_ok) +
                      " err=" + std::to_string(worker_stats.jobs_error) +
                      " metadata_updates=" +
                      std::to_string(worker_stats.metadata_updates) +
                      " artwork_indexed=" +
                      std::to_string(worker_stats.artwork_indexed) +
                      " artwork_missing=" +
                      std::to_string(worker_stats.artwork_missing));
    return worker_stats.jobs_error > 0 ? 3 : 0;
  }

  if (args.mode_daemon) {
    gb::core::Log(gb::core::LogLevel::Info,
                  "daemon mode poll_seconds=" +
                      std::to_string(args.poll_seconds) +
                      " rescan_seconds=" +
                      std::to_string(args.rescan_seconds));

    // Samba uploads bypass the UI's explicit Rescan action. Scan once when
    // the daemon starts, then watch a lightweight metadata snapshot so a new,
    // replaced, renamed, or removed ROM is reflected automatically without
    // continuously rewriting the catalog database.
    RunScanOnce(db, args);
    auto library_snapshot = LibrarySnapshot(args);
    auto next_rescan = std::chrono::steady_clock::now() +
                       std::chrono::seconds(args.rescan_seconds);

    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_rescan) {
        auto current_snapshot = LibrarySnapshot(args);
        if (current_snapshot != library_snapshot) {
          if (RunScanOnce(db, args) == 0) {
            library_snapshot = std::move(current_snapshot);
          }
        }
        next_rescan = now + std::chrono::seconds(args.rescan_seconds);
      }

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
