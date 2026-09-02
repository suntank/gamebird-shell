#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

#include "db/db.h"

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
           ("gamebird-library-views-" + std::to_string(::getpid()) + "-" +
            std::to_string(nonce));
    std::filesystem::create_directories(path);
  }

  ~TempTree() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

int FindGameId(const std::vector<gb::db::GameSummary>& games,
               const std::string& title) {
  for (const auto& game : games) {
    if (game.title == title) {
      return game.id;
    }
  }
  return 0;
}

}  // namespace

int main() {
  TempTree temp;
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

  for (const std::string title : {"Alpha", "Bravo", "Charlie"}) {
    Expect(db.UpsertGame(gb::db::GameRecord{
               .system_id = "snes",
               .library_root = temp.path.string(),
               .path = (temp.path / (title + ".sfc")).string(),
               .filename = title + ".sfc",
               .title = title,
               .sort_title = title,
               .size_bytes = 1,
               .mtime = 1,
           }),
           "insert game " + title);
  }

  std::vector<gb::db::GameSummary> all_games;
  Expect(db.ListGamesBySystem("snes", false, all_games) && all_games.size() == 3,
         "system list contains games");
  const int alpha = FindGameId(all_games, "Alpha");
  const int bravo = FindGameId(all_games, "Bravo");
  const int charlie = FindGameId(all_games, "Charlie");
  Expect(alpha > 0 && bravo > 0 && charlie > 0, "resolve game ids");

  bool favorite = false;
  Expect(db.ToggleGameFavorite(bravo, favorite) && favorite,
         "mark Bravo favorite");
  Expect(db.ToggleGameFavorite(charlie, favorite) && favorite,
         "mark Charlie favorite");
  bool hidden = false;
  Expect(db.ToggleGameHidden(charlie, hidden) && hidden,
         "hide Charlie");
  Expect(db.UpdateLastPlayed(alpha, 100), "set Alpha played time");
  Expect(db.UpdateLastPlayed(bravo, 200), "set Bravo played time");
  Expect(db.UpdateLastPlayed(charlie, 300), "set Charlie played time");
  Expect(db.UpsertGameMetadata(gb::db::MetadataUpdate{
             .game_id = bravo,
             .release_year = 1994,
             .genre = "Action",
             .players = 1,
             .description = "Test metadata",
             .source = "local_heuristic",
         }),
         "save metadata");

  std::vector<gb::db::GameSummary> recent;
  Expect(db.ListRecentGames(false, 30, recent) && recent.size() == 2,
         "recent excludes hidden games");
  Expect(recent.size() == 2 && recent.front().title == "Bravo" &&
             recent.back().title == "Alpha",
         "recent uses most-recent-first order");
  Expect(db.ListRecentGames(true, 30, recent) && recent.size() == 3 &&
             recent.front().title == "Charlie",
         "recent can include hidden games");

  std::vector<gb::db::GameSummary> favorites;
  Expect(db.ListFavoriteGames(false, favorites) && favorites.size() == 1 &&
             favorites.front().title == "Bravo",
         "favorites excludes hidden games");
  Expect(db.ListFavoriteGames(true, favorites) && favorites.size() == 2,
         "favorites can include hidden games");

  gb::db::GameDetails details;
  Expect(db.GetGameDetails(bravo, details), "load game details");
  Expect(details.title == "Bravo" && details.system_name == "Super Nintendo" &&
             details.is_favorite && details.release_year == 1994 &&
             details.genre == "Action" && details.metadata_source == "local_heuristic",
         "details joins game, system, and metadata");

  if (failures != 0) {
    std::cerr << failures << " library view assertion(s) failed\n";
    return 1;
  }
  std::cout << "library view tests passed\n";
  return 0;
}
