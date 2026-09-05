#include "core/play_session.h"

#include <cstdint>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace gb::core {
namespace {
namespace fs = std::filesystem;
fs::path DataDirectory(const std::string& db_path) {
  return fs::absolute(db_path).parent_path();
}
bool NonemptyFile(const fs::path& path) {
  std::error_code ec;
  return fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0 && !ec;
}
bool AtomicText(const fs::path& path, const std::string& text, std::string& error) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) { error = ec.message(); return false; }
  const auto tmp = path.string() + ".tmp-" + std::to_string(::getpid());
  std::ofstream out(tmp, std::ios::trunc);
  out << text;
  out.close();
  if (!out) { error = "Could not write " + tmp; fs::remove(tmp, ec); return false; }
  fs::rename(tmp, path, ec);
  if (ec) { error = ec.message(); fs::remove(tmp, ec); return false; }
  return true;
}
bool AtomicCopy(const fs::path& source, const fs::path& target, std::string& error) {
  const auto tmp = target.string() + ".tmp-" + std::to_string(::getpid());
  std::error_code ec;
  fs::copy_file(source, tmp, fs::copy_options::overwrite_existing, ec);
  if (!ec) fs::rename(tmp, target, ec);
  if (ec) { error = ec.message(); fs::remove(tmp, ec); return false; }
  return true;
}
bool ManualStateName(const std::string& name) {
  const auto pos = name.rfind(".state");
  if (pos == std::string::npos) return false;
  const auto suffix = name.substr(pos + 6);
  return std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
}
std::string Identity(const std::string& path) {
  std::error_code ec;
  const auto absolute = fs::weakly_canonical(fs::absolute(path), ec);
  std::string result = (ec ? fs::path(path) : absolute).string();
  const auto size = fs::file_size(path, ec);
  if (!ec) result += ":" + std::to_string(size);
  const auto time = fs::last_write_time(path, ec);
  if (!ec) result += ":" + std::to_string(time.time_since_epoch().count());
  return result;
}
}  // namespace

bool LoadBrowseState(const std::string& db_path, BrowseState& out) {
  out = {};
  std::ifstream in(DataDirectory(db_path) / "browse-state");
  std::string magic;
  BrowseState value;
  if (!(in >> magic) || magic != "GAMEBIRD-BROWSE-1" ||
      !(in >> std::quoted(value.screen) >> std::quoted(value.view) >>
        std::quoted(value.system_id))) return false;
  if ((value.screen != "systems" && value.screen != "games") ||
      (value.view != "system" && value.view != "recent" && value.view != "favorites")) return false;
  std::string key;
  int id;
  while (in >> std::quoted(key)) {
    if (!(in >> id) || id <= 0 || key.size() > 512 || value.selected_games.size() >= 1024) return false;
    value.selected_games[key] = id;
  }
  if (!in.eof()) return false;
  out = std::move(value);
  return true;
}
bool SaveBrowseState(const std::string& db_path, const BrowseState& state,
                     std::string& error) {
  std::ostringstream out;
  out << "GAMEBIRD-BROWSE-1\n" << std::quoted(state.screen) << ' '
      << std::quoted(state.view) << ' ' << std::quoted(state.system_id) << '\n';
  for (const auto& [key, id] : state.selected_games)
    out << std::quoted(key) << ' ' << id << '\n';
  return AtomicText(DataDirectory(db_path) / "browse-state", out.str(), error);
}

fs::path ContinueDirectory(const std::string& db_path, const EffectiveLaunch& launch) {
  // Stable across processes; changed ROMs/cores cannot silently load old states.
  const std::string identity = Identity(launch.info.rom_path) + "\n" + Identity(launch.effective_core);
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : identity) { hash ^= c; hash *= 1099511628211ULL; }
  std::ostringstream key;
  key << launch.info.game_id << '-' << std::hex << hash;
  return DataDirectory(db_path) / "continue" / key.str();
}
bool HasContinueSave(const std::string& db_path, const EffectiveLaunch& launch, bool backup) {
  return launch.info.launch_type == "retroarch" && !launch.effective_core.empty() &&
         NonemptyFile(ContinueDirectory(db_path, launch) / (backup ? "previous.auto" : "current.auto"));
}
fs::path PlayResultPath(const std::string& db_path, int game_id) {
  return DataDirectory(db_path) / "continue" / ("result-" + std::to_string(game_id));
}

void AppendRetroArchConfig(std::vector<std::string>& argv, const std::string& path) {
  std::vector<std::string> clean;
  std::string configs;
  auto append = [&](const std::string& config) {
    if (config.empty()) return;
    if (!configs.empty()) configs += '|';
    configs += config;
  };
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--appendconfig" && i + 1 < argv.size()) append(argv[++i]);
    else if (argv[i].starts_with("--appendconfig=")) append(argv[i].substr(15));
    else clean.push_back(argv[i]);
  }
  append(path);
  clean.push_back("--appendconfig");
  clean.push_back(configs);
  argv = std::move(clean);
}

PlaySession::~PlaySession() {
  if (!staging_.empty()) { std::error_code ec; fs::remove_all(staging_, ec); }
}
fs::path PlaySession::AutoStatePath() const { return staging_ / state_name_; }
bool PlaySession::Prepare(const std::string& db_path, const EffectiveLaunch& launch,
                          PlayMode mode, std::vector<std::string>& argv, std::string& error) {
  if (launch.info.launch_type != "retroarch") {
    if (mode != PlayMode::Fresh) { error = "This application cannot resume"; return false; }
    return true;
  }
  if (mode != PlayMode::Fresh && !HasContinueSave(db_path, launch, mode == PlayMode::Backup)) {
    error = "Resume save unavailable for this game/core";
    return false;
  }
  directory_ = ContinueDirectory(db_path, launch);
  std::error_code ec;
  fs::create_directories(directory_, ec);
  if (ec) { error = ec.message(); return false; }
  std::string pattern = (directory_ / "run-XXXXXX").string();
  if (!::mkdtemp(pattern.data())) { error = "Cannot create save staging directory"; return false; }
  staging_ = pattern;
  state_name_ = fs::path(launch.info.rom_path).stem().string() + ".state.auto";
  std::ifstream name_file(directory_ / "state-name");
  std::string saved_name;
  if (std::getline(name_file, saved_name)) {
    if (saved_name.empty() || fs::path(saved_name).filename() != saved_name ||
        !saved_name.ends_with(".state.auto")) { error = "Invalid resume filename"; return false; }
    state_name_ = saved_name;
  }
  const auto manual = directory_ / "manual";
  if (fs::is_directory(manual, ec)) {
    for (fs::directory_iterator it(manual, ec), end; !ec && it != end; it.increment(ec)) {
      if (ManualStateName(it->path().filename().string()) && NonemptyFile(it->path()))
        fs::copy_file(it->path(), staging_ / it->path().filename(), fs::copy_options::overwrite_existing, ec);
    }
    if (ec) { error = "Cannot restore manual save slots: " + ec.message(); return false; }
  }
  ec.clear();
  if (mode != PlayMode::Fresh) {
    fs::copy_file(directory_ / (mode == PlayMode::Backup ? "previous.auto" : "current.auto"), AutoStatePath(), ec);
    if (ec) { error = ec.message(); return false; }
    // Set a known old timestamp so an unchanged input is never reported as saved.
    fs::last_write_time(AutoStatePath(), fs::file_time_type::clock::now() - std::chrono::hours(24), ec);
    if (ec) { error = ec.message(); return false; }
    initial_write_ = fs::last_write_time(AutoStatePath(), ec);
    if (ec) { error = ec.message(); return false; }
    seeded_ = true;
  }
  std::string config = "savestate_auto_save = \"true\"\nsavestate_auto_load = \"";
  config += mode == PlayMode::Fresh ? "false" : "true";
  config += "\"\nsavestate_auto_index = \"false\"\nconfig_save_on_exit = \"false\"\n";
  // -S <file> is redirected by modern RetroArch's directory handling. Use an
  // explicit directory and retain the emitted basename (also handles archives).
  config += "sort_savestates_enable = \"false\"\nsort_savestates_by_content_enable = \"false\"\n";
  config += "savestates_in_content_dir = \"false\"\nauto_overrides_enable = \"false\"\n";
  std::ostringstream location;
  location << "savestate_directory = " << std::quoted(staging_.string()) << '\n';
  config += location.str();
  if (!AtomicText(staging_ / "session.cfg", config, error)) return false;
  std::vector<std::string> clean;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if ((argv[i] == "--savestate" || argv[i] == "-S") && i + 1 < argv.size()) ++i;
    else if (!argv[i].starts_with("--savestate=") &&
             !(argv[i].starts_with("-S") && argv[i].size() > 2)) clean.push_back(argv[i]);
  }
  argv = std::move(clean);
  AppendRetroArchConfig(argv, (staging_ / "session.cfg").string());
  return true;
}
bool PlaySession::Finish(bool clean_exit, std::string& status) {
  if (staging_.empty()) { status = clean_exit ? "Returned to menu" : "Game exited with an error"; return false; }
  if (!clean_exit) { status = "Game interrupted; previous save kept"; return false; }
  std::error_code ec;
  // Keep numbered quick-save slots made in the RetroArch menu across sessions.
  // These are independent of the current/previous automatic Continue states.
  for (fs::directory_iterator it(staging_, ec), end; !ec && it != end; it.increment(ec)) {
    if (!ManualStateName(it->path().filename().string()) || !NonemptyFile(it->path())) continue;
    fs::create_directories(directory_ / "manual", ec);
    std::string error;
    if (ec || !AtomicCopy(it->path(), directory_ / "manual" / it->path().filename(), error)) {
      status = "Manual save failed; previous kept";
      return false;
    }
  }
  if (ec) { status = "Cannot read saves; previous kept"; return false; }
  fs::path output;
  for (fs::directory_iterator it(staging_, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->path().filename().string().ends_with(".state.auto") || !NonemptyFile(it->path())) continue;
    if (seeded_ && it->path() == AutoStatePath() && fs::last_write_time(it->path(), ec) == initial_write_) continue;
    if (!output.empty()) { status = "Ambiguous save; previous kept"; return false; }
    output = it->path();
  }
  if (ec || output.empty()) {
    status = "No new resume save; previous kept";
    return false;
  }
  if (NonemptyFile(directory_ / "current.auto") && output.filename() != state_name_) {
    status = "Save name changed; previous kept";
    return false;
  }
  std::string error;
  const auto current = directory_ / "current.auto";
  if (NonemptyFile(current) && !AtomicCopy(current, directory_ / "previous.auto", error)) {
    status = "Save backup failed; previous kept";
    return false;
  }
  if (!AtomicText(directory_ / "state-name", output.filename().string() + "\n", error) ||
      !AtomicCopy(output, current, error)) {
    status = "Resume save failed; previous kept";
    return false;
  }
  status = "Progress saved - ready to resume";
  return true;
}
}  // namespace gb::core
