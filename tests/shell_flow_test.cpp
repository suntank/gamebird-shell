// Exercise the real shell router with an isolated catalog, without SDL or GPIO.
#define main gamebird_shell_program_main
#include "../src/main.cpp"
#undef main
#include <iostream>
#include <unistd.h>

namespace {
int failures = 0;
void Expect(bool condition, const char* message) {
  if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void SavePreview(const std::filesystem::path& dir, const char* name,
                 gb::ui::UIState& ui, LibraryState& lib) {
  if (dir.empty()) return;
  std::filesystem::create_directories(dir);
  gb::render::Surface240 surface;
  DrawScreen(surface, ui, gb::render::DefaultTheme(), gb::core::NowMs(), lib);
  std::ofstream out(dir / (std::string(name) + ".ppm"), std::ios::binary);
  out << "P6\n240 240\n255\n";
  for (int i = 0; i < 240 * 240; ++i) {
    const auto p = surface.Pixels()[i];
    const char pixel[] = {static_cast<char>(((p >> 11) & 31) * 255 / 31),
                          static_cast<char>(((p >> 5) & 63) * 255 / 63),
                          static_cast<char>((p & 31) * 255 / 31)};
    out.write(pixel, 3);
  }
}
}

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  using gb::ui::Screen;
  using gb::platform::Button;
  const fs::path dir = fs::temp_directory_path() / ("gamebird-flow-" + std::to_string(getpid()));
  fs::create_directories(dir);
  const fs::path previews = argc > 1 ? argv[1] : "";
  Args args;
  args.db_path = (dir / "catalog.db").string();
  args.settings_path = (dir / "settings.json").string();
  args.systems_dir = (dir / "systems.d").string();
  const auto core = dir / "snes9x_libretro.so";
  std::ofstream(core) << "core";
  gb::db::Database db;
  Expect(db.Open(args.db_path) && db.InitSchema(), "catalog initialized");
  const std::string command = "/bin/true -L " + core.string() + " {rom_path}";
  Expect(db.UpsertSystem({"snes", "Super Nintendo", ".sfc", "retroarch", command}), "system inserted");
  Expect(db.UpsertSystem({"gb", "Game Boy", ".gb", "retroarch", command}), "second system inserted");
  for (const auto& title : {"Chrono Trigger", "Super Metroid", "Zelda"}) {
    const auto rom = dir / (std::string(title) + ".sfc");
    std::ofstream(rom) << "rom";
    Expect(db.UpsertGame({.system_id="snes", .library_root=dir.string(), .path=rom.string(),
        .filename=rom.filename().string(), .title=title, .sort_title=title, .size_bytes=3}), "game inserted");
  }
  LibraryState lib;
  lib.db_ready = true;
  lib.settings.show_diagnostics = false;
  LoadSystems(db, lib);
  const auto snes = std::find_if(lib.systems.begin(), lib.systems.end(), [](const auto& system) { return system.id == "snes"; });
  SelectSystem(db, lib, static_cast<int>(snes - lib.systems.begin()));
  gb::ui::UIState ui;
  ui.show_diagnostics = false;
  RefreshContinue(db, lib, ui);
  Expect(ui.screen == Screen::Home && !ui.continue_available, "first boot shows useful empty Home");
  ui.home_selected = 1;
  SavePreview(previews, "01-first-boot", ui, lib);
  auto press = [&](Button button) { HandleButton(button, ui, lib, db, args); RememberBrowse(lib, ui); };
  ui.home_selected = 1; press(Button::A);
  Expect(ui.screen == Screen::Systems, "A opens browse");
  press(Button::Start);
  Expect(ui.screen == Screen::SystemMenu, "Start opens system menu");
  SavePreview(previews, "02-system-menu", ui, lib);
  press(Button::B); press(Button::A); press(Button::Down);
  Expect(ui.screen == Screen::GameList && lib.games[lib.game_selected].title == "Super Metroid", "browse selected game");
  const int metroid = lib.games[lib.game_selected].id;
  SavePreview(previews, "03-game-browser", ui, lib);
  press(Button::A);
  Expect(ui.screen == Screen::GameMenu && lib.pending_launch_game_id == 0, "A opens game menu without launching");
  Expect(GameActions(lib).front().first == GameAction::Fresh, "no nonexistent Resume action");
  SavePreview(previews, "04-new-game-menu", ui, lib);
  auto choose = [&](GameAction action) {
    const auto actions = GameActions(lib);
    for (std::size_t i = 0; i < actions.size(); ++i)
      if (actions[i].first == action) { lib.context_selected = static_cast<int>(i); press(Button::A); return; }
    Expect(false, "requested action exists");
  };
  choose(GameAction::Favorite);
  Expect(lib.menu_game.is_favorite, "favorite action is in menu");
  choose(GameAction::Details);
  Expect(ui.screen == Screen::Details, "details opened");
  SavePreview(previews, "05-details", ui, lib);
  press(Button::B);
  Expect(ui.screen == Screen::GameMenu, "B returns from details to menu");
  choose(GameAction::Options);
  Expect(ui.screen == Screen::LaunchOptions && lib.launch_options_game_id == metroid, "options use contextual game's identity");
  press(Button::B); press(Button::B);
  Expect(ui.screen == Screen::GameList && lib.games[lib.game_selected].id == metroid, "back restores exact game");
  press(Button::B); press(Button::A);
  Expect(lib.games[lib.game_selected].id == metroid, "reopening console preserves game");
  press(Button::Right); press(Button::Left);
  Expect(lib.games[lib.game_selected].id == metroid, "switching consoles preserves per-system game");
  // Insert a title before the selected item to exercise ID-based restoration.
  const auto added = dir / "A new game.sfc"; std::ofstream(added) << "rom";
  db.UpsertGame({.system_id="snes", .library_root=dir.string(), .path=added.string(),
      .filename=added.filename().string(), .title="A new game", .sort_title="A new game", .size_bytes=3});
  ReloadGameList(db, lib);
  Expect(lib.games[lib.game_selected].id == metroid, "catalog rescan does not jump selection");
  gb::core::EffectiveLaunch effective; std::string error;
  Expect(gb::core::ResolveEffectiveLaunch(db, metroid, effective, error), "resolve game");
  const auto save_dir = gb::core::ContinueDirectory(args.db_path, effective);
  fs::create_directories(save_dir);
  std::ofstream(save_dir / "current.auto") << "save";
  std::ofstream(save_dir / "previous.auto") << "previous";
  db.UpdateLastPlayed(metroid, 123456789);
  ui.screen = Screen::Home; ui.home_selected = 0;
  RefreshContinue(db, lib, ui);
  Expect(ui.continue_available && ui.continue_title == "Super Metroid", "last game featured");
  SavePreview(previews, "06-continue-home", ui, lib);
  press(Button::A);
  Expect(lib.can_resume && lib.can_resume_backup, "current and previous saves offered");
  SavePreview(previews, "07-resume-menu", ui, lib);
  press(Button::A);
  Expect(lib.pending_launch_game_id == metroid && lib.pending_play_mode == gb::core::PlayMode::Resume, "Continue takes two A presses to resume");
  lib.pending_launch_game_id = 0;
  choose(GameAction::Fresh);
  Expect(lib.pending_play_mode == gb::core::PlayMode::Fresh && fs::exists(save_dir / "current.auto"), "start fresh leaves save untouched before successful exit");
  lib.pending_launch_game_id = 0;
  choose(GameAction::Backup);
  Expect(lib.pending_play_mode == gb::core::PlayMode::Backup, "previous save intent passed to launcher");
  lib.pending_launch_game_id = 0;
  choose(GameAction::Hide);
  Expect(ui.screen == Screen::Home && !ui.continue_available, "hidden game removed from Continue");
  std::string save_error;
  Expect(gb::core::SaveBrowseState(args.db_path, lib.browse, save_error), "persist positions");
  gb::core::BrowseState restored;
  Expect(gb::core::LoadBrowseState(args.db_path, restored) && restored == lib.browse, "positions survive restart");
  db.Close(); fs::remove_all(dir);
  return failures ? 1 : 0;
}
