#pragma once

#include <string>
#include <vector>

namespace gb::scrape::providers {

struct LibretroArtworkCandidate {
  std::string title;
  std::string encoded_filename;
  int score = 0;
};

struct LibretroGameMetadata {
  int release_year = 0;
  std::string publisher;
  std::string developer;
  std::string genre;
  int players = 0;
};

struct LibretroScrapeResult {
  bool matched = false;
  bool downloaded = false;
  std::string matched_title;
  std::string artwork_url;
  std::string artwork_path;
  LibretroGameMetadata metadata;
  std::string error;
};

// Exposed for deterministic parser and matching tests.
std::vector<LibretroArtworkCandidate> ParseLibretroThumbnailIndex(
    const std::string& html);
LibretroArtworkCandidate SelectBestLibretroArtwork(
    const std::string& rom_title,
    const std::vector<LibretroArtworkCandidate>& candidates);
std::string ParseLibretroMetadataValue(const std::string& dat,
                                       const std::string& matched_title,
                                       const std::string& field);

class LibretroProvider {
 public:
  LibretroScrapeResult Scrape(const std::string& system_id,
                              const std::string& rom_title,
                              const std::string& destination_path);

 private:
  bool FetchText(const std::string& url, std::string& text, std::string& error);
  bool DownloadFile(const std::string& url,
                    const std::string& destination_path,
                    std::string& error);
  bool LoadIndex(const std::string& system_id,
                 std::string& catalog_name,
                 std::vector<LibretroArtworkCandidate>& candidates,
                 std::string& error);
  std::string MetadataValue(const std::string& catalog_name,
                            const std::string& kind,
                            const std::string& matched_title);

  std::string cached_system_id_;
  std::string cached_catalog_name_;
  std::vector<LibretroArtworkCandidate> cached_candidates_;
  std::vector<std::pair<std::string, std::string>> metadata_cache_;
};

}  // namespace gb::scrape::providers
