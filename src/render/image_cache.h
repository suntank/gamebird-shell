#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/surface_240.h"

namespace gb::render {

struct CachedImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint16_t> pixels;
  // Empty for opaque images; PNG transparency is composited at draw time.
  std::vector<std::uint8_t> alpha;
};

class ImageCache {
 public:
  explicit ImageCache(std::size_t max_entries = 8);

  // Detects PNG/JPEG from file bytes, including images with cached .png names.
  const CachedImage* LoadImage(const std::string& path, std::string& error);
  const CachedImage* LoadPng(const std::string& path, std::string& error);
  void Clear();

 private:
  struct Entry {
    CachedImage image;
    std::string error;
    std::uint64_t last_used = 0;
  };

  std::size_t max_entries_;
  std::uint64_t use_counter_ = 0;
  std::unordered_map<std::string, Entry> entries_;
};

void BlitImageFit(Surface240& surface,
                  const CachedImage& image,
                  Rect bounds,
                  std::uint16_t background,
                  bool trim_transparent = false);

}  // namespace gb::render
