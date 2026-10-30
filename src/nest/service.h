#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gb::nest {
namespace fs = std::filesystem;
struct Config {
  std::string api_base_url = "https://dbworker.suntank.workers.dev";
  std::string cdn_base_url = "https://cdn.gamebird.games";
  fs::path data_dir;
  fs::path games_dir;       // One package directory per slug, outside library scanning.
  fs::path launchers_dir;   // Library records (.gbgame), separate from game payloads.
  fs::path legacy_games_dir;
  fs::path legacy_data_dir;
  std::string device_id;    // Empty reads Raspberry Pi serial; never invents an ID.
  int timeout_seconds = 10;
  int max_retries = 2;
};
Config DefaultConfig();
Config LoadConfig(const fs::path& path, const fs::path& default_data_dir,
                  const fs::path& default_games_dir, const fs::path& default_launchers_dir);
struct Entry {
  std::string id, slug, title, description, version, icon_url;
  std::vector<std::string> screenshot_urls, tags;
  std::optional<double> rating;
  bool mature_content = false;
  std::uint64_t size_bytes = 0;
  std::string sha256, download_path, release_date;
};
struct DownloadInfo { std::string version, sha256, path; std::uint64_t size_bytes = 0; };
struct ChangelogEntry { std::string version, date; std::vector<std::string> notes; };
struct Manifest : Entry {
  DownloadInfo download;
  std::vector<ChangelogEntry> changelog;
  int rating_count = 0;
  std::string author, engine_version, min_firmware_version;
};
struct CatalogQuery {
  int page = 1, per_page = 25;
  std::vector<std::string> tags;
  std::optional<double> min_rating;
  bool include_mature = true;
  std::string sort_by = "title";
};
struct CatalogPage {
  std::vector<Entry> entries;
  int page = 1, per_page = 25, total = 0, total_pages = 1;
  bool offline = false;
};
struct Tag { std::string id, name, category; };
struct InstalledGame {
  std::string id, title, installed_version;
  fs::path install_path, launcher_path;
  std::vector<std::string> installed_files;
  int package_format_version = 0;
};
struct UpdateInfo { std::string game_id, title, installed_version, latest_version; };
struct Progress { std::string stage; std::uint64_t completed = 0, total = 0; };
// Return false to cancel an operation. Callback is only invoked on caller's thread.
using ProgressCallback = std::function<bool(const Progress&)>;
// Synchronous API, intended for a single background worker. Errors throw runtime_error.
// Local state writes are atomic; archive contents are verified/staged before installation.
class Service {
 public:
  explicit Service(Config config = DefaultConfig());
  const Config& GetConfig() const { return config_; }
  CatalogPage FetchCatalog(const CatalogQuery& query = {});
  std::vector<Entry> FetchAllCatalog();
  std::vector<Entry> CachedCatalog() const;
  std::vector<Tag> FetchTags();
  Manifest FetchManifest(const std::string& slug);
  fs::path CacheImage(const std::string& url);
  std::map<std::string, InstalledGame> LoadInstalled() const;
  std::vector<UpdateInfo> FindUpdates(const std::vector<Entry>& entries) const;
  void Install(const Manifest& manifest, const ProgressCallback& progress = {});
  void Uninstall(const std::string& game_id);
  bool RefreshLaunchers();
  fs::path LaunchPath(const InstalledGame& game) const;
  std::map<std::string, int> LoadRatings() const;
  std::map<std::string, int> SyncRatings();
  bool SetRating(const std::string& slug, int rating);
  bool RemoveRating(const std::string& slug);
  std::string RequestDeveloperCode();
  bool ParentalEnabled() const;
  bool ParentalLocked() const;
  bool SetPin(const std::string& pin);
  bool VerifyPin(const std::string& pin) const;
  bool Unlock(const std::string& pin);
  void Lock();
  bool RemovePin(const std::string& pin);
  static bool IsNewer(const std::string& latest, const std::string& current);
 private:
  Config config_;
  bool unlocked_ = false;
  void SaveInstalled(const std::map<std::string, InstalledGame>& games) const;
  void SaveRatings(const std::map<std::string, int>& ratings) const;
  void WriteLauncher(InstalledGame& game) const;
};
}  // namespace gb::nest
