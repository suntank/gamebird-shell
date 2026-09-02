#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <unistd.h>

#include "db/db.h"
#include "scrape/jobs.h"

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct TempTree {
  std::filesystem::path path;

  TempTree() {
    path = std::filesystem::temp_directory_path() /
           ("gamebird-artwork-index-" + std::to_string(::getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }

  ~TempTree() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

}  // namespace

int main() {
  TempTree temp;
  const auto rom_dir = temp.path / "roms" / "snes";
  const auto rom_path = rom_dir / "Artwork Test.sfc";
  const auto artwork_path = temp.path / "artwork" / "snes" / "Artwork Test.png";
  std::filesystem::create_directories(rom_dir);
  std::filesystem::create_directories(artwork_path.parent_path());
  std::ofstream(rom_path) << "ROM";
  std::ofstream(artwork_path) << "PNG fixture";

  gb::db::Database db;
  Expect(db.Open((temp.path / "catalog.db").string()), "open catalog");
  Expect(db.InitSchema(), "initialize schema");
  Expect(db.UpsertSystem(gb::db::SystemRecord{
             .id = "snes",
             .name = "Super Nintendo",
             .rom_extensions = ".sfc",
             .launch_type = "retroarch",
             .launch_template = "retroarch {rom_path}",
         }),
         "insert system");
  Expect(db.UpsertGame(gb::db::GameRecord{
             .system_id = "snes",
             .library_root = (temp.path / "roms").string(),
             .path = rom_path.string(),
             .filename = rom_path.filename().string(),
             .title = "Artwork Test",
             .sort_title = "artwork test",
             .size_bytes = 3,
             .mtime = 1,
         }),
         "insert game");
  std::vector<gb::db::GameSummary> games;
  Expect(db.ListGamesBySystem("snes", false, games) && games.size() == 1,
         "list inserted game");

  Expect(db.EnqueueJob("build_thumb", "{}"), "enqueue artwork index job");
  gb::scrape::WorkerConfig config;
  config.artwork_dir = (temp.path / "artwork").string();
  gb::scrape::WorkerStats stats;
  std::string info;
  Expect(gb::scrape::ProcessOneQueuedJob(db, config, stats, info),
         "process artwork index job");
  Expect(stats.jobs_ok == 1 && stats.artwork_indexed == 1 && stats.artwork_missing == 0,
         "artwork job reports indexed cover");

  gb::db::GameDetails details;
  Expect(db.GetGameDetails(games.front().id, details), "load indexed game details");
  Expect(details.box_art_path == artwork_path.string(),
         "details expose indexed local artwork path");

  if (failures != 0) {
    std::cerr << failures << " artwork index assertion(s) failed\n";
    return 1;
  }
  std::cout << "artwork index tests passed\n";
  return 0;
}
