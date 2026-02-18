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
  std::error_code ec;
  const auto rel = std::filesystem::relative(file, root, ec);
  if (!ec && !rel.empty()) {
    const auto first = (*rel.begin()).string();
    if (FindSystemById(systems, first)) {
      return first;
    }
  }

  const auto root_name = root.filename().string();
  if (FindSystemById(systems, root_name)) {
    return root_name;
  }

  const auto ext = ToLower(file.extension().string());
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

  if (!db.BeginIncrementalScan()) {
    stats.warnings.push_back("failed to begin scan transaction");
    return false;
  }

  for (const auto& root_raw : roots) {
    const std::filesystem::path root = root_raw;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      stats.warnings.push_back("root does not exist: " + root.string());
      continue;
    }

    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
      if (ec) {
        continue;
      }

      const auto& entry = *it;
      if (!entry.is_regular_file(ec) || ec) {
        continue;
      }

      const auto file_path = entry.path();
      const std::string system_id =
          DetectSystemId(root, file_path, systems, extension_to_system);
      if (system_id.empty()) {
        continue;
      }

      ++stats.files_seen;

      std::error_code size_ec;
      const auto size = static_cast<std::int64_t>(entry.file_size(size_ec));
      if (size_ec) {
        stats.warnings.push_back("failed to read size: " + file_path.string());
        continue;
      }

      const std::string title = NormalizeTitle(file_path.stem().string());
      GameRecord game{};
      game.system_id = system_id;
      game.path = file_path.string();
      game.filename = file_path.filename().string();
      game.title = title.empty() ? game.filename : title;
      game.sort_title = ToLower(game.title);
      game.size_bytes = size;
      game.mtime = LastWriteSeconds(file_path);

      if (db.UpsertGame(game)) {
        ++stats.games_upserted;
      } else {
        stats.warnings.push_back("failed to upsert game: " + file_path.string());
      }
    }
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
