// Exercise the native controller's screen models without network access.
#include "../src/ui/screens/nest.cpp"

#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <png.h>

namespace gb::ui::screens {
struct NestControllerTestAccess {
  using Result = NestController::Impl::Result;
  static NestController::Impl& State(NestController& controller) { return *controller.impl_; }
};
}

namespace {
int failures = 0;
void Expect(const bool condition, const char* message) {
  if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void Save(const std::filesystem::path& dir, const std::string& name,
          const gb::render::Surface240& surface) {
  if (dir.empty()) return;
  std::filesystem::create_directories(dir);
  std::ofstream out(dir / (name + ".ppm"), std::ios::binary);
  out << "P6\n240 240\n255\n";
  for (int i = 0; i < 240 * 240; ++i) {
    const auto p = surface.Pixels()[i];
    const char pixel[] = {static_cast<char>(((p >> 11) & 31) * 255 / 31),
                          static_cast<char>(((p >> 5) & 63) * 255 / 63),
                          static_cast<char>((p & 31) * 255 / 31)};
    out.write(pixel, 3);
  }
}
void Wait(gb::ui::screens::NestController& controller) {
  for (int i = 0; i < 2000 && controller.Busy(); ++i) {
    controller.Tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Expect(!controller.Busy(), "local background task completes");
}
}

int main(int argc, char** argv) {
  using gb::platform::Button;
  using gb::ui::screens::NestController;
  using gb::ui::screens::NestControllerTestAccess;
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / ("gamebird-nest-ui-" + std::to_string(getpid()));
  const fs::path previews = argc > 1 ? argv[1] : "";
  fs::create_directories(dir);
  auto config = gb::nest::DefaultConfig();
  config.data_dir = dir / "state";
  config.games_dir = dir / "games";
  config.launchers_dir = dir / "apps";
  config.legacy_data_dir = dir / "no-legacy-state";
  config.legacy_games_dir = dir / "no-legacy-games";
  config.device_id = "ui-test";
  config.api_base_url = "file://" + (dir / "offline-api").string();
  config.cdn_base_url = "file://" + (dir / "offline-cdn").string();
  NestController controller(config);
  controller.Open(); Wait(controller);
  auto& s = NestControllerTestAccess::State(controller);
  using View = decltype(s.view);
  using Work = decltype(s.working);
  const auto theme = gb::render::DefaultTheme();
  gb::render::Surface240 surface;
  gb::render::Surface240 backdrop;
  gb::render::DrawThemeBackground(backdrop, theme);
  auto draw = [&](const std::string& name) {
    controller.Draw(surface, theme);
    bool margins = true;
    for (int y = 0; y < 240; ++y)
      for (const int x : {0, 1, 238, 239})
        margins &= surface.Pixels()[y * 240 + x] == backdrop.Pixels()[y * 240 + x];
    Expect(margins, "screen rendering stays inside shell margins");
    Save(previews, name, surface);
  };
  draw("01-nest-home");
  Expect(surface.Pixels()[70 * 240 + 16] == theme.accent, "catalog is the first highlighted Nest action");
  controller.HandleButton(Button::Up);
  Expect(s.home_selected == 5, "home navigation wraps");
  controller.HandleButton(Button::Down);
  Expect(s.home_selected == 0, "home returns to browse action");

  gb::nest::Entry game;
  game.id = game.slug = "sky-garden";
  game.title = "Sky Garden";
  game.version = "1.2.0";
  game.description = "Explore floating gardens and bring their colors back to life. A relaxing adventure made for your Game Bird. Find secret paths and help the birds build a new home.";
  game.icon_url = "fixture-icon";
  game.screenshot_urls = {"fixture-screenshot"};
  game.tags = {"Adventure", "Single player"}; game.rating = 4.7;
  // Cache a small piece of fixture artwork, exercising the real PNG renderer.
  png_image image{}; image.version = PNG_IMAGE_VERSION;
  image.width = image.height = 64; image.format = PNG_FORMAT_RGB;
  std::vector<unsigned char> pixels(64 * 64 * 3);
  for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
    const int i = (y * 64 + x) * 3;
    pixels[i] = static_cast<unsigned char>(45 + y);
    pixels[i + 1] = static_cast<unsigned char>(95 + x * 2);
    pixels[i + 2] = static_cast<unsigned char>(130 + y);
  }
  Expect(png_image_write_to_file(&image, (dir / "art.png").c_str(), 0, pixels.data(), 0, nullptr), "write UI artwork fixture");
  s.image_paths[game.icon_url] = (dir / "art.png").string();
  s.image_paths[game.screenshot_urls.front()] = (dir / "art.png").string();
  s.catalog.entries = {game, game, game};
  s.catalog.entries[1].title = "Little Robot"; s.catalog.entries[2].title = "Nest Racer";
  s.catalog.total = 45; s.catalog.total_pages = 2;
  s.cached = s.catalog.entries;
  s.view = View::Catalog;
  draw("02-nest-catalog");
  Expect(surface.Pixels()[155 * 240 + 16] == theme.accent, "catalog highlights selected game");
  bool footer_gap_clear = true;
  for (int x = 16; x < 224; ++x)
    footer_gap_clear &= surface.Pixels()[220 * 240 + x] == backdrop.Pixels()[220 * 240 + x];
  Expect(footer_gap_clear, "two-line controls do not overlap a previously drawn footer");
  controller.HandleButton(Button::Right);
  Expect(s.screenshot == 1, "right opens a game screenshot");
  controller.HandleButton(Button::B);
  Expect(s.view == View::Home, "B returns catalog to Nest home");
  s.view = View::Catalog; s.catalog.offline = true;
  draw("03-nest-offline-catalog");

  gb::nest::Manifest manifest;
  static_cast<gb::nest::Entry&>(manifest) = game;
  manifest.author = "Game Bird Studio"; manifest.release_date = "2026-10-10";
  manifest.rating_count = 24;
  manifest.download.version = game.version; manifest.download.size_bytes = 12451840;
  manifest.changelog = {{"1.2.0", "2026-10-10", {"New sky islands", "Improved controls"}}};
  s.manifest = manifest; s.view = View::Detail; s.screenshot = 0;
  draw("04-nest-details");
  for (int i = 0; i < 50; ++i) controller.HandleButton(Button::Down);
  Expect(s.scroll == static_cast<int>(s.DetailLines().size()) - 5, "detail scrolling includes full description and changelog");
  draw("05-nest-details-scrolled");
  s.scroll = 0;

  gb::nest::InstalledGame installed;
  installed.id = game.slug; installed.title = game.title;
  installed.installed_version = game.version;
  installed.install_path = config.games_dir / game.slug; installed.launcher_path = config.launchers_dir / "nest-sky-garden.gbgame";
  installed.package_format_version = 1;
  s.installed = {{game.slug, installed}}; s.ratings = {{game.slug, 4}};
  s.view = View::Installed;
  draw("06-nest-installed");
  controller.HandleButton(Button::Y);
  Expect(s.view == View::Rating && s.pending_rating == 4, "installed games offer saved rating");
  controller.HandleButton(Button::Right);
  controller.HandleButton(Button::Right);
  Expect(s.pending_rating == 5, "rating clamps at five stars");
  draw("07-nest-rating");
  controller.HandleButton(Button::B);
  Expect(s.view == View::Installed, "cancelling rating restores installed list");

  s.view = View::Updates;
  s.updates = {{game.slug, game.title, "1.1.0", "1.2.0"}};
  draw("08-nest-updates");
  s.view = View::Filters;
  s.tags = {{"single", "Single player", "players"}, {"adventure", "Adventure", "genre"}, {"pixel", "Pixel art", "style"}};
  s.filter_selected = 2;
  controller.HandleButton(Button::A);
  Expect(s.draft_rating == 4, "four star rating filter toggles");
  s.filter_selected = 5; controller.HandleButton(Button::A);
  Expect(s.draft_tags == std::vector<std::string>{"adventure"}, "tag filters toggle by ID");
  draw("09-nest-filters");
  controller.HandleButton(Button::X);
  Expect(!s.draft_rating && s.draft_tags.empty(), "X clears all filters");

  s.view = View::Parental; s.parental_enabled = s.parental_locked = true;
  draw("10-nest-parental");
  controller.HandleButton(Button::A);
  Expect(s.view == View::Pin, "locked parental settings require PIN");
  controller.HandleButton(Button::Up); controller.HandleButton(Button::Right);
  Expect(s.pin[0] == 1 && s.pin_cursor == 1, "PIN digits use controller navigation");
  draw("11-nest-pin");
  controller.HandleButton(Button::B);

  auto mature = game; mature.slug = mature.id = "mature"; mature.mature_content = true;
  s.cached.push_back(mature); s.catalog.entries.push_back(mature);
  auto mature_installed = installed; mature_installed.id = mature.slug;
  s.installed[mature.slug] = mature_installed;
  auto unclassified = installed; unclassified.id = "legacy-without-metadata";
  s.installed[unclassified.id] = unclassified;
  s.FilterMature();
  Expect(s.VisibleInstalled().size() == 1, "parental PIN hides mature installed games offline");
  Expect(std::none_of(s.catalog.entries.begin(), s.catalog.entries.end(), [](const auto& entry) { return entry.mature_content; }), "parental PIN filters saved catalog");

  s.view = View::Developer; s.code = "GB-4821";
  draw("12-nest-developer-code");
  s.view = View::Transfer; s.working = Work::Install;
  s.progress_percent = 62; s.progress_stage = "Downloading game package";
  s.message = "Your game will appear in Apps.";
  std::promise<NestControllerTestAccess::Result> download;
  s.future = download.get_future();
  draw("13-nest-transfer");
  download.set_value(NestControllerTestAccess::Result{});
  s.future.get();
  s.transfer_success = true; s.progress_percent = 100; s.message = "Installed and ready to play";
  draw("14-nest-installed-success");
  s.view = View::Remove;
  draw("15-nest-remove-confirmation");
  s.view = View::Catalog; s.error = "Could not reach the catalog. Check your connection and try again.";
  draw("16-nest-network-error");

  fs::create_directories(config.games_dir);
  std::ofstream(config.data_dir / "installed_games.json") <<
      "{\"games\":[{\"id\":\"sky-garden\",\"title\":\"Sky Garden\",\"installed_version\":\"1.2.0\",\"install_path\":\"" <<
      installed.install_path.string() << "\",\"package_format_version\":1,\"installed_files\":[],\"launcher_path\":\"" << installed.launcher_path.string() << "\"}]}";
  s.parental_enabled = s.parental_locked = false;
  s.Details(game.slug, View::Installed); Wait(controller);
  Expect(s.manifest && s.manifest->title == game.title && s.error.empty(), "installed details work offline without a cached manifest");
  Expect(s.message == "Offline / installed game", "local details explain offline state");
  controller.HandleButton(Button::X);
  Expect(s.view == View::Remove, "offline installed games can still be removed");
  controller.HandleButton(Button::B);
  controller.HandleButton(Button::A);
  Expect(s.launch_path == installed.launcher_path.string(), "offline installed game can request normal shell launch");
  Expect(controller.TakeLaunchPath() == installed.launcher_path.string(), "shell consumes registered Nest launcher path");
  Wait(controller);
  static_cast<gb::nest::Entry&>(*s.manifest) = mature;
  s.parental_enabled = true; s.parental_locked = false; s.force_locked = false;
  s.launch_path = installed.launcher_path.string();
  controller.TakeLaunchPath();
  Expect(s.view == View::Home, "relocking after mature game launch cannot leave a blank details screen");
  Wait(controller);
  controller.ReportStatus("The game could not be launched. Try again from the game details.", true);
  s.Start({Work::Refresh, View::Home, [](gb::nest::Service&, const auto&) { return NestControllerTestAccess::Result{}; }});
  Wait(controller);
  Expect(s.external_error && !s.error.empty(), "launch failures remain visible across background relock results");
  draw("17-nest-launch-error");
  controller.HandleButton(Button::B);
  Expect(!s.external_error && s.error.empty(), "B dismisses the shell launch error");

  // A queued catalog task must never be able to discard the session relock.
  Expect(s.service->SetPin("1234") && s.service->Unlock("1234"), "prepare unlocked PIN session");
  s.error.clear(); s.parental_enabled = true; s.parental_locked = false; s.force_locked = false;
  s.Home();
  auto entered = std::make_shared<std::promise<void>>();
  auto entered_signal = entered->get_future();
  auto release = std::make_shared<std::promise<void>>();
  auto release_signal = std::make_shared<std::future<void>>(release->get_future());
  s.Start({Work::Refresh, View::Home, [entered, release_signal](gb::nest::Service&, const auto&) {
    entered->set_value(); release_signal->wait(); return NestControllerTestAccess::Result{};
  }});
  entered_signal.wait();
  Expect(controller.HandleButton(Button::B), "B from Nest home exits to the shell");
  Expect(s.parental_locked && s.force_locked, "Nest exit locks UI immediately");
  controller.Open();
  Expect(s.parental_locked, "reopening Nest cannot show unlocked cached content");
  s.Start({Work::Refresh, View::Home, [](gb::nest::Service& api, const auto&) {
    if (!api.ParentalLocked()) throw std::runtime_error("Queued request discarded PIN relock");
    return NestControllerTestAccess::Result{};
  }});
  release->set_value();
  Wait(controller);
  Expect(s.error.empty() && s.service->ParentalLocked(), "replaced pending request still relocks backend session");
  fs::remove_all(dir);
  if (failures) return 1;
  std::cout << "Nest controller screens passed\n";
  return 0;
}
