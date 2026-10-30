// Exercise the real shell router with an isolated catalog, without SDL or GPIO.
#define main gamebird_shell_program_main
#include "../src/main.cpp"
#undef main
#include <iostream>
#include <thread>
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
  lib.system_artwork_dir = (dir / "system-art").string();
  auto cards = BuildSystemCards(lib);
  Expect(cards.size() == lib.systems.size(), "bundled art does not add systems");
  for (std::size_t i = 0; i < lib.systems.size(); ++i) {
    if (lib.systems[i].id != "snes") continue;
    Expect(fs::is_regular_file(cards[i].icon_path) && fs::is_regular_file(cards[i].logo_path),
           "supported system gets bundled console and logo without data artwork");
    fs::create_directories(lib.system_artwork_dir);
    const auto custom = fs::path(lib.system_artwork_dir) / "snes-logo.png";
    fs::copy_file(cards[i].logo_path, custom);
    cards = BuildSystemCards(lib);
    Expect(cards[i].logo_path == custom.string() && fs::is_regular_file(cards[i].icon_path),
           "custom logo takes priority while console keeps its bundled default");
  }
  const auto snes = std::find_if(lib.systems.begin(), lib.systems.end(), [](const auto& system) { return system.id == "snes"; });
  SelectSystem(db, lib, static_cast<int>(snes - lib.systems.begin()));
  gb::ui::UIState ui;
  ui.show_diagnostics = false;
  RefreshContinue(db, lib, ui);
  Expect(ui.screen == Screen::Home && !ui.continue_available, "first boot shows useful empty Home");
  ui.home_selected = gb::ui::HomeIndex(gb::ui::HomeItem::Browse);
  SavePreview(previews, "01-first-boot", ui, lib);
  auto press = [&](Button button) { HandleButton(button, ui, lib, db, args); RememberBrowse(lib, ui); };
  ui.home_selected = gb::ui::HomeIndex(gb::ui::HomeItem::Browse); press(Button::A);
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
  ui.screen = Screen::Home; ui.home_selected = gb::ui::HomeIndex(gb::ui::HomeItem::Continue);
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

  // Opening the first Start-menu entry imports native launchers and refreshes
  // the real catalog, while B returns to the shell without a launch or reboot.
  const auto apps = dir / "apps";
  const auto nest = dir / "nest";
  fs::create_directories(apps);
  fs::create_directories(nest / "games");
  fs::create_directories(args.systems_dir);
  args.defaults_json = (dir / "defaults.json").string();
  args.nest_config = (dir / "nest-config.json").string();
  std::ofstream(args.defaults_json) << "{\"library\":{\"roots\":[\"" << apps.string() << "\"]}}";
  std::ofstream(args.nest_config) << "{\"api_base_url\":\"file://" << dir.string()
      << "\",\"cdn_base_url\":\"file://" << dir.string()
      << "\",\"legacy_data_dir\":\"" << (dir / "legacy-store").string() << "\"}";
  std::ofstream(fs::path(args.systems_dir) / "apps.json") << R"({"id":"apps","name":"GAMEBIRD","rom_extensions":[".gbgame"],"launch_type":"exec","launch_template":"{rom_path}"})";
  fs::create_directories(nest / "games/test-game");
  std::ofstream(nest / "games/test-game/game.json") << R"({"format_version":1,"runtime":"python3","entrypoint":"main.py"})";
  std::ofstream(nest / "games/test-game/main.py") << "print('fixture')\n";
  std::ofstream(nest / "installed_games.json") << "{\"games\":[{\"id\":\"test-game\",\"title\":\"Test Game!\","
      << "\"installed_version\":\"1.0.0\",\"package_format_version\":1,\"install_path\":\"" << (nest / "games/test-game").string()
      << "\",\"installed_files\":[\"game.json\",\"main.py\"]}]}";
  ui.screen = Screen::Home;
  ui.home_selected = gb::ui::HomeIndex(gb::ui::HomeItem::Nest);
  SavePreview(previews, "08-nest-start-menu", ui, lib);
  press(Button::A);
  Expect(ui.screen == Screen::Nest && lib.nest, "top Start entry opens integrated Nest");
  for (int i = 0; lib.nest && lib.nest->Busy() && i < 2000; ++i) {
    lib.nest->Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(lib.nest && !lib.nest->Busy(), "Nest initializes without network access");
  if (lib.nest && lib.nest->TakeLibraryChanged()) RefreshNestLibrary(db, lib, args);
  std::vector<gb::db::GameSummary> nest_games;
  db.ListGamesBySystem("apps", false, nest_games);
  Expect(nest_games.size() == 1 && nest_games.front().title == "Test Game!", "Nest launcher enters shell catalog with publisher title");
  SavePreview(previews, "09-integrated-nest", ui, lib);
  press(Button::Down); press(Button::A);  // Installed games.
  for (int i = 0; lib.nest && lib.nest->Busy() && i < 2000; ++i) {
    PollNest(db, lib, ui, args);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  press(Button::X); PollNest(db, lib, ui, args);
  Expect(!nest_games.empty() && lib.pending_launch_game_id == nest_games.front().id &&
      lib.pending_play_mode == gb::core::PlayMode::Fresh, "Nest Play enters normal shell launch pipeline");
  lib.pending_launch_game_id = 0;
  press(Button::B);  // Installed games -> Nest home.
  press(Button::B);
  Expect(ui.screen == Screen::Home && ui.running && lib.pending_launch_game_id == 0, "B returns from Nest to shell without launching or exiting");
  lib.nest.reset();
  db.Close(); fs::remove_all(dir);
  return failures ? 1 : 0;
}
