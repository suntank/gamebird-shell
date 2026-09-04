#include "db/queries.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

#include <sys/stat.h>

#include "core/logging.h"

namespace gb::db {

namespace {

std::string ReadFileText(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

std::string ExtractString(const std::string& json, const std::string& key) {
  const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
  std::smatch match;
  if (std::regex_search(json, match, re) && match.size() > 1) {
    return match[1].str();
  }
  return {};
}

std::vector<std::string> ExtractStringArray(const std::string& json,
                                            const std::string& key) {
  const std::regex key_re("\\\"" + key +
                              "\\\"\\s*:\\s*\\[([\\s\\S]*?)\\]",
                          std::regex::icase);
  std::smatch match;
  if (!std::regex_search(json, match, key_re) || match.size() < 2) {
    return {};
  }

  std::vector<std::string> out;
  const std::string body = match[1].str();
  const std::regex item_re("\\\"([^\\\"]+)\\\"");
  auto begin = std::sregex_iterator(body.begin(), body.end(), item_re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    out.push_back((*it)[1].str());
  }
  return out;
}

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

std::string NormalizeTitle(const std::string& filename_stem) {
  std::string out;
  out.reserve(filename_stem.size());
  for (const char c : filename_stem) {
    if (c == '_' || c == '.' || c == '-') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }

  std::string collapsed;
  collapsed.reserve(out.size());
  bool prev_space = true;
  for (const char c : out) {
    const bool is_space = std::isspace(static_cast<unsigned char>(c)) != 0;
    if (is_space) {
      if (!prev_space) {
        collapsed.push_back(' ');
      }
      prev_space = true;
    } else {
      collapsed.push_back(c);
      prev_space = false;
    }
  }

  if (!collapsed.empty() && collapsed.back() == ' ') {
    collapsed.pop_back();
  }

  return collapsed;
}

std::int64_t LastWriteSeconds(const std::filesystem::path& path) {
  std::error_code ec;
  const auto ftime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return 0;
  }

  const auto system_tp =
      std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          ftime - std::filesystem::file_time_type::clock::now() +
          std::chrono::system_clock::now());
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(system_tp.time_since_epoch())
          .count());
}

std::int64_t NowUnixSeconds() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::string StableRootPath(const std::filesystem::path& root) {
  std::error_code ec;
  auto absolute = std::filesystem::absolute(root, ec);
  if (ec) {
    absolute = root;
  }
  return absolute.lexically_normal().string();
}

std::vector<std::string> LegacyRootPrefixes(
    const std::filesystem::path& raw_root,
    const std::string& stable_root) {
  std::vector<std::string> prefixes;
  const auto add = [&](const std::string& value) {
    if (!value.empty() &&
        std::find(prefixes.begin(), prefixes.end(), value) == prefixes.end()) {
      prefixes.push_back(value);
    }
  };
  add(raw_root.string());
  add(raw_root.lexically_normal().string());
  add(stable_root);
  return prefixes;
}

struct RootScanResult {
  std::filesystem::path raw_root;
  LibraryRootState state;
  std::vector<GameRecord> games;
  std::vector<std::string> legacy_prefixes;
  bool complete = false;
};

std::string JoinCsv(const std::vector<std::string>& values) {
  std::ostringstream oss;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      oss << ',';
    }
    oss << values[i];
  }
  return oss.str();
}

const SystemDefinition* FindSystemById(const std::vector<SystemDefinition>& systems,
                                       const std::string& id) {
  for (const auto& s : systems) {
    if (s.id == id) {
      return &s;
    }
  }
  return nullptr;
}

std::string DetectSystemId(const std::filesystem::path& root,
                           const std::filesystem::path& file,
                           const std::vector<SystemDefinition>& systems,
                           const std::unordered_map<std::string, std::string>&
                               extension_to_system) {
  const auto ext = ToLower(file.extension().string());
  const auto accepts_extension = [&](const SystemDefinition* system) {
    return system != nullptr &&
           std::find(system->rom_extensions.begin(),
                     system->rom_extensions.end(), ext) !=
               system->rom_extensions.end();
  };

  std::error_code ec;
  const auto rel = std::filesystem::relative(file, root, ec);
  if (!ec && !rel.empty()) {
    const auto first = (*rel.begin()).string();
    const auto* system = FindSystemById(systems, first);
    if (system != nullptr) {
      // A recognized folder selects the system, but it must not turn save
      // files, screenshots, or other sidecars into launchable games.
      return accepts_extension(system) ? first : std::string{};
    }
  }

  const auto root_name = root.filename().string();
  const auto* root_system = FindSystemById(systems, root_name);
  if (root_system != nullptr) {
    return accepts_extension(root_system) ? root_name : std::string{};
  }

  const auto it = extension_to_system.find(ext);
  if (it != extension_to_system.end()) {
    return it->second;
  }

  return {};
}

}  // namespace

bool LoadLibraryRootsFromDefaults(const std::string& path,
                                  std::vector<std::string>& roots,
                                  std::string& error) {
  roots.clear();
  const std::string json = ReadFileText(path);
  if (json.empty()) {
    error = "failed to read defaults file: " + path;
    return false;
  }

  roots = ExtractStringArray(json, "roots");
  if (roots.empty()) {
    error = "no library.roots found in: " + path;
    return false;
  }

  return true;
}

bool LoadSystemsFromDirectory(const std::string& directory,
                              std::vector<SystemDefinition>& systems,
                              std::string& error) {
  systems.clear();

  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      files.push_back(entry.path());
    }
  }

  if (files.empty()) {
    error = "no systems definitions found in: " + directory;
    return false;
  }

  std::sort(files.begin(), files.end());

  for (const auto& path : files) {
    const std::string json = ReadFileText(path.string());
    if (json.empty()) {
      continue;
    }

    SystemDefinition def;
    def.id = ExtractString(json, "id");
    def.name = ExtractString(json, "name");
    def.launch_type = ExtractString(json, "launch_type");
    def.launch_template = ExtractString(json, "launch_template");
    def.rom_extensions = ExtractStringArray(json, "rom_extensions");

    if (def.id.empty() || def.name.empty()) {
      continue;
    }

    for (auto& ext : def.rom_extensions) {
      ext = ToLower(ext);
    }

    systems.push_back(def);
  }

  if (systems.empty()) {
    error = "systems definitions could not be parsed from: " + directory;
    return false;
  }

  return true;
}

bool RunIncrementalScan(Database& db, const ScanConfig& config, ScanStats& stats) {
  stats = ScanStats{};

  std::vector<std::string> roots;
  if (!config.override_roots.empty()) {
    roots = config.override_roots;
  } else {
    std::string error;
    if (!LoadLibraryRootsFromDefaults(config.defaults_json_path, roots, error)) {
      stats.warnings.push_back(error);
      return false;
    }
  }

  std::vector<SystemDefinition> systems;
  {
    std::string error;
    if (!LoadSystemsFromDirectory(config.systems_dir, systems, error)) {
      stats.warnings.push_back(error);
      return false;
    }
  }

  std::unordered_map<std::string, std::string> extension_to_system;
  for (const auto& sys : systems) {
    if (!db.UpsertSystem(SystemRecord{.id = sys.id,
                                      .name = sys.name,
                                      .rom_extensions = JoinCsv(sys.rom_extensions),
                                      .launch_type = sys.launch_type,
                                      .launch_template = sys.launch_template})) {
      stats.warnings.push_back("failed to upsert system: " + sys.id);
      return false;
    }

    for (const auto& ext : sys.rom_extensions) {
      if (!ext.empty() && !extension_to_system.count(ext)) {
        extension_to_system.emplace(ext, sys.id);
      }
    }
  }

  stats.systems_loaded = static_cast<int>(systems.size());

  std::vector<RootScanResult> root_results;
  std::set<std::string> scanned_root_paths;
  for (const auto& root_raw : roots) {
    RootScanResult result;
    result.raw_root = std::filesystem::path(root_raw);
    result.state.root_path = StableRootPath(result.raw_root);
    result.state.last_scan_at = NowUnixSeconds();
    result.legacy_prefixes =
        LegacyRootPrefixes(result.raw_root, result.state.root_path);

    if (!scanned_root_paths.insert(result.state.root_path).second) {
      stats.warnings.push_back("duplicate library root ignored: " +
                               result.state.root_path);
      continue;
    }

    LibraryRootState previous;
    bool had_previous = false;
    if (!db.GetLibraryRootState(result.state.root_path, previous, had_previous)) {
      stats.warnings.push_back("failed to read library root state: " +
                               result.state.root_path);
      return false;
    }
    int existing_games = 0;
    if (!db.CountGamesUnderRoot(result.state.root_path, result.legacy_prefixes,
                                existing_games)) {
      stats.warnings.push_back("failed to inspect games for library root: " +
                               result.state.root_path);
      return false;
    }

    std::error_code ec;
    const auto root_status = std::filesystem::status(result.raw_root, ec);
    if (ec || !std::filesystem::exists(root_status) ||
        !std::filesystem::is_directory(root_status)) {
      result.state.status = "unavailable";
      result.state.error = ec ? ec.message() : "root does not exist or is not a directory";
      result.state.device_id = had_previous ? previous.device_id : 0;
      ++stats.roots_unavailable;
      stats.warnings.push_back("library storage unavailable: " +
                               result.state.root_path + " (" +
                               result.state.error + ")");
      root_results.push_back(std::move(result));
      continue;
    }

    struct stat stat_buf {};
    if (::stat(result.raw_root.c_str(), &stat_buf) != 0) {
      result.state.status = "unavailable";
      result.state.error = "failed to identify storage device";
      result.state.device_id = had_previous ? previous.device_id : 0;
      ++stats.roots_unavailable;
      stats.warnings.push_back("library storage unavailable: " +
                               result.state.root_path + " (" +
                               result.state.error + ")");
      root_results.push_back(std::move(result));
      continue;
    }
    result.state.device_id = static_cast<std::int64_t>(stat_buf.st_dev);

    int regular_files_seen = 0;
    std::filesystem::recursive_directory_iterator it(result.raw_root, ec), end;
    if (ec) {
      result.state.status = "error";
      result.state.error = "could not open root: " + ec.message();
    }

    while (result.state.status.empty() && it != end) {
      const auto entry = *it;
      std::error_code entry_ec;
      const bool is_regular = entry.is_regular_file(entry_ec);
      if (entry_ec) {
        result.state.status = "error";
        result.state.error = "could not inspect " + entry.path().string() +
                             ": " + entry_ec.message();
        break;
      }
      if (is_regular) {
        ++regular_files_seen;
      }

      if (is_regular) {
        const auto file_path = entry.path();
        const std::string system_id =
            DetectSystemId(result.raw_root, file_path, systems, extension_to_system);
        if (!system_id.empty()) {
          ++result.state.files_seen;

          std::error_code size_ec;
          const auto size = static_cast<std::int64_t>(entry.file_size(size_ec));
          if (size_ec) {
            result.state.status = "error";
            result.state.error = "failed to read size: " + file_path.string();
            break;
          }

          const std::string title = NormalizeTitle(file_path.stem().string());
          GameRecord game{};
          game.system_id = system_id;
          game.library_root = result.state.root_path;
          game.path = file_path.string();
          game.filename = file_path.filename().string();
          game.title = title.empty() ? game.filename : title;
          game.sort_title = ToLower(game.title);
          game.size_bytes = size;
          game.mtime = LastWriteSeconds(file_path);
          result.games.push_back(std::move(game));
        }
      }

      it.increment(ec);
      if (ec) {
        result.state.status = "error";
        result.state.error = "directory traversal failed: " + ec.message();
        break;
      }
    }

    if (result.state.status.empty() && regular_files_seen == 0) {
      if (had_previous && previous.device_id != 0 &&
          previous.device_id != result.state.device_id) {
        result.state.status = "unavailable";
        result.state.error =
            "storage device changed and the root is unexpectedly empty";
        result.state.device_id = previous.device_id;
      } else if (existing_games > 0 &&
                 (!had_previous || previous.device_id == 0 ||
                  (previous.status == "unavailable" &&
                   previous.error.find("unexpectedly empty; preserving catalog") !=
                       std::string::npos))) {
        result.state.status = "unavailable";
        result.state.error =
            "root is unexpectedly empty; preserving catalog until storage is confirmed";
        // An empty mount point cannot establish which storage device should be
        // trusted. Keep the baseline unknown until a non-empty scan succeeds.
        result.state.device_id = 0;
      }
    }

    if (result.state.status.empty()) {
      result.state.status = "ok";
      result.complete = true;
      ++stats.roots_ok;
    } else if (result.state.status == "unavailable") {
      ++stats.roots_unavailable;
      stats.warnings.push_back("library storage unavailable: " +
                               result.state.root_path + " (" +
                               result.state.error + ")");
    } else {
      ++stats.roots_error;
      stats.warnings.push_back("library scan incomplete: " +
                               result.state.root_path + " (" +
                               result.state.error + ")");
    }
    root_results.push_back(std::move(result));
  }

  if (!db.BeginIncrementalScan()) {
    stats.warnings.push_back("failed to begin scan transaction");
    return false;
  }

  for (const auto& result : root_results) {
    if (!db.UpsertLibraryRootState(result.state)) {
      stats.warnings.push_back("failed to save library root state: " +
                               result.state.root_path);
      db.AbortIncrementalScan();
      return false;
    }
    if (result.complete &&
        !db.MarkGamesMissingUnderRoot(result.state.root_path,
                                      result.legacy_prefixes)) {
      stats.warnings.push_back("failed to update games for root: " +
                               result.state.root_path);
      db.AbortIncrementalScan();
      return false;
    }
    for (const auto& game : result.games) {
      if (!db.UpsertGame(game)) {
        stats.warnings.push_back("failed to upsert game: " + game.path);
        db.AbortIncrementalScan();
        return false;
      }
      ++stats.games_upserted;
    }
    stats.files_seen += result.state.files_seen;
    stats.roots.push_back(result.state);
  }

  if (!db.EndIncrementalScan(config.hide_missing)) {
    stats.warnings.push_back("failed to commit scan transaction");
    return false;
  }

  const auto summary = db.ReadSummary();
  stats.total_games = summary.total_games;
  stats.present_games = summary.present_games;
  stats.missing_games = summary.missing_games;

  return true;
}

}  // namespace gb::db
