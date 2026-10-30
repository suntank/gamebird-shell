#include "ui/screens/nest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "render/image_cache.h"
#include "render/assets.h"
#include "ui/widgets/chrome.h"
#include "ui/widgets/list.h"
#include "ui/widgets/text.h"

namespace gb::ui::screens {
namespace {
using platform::Button;
using widgets::DrawContentText;
using widgets::DrawMenuFooter;
using widgets::DrawMenuFrame;
using widgets::DrawText;
using widgets::FitLabel;

std::string Rating(const std::optional<double>& rating) {
  if (!rating) return "NOT RATED";
  std::ostringstream text;
  text << std::fixed << std::setprecision(1) << *rating << " / 5";
  return text.str();
}
std::string Size(const std::uint64_t size) {
  if (size == 0) return "";
  std::ostringstream text;
  text << std::fixed << std::setprecision(size < 10 * 1024 * 1024 ? 1 : 0)
       << static_cast<double>(size) / (1024 * 1024) << " MB";
  return text.str();
}
std::vector<std::string> Wrap(const std::string& text, const int columns = 34) {
  std::vector<std::string> lines;
  std::istringstream words(text);
  std::string line, word;
  while (words >> word) {
    while (word.size() > static_cast<std::size_t>(columns)) {
      if (!line.empty()) { lines.push_back(line); line.clear(); }
      lines.push_back(word.substr(0, columns));
      word.erase(0, columns);
    }
    if (line.size() + word.size() + 1 > static_cast<std::size_t>(columns)) {
      lines.push_back(line); line.clear();
    }
    if (!line.empty()) line += ' ';
    line += word;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}
void Center(render::Surface240& surface, const int y, const std::string& text,
            const std::uint16_t color, const int scale = 1) {
  const auto fitted = FitLabel(text, 208, scale);
  DrawText(surface, std::max(16, (240 - widgets::MeasureTextWidth(fitted, scale)) / 2),
           y, fitted, color, scale);
}
void Message(render::Surface240& surface, const render::Theme& theme,
             const std::string& title, const std::string& detail = {}) {
  Center(surface, 94, title, theme.text);
  const auto lines = Wrap(detail);
  for (int i = 0; i < std::min(4, static_cast<int>(lines.size())); ++i)
    Center(surface, 118 + i * 13, lines[i], theme.text_dim);
}
void NestMark(render::Surface240& surface, const render::Theme& theme,
              const int x, const int y, const int scale = 1) {
  static render::ImageCache images(1);
  std::string error;
  if (const auto* bird = images.LoadPng(render::BundledSystemArt("apps-icon.png").string(), error)) {
    render::BlitImageFit(surface, *bird, {x, y, 22 * scale, 18 * scale}, std::nullopt, true);
    return;
  }
  // A small bird sitting in its nest, drawn with the shell's pixel palette.
  surface.FillRect(x + 7 * scale, y, 8 * scale, 7 * scale, theme.accent);
  surface.FillRect(x + 5 * scale, y + 5 * scale, 12 * scale, 8 * scale, theme.accent);
  surface.FillRect(x + 15 * scale, y + 3 * scale, 4 * scale, 2 * scale, theme.text);
  surface.FillRect(x + 12 * scale, y + 2 * scale, scale, scale, theme.bg);
  surface.FillRect(x + 2 * scale, y + 12 * scale, 18 * scale, 2 * scale, theme.text_dim);
  surface.FillRect(x + 4 * scale, y + 14 * scale, 14 * scale, 2 * scale, theme.text_dim);
  surface.FillRect(x + 7 * scale, y + 16 * scale, 8 * scale, 2 * scale, theme.text_dim);
}
void Stars(render::Surface240& surface, const render::Theme& theme,
           const int x, const int y, const int rating, const int scale = 1) {
  constexpr std::array<std::string_view, 11> shape{
      ".....#.....", "....###....", "....###....", "###########",
      ".#########.", "..#######..", "...#####...", "..###.###..",
      "..##...##..", ".##.....##.", ".#.......#."};
  for (int star = 0; star < 5; ++star) {
    const auto color = star < rating ? theme.accent : theme.panel_border;
    const int left = x + star * 17 * scale;
    for (int row = 0; row < 11; ++row)
      for (int column = 0; column < 11; ++column)
        if (shape[row][column] == '#')
          surface.FillRect(left + column * scale, y + row * scale, scale, scale, color);
  }
}
int Move(const int selected, const int delta, const int count) {
  return count == 0 ? 0 : (selected + delta + count) % count;
}
}  // namespace

struct NestController::Impl {
  enum class View { Home, Catalog, Detail, Installed, Updates, Filters, Parental,
                    Pin, Developer, Transfer, Remove, Rating };
  enum class Work { Refresh, Catalog, Manifest, Image, Installed, Updates, Tags,
                    Install, Uninstall, Rating, Code, Parental };
  struct SharedProgress {
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    nest::Progress value;
  };
  struct Result {
    Work kind = Work::Refresh;
    nest::CatalogPage catalog;
    std::optional<nest::Manifest> manifest;
    std::map<std::string, nest::InstalledGame> installed;
    std::map<std::string, int> ratings;
    std::vector<nest::Entry> cached;
    std::vector<nest::UpdateInfo> updates;
    std::vector<nest::Tag> tags;
    std::string code, image_url, image_path, error, message;
    bool parental_enabled = false, parental_locked = false, library_changed = false;
    bool unlocked_session = false;
    unsigned lock_epoch = 0;
  };
  struct Job {
    Work kind;
    View view;
    std::function<Result(nest::Service&, const nest::ProgressCallback&)> run;
  };

  std::shared_ptr<nest::Service> service;
  std::shared_ptr<std::atomic<bool>> required_lock = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<unsigned>> lock_epoch = std::make_shared<std::atomic<unsigned>>(0);
  std::shared_ptr<SharedProgress> progress;
  std::future<Result> future;
  std::optional<Job> pending;
  Work working = Work::Refresh;
  View working_view = View::Home;
  View view = View::Home, detail_return = View::Catalog, rating_return = View::Detail;
  std::map<std::string, nest::InstalledGame> installed;
  std::map<std::string, int> ratings;
  std::vector<nest::Entry> cached;
  nest::CatalogPage catalog;
  std::optional<nest::Manifest> manifest;
  std::vector<nest::UpdateInfo> updates;
  std::vector<nest::Tag> tags;
  nest::CatalogQuery query;
  std::vector<std::string> draft_tags;
  std::optional<double> draft_rating;
  std::map<std::string, std::string> image_paths;
  mutable render::ImageCache images{5};
  int home_selected = 0, catalog_selected = 0, installed_selected = 0;
  int update_selected = 0, filter_selected = 0, parental_selected = 0;
  int screenshot = 0, scroll = 0, pending_rating = 3, pin_cursor = 0;
  std::array<int, 4> pin{}, confirmation{};
  bool confirming_pin = false;
  std::string pin_action, rating_slug, rating_title;
  std::string code, message, error, launch_path;
  bool parental_enabled = false, parental_locked = false, library_changed = false;
  bool force_locked = true;
  bool external_error = false;
  bool transfer_success = false;
  int progress_percent = 0;
  std::string progress_stage;

  ~Impl() {
    if (progress) progress->cancel = true;
    if (future.valid()) future.wait();
  }
  void Start(Job job) {
    if (future.valid()) {
      if (progress) progress->cancel = true;
      pending = std::move(job);
      return;
    }
    pending.reset();
    working = job.kind;
    working_view = job.view;
    progress = std::make_shared<SharedProgress>();
    progress_percent = 0;
    progress_stage = "Connecting...";
    auto state = progress;
    auto worker_service = service;
    auto lock_requested = required_lock;
    const unsigned epoch = lock_epoch->load();
    const bool known_parental_enabled = parental_enabled;
    const bool known_parental_locked = parental_locked;
    future = std::async(std::launch::async,
        [job = std::move(job), state, worker_service, lock_requested, epoch,
         known_parental_enabled, known_parental_locked]() mutable {
      Result result;
      result.kind = job.kind;
      try {
        if (lock_requested->exchange(false)) worker_service->Lock();
        result = job.run(*worker_service, [state](const nest::Progress& value) {
          { std::lock_guard<std::mutex> guard(state->mutex); state->value = value; }
          return !state->cancel.load();
        });
        result.kind = job.kind;
      } catch (const std::exception& e) { result.error = e.what(); }
      result.lock_epoch = epoch;
      result.parental_enabled = known_parental_enabled;
      result.parental_locked = known_parental_locked;
      try {
        result.parental_enabled = worker_service->ParentalEnabled();
        result.parental_locked = worker_service->ParentalLocked();
        result.installed = worker_service->LoadInstalled();
        result.ratings = worker_service->LoadRatings();
        result.cached = worker_service->CachedCatalog();
      } catch (const std::exception& e) {
        if (result.error.empty()) result.error = e.what();
      }
      return result;
    });
  }
  bool Active() const { return future.valid() || pending.has_value(); }
  bool Loading() const {
    if (pending && pending->view == view) return pending->kind != Work::Image;
    return future.valid() && working_view == view && working != Work::Image &&
           working != Work::Refresh && working != Work::Installed && view != View::Transfer;
  }
  bool Mature(const std::string& slug) const {
    const auto matches = [&](const nest::Entry& entry) {
      return entry.slug == slug || entry.id == slug;
    };
    if (manifest && matches(*manifest)) return manifest->mature_content;
    for (const auto& entry : catalog.entries) if (matches(entry)) return entry.mature_content;
    for (const auto& entry : cached) if (matches(entry)) return entry.mature_content;
    // A legacy package without cached content metadata cannot be classified
    // safely offline. The parent's PIN can reveal it for this session.
    return parental_locked;
  }
  std::vector<nest::InstalledGame> VisibleInstalled() const {
    std::vector<nest::InstalledGame> result;
    for (const auto& [slug, game] : installed)
      if (!parental_locked || !Mature(slug)) result.push_back(game);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
      return left.title < right.title;
    });
    return result;
  }
  void FilterMature() {
    if (!parental_locked) return;
    std::erase_if(catalog.entries, [](const nest::Entry& entry) { return entry.mature_content; });
    std::erase_if(updates, [&](const nest::UpdateInfo& item) { return Mature(item.game_id); });
    if (manifest && manifest->mature_content) manifest.reset();
    catalog_selected = std::clamp(catalog_selected, 0, std::max(0, static_cast<int>(catalog.entries.size()) - 1));
  }
  void Home() { view = View::Home; error.clear(); message.clear(); external_error = false; }
  void FetchCatalog(const int page = 1) {
    view = View::Catalog; error.clear(); message.clear(); screenshot = 0;
    catalog_selected = 0;
    query.page = page; query.include_mature = !parental_locked;
    const auto request = query;
    Start({Work::Catalog, view, [request](nest::Service& api, const auto&) {
      Result result; result.catalog = api.FetchCatalog(request); return result;
    }});
  }
  std::string CurrentImage() const {
    const nest::Entry* game = nullptr;
    if (view == View::Catalog && !catalog.entries.empty())
      game = &catalog.entries[std::clamp(catalog_selected, 0, static_cast<int>(catalog.entries.size()) - 1)];
    else if ((view == View::Detail || view == View::Remove) && manifest) game = &*manifest;
    if (!game) return {};
    if (screenshot > 0 && screenshot <= static_cast<int>(game->screenshot_urls.size()))
      return game->screenshot_urls[screenshot - 1];
    return game->icon_url;
  }
  void FetchImage() {
    const auto url = CurrentImage();
    if (url.empty() || image_paths.contains(url) || (pending && pending->kind != Work::Image)) return;
    Start({Work::Image, view, [url](nest::Service& api, const auto&) {
      Result result; result.image_url = url;
      try {
        result.image_path = api.CacheImage(url).string();
        result.library_changed = api.RefreshLaunchers();
      } catch (...) {}
      return result;
    }});
  }
  void Details(const std::string& slug, const View back) {
    detail_return = back; view = View::Detail;
    manifest.reset(); error.clear(); message.clear(); scroll = 0; screenshot = 0;
    Start({Work::Manifest, view, [slug](nest::Service& api, const auto&) {
      Result result;
      try { result.manifest = api.FetchManifest(slug); }
      catch (const std::exception& problem) {
        if (std::string(problem.what()).find("parental controls") != std::string::npos) throw;
        const auto installed_games = api.LoadInstalled();
        const auto local = installed_games.find(slug);
        if (local == installed_games.end()) throw;
        nest::Manifest fallback;
        bool classified = false;
        for (const auto& entry : api.CachedCatalog()) {
          if (entry.id == slug || entry.slug == slug) {
            static_cast<nest::Entry&>(fallback) = entry;
            classified = true; break;
          }
        }
        if (api.ParentalLocked() && (!classified || fallback.mature_content)) throw;
        fallback.id = fallback.slug = local->second.id;
        fallback.title = local->second.title;
        fallback.version = fallback.download.version = local->second.installed_version;
        if (fallback.description.empty()) fallback.description = "This game is installed on your device. Connect to Wi-Fi to load its store description and check for updates.";
        result.manifest = std::move(fallback);
        result.message = "Offline / installed game";
      }
      return result;
    }});
  }
  std::string Slug() const {
    if (!manifest) return {};
    return manifest->slug.empty() ? manifest->id : manifest->slug;
  }
  std::vector<std::string> DetailLines() const {
    if (!manifest) return {};
    const auto& game = *manifest;
    std::vector<std::string> result;
    const auto append = [&](const std::string& text) {
      const auto wrapped = Wrap(text);
      result.insert(result.end(), wrapped.begin(), wrapped.end());
    };
    if (!game.author.empty()) append("BY " + game.author);
    if (!game.release_date.empty()) append("RELEASED " + game.release_date.substr(0, 10));
    if (game.rating_count > 0) append(std::to_string(game.rating_count) + " PLAYER RATINGS");
    std::string tags_text;
    for (const auto& tag : game.tags) { if (!tags_text.empty()) tags_text += " / "; tags_text += tag; }
    if (!tags_text.empty()) append(tags_text);
    if (game.mature_content) append("MATURE CONTENT / 18+");
    append(game.description.empty() ? "No description available." : game.description);
    for (const auto& release : game.changelog) {
      append("VERSION " + release.version + (release.date.empty() ? "" : " / " + release.date));
      for (const auto& note : release.notes) append("- " + note);
    }
    return result;
  }
  const nest::InstalledGame* InstalledDetail() const {
    if (!manifest) return nullptr;
    auto found = installed.find(Slug());
    if (found == installed.end()) found = installed.find(manifest->id);
    return found == installed.end() ? nullptr : &found->second;
  }
  bool HasUpdate() const {
    const auto* game = InstalledDetail();
    if (!manifest || !game) return false;
    const auto latest = manifest->download.version.empty() ? manifest->version : manifest->download.version;
    return game->package_format_version != 1 || nest::Service::IsNewer(latest, game->installed_version);
  }
  void Install() {
    if (!manifest || (parental_locked && manifest->mature_content)) return;
    const auto game = *manifest;
    view = View::Transfer; error.clear(); message.clear(); transfer_success = false;
    Start({Work::Install, view, [game](nest::Service& api, const auto& callback) {
      Result result; api.Install(game, callback);
      result.library_changed = true; result.message = "Installed and ready to play"; return result;
    }});
  }
  void OpenInstalled() {
    view = View::Installed; error.clear(); message.clear();
    Start({Work::Installed, view, [](nest::Service& api, const auto&) {
      Result result;
      try { api.SyncRatings(); } catch (...) { result.message = "Showing saved ratings"; }
      return result;
    }});
  }
  void Launch(const nest::InstalledGame& game) {
    if (game.package_format_version != 1) {
      message = "Update this game to a game.json package";
      return;
    }
    if (game.launcher_path.empty()) {
      message = "Reopen Nest to restore this library entry";
      return;
    }
    launch_path = game.launcher_path.string();
  }
  void OpenFilters() {
    view = View::Filters; error.clear(); filter_selected = 0;
    draft_tags = query.tags; draft_rating = query.min_rating;
    if (!tags.empty()) return;
    Start({Work::Tags, view, [](nest::Service& api, const auto&) {
      Result result; result.tags = api.FetchTags(); return result;
    }});
  }
  void OpenUpdates() {
    view = View::Updates; error.clear(); message.clear(); update_selected = 0;
    Start({Work::Updates, view, [](nest::Service& api, const auto&) {
      Result result; result.updates = api.FindUpdates(api.FetchAllCatalog()); return result;
    }});
  }
  void ParentalJob(const std::string& action, const std::string& digits = {}) {
    Start({Work::Parental, View::Parental, [action, digits](nest::Service& api, const auto&) {
      Result result;
      bool success = true;
      if (action == "set") success = api.SetPin(digits);
      if (action == "unlock") success = api.Unlock(digits);
      if (action == "remove") success = api.RemovePin(digits);
      if (action == "lock") api.Lock();
      if (!success) throw std::runtime_error("Incorrect PIN. Try again.");
      result.unlocked_session = action == "unlock";
      result.message = action == "set" ? "PIN saved. Mature games hidden." :
                       action == "unlock" ? "Unlocked for this Nest session" :
                       action == "remove" ? "Parental PIN removed" : "Mature games hidden";
      return result;
    }});
  }
  void Relock() {
    required_lock->store(true);
    ++*lock_epoch;
    force_locked = true;
    parental_locked = parental_enabled;
    const bool hides_detail = parental_locked && manifest && manifest->mature_content &&
                              (view == View::Detail || view == View::Remove);
    const bool hides_rating = parental_locked && view == View::Rating && Mature(rating_slug);
    FilterMature();
    if (hides_detail || hides_rating) Home();
    Start({Work::Refresh, View::Home, [](nest::Service& api, const auto&) {
      Result result; api.Lock(); return result;
    }});
  }
  void DrawArt(render::Surface240& surface, const render::Theme& theme,
               const render::Rect bounds) const {
    surface.FillRect(bounds.x, bounds.y, bounds.w, bounds.h, theme.bg);
    surface.StrokeRect(bounds.x, bounds.y, bounds.w, bounds.h, theme.panel_border);
    const auto url = CurrentImage();
    const auto found = image_paths.find(url);
    if (found != image_paths.end() && !found->second.empty()) {
      std::string ignored;
      if (const auto* image = images.LoadImage(found->second, ignored)) {
        render::BlitImageFit(surface, *image, bounds, theme.bg); return;
      }
    }
    NestMark(surface, theme, bounds.x + bounds.w / 2 - 22,
             bounds.y + bounds.h / 2 - 18, 2);
  }
};

NestController::NestController() : impl_(std::make_unique<Impl>()) {}
NestController::NestController(const nest::Config& config) : NestController() { Configure(config); }
NestController::~NestController() = default;
void NestController::Configure(const nest::Config& config) {
  impl_ = std::make_unique<Impl>();
  impl_->service = std::make_shared<nest::Service>(config);
}
void NestController::Open() {
  auto& s = *impl_;
  if (!s.service) s.service = std::make_shared<nest::Service>();
  s.required_lock->store(true);
  ++*s.lock_epoch;
  s.force_locked = true;
  s.parental_locked = s.parental_enabled;
  s.FilterMature();
  s.Home();
  s.Start({Impl::Work::Refresh, Impl::View::Home, [](nest::Service& api, const auto&) {
    Impl::Result result;
    api.Lock();
    result.library_changed = api.RefreshLaunchers();
    return result;
  }});
}

bool NestController::HandleButton(const Button button) {
  auto& s = *impl_;
  using View = Impl::View;
  using Work = Impl::Work;
  if (s.external_error) {
    if (button == Button::B) { s.error.clear(); s.external_error = false; }
    return false;
  }
  if (button == Button::Start && s.view != View::Catalog && s.view != View::Filters) {
    if (s.view == View::Transfer && s.Active()) return false;
    s.Home(); return false;
  }
  if (button == Button::B) {
    if (s.view == View::Home) { s.Relock(); return true; }
    if (s.view == View::Transfer && s.Active()) {
      if (s.progress) s.progress->cancel = true;
      s.progress_stage = "Cancelling safely...";
      return false;
    }
    if (s.view == View::Detail) s.view = s.detail_return;
    else if (s.view == View::Rating) s.view = s.rating_return;
    else if (s.view == View::Remove || s.view == View::Transfer) s.view = View::Detail;
    else if (s.view == View::Filters) s.view = View::Catalog;
    else if (s.view == View::Pin) s.view = View::Parental;
    else s.Home();
    s.error.clear(); s.message.clear();
    return false;
  }
  if (s.Loading()) return false;
  const int delta = button == Button::Up ? -1 : button == Button::Down ? 1 : 0;
  if (s.view == View::Home) {
    if (delta) s.home_selected = Move(s.home_selected, delta, 6);
    if (button != Button::A) return false;
    switch (s.home_selected) {
      case 0: s.FetchCatalog(s.query.page); break;
      case 1: s.OpenInstalled(); break;
      case 2: s.OpenUpdates(); break;
      case 3: s.view = View::Parental; s.parental_selected = 0; s.error.clear(); break;
      case 4:
        s.view = View::Developer; s.error.clear(); s.code.clear();
        s.Start({Work::Code, s.view, [](nest::Service& api, const auto&) {
          Impl::Result result; result.code = api.RequestDeveloperCode(); return result;
        }}); break;
      case 5: s.Relock(); return true;
    }
    return false;
  }
  if (s.view == View::Catalog) {
    if (button == Button::Start || button == Button::Y) { s.OpenFilters(); return false; }
    if (button == Button::X) { s.FetchCatalog(s.query.page); return false; }
    if (button == Button::L && s.query.page > 1) s.FetchCatalog(s.query.page - 1);
    else if (button == Button::R && s.query.page < s.catalog.total_pages) s.FetchCatalog(s.query.page + 1);
    if (s.catalog.entries.empty()) return false;
    if (delta) {
      s.catalog_selected = Move(s.catalog_selected, delta, static_cast<int>(s.catalog.entries.size()));
      s.screenshot = 0; s.FetchImage();
    }
    const auto& game = s.catalog.entries[s.catalog_selected];
    if (button == Button::Left || button == Button::Right) {
      s.screenshot = std::clamp(s.screenshot + (button == Button::Right ? 1 : -1),
                                0, static_cast<int>(game.screenshot_urls.size()));
      s.FetchImage();
    }
    if (button == Button::A) s.Details(game.slug.empty() ? game.id : game.slug, View::Catalog);
    return false;
  }
  if (s.view == View::Installed) {
    const auto games = s.VisibleInstalled();
    if (games.empty()) return false;
    s.installed_selected = std::clamp(s.installed_selected, 0, static_cast<int>(games.size()) - 1);
    if (delta) s.installed_selected = Move(s.installed_selected, delta, static_cast<int>(games.size()));
    const auto& game = games[s.installed_selected];
    if (button == Button::A) s.Details(game.id, View::Installed);
    if (button == Button::Y) {
      s.rating_slug = game.id; s.rating_title = game.title;
      s.pending_rating = s.ratings.contains(game.id) ? s.ratings.at(game.id) : 3;
      s.rating_return = View::Installed; s.view = View::Rating;
    }
    if (button == Button::X) s.Launch(game);
    return false;
  }
  if (s.view == View::Updates) {
    if (button == Button::X) { s.OpenUpdates(); return false; }
    if (s.updates.empty()) return false;
    if (delta) s.update_selected = Move(s.update_selected, delta, static_cast<int>(s.updates.size()));
    if (button == Button::A) s.Details(s.updates[s.update_selected].game_id, View::Updates);
    return false;
  }
  if (s.view == View::Detail) {
    if (!s.manifest) return false;
    if (delta) s.scroll = std::clamp(s.scroll + delta, 0,
        std::max(0, static_cast<int>(s.DetailLines().size()) - 5));
    if (button == Button::Left || button == Button::Right) {
      s.screenshot = std::clamp(s.screenshot + (button == Button::Right ? 1 : -1),
                               0, static_cast<int>(s.manifest->screenshot_urls.size()));
      s.FetchImage();
    }
    if (button == Button::A) {
      if (!s.InstalledDetail() || s.HasUpdate()) s.Install();
      else s.Launch(*s.InstalledDetail());
    }
    if (button == Button::X && s.InstalledDetail()) s.view = View::Remove;
    if (button == Button::Y && s.InstalledDetail()) {
      s.rating_slug = s.Slug(); s.rating_title = s.manifest->title;
      s.pending_rating = s.ratings.contains(s.rating_slug) ? s.ratings.at(s.rating_slug) : 3;
      s.rating_return = View::Detail; s.view = View::Rating;
    }
    return false;
  }
  if (s.view == View::Remove && button == Button::A) {
    const auto slug = s.Slug();
    s.view = View::Transfer; s.error.clear(); s.transfer_success = false;
    s.Start({Work::Uninstall, s.view, [slug](nest::Service& api, const auto&) {
      Impl::Result result; api.Uninstall(slug); result.library_changed = true;
      result.message = "Game removed from your library."; return result;
    }});
    return false;
  }
  if (s.view == View::Transfer && !s.Active() && button == Button::A) { s.view = View::Detail; return false; }
  if (s.view == View::Rating) {
    if (button == Button::Left || button == Button::Down) s.pending_rating = std::max(1, s.pending_rating - 1);
    if (button == Button::Right || button == Button::Up) s.pending_rating = std::min(5, s.pending_rating + 1);
    if (button == Button::A) {
      const auto slug = s.rating_slug; const int rating = s.pending_rating;
      s.view = s.rating_return;
      s.Start({Work::Rating, s.view, [slug, rating](nest::Service& api, const auto&) {
        Impl::Result result;
        result.message = api.SetRating(slug, rating) ? "Rating saved. Thank you!" : "Rating saved on this device";
        return result;
      }});
    }
    return false;
  }
  if (s.view == View::Filters) {
    const int count = 4 + static_cast<int>(s.tags.size());
    if (delta) s.filter_selected = Move(s.filter_selected, delta, count);
    if (button == Button::X) { s.draft_tags.clear(); s.draft_rating.reset(); }
    if (button == Button::Start || (button == Button::A && s.filter_selected == 0)) {
      s.query.tags = s.draft_tags; s.query.min_rating = s.draft_rating;
      s.FetchCatalog(); return false;
    }
    if (button == Button::A && s.filter_selected > 0 && s.filter_selected <= 3) {
      const double value = s.filter_selected + 2;
      s.draft_rating = s.draft_rating == value ? std::nullopt : std::optional<double>(value);
    } else if (button == Button::A && s.filter_selected >= 4) {
      const auto& id = s.tags[s.filter_selected - 4].id;
      const auto found = std::find(s.draft_tags.begin(), s.draft_tags.end(), id);
      if (found == s.draft_tags.end()) s.draft_tags.push_back(id);
      else s.draft_tags.erase(found);
    }
    return false;
  }
  if (s.view == View::Parental) {
    const int count = !s.parental_enabled || s.parental_locked ? 2 : 3;
    if (delta) s.parental_selected = Move(s.parental_selected, delta, count);
    if (button == Button::A) {
      s.error.clear(); s.message.clear();
      if (s.parental_selected == count - 1) { s.Home(); return false; }
      if (s.parental_enabled && !s.parental_locked && s.parental_selected == 0) { s.ParentalJob("lock"); return false; }
      s.pin_action = !s.parental_enabled ? "set" : s.parental_locked ? "unlock" : "remove";
      s.pin.fill(0); s.confirmation.fill(0); s.pin_cursor = 0; s.confirming_pin = false; s.view = View::Pin;
    }
    return false;
  }
  if (s.view == View::Pin) {
    auto& digits = s.confirming_pin ? s.confirmation : s.pin;
    if (delta) digits[s.pin_cursor] = (digits[s.pin_cursor] - delta + 10) % 10;
    if (button == Button::Left) s.pin_cursor = std::max(0, s.pin_cursor - 1);
    if (button == Button::Right) s.pin_cursor = std::min(3, s.pin_cursor + 1);
    if (button == Button::A) {
      if (s.pin_action == "set" && !s.confirming_pin) { s.confirming_pin = true; s.pin_cursor = 0; return false; }
      if (s.confirming_pin && s.pin != s.confirmation) {
        s.error = "PINs do not match. Try again."; s.confirming_pin = false; s.pin_cursor = 0; return false;
      }
      std::string value;
      for (const auto digit : s.pin) value += static_cast<char>('0' + digit);
      s.view = View::Parental; s.parental_selected = 0;
      s.ParentalJob(s.pin_action, value);
    }
  }
  return false;
}

bool NestController::Tick() {
  auto& s = *impl_;
  bool changed = false;
  if (s.progress) {
    std::lock_guard<std::mutex> guard(s.progress->mutex);
    const auto& value = s.progress->value;
    const int percent = value.total > 0 ? static_cast<int>(100.0 * value.completed / value.total) : 0;
    if ((!value.stage.empty() && value.stage != s.progress_stage) || percent != s.progress_percent) {
      if (!value.stage.empty()) s.progress_stage = value.stage;
      s.progress_percent = percent; changed = true;
    }
  }
  if (!s.future.valid() || s.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return changed;
  auto result = s.future.get();
  s.installed = std::move(result.installed); s.ratings = std::move(result.ratings);
  s.cached = std::move(result.cached);
  if (result.unlocked_session && result.lock_epoch == s.lock_epoch->load()) s.force_locked = false;
  s.parental_enabled = result.parental_enabled;
  s.parental_locked = s.parental_enabled && (s.force_locked || result.parental_locked);
  s.library_changed |= result.library_changed;
  const bool visible = s.view == s.working_view && !s.pending;
  if (visible && !s.external_error) { s.error = result.error; if (!result.message.empty()) s.message = result.message; }
  using Work = Impl::Work;
  if (result.error.empty()) {
    switch (result.kind) {
      case Work::Catalog: s.catalog = std::move(result.catalog); break;
      case Work::Manifest: s.manifest = std::move(result.manifest); break;
      case Work::Image: s.image_paths[result.image_url] = std::move(result.image_path); break;
      case Work::Updates: s.updates = std::move(result.updates); break;
      case Work::Tags: s.tags = std::move(result.tags); break;
      case Work::Code: s.code = std::move(result.code); break;
      case Work::Install: case Work::Uninstall:
        s.transfer_success = true; s.progress_percent = 100; break;
      default: break;
    }
  }
  const bool blocked_manifest = s.parental_locked && s.manifest && s.manifest->mature_content;
  s.FilterMature();
  if (blocked_manifest && s.view == Impl::View::Detail) s.error = "Mature content is locked. Enter your PIN in parental controls.";
  if (s.pending) {
    auto job = std::move(*s.pending); s.pending.reset(); s.Start(std::move(job));
  } else if ((result.kind == Work::Catalog || result.kind == Work::Manifest) && result.error.empty()) s.FetchImage();
  return true;
}

void NestController::Draw(render::Surface240& surface, const render::Theme& theme) const {
  const auto& s = *impl_;
  using View = Impl::View;
  std::string title = "GAME BIRD NEST";
  switch (s.view) {
    case View::Catalog: title = "NEST / DISCOVER"; break;
    case View::Detail: title = "NEST / GAME DETAILS"; break;
    case View::Installed: title = "NEST / INSTALLED"; break;
    case View::Updates: title = "NEST / GAME UPDATES"; break;
    case View::Filters: title = "NEST / FILTERS"; break;
    case View::Parental: case View::Pin: title = "NEST / PARENTAL CONTROLS"; break;
    case View::Developer: title = "NEST / DEVELOPER CODE"; break;
    case View::Transfer: title = "NEST / " + std::string(s.working == Impl::Work::Uninstall ? "REMOVE GAME" : "INSTALL GAME"); break;
    case View::Remove: title = "NEST / REMOVE GAME"; break;
    case View::Rating: title = "NEST / RATE GAME"; break;
    default: break;
  }
  DrawMenuFrame(surface, theme, title);
  if (s.external_error) {
    Message(surface, theme, "COULD NOT COMPLETE REQUEST", s.error);
    DrawMenuFooter(surface, theme, "B:BACK"); return;
  }
  if (s.Loading()) {
    NestMark(surface, theme, 98, 63, 2);
    Message(surface, theme, s.view == View::Installed ? "SYNCING YOUR GAMES" : "LOADING...", "B returns immediately");
    DrawMenuFooter(surface, theme, "B:BACK"); return;
  }
  if (s.view == View::Home) {
    NestMark(surface, theme, 22, 38, 1);
    DrawContentText(surface, 52, 39, "A home for indie games", theme.text);
    DrawContentText(surface, 52, 51, s.parental_locked ? "FAMILY FILTER ON" : "DISCOVER SOMETHING NEW", theme.text);
    const std::vector<std::string> rows = {"Browse catalog", "Installed games", "Check for game updates", "Parental controls", "Developer code", "Back to Game Bird"};
    widgets::DrawList(surface, 16, 70, 208, 132, 22, rows, s.home_selected, theme);
    DrawMenuFooter(surface, theme, "A:OPEN  B:GAME BIRD"); return;
  }
  if (!s.error.empty() && s.view != View::Parental && s.view != View::Pin && s.view != View::Transfer) {
    Message(surface, theme, "COULD NOT COMPLETE REQUEST", s.error);
    DrawMenuFooter(surface, theme, s.view == View::Catalog || s.view == View::Updates ? "X:RETRY  B:BACK" : "B:BACK"); return;
  }
  if (s.view == View::Catalog) {
    if (s.catalog.entries.empty()) {
      Message(surface, theme, "NO GAMES FOUND", "Try changing your filters or connect to Wi-Fi.");
      DrawMenuFooter(surface, theme, "START:FILTERS X:RETRY B:BACK"); return;
    }
    const auto& game = s.catalog.entries[s.catalog_selected];
    const int count = static_cast<int>(s.query.tags.size()) + (s.query.min_rating ? 1 : 0);
    DrawContentText(surface, 16, 35, s.catalog.offline ? "OFFLINE / SAVED CATALOG" : "DISCOVER INDIE GAMES", theme.text);
    DrawContentText(surface, 16, 47, "PAGE " + std::to_string(s.query.page) + "/" + std::to_string(s.catalog.total_pages) + (count ? "  FILTERS " + std::to_string(count) : ""), theme.text_dim);
    s.DrawArt(surface, theme, {16, 61, 208, 90});
    if (game.rating) {
      surface.FillRect(163, 65, 57, 13, theme.panel);
      DrawText(surface, 167, 68, Rating(game.rating), theme.text);
    }
    if (game.mature_content) {
      surface.FillRect(20, 133, 80, 13, theme.panel);
      DrawText(surface, 24, 136, "MATURE 18+", theme.text);
    }
    if (!game.screenshot_urls.empty()) {
      const auto text = s.screenshot == 0 ? "ART  < >" : "SHOT " + std::to_string(s.screenshot) + " < >";
      surface.FillRect(143, 133, 77, 13, theme.panel);
      DrawText(surface, 147, 136, text, theme.text);
    }
    const int first = std::clamp(s.catalog_selected - 1, 0, std::max(0, static_cast<int>(s.catalog.entries.size()) - 3));
    for (int row = 0; row < 3 && first + row < static_cast<int>(s.catalog.entries.size()); ++row) {
      const int i = first + row, y = 155 + row * 17;
      const bool active = i == s.catalog_selected;
      surface.FillRect(16, y, 208, 15, active ? theme.accent : theme.bg);
      DrawText(surface, 22, y + 4, FitLabel(s.catalog.entries[i].title, 194), theme.text_dim);
    }
    DrawMenuFooter(surface, theme, "A:DETAILS START:FILTER L/R:PAGE B:BACK"); return;
  }
  if (s.view == View::Detail && s.manifest) {
    const auto& game = *s.manifest;
    const auto* local = s.InstalledDetail();
    DrawContentText(surface, 16, 37, game.title, theme.text);
    s.DrawArt(surface, theme, {16, 52, 86, 70});
    DrawText(surface, 112, 56, FitLabel("v" + game.version, 110), theme.text_dim);
    Stars(surface, theme, 111, 73, game.rating ? static_cast<int>(std::round(*game.rating)) : 0);
    DrawText(surface, 112, 91, Rating(game.rating), theme.text);
    DrawText(surface, 112, 106, local ? (s.HasUpdate() ? "UPDATE READY" : "INSTALLED") : Size(game.download.size_bytes), theme.text);
    const auto lines = s.DetailLines();
    for (int row = 0; row < 5 && s.scroll + row < static_cast<int>(lines.size()); ++row)
      DrawText(surface, 16, 130 + row * 12, lines[s.scroll + row], theme.text);
    if (!s.message.empty()) DrawContentText(surface, 16, 195, s.message, theme.text);
    else if (lines.size() > 5) DrawContentText(surface, 16, 195, "UP/DOWN:READ MORE  <>:ART", theme.text_dim);
    else if (!game.screenshot_urls.empty()) DrawContentText(surface, 16, 195, "LEFT/RIGHT:VIEW SCREENSHOTS", theme.text_dim);
    DrawMenuFooter(surface, theme, !local ? "A:INSTALL  B:BACK" : s.HasUpdate() ? "A:UPDATE X:REMOVE Y:RATE B:BACK" : "A:PLAY X:REMOVE Y:RATE B:BACK"); return;
  }
  if (s.view == View::Installed) {
    const auto games = s.VisibleInstalled();
    DrawContentText(surface, 16, 38, std::to_string(games.size()) + " GAMES READY TO PLAY", theme.text_dim);
    if (games.empty()) Message(surface, theme,
        s.parental_locked && !s.installed.empty() ? "FAMILY FILTER IS ON" : "YOUR NEST IS EMPTY",
        s.parental_locked && !s.installed.empty() ? "Unlock parental controls to view all your games." : "Find your next game in Browse catalog.");
    const int selected = std::clamp(s.installed_selected, 0, std::max(0, static_cast<int>(games.size()) - 1));
    const int first = std::clamp(selected - 2, 0, std::max(0, static_cast<int>(games.size()) - 5));
    for (int row = 0; row < 5 && first + row < static_cast<int>(games.size()); ++row) {
      const int i = first + row, y = 56 + row * 28;
      const bool active = i == selected;
      surface.FillRect(16, y, 208, 25, active ? theme.accent : theme.bg);
      const auto& game = games[i];
      DrawText(surface, 22, y + 4, FitLabel(game.title, 194), theme.text);
      std::string sub = "v" + game.installed_version;
      const auto rating = s.ratings.find(game.id);
      if (rating != s.ratings.end()) sub += "    YOUR RATING " + std::to_string(rating->second) + "/5";
      DrawText(surface, 22, y + 15, FitLabel(sub, 194), theme.text_dim);
    }
    if (!s.message.empty()) DrawContentText(surface, 16, 199, s.message, theme.text);
    DrawMenuFooter(surface, theme, "A:DETAILS X:PLAY Y:RATE B:BACK"); return;
  }
  if (s.view == View::Updates) {
    DrawContentText(surface, 16, 39, std::to_string(s.updates.size()) + " UPDATES AVAILABLE", theme.text_dim);
    if (s.updates.empty()) Message(surface, theme, "YOU ARE UP TO DATE", "All your installed games are current.");
    std::vector<std::string> rows;
    for (const auto& item : s.updates) rows.push_back(item.title);
    widgets::DrawList(surface, 16, 60, 208, 120, 24, rows, s.update_selected, theme);
    if (!s.updates.empty()) {
      const auto& item = s.updates[std::clamp(s.update_selected, 0, static_cast<int>(s.updates.size()) - 1)];
      DrawContentText(surface, 16, 192, "v" + item.installed_version + " > v" + item.latest_version, theme.text);
    }
    DrawMenuFooter(surface, theme, "A:VIEW UPDATE X:CHECK B:BACK"); return;
  }
  if (s.view == View::Filters) {
    const int count = static_cast<int>(s.draft_tags.size()) + (s.draft_rating ? 1 : 0);
    DrawContentText(surface, 16, 38, std::to_string(count) + " FILTERS SELECTED", theme.text_dim);
    std::vector<std::string> rows{"Apply filters"};
    for (int value = 3; value <= 5; ++value)
      rows.push_back(std::string(s.draft_rating == value ? "[X] " : "[ ] ") + std::to_string(value) + (value < 5 ? "+ stars" : " stars"));
    for (const auto& tag : s.tags)
      rows.push_back(std::string(std::find(s.draft_tags.begin(), s.draft_tags.end(), tag.id) != s.draft_tags.end() ? "[X] " : "[ ] ") + tag.name);
    widgets::DrawList(surface, 16, 56, 208, 126, 18, rows, s.filter_selected, theme);
    if (s.filter_selected >= 4 && s.filter_selected - 4 < static_cast<int>(s.tags.size()))
      DrawContentText(surface, 16, 194, s.tags[s.filter_selected - 4].category, theme.text_dim);
    DrawMenuFooter(surface, theme, "A:TOGGLE START:APPLY X:CLEAR B:BACK"); return;
  }
  if (s.view == View::Parental) {
    DrawContentText(surface, 16, 42, s.parental_enabled ? (s.parental_locked ? "FAMILY FILTER ON" : "UNLOCKED THIS SESSION") : "PARENTAL PIN NOT SET", theme.text);
    const auto explanation = Wrap(s.parental_enabled ? "Mature games stay hidden in Nest until the PIN is entered. Leaving Nest locks it again." : "Set a four digit PIN to hide mature games inside Nest.");
    for (int i = 0; i < static_cast<int>(explanation.size()); ++i) DrawText(surface, 16, 62 + 12 * i, explanation[i], theme.text_dim);
    std::vector<std::string> rows;
    if (!s.parental_enabled) rows = {"Set parental PIN", "Back"};
    else if (s.parental_locked) rows = {"Unlock with PIN", "Back"};
    else rows = {"Lock now", "Remove PIN", "Back"};
    widgets::DrawList(surface, 16, 110, 208, 66, 22, rows, s.parental_selected, theme);
    DrawContentText(surface, 16, 188, s.error.empty() ? s.message : s.error, theme.text);
    DrawMenuFooter(surface, theme, "A:SELECT  B:BACK");
    return;
  }
  if (s.view == View::Pin) {
    Center(surface, 54, s.confirming_pin ? "CONFIRM YOUR PIN" : s.pin_action == "set" ? "CHOOSE A FOUR DIGIT PIN" : "ENTER YOUR PIN", theme.text);
    const auto& digits = s.confirming_pin ? s.confirmation : s.pin;
    for (int i = 0; i < 4; ++i) {
      const int x = 39 + i * 42;
      surface.FillRect(x, 87, 34, 42, i == s.pin_cursor ? theme.accent : theme.bg);
      surface.StrokeRect(x, 87, 34, 42, i == s.pin_cursor ? theme.text : theme.panel_border);
      DrawText(surface, x + 11, 102, std::to_string(digits[i]), theme.text, 2);
    }
    Center(surface, 148, "UP/DOWN:CHANGE DIGIT", theme.text_dim);
    Center(surface, 164, "LEFT/RIGHT:MOVE", theme.text_dim);
    if (!s.error.empty()) Center(surface, 188, s.error, theme.text);
    DrawMenuFooter(surface, theme, "A:CONFIRM  B:CANCEL"); return;
  }
  if (s.view == View::Developer) {
    Center(surface, 53, "LINK YOUR DEVELOPER ACCOUNT", theme.text);
    surface.FillRect(28, 82, 184, 48, theme.bg);
    surface.StrokeRect(28, 82, 184, 48, theme.panel_border);
    Center(surface, 99, s.code.empty() ? "NO CODE RECEIVED" : s.code, theme.text, 2);
    Center(surface, 151, "Enter this code on the", theme.text_dim);
    Center(surface, 166, "Game Bird website to link", theme.text_dim);
    Center(surface, 181, "this device to your account.", theme.text_dim);
    DrawMenuFooter(surface, theme, "B:BACK"); return;
  }
  if (s.view == View::Transfer) {
    Center(surface, 50, s.manifest ? s.manifest->title : "GAME BIRD NEST", theme.text);
    NestMark(surface, theme, 98, 71, 2);
    widgets::DrawProgress(surface, theme, 123, s.progress_percent, s.transfer_success);
    Center(surface, 146, s.Active() ? s.progress_stage : s.error.empty() ? "COMPLETE" : "COULD NOT FINISH", theme.text);
    const auto lines = Wrap(s.error.empty() ? s.message : s.error);
    for (int i = 0; i < std::min(3, static_cast<int>(lines.size())); ++i) Center(surface, 164 + i * 12, lines[i], theme.text_dim);
    DrawMenuFooter(surface, theme, s.Active() ? "B:CANCEL" : "A:CONTINUE  B:BACK"); return;
  }
  if (s.view == View::Remove) {
    Message(surface, theme, "REMOVE THIS GAME?", s.manifest ? s.manifest->title : "");
    Center(surface, 166, "Downloaded game files are removed.", theme.text_dim);
    DrawMenuFooter(surface, theme, "A:REMOVE  B:KEEP GAME"); return;
  }
  if (s.view == View::Rating) {
    Center(surface, 54, s.rating_title, theme.text);
    Center(surface, 79, "HOW WAS YOUR GAME?", theme.text_dim);
    Stars(surface, theme, 42, 111, s.pending_rating, 2);
    Center(surface, 149, std::to_string(s.pending_rating) + " / 5", theme.text);
    Center(surface, 178, "LEFT/RIGHT:CHOOSE RATING", theme.text_dim);
    DrawMenuFooter(surface, theme, "A:SAVE RATING  B:CANCEL"); return;
  }
  Message(surface, theme, "NO GAME SELECTED");
  DrawMenuFooter(surface, theme, "B:BACK");
}

bool NestController::TakeLibraryChanged() {
  if (impl_->Active()) return false;
  return std::exchange(impl_->library_changed, false);
}
std::string NestController::TakeLaunchPath() {
  auto path = std::exchange(impl_->launch_path, {});
  if (!path.empty()) impl_->Relock();
  return path;
}
bool NestController::Busy() const { return impl_->Active(); }
const std::map<std::string, nest::InstalledGame>& NestController::Installed() const { return impl_->installed; }
const std::vector<nest::Entry>& NestController::Catalog() const { return impl_->cached; }
void NestController::ReportStatus(const std::string& message, const bool failed) {
  impl_->external_error = failed;
  if (failed) impl_->error = message;
  else { impl_->error.clear(); impl_->message = message; }
}

}  // namespace gb::ui::screens
