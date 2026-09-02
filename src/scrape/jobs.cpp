#include "scrape/jobs.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/logging.h"
#include "db/queries.h"
#include "scrape/providers/provider_local_dat.h"
#include "scrape/providers/provider_none.h"

namespace gb::scrape {

namespace {

bool RunScanJob(db::Database& db,
                const WorkerConfig& cfg,
                WorkerStats& stats,
                std::string& info,
                std::string& error) {
  db::ScanConfig scan_cfg;
  scan_cfg.defaults_json_path = cfg.defaults_json_path;
  scan_cfg.systems_dir = cfg.systems_dir;
  scan_cfg.override_roots = cfg.override_roots;
  scan_cfg.hide_missing = cfg.hide_missing;

  db::ScanStats scan_stats;
  if (!db::RunIncrementalScan(db, scan_cfg, scan_stats)) {
    error = "scan failed";
    if (!scan_stats.warnings.empty()) {
      error += ": " + scan_stats.warnings.front();
    }
    return false;
  }

  stats.scan_files_seen += scan_stats.files_seen;
  stats.scan_roots_ok += scan_stats.roots_ok;
  stats.scan_roots_unavailable += scan_stats.roots_unavailable;
  stats.scan_roots_error += scan_stats.roots_error;
  info = "scan ok files_seen=" + std::to_string(scan_stats.files_seen) +
         " upserted=" + std::to_string(scan_stats.games_upserted) +
         " roots_ok=" + std::to_string(scan_stats.roots_ok) +
         " roots_unavailable=" + std::to_string(scan_stats.roots_unavailable) +
         " roots_error=" + std::to_string(scan_stats.roots_error);
  return true;
}

bool RunIdentifyJob(db::Database& db,
                    const WorkerConfig& cfg,
                    WorkerStats& stats,
                    std::string& info,
                    std::string& error) {
  std::vector<db::MetadataCandidate> games;
  if (!db.ListGamesNeedingMetadata(games, cfg.metadata_batch)) {
    error = db.LastError();
    return false;
  }

  int updated = 0;
  for (const auto& game : games) {
    auto meta = providers::BuildHeuristicMetadata(game);
    if (meta.genre.empty()) {
      meta = providers::BuildNoneMetadata(game);
    }
    if (db.UpsertGameMetadata(meta)) {
      ++updated;
    }
  }

  stats.metadata_updates += updated;
  info = "identify ok updated=" + std::to_string(updated);
  return true;
}

std::vector<std::filesystem::path> ArtworkPathsFor(
    const db::AssetCandidate& game,
    const std::filesystem::path& artwork_dir) {
  const std::filesystem::path rom_path(game.rom_path);
  const std::string stem = rom_path.stem().string();
  std::vector<std::filesystem::path> paths;
  const auto add = [&](const std::filesystem::path& path) {
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
      paths.push_back(path);
    }
  };
  add(artwork_dir / game.system_id / (stem + ".png"));
  add(artwork_dir / game.system_id / (game.title + ".png"));
  add(rom_path.parent_path() / (stem + ".png"));
  return paths;
}

bool RunArtworkIndexJob(db::Database& db,
                        const WorkerConfig& cfg,
                        WorkerStats& stats,
                        std::string& info,
                        std::string& error) {
  std::vector<db::AssetCandidate> games;
  if (!db.ListPresentAssetCandidates(games)) {
    error = db.LastError();
    return false;
  }

  const std::filesystem::path artwork_dir = cfg.artwork_dir.empty()
                                                ? std::filesystem::path("./data/artwork")
                                                : std::filesystem::path(cfg.artwork_dir);
  int indexed = 0;
  int missing = 0;
  for (const auto& game : games) {
    std::filesystem::path found;
    for (const auto& candidate : ArtworkPathsFor(game, artwork_dir)) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
        found = candidate;
        break;
      }
    }
    if (found.empty()) {
      ++missing;
      continue;
    }
    if (!db.UpsertGameBoxArt(game.game_id, found.string())) {
      error = db.LastError();
      return false;
    }
    ++indexed;
  }
  stats.artwork_indexed += indexed;
  stats.artwork_missing += missing;
  info = "artwork index ok indexed=" + std::to_string(indexed) +
         " missing=" + std::to_string(missing);
  return true;
}

bool RunPlaceholderJob(const std::string& type, std::string& info) {
  info = type + " placeholder ok";
  return true;
}

}  // namespace

void EnqueueDefaultJobs(db::Database& db,
                        const bool include_scan,
                        const bool include_identify,
                        const bool include_scrape) {
  if (include_scan) {
    db.EnqueueJob("scan", "{}");
  }
  if (include_identify) {
    db.EnqueueJob("identify", "{}");
  }
  if (include_scrape) {
    db.EnqueueJob("scrape", "{}");
    db.EnqueueJob("download_art", "{}");
    db.EnqueueJob("build_thumb", "{}");
  }
}

bool ProcessOneQueuedJob(db::Database& db,
                         const WorkerConfig& cfg,
                         WorkerStats& stats,
                         std::string& info) {
  db::JobRecord job;
  if (!db.ClaimNextQueuedJob(job)) {
    return false;
  }

  ++stats.jobs_claimed;

  std::string step_info;
  std::string step_error;
  bool ok = false;

  if (job.type == "scan") {
    ok = RunScanJob(db, cfg, stats, step_info, step_error);
  } else if (job.type == "identify") {
    ok = RunIdentifyJob(db, cfg, stats, step_info, step_error);
  } else if (job.type == "build_thumb") {
    ok = RunArtworkIndexJob(db, cfg, stats, step_info, step_error);
  } else if (job.type == "scrape" || job.type == "download_art") {
    ok = RunPlaceholderJob(job.type, step_info);
  } else {
    step_error = "unknown job type: " + job.type;
  }

  if (ok) {
    db.MarkJobOk(job.id);
    ++stats.jobs_ok;
    info = "job#" + std::to_string(job.id) + " " + step_info;
    return true;
  }

  db.MarkJobError(job.id, step_error.empty() ? "job failed" : step_error);
  ++stats.jobs_error;
  info = "job#" + std::to_string(job.id) + " failed: " + step_error;
  return true;
}

void ProcessJobsUntilEmpty(db::Database& db,
                           const WorkerConfig& cfg,
                           WorkerStats& stats) {
  while (true) {
    std::string info;
    if (!ProcessOneQueuedJob(db, cfg, stats, info)) {
      break;
    }
    if (!info.empty()) {
      core::Log(core::LogLevel::Info, info);
    }
  }
}

}  // namespace gb::scrape
