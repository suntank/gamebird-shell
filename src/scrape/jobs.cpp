#include "scrape/jobs.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/logging.h"
#include "db/queries.h"
#include "scrape/providers/provider_local_dat.h"
#include "scrape/providers/provider_libretro.h"
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

bool RunScrapeJob(db::Database& db,
                  const WorkerConfig& cfg,
                  WorkerStats& stats,
                  std::string& info,
                  std::string& error) {
  ScrapeSession session;
  ScrapeProgress progress;
  if (!session.Begin(db, cfg, progress)) {
    error = progress.last_error.empty() ? "could not prepare scrape" : progress.last_error;
    return false;
  }
  while (!progress.finished) {
    if (!session.ProcessNext(db, progress)) {
      error = progress.last_error.empty() ? "scrape failed" : progress.last_error;
      return false;
    }
  }

  stats.scrape_downloaded += progress.downloaded;
  stats.scrape_skipped += progress.skipped_existing;
  stats.scrape_matched += progress.matched;
  stats.artwork_indexed += progress.downloaded;
  stats.artwork_missing += progress.missing;
  info = "scrape ok downloaded=" + std::to_string(progress.downloaded) +
         " skipped=" + std::to_string(progress.skipped_existing) +
         " missing=" + std::to_string(progress.missing);
  return true;
}

bool RunDownloadCompatibilityJob(std::string& info) {
  info = "download_art already handled by scrape";
  return true;
}

}  // namespace

bool ScrapeSession::Begin(db::Database& db,
                          const WorkerConfig& cfg,
                          ScrapeProgress& progress) {
  cfg_ = cfg;
  games_.clear();
  next_game_ = 0;
  active_ = false;
  progress = ScrapeProgress{};

  if (cfg.provider != "libretro") {
    progress.last_error = cfg.provider == "none" ? "scraping is disabled"
                                                   : "unknown scraper: " + cfg.provider;
    return false;
  }

  std::vector<db::AssetCandidate> candidates;
  if (!db.ListPresentAssetCandidates(candidates)) {
    progress.last_error = db.LastError();
    return false;
  }

  for (const auto& game : candidates) {
    std::error_code ec;
    if (!cfg.overwrite_artwork && !game.box_art_path.empty() &&
        std::filesystem::is_regular_file(game.box_art_path, ec) && !ec) {
      ++progress.skipped_existing;
      continue;
    }
    games_.push_back(game);
  }

  progress.total = static_cast<int>(games_.size());
  progress.finished = games_.empty();
  active_ = !progress.finished;
  return true;
}

bool ScrapeSession::ProcessNext(db::Database& db, ScrapeProgress& progress) {
  if (progress.finished || !active_) {
    progress.finished = true;
    return true;
  }
  if (next_game_ >= games_.size()) {
    progress.finished = true;
    active_ = false;
    return true;
  }

  const auto& game = games_[next_game_++];
  progress.current_title = game.title;
  const std::filesystem::path rom_path(game.rom_path);
  const std::filesystem::path destination =
      std::filesystem::path(cfg_.artwork_dir) / game.system_id /
      (rom_path.stem().string() + ".png");
  const auto scraped = provider_.Scrape(game.system_id, game.title,
                                         destination.string());
  ++progress.completed;
  if (scraped.matched) ++progress.matched;

  if (!scraped.matched || !scraped.downloaded) {
    ++progress.missing;
    if (!scraped.error.empty()) progress.last_error = scraped.error;
  } else {
    if (!db.UpsertGameBoxArt(game.game_id, scraped.artwork_path)) {
      progress.last_error = db.LastError();
      active_ = false;
      return false;
    }
    ++progress.downloaded;

    db::GameDetails existing;
    db.GetGameDetails(game.game_id, existing);
    db::MetadataUpdate metadata;
    metadata.game_id = game.game_id;
    metadata.release_year = scraped.metadata.release_year > 0
                                ? scraped.metadata.release_year
                                : existing.release_year;
    metadata.publisher = scraped.metadata.publisher;
    metadata.developer = scraped.metadata.developer;
    metadata.genre = scraped.metadata.genre.empty() ? existing.genre
                                                     : scraped.metadata.genre;
    metadata.players = scraped.metadata.players > 0 ? scraped.metadata.players
                                                     : existing.players;
    metadata.description = existing.description;
    metadata.source = "libretro";
    metadata.source_id = scraped.matched_title;
    if (!db.UpsertGameMetadata(metadata)) {
      progress.last_error = db.LastError();
      active_ = false;
      return false;
    }
  }

  if (next_game_ >= games_.size()) {
    progress.finished = true;
    active_ = false;
  }
  return true;
}

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
  } else if (job.type == "scrape") {
    ok = RunScrapeJob(db, cfg, stats, step_info, step_error);
  } else if (job.type == "download_art") {
    ok = RunDownloadCompatibilityJob(step_info);
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
