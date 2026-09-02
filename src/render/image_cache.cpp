#include "render/image_cache.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>

#include <png.h>

namespace gb::render {

namespace {

constexpr int kMaxDecodedDimension = 4096;
constexpr std::size_t kMaxDecodedPixels = 4U * 1024U * 1024U;
constexpr int kCacheDimension = 240;

bool DecodePng(const std::string& path, CachedImage& image, std::string& error) {
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.c_str(), "rb"),
                                                       &std::fclose);
  if (!file) {
    error = "cannot open PNG";
    return false;
  }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) {
    error = "cannot initialize PNG reader";
    return false;
  }
  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    error = "cannot initialize PNG metadata";
    return false;
  }
  if (setjmp(png_jmpbuf(png)) != 0) {
    png_destroy_read_struct(&png, &info, nullptr);
    error = "invalid PNG data";
    return false;
  }

  png_init_io(png, file.get());
  png_read_info(png, info);
  png_uint_32 width = 0;
  png_uint_32 height = 0;
  int bit_depth = 0;
  int color_type = 0;
  png_get_IHDR(png, info, &width, &height, &bit_depth, &color_type, nullptr,
               nullptr, nullptr);
  if (width == 0 || height == 0 || width > kMaxDecodedDimension ||
      height > kMaxDecodedDimension ||
      static_cast<std::size_t>(width) * height > kMaxDecodedPixels) {
    png_destroy_read_struct(&png, &info, nullptr);
    error = "PNG dimensions exceed cache limits";
    return false;
  }

  if (bit_depth == 16) {
    png_set_strip_16(png);
  }
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_palette_to_rgb(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) {
    png_set_tRNS_to_alpha(png);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }
  if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 &&
      png_get_valid(png, info, PNG_INFO_tRNS) == 0) {
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
  }
  png_read_update_info(png, info);

  const png_size_t row_bytes = png_get_rowbytes(png, info);
  if (row_bytes == 0 || row_bytes >
                            std::numeric_limits<std::size_t>::max() / height) {
    png_destroy_read_struct(&png, &info, nullptr);
    error = "PNG row data is invalid";
    return false;
  }
  std::vector<png_byte> source(row_bytes * height);
  std::vector<png_bytep> rows(height);
  for (png_uint_32 y = 0; y < height; ++y) {
    rows[y] = source.data() + static_cast<std::size_t>(y) * row_bytes;
  }
  png_read_image(png, rows.data());
  png_read_end(png, nullptr);
  png_destroy_read_struct(&png, &info, nullptr);

  const double scale = std::max(1.0, std::max(static_cast<double>(width) / kCacheDimension,
                                               static_cast<double>(height) / kCacheDimension));
  image.width = std::max(1, static_cast<int>(width / scale));
  image.height = std::max(1, static_cast<int>(height / scale));
  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
  for (int y = 0; y < image.height; ++y) {
    const auto source_y = std::min<png_uint_32>(
        height - 1, static_cast<png_uint_32>(y * scale));
    const png_bytep row = rows[source_y];
    for (int x = 0; x < image.width; ++x) {
      const auto source_x = std::min<png_uint_32>(
          width - 1, static_cast<png_uint_32>(x * scale));
      const png_bytep pixel = row + static_cast<std::size_t>(source_x) * 4;
      const std::uint16_t r = static_cast<std::uint16_t>(pixel[0] * pixel[3] / 255);
      const std::uint16_t g = static_cast<std::uint16_t>(pixel[1] * pixel[3] / 255);
      const std::uint16_t b = static_cast<std::uint16_t>(pixel[2] * pixel[3] / 255);
      image.pixels[static_cast<std::size_t>(y) * image.width + x] =
          Rgb565(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                 static_cast<std::uint8_t>(b));
    }
  }
  return true;
}

}  // namespace

ImageCache::ImageCache(const std::size_t max_entries) : max_entries_(max_entries) {}

const CachedImage* ImageCache::LoadPng(const std::string& path, std::string& error) {
  error.clear();
  if (path.empty()) {
    error = "no artwork path";
    return nullptr;
  }
  if (auto it = entries_.find(path); it != entries_.end()) {
    it->second.last_used = ++use_counter_;
    error = it->second.error;
    return it->second.image.pixels.empty() ? nullptr : &it->second.image;
  }

  if (max_entries_ > 0 && entries_.size() >= max_entries_) {
    const auto victim = std::min_element(
        entries_.begin(), entries_.end(),
        [](const auto& left, const auto& right) {
          return left.second.last_used < right.second.last_used;
        });
    if (victim != entries_.end()) {
      entries_.erase(victim);
    }
  }

  Entry entry;
  entry.last_used = ++use_counter_;
  if (!DecodePng(path, entry.image, entry.error)) {
    error = entry.error;
  }
  const auto [it, inserted] = entries_.emplace(path, std::move(entry));
  (void)inserted;
  return it->second.image.pixels.empty() ? nullptr : &it->second.image;
}

void ImageCache::Clear() { entries_.clear(); }

void BlitImageFit(Surface240& surface,
                  const CachedImage& image,
                  const Rect bounds,
                  const std::uint16_t background) {
  surface.FillRect(bounds.x, bounds.y, bounds.w, bounds.h, background);
  if (image.width <= 0 || image.height <= 0 || image.pixels.empty() || bounds.w <= 0 ||
      bounds.h <= 0) {
    return;
  }
  const double scale = std::min(static_cast<double>(bounds.w) / image.width,
                                static_cast<double>(bounds.h) / image.height);
  const int width = std::max(1, static_cast<int>(image.width * scale));
  const int height = std::max(1, static_cast<int>(image.height * scale));
  surface.BlitScaled(image.pixels.data(), image.width, image.height,
                     bounds.x + (bounds.w - width) / 2,
                     bounds.y + (bounds.h - height) / 2, width, height);
}

}  // namespace gb::render
