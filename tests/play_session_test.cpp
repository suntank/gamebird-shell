#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include "core/play_session.h"

namespace fs = std::filesystem;
int failures = 0;
void Expect(bool ok, const char* text) { if (!ok) { ++failures; std::cerr << text << '\n'; } }
std::string Read(const fs::path& p) { std::ifstream f(p); return {std::istreambuf_iterator<char>(f), {}}; }
void Write(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }
int main() {
  using namespace gb::core;
  const auto dir = fs::temp_directory_path() / ("gamebird-session-test-" + std::to_string(getpid()));
  fs::create_directories(dir);
  const auto db = (dir / "catalog.db").string();
  EffectiveLaunch launch;
  launch.info.game_id = 7;
  launch.info.launch_type = "retroarch";
  launch.info.rom_path = (dir / "game.sfc").string();
  launch.effective_core = (dir / "core.so").string();
  Write(launch.info.rom_path, "rom"); Write(launch.effective_core, "core");
  launch.argv = {"retroarch", "--appendconfig", "base.cfg|input.cfg", "-L", launch.effective_core,
                 launch.info.rom_path, "--appendconfig=extra.cfg"};
  const auto saves = ContinueDirectory(db, launch);
  std::string error, status;
  Expect(!HasContinueSave(db, launch), "new game has no resume option");
  {
    PlaySession session; auto argv = launch.argv;
    Expect(!session.Prepare(db, launch, PlayMode::Resume, argv, error), "missing resume is rejected");
  }
  {
    PlaySession session; auto argv = launch.argv;
    Expect(session.Prepare(db, launch, PlayMode::Fresh, argv, error), "prepare fresh session");
    const auto configs = ExtractAppendConfigs(argv);
    Expect(configs.size() == 4 && configs[0] == "base.cfg" && configs[1] == "input.cfg" && configs[2] == "extra.cfg", "all existing append configurations preserved");
    Expect(Read(configs.back()).find("savestate_auto_load = \"false\"") != std::string::npos, "fresh disables auto load");
    Write(session.AutoStatePath(), "save-one");
    Write(session.AutoStatePath().parent_path() / "game.state2", "manual-slot");
    Expect(session.Finish(true, status), "successful exit promotes new save");
  }
  Expect(HasContinueSave(db, launch), "saved game can resume");
  Expect(Read(saves / "current.auto") == "save-one", "current state content");
  {
    PlaySession session; auto argv = launch.argv;
    Expect(session.Prepare(db, launch, PlayMode::Resume, argv, error), "prepare resume");
    Expect(Read(session.AutoStatePath()) == "save-one", "resume seeds matching save");
    Expect(Read(session.AutoStatePath().parent_path() / "game.state2") == "manual-slot", "manual slots survive a new session");
    Expect(!session.Finish(true, status), "unchanged input is not falsely confirmed saved");
  }
  {
    PlaySession session; auto argv = launch.argv;
    Expect(session.Prepare(db, launch, PlayMode::Resume, argv, error), "resume second time");
    Write(session.AutoStatePath(), "save-two");
    Expect(session.Finish(true, status), "updated state committed");
  }
  Expect(Read(saves / "current.auto") == "save-two" && Read(saves / "previous.auto") == "save-one", "previous state retained on rotation");
  {
    PlaySession session; auto argv = launch.argv;
    Expect(session.Prepare(db, launch, PlayMode::Fresh, argv, error), "start fresh with existing saves");
    Expect(!fs::exists(session.AutoStatePath()), "fresh never seeds a resume state");
    Write(session.AutoStatePath(), "broken");
    Expect(!session.Finish(false, status), "crashed game cannot promote state");
  }
  Expect(Read(saves / "current.auto") == "save-two" && Read(saves / "previous.auto") == "save-one", "crash keeps both good saves");
  {
    PlaySession session; auto argv = launch.argv;
    Expect(session.Prepare(db, launch, PlayMode::Backup, argv, error), "backup is selectable");
    Expect(Read(session.AutoStatePath()) == "save-one", "backup resumes previous state");
    Write(session.AutoStatePath(), "");
    Expect(!session.Finish(true, status), "empty output is never promoted");
  }
  // Archive extraction may produce a different basename than the archive.
  auto archive = launch;
  archive.info.game_id = 8;
  archive.info.rom_path = (dir / "bundle.zip").string();
  Write(archive.info.rom_path, "archive");
  {
    PlaySession session; auto argv = archive.argv;
    Expect(session.Prepare(db, archive, PlayMode::Fresh, argv, error), "prepare archive session");
    Write(session.AutoStatePath().parent_path() / "Inner Game.state.auto", "archive-save");
    Expect(session.Finish(true, status), "discover actual archive state basename");
  }
  {
    PlaySession session; auto argv = archive.argv;
    Expect(session.Prepare(db, archive, PlayMode::Resume, argv, error), "resume archive session");
    Expect(session.AutoStatePath().filename() == "Inner Game.state.auto" &&
           Read(session.AutoStatePath()) == "archive-save", "archive resume uses emitted basename");
  }
  Write(launch.effective_core, "updated core");
  Expect(!HasContinueSave(db, launch), "changed core cannot load stale state");
  launch.info.launch_type = "exec";
  {
    PlaySession session; auto argv = launch.argv;
    Expect(session.Prepare(db, launch, PlayMode::Fresh, argv, error), "ordinary apps still launch");
    Expect(argv == launch.argv, "no RetroArch options injected into ordinary apps");
  }
  BrowseState browse;
  browse.screen = "games"; browse.view = "favorites"; browse.system_id = "system with \"quotes\"";
  browse.selected_games = {{"system:snes", 7}, {"favorites", 12}, {"recent", 10}};
  Expect(SaveBrowseState(db, browse, error), "save browsing state");
  BrowseState loaded;
  Expect(LoadBrowseState(db, loaded) && loaded == browse, "browsing positions round trip independently");
  Write(dir / "browse-state", "GAMEBIRD-BROWSE-1\n\"games\" \"recent\" \"snes\"\n\"recent\" invalid");
  Expect(!LoadBrowseState(db, loaded), "damaged browsing state fails safely");
  fs::remove_all(dir);
  return failures ? 1 : 0;
}
