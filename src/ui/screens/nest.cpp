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
using widgets::DrawText;
using widgets::FitLabel;

constexpr auto kNestBackground = render::Rgb565(8, 17, 29);
constexpr auto kNestPanel = render::Rgb565(17, 29, 43);
constexpr auto kNestMuted = render::Rgb565(185, 204, 220);
constexpr int kDetailRows = 8;

std::string PlainText(std::string text) {
  // Store descriptions commonly contain typographic quotes; the shell font
  // has ASCII glyphs, so keep their readable equivalents rather than "???".
  for (const auto& [from, to] : std::array<std::pair<std::string_view, std::string_view>, 8>{{
           {"\xe2\x80\x98", "'"}, {"\xe2\x80\x99", "'"}, {"\xe2\x80\x9c", "\""},
           {"\xe2\x80\x9d", "\""}, {"\xe2\x80\x93", "-"}, {"\xe2\x80\x94", "-"},
           {"\xe2\x80\xa6", "..."}, {"\xc2\xa0", " "}}}) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
      text.replace(at, from.size(), to);
      at += to.size();
    }
  }
  return text;
}

std::string FitRead(const std::string& input, const int width = 224) {
  const auto text = PlainText(input);
  const int count = std::max(0, width / 9);
  if (static_cast<int>(text.size()) <= count) return text;
  return count <= 3 ? text.substr(0, count) : text.substr(0, count - 3) + "...";
}
void Read(render::Surface240& surface, const int x, const int y,
          const std::string& text, const std::uint16_t color, const int width = 224) {
  widgets::DrawReadableText(surface, x, y, FitRead(text, width), color);
}
void Large(render::Surface240& surface, const int x, const int y,
           const std::string& text, const std::uint16_t color, const int width = 224) {
  DrawText(surface, x, y, FitLabel(PlainText(text), width, 2), color, 2);
}
void Frame(render::Surface240& surface, const render::Theme& theme,
           const std::string& title, const std::string& counter = {}) {
  surface.Clear(kNestBackground);
  const auto info = FitRead(counter, 144);
  const int info_width = static_cast<int>(info.size()) * 9;
  Large(surface, 8, 8, title, theme.text, info.empty() ? 224 : std::min(164, 216 - info_width));
  if (!info.empty()) Read(surface, 232 - info_width, 10, info, kNestMuted);
  surface.FillRect(8, 26, 224, 1, theme.panel_border);
}
void Footer(render::Surface240& surface, const render::Theme& theme,
            const std::string& first, const std::string& second = {}) {
  surface.FillRect(8, 205, 224, 1, theme.panel_border);
  Read(surface, 8, 211, first, theme.text);
  if (!second.empty()) Read(surface, 8, 226, second, kNestMuted);
}

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
std::vector<std::string> Wrap(const std::string& text, const int columns = 24) {
  std::vector<std::string> lines;
  std::istringstream words(PlainText(text));
  std::string line, word;
  while (words >> word) {
    while (word.size() > static_cast<std::size_t>(columns)) {
      if (!line.empty()) { lines.push_back(line); line.clear(); }
      lines.push_back(word.substr(0, columns));
      word.erase(0, columns);
    }
    if (!line.empty() && line.size() + word.size() + 1 > static_cast<std::size_t>(columns)) {
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
  const auto fitted = scale == 2 ? FitLabel(text, 224, 2) : FitRead(text);
  const int width = scale == 2 ? widgets::MeasureTextWidth(fitted, 2) : widgets::MeasureReadableTextWidth(fitted);
  if (scale == 2) Large(surface, (240 - width) / 2, y, fitted, color);
  else Read(surface, (240 - width) / 2, y, fitted, color);
}
void Message(render::Surface240& surface, const render::Theme& theme,
             const std::string& title, const std::string& detail = {}) {
  const auto titles = Wrap(title, 18);
  for (int i = 0; i < std::min(2, static_cast<int>(titles.size())); ++i)
    Center(surface, 54 + i * 20, titles[i], theme.text, 2);
  const auto lines = Wrap(detail);
  for (int i = 0; i < std::min(6, static_cast<int>(lines.size())); ++i)
    Read(surface, 8, 104 + i * 16, lines[i], kNestMuted);
}
void ReadList(render::Surface240& surface, const render::Theme& theme,
              const std::vector<std::string>& rows, const int selected,
              const int y = 48, const int height = 50, const int visible = 3) {
  const int count = static_cast<int>(rows.size());
  if (!count) return;
  const int current = std::clamp(selected, 0, count - 1);
  const int first = std::clamp(current - 1, 0, std::max(0, count - visible));
  for (int row = 0; row < visible && first + row < count; ++row) {
    const int index = first + row, top = y + row * height;
    surface.FillRect(8, top, 220, height - 4, index == current ? theme.accent : kNestPanel);
    if (index == current) surface.FillRect(8, top + 4, 3, height - 12, theme.text);
    const auto lines = Wrap(rows[index], 17);
    for (int line = 0; line < std::min(height >= 44 ? 2 : 1, static_cast<int>(lines.size())); ++line)
      Large(surface, 16, top + 7 + line * 17, lines[line], theme.text, 204);
  }
  if (count > visible) {
    const int track = visible * height - 4, thumb = std::max(12, track * visible / count);
    surface.FillRect(232, y, 2, track, theme.panel_border);
    surface.FillRect(232, y + (track - thumb) * first / (count - visible), 2, thumb, theme.text);
  }
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
                    Pin, Developer, Transfer, Remove, Rating, Artwork };
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
  View art_return = View::Catalog;
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
    const auto source = view == View::Artwork ? art_return : view;
    if (source == View::Installed) {
      const auto games = VisibleInstalled();
      if (games.empty()) return {};
      const auto& local = games[std::clamp(installed_selected, 0, static_cast<int>(games.size()) - 1)];
      if (!local.icon_url.empty()) return local.icon_url;
      for (const auto& entry : cached)
        if (entry.slug == local.id || entry.id == local.id) return entry.icon_url;
      return {};
    }
    if (source == View::Catalog && !catalog.entries.empty())
      game = &catalog.entries[std::clamp(catalog_selected, 0, static_cast<int>(catalog.entries.size()) - 1)];
    else if ((source == View::Detail || source == View::Remove) && manifest) game = &*manifest;
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
    append(game.title);
    append(game.description.empty() ? "No description available." : game.description);
    append("VERSION " + game.version);
    if (game.download.size_bytes) append(Size(game.download.size_bytes));
    append(Rating(game.rating));
    if (!game.author.empty()) append("BY " + game.author);
    if (!game.release_date.empty()) append("RELEASED " + game.release_date.substr(0, 10));
    if (game.rating_count > 0) append(std::to_string(game.rating_count) + " PLAYER RATINGS");
    std::string tags_text;
    for (const auto& tag : game.tags) { if (!tags_text.empty()) tags_text += " / "; tags_text += tag; }
    if (!tags_text.empty()) append(tags_text);
    if (game.mature_content) append("MATURE CONTENT / 18+");
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
    surface.FillRect(bounds.x, bounds.y, bounds.w, bounds.h, kNestBackground);
    surface.StrokeRect(bounds.x, bounds.y, bounds.w, bounds.h, theme.panel_border);
    const auto url = CurrentImage();
    const auto found = image_paths.find(url);
    if (found != image_paths.end() && !found->second.empty()) {
      std::string ignored;
      if (const auto* image = images.LoadImage(found->second, ignored)) {
        render::BlitImageFit(surface, *image, bounds, kNestBackground); return;
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
    if (s.view == View::Artwork) s.view = s.art_return;
    else if (s.view == View::Detail) s.view = s.detail_return;
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
  if (s.view == View::Artwork) {
    int count = 0;
    if (s.art_return == View::Catalog && !s.catalog.entries.empty())
      count = static_cast<int>(s.catalog.entries[s.catalog_selected].screenshot_urls.size());
    else if (s.art_return == View::Detail && s.manifest)
      count = static_cast<int>(s.manifest->screenshot_urls.size());
    if (button == Button::Left || button == Button::Right) {
      s.screenshot = std::clamp(s.screenshot + (button == Button::Right ? 1 : -1), 0, count);
      s.FetchImage();
    }
    return false;
  }
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
    if (button == Button::X) {
      if (!s.error.empty() || s.catalog.entries.empty()) s.FetchCatalog(s.query.page);
      else { s.art_return = s.view; s.view = View::Artwork; s.FetchImage(); }
      return false;
    }
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
    if (delta) {
      s.installed_selected = Move(s.installed_selected, delta, static_cast<int>(games.size()));
      s.message.clear();
      s.FetchImage();
    }
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
        std::max(1, static_cast<int>(s.DetailLines().size()) - kDetailRows + 1));
    if (button == Button::Select) {
      s.art_return = s.view; s.view = View::Artwork; s.FetchImage(); return false;
    }
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
  } else if ((result.kind == Work::Catalog || result.kind == Work::Manifest || result.kind == Work::Installed) && result.error.empty()) s.FetchImage();
  return true;
}

void NestController::Draw(render::Surface240& surface, const render::Theme& theme) const {
  const auto& s = *impl_;
  using View = Impl::View;
  std::string title = "NEST";
  switch (s.view) {
    case View::Installed: title = "INSTALLED"; break;
    case View::Updates: title = "GAME UPDATES"; break;
    case View::Filters: title = "FILTERS"; break;
    case View::Parental: title = "FAMILY FILTER"; break;
    case View::Pin: title = "PARENTAL PIN"; break;
    case View::Developer: title = "LINK ACCOUNT"; break;
    case View::Transfer:
      title = s.Active() ? (s.working == Impl::Work::Uninstall ? "REMOVING" : "INSTALLING") :
          !s.error.empty() ? "REQUEST FAILED" : s.working == Impl::Work::Uninstall ? "REMOVED" : "READY";
      break;
    case View::Remove: title = "REMOVE GAME"; break;
    case View::Rating: title = "RATE GAME"; break;
    default: break;
  }
  Frame(surface, theme, title);
  if (s.external_error) {
    Message(surface, theme, "REQUEST FAILED", s.error);
    Footer(surface, theme, "B Back"); return;
  }
  if (s.Loading()) {
    NestMark(surface, theme, 98, 42, 2);
    Center(surface, 110, "LOADING...", theme.text, 2);
    Center(surface, 150, "Please wait", kNestMuted);
    Footer(surface, theme, "B Back"); return;
  }
  if (s.view == View::Home) {
    Frame(surface, theme, "NEST", std::to_string(s.home_selected + 1) + "/6");
    Read(surface, 8, 33, s.parental_locked ? "Family filter is on" : "Find your next game", kNestMuted);
    ReadList(surface, theme, {"Browse games", "Installed games", "Game updates", "Family filter", "Developer code", "Back to shell"}, s.home_selected);
    Footer(surface, theme, "A Open  B Shell", "UP/DOWN Scroll"); return;
  }
  if (!s.error.empty() && s.view != View::Parental && s.view != View::Pin && s.view != View::Transfer) {
    Message(surface, theme, "REQUEST FAILED", s.error);
    Footer(surface, theme, s.view == View::Catalog || s.view == View::Updates ? "X Retry  B Back" : "B Back"); return;
  }
  const auto featured = [&](const std::string& name) {
    const auto names = Wrap(name, 18);
    const int art_height = names.size() > 1 ? 136 : 152;
    s.DrawArt(surface, theme, {8, 28, 224, art_height});
    for (int row = 0; row < std::min(2, static_cast<int>(names.size())); ++row) {
      std::string line = names[row];
      if (row == 1 && names.size() > 2) line = FitLabel(line + "...", 216, 2);
      Large(surface, 8, 34 + art_height + row * 16, line, theme.text);
    }
  };
  if (s.view == View::Catalog) {
    if (s.catalog.entries.empty()) {
      Message(surface, theme, "NO GAMES FOUND", "Change your filters or connect to Wi-Fi.");
      Footer(surface, theme, "Y Filters  X Retry", "B Back"); return;
    }
    const int selected = std::clamp(s.catalog_selected, 0, static_cast<int>(s.catalog.entries.size()) - 1);
    const auto& game = s.catalog.entries[selected];
    const auto counter = std::string(s.catalog.offline ? "OFFLINE " : "") +
        std::to_string(selected + 1) + "/" + std::to_string(s.catalog.entries.size()) +
        (s.catalog.total_pages > 1 ? " P" + std::to_string(s.query.page) + "/" + std::to_string(s.catalog.total_pages) : "");
    Frame(surface, theme, "NEST", counter);
    featured(game.title);
    if (game.mature_content) {
      surface.FillRect(10, 30, 35, 15, kNestBackground);
      Read(surface, 14, 32, "18+", theme.text);
    }
    Footer(surface, theme, "A Details X Art B Back",
           s.catalog.total_pages > 1 ? "U/D Game Y Filter L/R Pg" : "UP/DOWN Game  Y Filter");
    return;
  }
  if (s.view == View::Artwork) {
    surface.Clear(kNestBackground);
    s.DrawArt(surface, theme, {8, 4, 224, 212});
    int count = 1;
    if (s.art_return == View::Catalog && !s.catalog.entries.empty())
      count += static_cast<int>(s.catalog.entries[s.catalog_selected].screenshot_urls.size());
    else if (s.art_return == View::Detail && s.manifest)
      count += static_cast<int>(s.manifest->screenshot_urls.size());
    Read(surface, 8, 225, "< " + std::to_string(s.screenshot + 1) + "/" + std::to_string(count) + " > Images  B Back", theme.text);
    return;
  }
  if (s.view == View::Detail && s.manifest) {
    const auto& game = *s.manifest;
    const auto* local = s.InstalledDetail();
    Frame(surface, theme, "GAME", local ? (s.HasUpdate() ? "UPDATE" : "INSTALLED") : "");
    if (s.scroll == 0) {
      featured(game.title);
    } else {
      const auto names = Wrap(game.title, 18);
      for (int row = 0; row < std::min(2, static_cast<int>(names.size())); ++row)
        Large(surface, 8, 34 + row * 16, names[row], theme.text);
      const auto lines = s.DetailLines();
      const int first = s.scroll - 1;
      for (int row = 0; row < kDetailRows && first + row < static_cast<int>(lines.size()); ++row)
        Read(surface, 8, 70 + row * 16, lines[first + row], theme.text);
      const int last = std::max(0, static_cast<int>(lines.size()) - kDetailRows);
      if (last > 0) {
        surface.FillRect(235, 70, 2, 123, theme.panel_border);
        const int thumb = std::max(12, 123 * kDetailRows / static_cast<int>(lines.size()));
        surface.FillRect(235, 70 + (123 - thumb) * first / last, 2, thumb, theme.text);
      }
    }
    const auto action = !local ? "A Install" : s.HasUpdate() ? "A Update" : "A Play";
    Footer(surface, theme, action + std::string(local ? " X Remove B Back" : "  B Back"),
           local ? "U/D Info Y Rate SEL Art" : "UP/DOWN Info  SEL Art");
    return;
  }
  if (s.view == View::Installed) {
    const auto games = s.VisibleInstalled();
    if (games.empty()) {
      Message(surface, theme, s.parental_locked && !s.installed.empty() ? "FAMILY FILTER ON" : "NO INSTALLED GAMES",
              s.parental_locked && !s.installed.empty() ? "Unlock the family filter to see your games." : "Browse Nest to find your next game.");
      Footer(surface, theme, "B Back"); return;
    }
    const int selected = std::clamp(s.installed_selected, 0, static_cast<int>(games.size()) - 1);
    Frame(surface, theme, "MY GAMES", std::to_string(selected + 1) + "/" + std::to_string(games.size()));
    featured(games[selected].title);
    Footer(surface, theme, "A Details X Play B Back", s.message.empty() ? "UP/DOWN Game  Y Rate" : s.message); return;
  }
  if (s.view == View::Updates) {
    if (s.updates.empty()) {
      Message(surface, theme, "ALL UP TO DATE", "Your installed games are current.");
      Footer(surface, theme, "X Check again  B Back"); return;
    }
    Frame(surface, theme, "UPDATES", std::to_string(s.update_selected + 1) + "/" + std::to_string(s.updates.size()));
    std::vector<std::string> rows;
    for (const auto& item : s.updates) rows.push_back(item.title);
    ReadList(surface, theme, rows, s.update_selected, 40);
    const auto& item = s.updates[std::clamp(s.update_selected, 0, static_cast<int>(s.updates.size()) - 1)];
    Read(surface, 8, 193, item.installed_version + " > " + item.latest_version, kNestMuted);
    Footer(surface, theme, "A Details  B Back", "UP/DOWN Scroll  X Check"); return;
  }
  if (s.view == View::Filters) {
    const int count = static_cast<int>(s.draft_tags.size()) + (s.draft_rating ? 1 : 0);
    Read(surface, 8, 33, std::to_string(count) + " selected", kNestMuted);
    std::vector<std::string> rows{"Apply filters"};
    for (int value = 3; value <= 5; ++value)
      rows.push_back(std::string(s.draft_rating == value ? "[X] " : "[ ] ") + std::to_string(value) + (value < 5 ? "+ stars" : " stars"));
    for (const auto& tag : s.tags)
      rows.push_back(std::string(std::find(s.draft_tags.begin(), s.draft_tags.end(), tag.id) != s.draft_tags.end() ? "[X] " : "[ ] ") + tag.name);
    ReadList(surface, theme, rows, s.filter_selected);
    Footer(surface, theme, "A Toggle START Apply", "UP/DOWN Scroll X Clear"); return;
  }
  if (s.view == View::Parental) {
    Read(surface, 8, 34, s.parental_enabled ? (s.parental_locked ? "Filter on" : "Unlocked this visit") : "No PIN set", theme.text);
    const auto copy = !s.error.empty() ? s.error : !s.message.empty() ? s.message :
        s.parental_enabled ? "Mature games stay hidden until you enter the PIN. Leaving Nest locks it." : "Set a four digit PIN to hide mature games.";
    const auto explanation = Wrap(copy);
    for (int i = 0; i < std::min(3, static_cast<int>(explanation.size())); ++i)
      Read(surface, 8, 56 + 16 * i, explanation[i], kNestMuted);
    std::vector<std::string> rows;
    if (!s.parental_enabled) rows = {"Set parental PIN", "Back"};
    else if (s.parental_locked) rows = {"Unlock with PIN", "Back"};
    else rows = {"Lock now", "Remove PIN", "Back"};
    ReadList(surface, theme, rows, s.parental_selected, 112, 30);
    Footer(surface, theme, "A Select  B Back"); return;
  }
  if (s.view == View::Pin) {
    Center(surface, 42, s.confirming_pin ? "Confirm your PIN" : s.pin_action == "set" ? "Choose four digits" : "Enter your PIN", theme.text);
    const auto& digits = s.confirming_pin ? s.confirmation : s.pin;
    for (int i = 0; i < 4; ++i) {
      const int x = 22 + i * 50;
      surface.FillRect(x, 74, 44, 54, i == s.pin_cursor ? theme.accent : kNestPanel);
      surface.StrokeRect(x, 74, 44, 54, i == s.pin_cursor ? theme.text : theme.panel_border);
      Large(surface, x + 16, 94, std::to_string(digits[i]), theme.text);
    }
    Center(surface, 148, "UP/DOWN Change digit", kNestMuted);
    Center(surface, 170, "LEFT/RIGHT Move", kNestMuted);
    if (!s.error.empty()) Read(surface, 8, 190, s.error, theme.text);
    Footer(surface, theme, "A Confirm  B Cancel"); return;
  }
  if (s.view == View::Developer) {
    Center(surface, 48, "Your linking code", kNestMuted);
    surface.FillRect(8, 76, 224, 48, kNestPanel);
    Center(surface, 93, s.code.empty() ? "NO CODE" : s.code, theme.text, 2);
    const auto lines = Wrap("Enter this code on the Game Bird website to link your account.");
    for (int i = 0; i < std::min(4, static_cast<int>(lines.size())); ++i)
      Read(surface, 8, 142 + i * 16, lines[i], kNestMuted);
    Footer(surface, theme, "B Back"); return;
  }
  if (s.view == View::Transfer) {
    const auto name = Wrap(s.manifest ? s.manifest->title : "Game Bird Nest", 18);
    for (int i = 0; i < std::min(2, static_cast<int>(name.size())); ++i)
      Large(surface, 8, 38 + i * 18, name[i], theme.text);
    Center(surface, 84, std::to_string(s.progress_percent) + "%", theme.text, 2);
    surface.FillRect(8, 111, 224, 18, kNestPanel);
    surface.FillRect(10, 113, 220 * std::clamp(s.progress_percent, 0, 100) / 100, 14,
                     s.transfer_success ? theme.success : theme.accent);
    const auto status = s.Active() ? s.progress_stage : !s.error.empty() ? "Could not finish" :
        s.working == Impl::Work::Uninstall ? "Game removed" : "Ready to play";
    const auto lines = Wrap(s.error.empty() ? status : s.error);
    for (int i = 0; i < std::min(4, static_cast<int>(lines.size())); ++i)
      Read(surface, 8, 141 + i * 16, lines[i], i == 0 ? theme.text : kNestMuted);
    Footer(surface, theme, s.Active() ? "B Cancel" : "A Continue  B Back"); return;
  }
  if (s.view == View::Remove) {
    Message(surface, theme, "REMOVE THIS GAME?", s.manifest ? s.manifest->title : "");
    Read(surface, 8, 171, "Your saves are kept.", kNestMuted);
    Footer(surface, theme, "A Remove  B Keep game"); return;
  }
  if (s.view == View::Rating) {
    const auto names = Wrap(s.rating_title, 18);
    for (int i = 0; i < std::min(2, static_cast<int>(names.size())); ++i)
      Large(surface, 8, 38 + i * 18, names[i], theme.text);
    Center(surface, 87, "How was your game?", kNestMuted);
    Stars(surface, theme, 42, 113, s.pending_rating, 2);
    Center(surface, 153, std::to_string(s.pending_rating) + " / 5", theme.text, 2);
    Footer(surface, theme, "A Save  B Cancel", "LEFT/RIGHT Choose"); return;
  }
  Message(surface, theme, "NO GAME SELECTED");
  Footer(surface, theme, "B Back");
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
