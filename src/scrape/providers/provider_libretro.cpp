#include "scrape/providers/provider_libretro.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "platform/proc.h"

namespace gb::scrape::providers {
namespace {

constexpr const char* kThumbBase = "https://thumbnails.libretro.com/";
constexpr const char* kDatabaseBase =
    "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/";

std::string CatalogForSystem(const std::string& system_id) {
  if (system_id == "snes") {
    return "Nintendo - Super Nintendo Entertainment System";
  }
  if (system_id == "nes") {
    return "Nintendo - Nintendo Entertainment System";
  }
  if (system_id == "gb") {
    return "Nintendo - Game Boy";
  }
  if (system_id == "gbc") {
    return "Nintendo - Game Boy Color";
  }
  if (system_id == "gba") {
    return "Nintendo - Game Boy Advance";
  }
  if (system_id == "genesis" || system_id == "megadrive") {
    return "Sega - Mega Drive - Genesis";
  }
  if (system_id == "mastersystem") {
    return "Sega - Master System - Mark III";
  }
  if (system_id == "gamegear") {
    return "Sega - Game Gear";
  }
  if (system_id == "n64") {
    return "Nintendo - Nintendo 64";
  }
  if (system_id == "psx" || system_id == "ps1") {
    return "Sony - PlayStation";
  }
  if (system_id == "atari2600") {
    return "Atari - 2600";
  }
  return {};
}

std::string UrlEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[(c >> 4) & 0xf]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  return out;
}

int HexDigit(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::string UrlDecode(const std::string& value) {
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      const int hi = HexDigit(value[i + 1]);
      const int lo = HexDigit(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i] == '+' ? ' ' : value[i]);
  }
  return out;
}

std::string StripExtension(std::string value) {
  if (value.size() >= 4 && value.substr(value.size() - 4) == ".png") {
    value.resize(value.size() - 4);
  }
  return value;
}

std::string CoreTitle(const std::string& value) {
  std::string out;
  int paren_depth = 0;
  int bracket_depth = 0;
  for (const unsigned char c : value) {
    if (c == '(') {
      ++paren_depth;
      continue;
    }
    if (c == ')' && paren_depth > 0) {
      --paren_depth;
      continue;
    }
    if (c == '[') {
      ++bracket_depth;
      continue;
    }
    if (c == ']' && bracket_depth > 0) {
      --bracket_depth;
      continue;
    }
    if (paren_depth == 0 && bracket_depth == 0 && std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return out;
}

std::unordered_set<std::string> Tokens(const std::string& value) {
  std::unordered_set<std::string> out;
  std::string token;
  for (const unsigned char c : value) {
    if (std::isalnum(c)) {
      token.push_back(static_cast<char>(std::tolower(c)));
    } else if (!token.empty()) {
      out.insert(token);
      token.clear();
    }
  }
  if (!token.empty()) out.insert(token);
  return out;
}

bool ContainsInsensitive(std::string haystack, std::string needle) {
  std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(needle.begin(), needle.end(), needle.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return haystack.find(needle) != std::string::npos;
}

int MatchScore(const std::string& query, const std::string& candidate) {
  const std::string query_core = CoreTitle(query);
  const std::string candidate_core = CoreTitle(candidate);
  int score = 0;
  if (!query_core.empty() && query_core == candidate_core) {
    score = 1000;
  } else if (!query_core.empty() &&
             (candidate_core.find(query_core) != std::string::npos ||
              query_core.find(candidate_core) != std::string::npos)) {
    score = 700;
  } else {
    const auto q = Tokens(query);
    const auto c = Tokens(candidate);
    int common = 0;
    for (const auto& token : q) {
      if (token.size() > 1 && c.contains(token)) ++common;
    }
    score = common * 60 - static_cast<int>(c.size() - std::min(c.size(), q.size())) * 3;
  }

  const bool query_ju = ContainsInsensitive(query, "(ju)") ||
                        ContainsInsensitive(query, "[ju]");
  const bool query_u = query_ju || ContainsInsensitive(query, "(u)") ||
                       ContainsInsensitive(query, "(usa)");
  const bool query_j = query_ju || ContainsInsensitive(query, "(j)") ||
                       ContainsInsensitive(query, "(japan)");
  const bool query_e = ContainsInsensitive(query, "(e)") ||
                       ContainsInsensitive(query, "(europe)");
  if (query_u && ContainsInsensitive(candidate, "usa")) score += 25;
  if (query_j && ContainsInsensitive(candidate, "japan")) score += 20;
  if (query_e && ContainsInsensitive(candidate, "europe")) score += 25;
  if (ContainsInsensitive(candidate, "virtual console") ||
      ContainsInsensitive(candidate, "switch online") ||
      ContainsInsensitive(candidate, "classic mini")) {
    score -= 15;
  }
  return score;
}

std::string RegexEscape(const std::string& value) {
  static const std::regex special(R"([.^$|()\[\]{}*+?\\])");
  return std::regex_replace(value, special, R"(\$&)");
}

bool IsPng(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  unsigned char magic[8]{};
  in.read(reinterpret_cast<char*>(magic), sizeof(magic));
  static constexpr unsigned char expected[8] = {
      0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  return in.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
         std::equal(std::begin(magic), std::end(magic), std::begin(expected));
}

}  // namespace

std::vector<LibretroArtworkCandidate> ParseLibretroThumbnailIndex(
    const std::string& html) {
  std::vector<LibretroArtworkCandidate> out;
  const std::regex href_re("href=\\\"([^\\\"]+\\.png)\\\"", std::regex::icase);
  for (auto it = std::sregex_iterator(html.begin(), html.end(), href_re);
       it != std::sregex_iterator(); ++it) {
    LibretroArtworkCandidate row;
    row.encoded_filename = (*it)[1].str();
    row.title = StripExtension(UrlDecode(row.encoded_filename));
    if (!row.title.empty()) out.push_back(std::move(row));
  }
  return out;
}

LibretroArtworkCandidate SelectBestLibretroArtwork(
    const std::string& rom_title,
    const std::vector<LibretroArtworkCandidate>& candidates) {
  LibretroArtworkCandidate best;
  for (auto candidate : candidates) {
    candidate.score = MatchScore(rom_title, candidate.title);
    if (candidate.score > best.score) best = std::move(candidate);
  }
  if (best.score < 300) return {};
  return best;
}

std::string ParseLibretroMetadataValue(const std::string& dat,
                                       const std::string& matched_title,
                                       const std::string& field) {
  const std::regex comment_re("comment\\s+\\\"" + RegexEscape(matched_title) +
                              "\\\"", std::regex::icase);
  std::smatch match;
  if (!std::regex_search(dat, match, comment_re)) return {};
  const std::size_t start = static_cast<std::size_t>(match.position());
  const std::size_t end = dat.find("\n)", start);
  const std::string block = dat.substr(start, end == std::string::npos ? 1024 : end - start);
  const std::regex quoted_re("(?:^|\\n)\\s*" + RegexEscape(field) +
                             "\\s+\\\"([^\\\"]*)\\\"", std::regex::icase);
  if (std::regex_search(block, match, quoted_re) && match.size() > 1) {
    return match[1].str();
  }
  const std::regex number_re("(?:^|\\n)\\s*" + RegexEscape(field) +
                             "\\s+([0-9]+)", std::regex::icase);
  if (std::regex_search(block, match, number_re) && match.size() > 1) {
    return match[1].str();
  }
  return {};
}

bool LibretroProvider::FetchText(const std::string& url,
                                 std::string& text,
                                 std::string& error) {
  const auto result = platform::RunProcessCapture(
      {"/usr/bin/curl", "--location", "--fail", "--silent", "--show-error",
       "--connect-timeout", "8", "--max-time", "30", "--user-agent",
       "GameBird-Shell/1.0", url});
  if (!result.process.exited_normally || result.process.exit_code != 0) {
    error = result.output.empty() ? result.process.error : result.output;
    return false;
  }
  text = result.output;
  return !text.empty();
}

bool LibretroProvider::DownloadFile(const std::string& url,
                                    const std::string& destination_path,
                                    std::string& error) {
  std::error_code ec;
  const std::filesystem::path destination(destination_path);
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    error = "could not create artwork directory: " + ec.message();
    return false;
  }
  const std::string temporary = destination_path + ".download";
  const auto result = platform::RunProcessBlocking(
      {"/usr/bin/curl", "--location", "--fail", "--silent", "--show-error",
       "--connect-timeout", "8", "--max-time", "45", "--user-agent",
       "GameBird-Shell/1.0", "--output", temporary, url});
  if (!result.exited_normally || result.exit_code != 0 || !IsPng(temporary)) {
    std::filesystem::remove(temporary, ec);
    error = result.error.empty() ? "artwork download failed" : result.error;
    return false;
  }
  std::filesystem::rename(temporary, destination, ec);
  if (ec) {
    std::filesystem::remove(destination, ec);
    ec.clear();
    std::filesystem::rename(temporary, destination, ec);
  }
  if (ec) {
    error = "could not install artwork: " + ec.message();
    return false;
  }
  return true;
}

bool LibretroProvider::LoadIndex(
    const std::string& system_id,
    std::string& catalog_name,
    std::vector<LibretroArtworkCandidate>& candidates,
    std::string& error) {
  catalog_name = CatalogForSystem(system_id);
  if (catalog_name.empty()) {
    error = "unsupported system: " + system_id;
    return false;
  }
  if (cached_system_id_ == system_id && !cached_candidates_.empty()) {
    catalog_name = cached_catalog_name_;
    candidates = cached_candidates_;
    return true;
  }
  std::string html;
  const std::string url = std::string(kThumbBase) + UrlEncode(catalog_name) +
                          "/Named_Boxarts/";
  if (!FetchText(url, html, error)) return false;
  candidates = ParseLibretroThumbnailIndex(html);
  if (candidates.empty()) {
    error = "thumbnail catalog contained no PNG files";
    return false;
  }
  cached_system_id_ = system_id;
  cached_catalog_name_ = catalog_name;
  cached_candidates_ = candidates;
  metadata_cache_.clear();
  return true;
}

std::string LibretroProvider::MetadataValue(const std::string& catalog_name,
                                             const std::string& kind,
                                             const std::string& matched_title) {
  auto found = std::find_if(metadata_cache_.begin(), metadata_cache_.end(),
                            [&](const auto& item) { return item.first == kind; });
  if (found == metadata_cache_.end()) {
    std::string dat;
    std::string ignored;
    const std::string url = std::string(kDatabaseBase) + kind + "/" +
                            UrlEncode(catalog_name) + ".dat";
    FetchText(url, dat, ignored);
    metadata_cache_.emplace_back(kind, std::move(dat));
    found = std::prev(metadata_cache_.end());
  }
  const std::string field = kind == "maxusers" ? "users" : kind;
  return ParseLibretroMetadataValue(found->second, matched_title, field);
}

LibretroScrapeResult LibretroProvider::Scrape(
    const std::string& system_id,
    const std::string& rom_title,
    const std::string& destination_path) {
  LibretroScrapeResult result;
  std::string catalog;
  std::vector<LibretroArtworkCandidate> candidates;
  if (!LoadIndex(system_id, catalog, candidates, result.error)) return result;

  const auto best = SelectBestLibretroArtwork(rom_title, candidates);
  if (best.title.empty()) {
    result.error = "no confident artwork match for " + rom_title;
    return result;
  }
  result.matched = true;
  result.matched_title = best.title;
  result.artwork_url = std::string(kThumbBase) + UrlEncode(catalog) +
                       "/Named_Boxarts/" + best.encoded_filename;
  result.artwork_path = destination_path;
  if (!DownloadFile(result.artwork_url, destination_path, result.error)) return result;
  result.downloaded = true;

  result.metadata.developer = MetadataValue(catalog, "developer", best.title);
  result.metadata.publisher = MetadataValue(catalog, "publisher", best.title);
  result.metadata.genre = MetadataValue(catalog, "genre", best.title);
  const std::string year = MetadataValue(catalog, "releaseyear", best.title);
  const std::string users = MetadataValue(catalog, "maxusers", best.title);
  if (!year.empty()) result.metadata.release_year = std::atoi(year.c_str());
  if (!users.empty()) result.metadata.players = std::atoi(users.c_str());
  return result;
}

}  // namespace gb::scrape::providers
