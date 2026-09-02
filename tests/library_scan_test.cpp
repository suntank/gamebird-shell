#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "db/db.h"
#include "db/queries.h"

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
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("gamebird-library-scan-" + std::to_string(::getpid()) + "-" +
            std::to_string(nonce));
    std::filesystem::create_directories(path);
  }

  ~TempTree() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

void WriteText(const std::filesystem::path& path, const std::string& value) {
  std::ofstream out(path, std::ios::binary);
  out << value;
}

gb::db::ScanStats Scan(gb::db::Database& db,
                       const std::filesystem::path& root,
                       const std::filesystem::path& systems) {
  gb::db::ScanConfig config;
  config.systems_dir = systems.string();
  config.override_roots = {root.string()};
  gb::db::ScanStats stats;
  Expect(gb::db::RunIncrementalScan(db, config, stats),
         "incremental scan succeeds");
  return stats;
}

}  // namespace

int main() {
  TempTree temp;
  const auto systems = temp.path / "systems.d";
  const auto root = temp.path / "roms";
  const auto offline_root = temp.path / "roms.offline";
  const auto snes_dir = root / "snes";
  const auto rom = snes_dir / "Super Metroid.sfc";
  std::filesystem::create_directories(systems);
  std::filesystem::create_directories(snes_dir);
  WriteText(systems / "snes.json",
            R"({"id":"snes","name":"Super Nintendo","rom_extensions":[".sfc"],"launch_type":"retroarch","launch_template":"retroarch {rom_path}"})");
  WriteText(rom, "ROM");

  gb::db::Database db;
  Expect(db.Open((temp.path / "catalog.db").string()), "open catalog");
  Expect(db.InitSchema(), "initialize catalog schema");

  auto stats = Scan(db, root, systems);
  Expect(stats.roots_ok == 1 && stats.roots_unavailable == 0,
         "available root is healthy");
  Expect(stats.present_games == 1 && stats.missing_games == 0,
         "new ROM is present");
  Expect(stats.roots.size() == 1 && stats.roots[0].status == "ok",
         "healthy root state is reported");

  std::filesystem::rename(root, offline_root);
  stats = Scan(db, root, systems);
  Expect(stats.roots_ok == 0 && stats.roots_unavailable == 1,
         "missing root is storage-unavailable");
  Expect(stats.present_games == 1 && stats.missing_games == 0,
         "temporary storage loss preserves the game catalog");
  Expect(stats.roots.size() == 1 && stats.roots[0].status == "unavailable",
         "unavailable root state is persisted and reported");

  std::filesystem::rename(offline_root, root);
  std::filesystem::remove(rom);
  stats = Scan(db, root, systems);
  Expect(stats.roots_ok == 1 && stats.roots_unavailable == 0,
         "restored empty root is scanned successfully");
  Expect(stats.present_games == 0 && stats.missing_games == 1,
         "ROM deleted from available storage is marked missing");

  WriteText(rom, "ROM");
  stats = Scan(db, root, systems);
  Expect(stats.present_games == 1 && stats.missing_games == 0,
         "restored ROM becomes present again");

  std::vector<gb::db::LibraryRootState> roots;
  Expect(db.ListLibraryRoots(roots) && roots.size() == 1,
         "library root health can be listed for diagnostics");
  Expect(!roots.empty() && roots.front().status == "ok" &&
             roots.front().files_seen == 1,
         "final root health contains scan details");

  const auto empty_mount = temp.path / "empty-mount";
  const auto absent_rom = empty_mount / "snes" / "Old Game.sfc";
  std::filesystem::create_directories(empty_mount);
  const auto stable_empty_mount =
      std::filesystem::absolute(empty_mount).lexically_normal().string();
  Expect(db.UpsertGame(gb::db::GameRecord{
             .system_id = "snes",
             .library_root = stable_empty_mount,
             .path = absent_rom.string(),
             .filename = absent_rom.filename().string(),
             .title = "Old Game",
             .sort_title = "old game",
             .size_bytes = 3,
             .mtime = 1,
         }),
         "seed catalog history for an empty mount point");
  stats = Scan(db, empty_mount, systems);
  Expect(stats.roots_unavailable == 1 && stats.present_games == 2,
         "first empty mount observation preserves catalog history");
  Expect(!stats.roots.empty() && stats.roots.front().device_id == 0,
         "empty mount does not establish a trusted device baseline");
  stats = Scan(db, empty_mount, systems);
  Expect(stats.roots_unavailable == 1 && stats.present_games == 2,
         "repeated empty mount observations remain conservative");

  if (failures != 0) {
    std::cerr << failures << " library scan assertion(s) failed\n";
    return 1;
  }
  std::cout << "library scan tests passed\n";
  return 0;
}
