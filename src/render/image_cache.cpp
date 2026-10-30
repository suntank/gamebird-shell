#include "render/image_cache.h"

#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>

#include <png.h>
#include <jpeglib.h>

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
  image.alpha.resize(image.pixels.size());
  for (int y = 0; y < image.height; ++y) {
    const auto source_y = std::min<png_uint_32>(
        height - 1, static_cast<png_uint_32>(y * scale));
    const png_bytep row = rows[source_y];
    for (int x = 0; x < image.width; ++x) {
      const auto source_x = std::min<png_uint_32>(
          width - 1, static_cast<png_uint_32>(x * scale));
      const png_bytep pixel = row + static_cast<std::size_t>(source_x) * 4;
      const auto index = static_cast<std::size_t>(y) * image.width + x;
      image.pixels[index] = Rgb565(pixel[0], pixel[1], pixel[2]);
      image.alpha[index] = pixel[3];
    }
  }
  if (std::all_of(image.alpha.begin(), image.alpha.end(),
                  [](const auto alpha) { return alpha == 255; })) image.alpha.clear();
  return true;
}

struct JpegError {
  jpeg_error_mgr handler{};
  std::jmp_buf jump;
};
struct JpegReader {
  jpeg_decompress_struct decoder{};
  JpegError error;
  std::vector<JSAMPLE> row;
  bool initialized = false;
  ~JpegReader() { if (initialized) jpeg_destroy_decompress(&decoder); }
};
void JpegFailure(j_common_ptr decoder) {
  auto* error = reinterpret_cast<JpegError*>(decoder->err);
  std::longjmp(error->jump, 1);
}
void JpegQuiet(j_common_ptr) {}

bool DecodeJpeg(const std::string& path, CachedImage& image, std::string& error) {
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path.c_str(), "rb"),
                                                    &std::fclose);
  if (!file) { error = "cannot open JPEG"; return false; }
  // Objects modified by libjpeg live on the heap so its C error jump never
  // skips a C++ destructor or leaves modified automatic objects indeterminate.
  auto state = std::make_unique<JpegReader>();
  state->decoder.err = jpeg_std_error(&state->error.handler);
  state->error.handler.error_exit = JpegFailure;
  state->error.handler.output_message = JpegQuiet;
  if (setjmp(state->error.jump) != 0) {
    image = {};
    error = "invalid JPEG data";
    return false;
  }
  state->initialized = true;
  jpeg_create_decompress(&state->decoder);
  jpeg_stdio_src(&state->decoder, file.get());
  if (jpeg_read_header(&state->decoder, TRUE) != JPEG_HEADER_OK) {
    error = "invalid JPEG header"; return false;
  }
  if (state->decoder.image_width == 0 || state->decoder.image_height == 0 ||
      state->decoder.image_width > kMaxDecodedDimension ||
      state->decoder.image_height > kMaxDecodedDimension ||
      static_cast<std::size_t>(state->decoder.image_width) *
          state->decoder.image_height > kMaxDecodedPixels) {
    error = "JPEG dimensions exceed cache limits"; return false;
  }
  state->decoder.out_color_space = JCS_RGB;
  state->decoder.scale_num = 1;
  state->decoder.scale_denom = 1;
  while (state->decoder.scale_denom < 8 &&
         std::max(state->decoder.image_width, state->decoder.image_height) /
             state->decoder.scale_denom > kCacheDimension * 2)
    state->decoder.scale_denom *= 2;
  if (!jpeg_start_decompress(&state->decoder) || state->decoder.output_components != 3) {
    error = "unsupported JPEG color format"; return false;
  }
  const auto width = state->decoder.output_width;
  const auto height = state->decoder.output_height;
  const double scale = std::max(1.0, std::max(static_cast<double>(width) / kCacheDimension,
                                            static_cast<double>(height) / kCacheDimension));
  image.width = std::max(1, static_cast<int>(width / scale));
  image.height = std::max(1, static_cast<int>(height / scale));
  image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
  state->row.resize(static_cast<std::size_t>(width) * 3);
  int destination_y = 0;
  while (state->decoder.output_scanline < height) {
    const auto source_y = state->decoder.output_scanline;
    JSAMPROW row = state->row.data();
    if (jpeg_read_scanlines(&state->decoder, &row, 1) != 1) {
      image = {}; error = "incomplete JPEG data"; return false;
    }
    while (destination_y < image.height &&
           static_cast<JDIMENSION>(destination_y * scale) == source_y) {
      for (int x = 0; x < image.width; ++x) {
        const auto source_x = std::min(width - 1, static_cast<JDIMENSION>(x * scale));
        const auto* pixel = state->row.data() + static_cast<std::size_t>(source_x) * 3;
        image.pixels[static_cast<std::size_t>(destination_y) * image.width + x] =
            Rgb565(pixel[0], pixel[1], pixel[2]);
      }
      ++destination_y;
    }
  }
  if (!jpeg_finish_decompress(&state->decoder)) {
    image = {}; error = "incomplete JPEG data"; return false;
  }
  return true;
}

bool DecodeImage(const std::string& path, CachedImage& image, std::string& error) {
  std::array<unsigned char, 8> signature{};
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) { error = "cannot open artwork"; return false; }
  const auto length = std::fread(signature.data(), 1, signature.size(), file);
  std::fclose(file);
  if (length == signature.size() && png_sig_cmp(signature.data(), 0, signature.size()) == 0)
    return DecodePng(path, image, error);
  if (length >= 3 && signature[0] == 0xff && signature[1] == 0xd8 && signature[2] == 0xff)
    return DecodeJpeg(path, image, error);
  error = "unsupported artwork format (PNG/JPEG required)";
  return false;
}

}  // namespace

ImageCache::ImageCache(const std::size_t max_entries) : max_entries_(max_entries) {}

const CachedImage* ImageCache::LoadPng(const std::string& path, std::string& error) {
  return LoadImage(path, error);
}

const CachedImage* ImageCache::LoadImage(const std::string& path, std::string& error) {
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
  if (!DecodeImage(path, entry.image, entry.error)) {
    entry.image = {};
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
                  const std::optional<std::uint16_t> background,
                  const bool trim_transparent) {
  if (background) surface.FillRect(bounds.x, bounds.y, bounds.w, bounds.h, *background);
  if (image.width <= 0 || image.height <= 0 || image.pixels.empty() || bounds.w <= 0 ||
      bounds.h <= 0) {
    return;
  }
  Rect source{0, 0, image.width, image.height};
  std::vector<std::uint16_t> cropped_pixels;
  std::vector<std::uint8_t> cropped_alpha;
  const auto* pixels = image.pixels.data();
  const auto* alpha = image.alpha.empty() ? nullptr : image.alpha.data();
  if (alpha && trim_transparent) {
    int left = image.width, top = image.height, right = -1, bottom = -1;
    for (int y = 0; y < image.height; ++y) {
      for (int x = 0; x < image.width; ++x) {
        if (image.alpha[static_cast<std::size_t>(y) * image.width + x] == 0) continue;
        left = std::min(left, x); top = std::min(top, y);
        right = std::max(right, x); bottom = std::max(bottom, y);
      }
    }
    if (right < left) return;
    source = {left, top, right - left + 1, bottom - top + 1};
    cropped_pixels.resize(static_cast<std::size_t>(source.w) * source.h);
    cropped_alpha.resize(cropped_pixels.size());
    for (int y = 0; y < source.h; ++y) {
      for (int x = 0; x < source.w; ++x) {
        const auto index = static_cast<std::size_t>(y + source.y) * image.width + x + source.x;
        const auto destination = static_cast<std::size_t>(y) * source.w + x;
        cropped_pixels[destination] = image.pixels[index];
        cropped_alpha[destination] = image.alpha[index];
      }
    }
    pixels = cropped_pixels.data();
    alpha = cropped_alpha.data();
  }
  const double scale = std::min(static_cast<double>(bounds.w) / source.w,
                                static_cast<double>(bounds.h) / source.h);
  const int width = std::max(1, static_cast<int>(source.w * scale));
  const int height = std::max(1, static_cast<int>(source.h * scale));
  surface.BlitScaled(pixels, source.w, source.h,
                     bounds.x + (bounds.w - width) / 2,
                     bounds.y + (bounds.h - height) / 2, width, height, alpha);
}

}  // namespace gb::render
