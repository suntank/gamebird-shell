#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <png.h>
#include <jpeglib.h>
#include <unistd.h>

#include "render/image_cache.h"

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool WritePng(const std::filesystem::path& path, const bool transparent = false) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (png == nullptr || info == nullptr || setjmp(png_jmpbuf(png)) != 0) {
    if (png != nullptr) {
      png_destroy_write_struct(&png, info == nullptr ? nullptr : &info);
    }
    std::fclose(file);
    return false;
  }
  png_init_io(png, file);
  png_set_IHDR(png, info, 2, 1, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  png_byte pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
  if (transparent) { pixels[3] = 0; pixels[7] = 128; }
  png_bytep row = pixels;
  png_write_row(png, row);
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  std::fclose(file);
  return true;
}

bool WriteJpeg(const std::filesystem::path& path, int width, int height) {
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  jpeg_compress_struct jpeg{};
  jpeg_error_mgr error{};
  jpeg.err = jpeg_std_error(&error);
  jpeg_create_compress(&jpeg);
  jpeg_stdio_dest(&jpeg, file);
  jpeg.image_width = width; jpeg.image_height = height;
  jpeg.input_components = 3; jpeg.in_color_space = JCS_RGB;
  jpeg_set_defaults(&jpeg);
  jpeg_set_quality(&jpeg, 100, TRUE);
  jpeg_start_compress(&jpeg, TRUE);
  std::vector<JSAMPLE> pixels(static_cast<std::size_t>(width) * 3);
  for (int x = 0; x < width; ++x) { pixels[x * 3] = 240; pixels[x * 3 + 1] = 48; pixels[x * 3 + 2] = 32; }
  JSAMPROW row = pixels.data();
  while (jpeg.next_scanline < jpeg.image_height) jpeg_write_scanlines(&jpeg, &row, 1);
  jpeg_finish_compress(&jpeg);
  jpeg_destroy_compress(&jpeg);
  std::fclose(file);
  return true;
}

}  // namespace

int main() {
  const auto temp = std::filesystem::temp_directory_path() /
                    ("gamebird-image-cache-" + std::to_string(::getpid()));
  std::filesystem::create_directories(temp);
  const auto png_path = temp / "cover.png";
  Expect(WritePng(png_path), "write PNG fixture");

  gb::render::ImageCache cache(2);
  std::string error;
  const auto* image = cache.LoadPng(png_path.string(), error);
  Expect(image != nullptr && error.empty(), "decode PNG fixture");
  Expect(image != nullptr && image->width == 2 && image->height == 1,
         "decoded dimensions preserved below cache size");
  Expect(image != nullptr && image->pixels.size() == 2 &&
             image->pixels[0] == gb::render::Rgb565(255, 0, 0) &&
             image->pixels[1] == gb::render::Rgb565(0, 255, 0),
         "decode converts RGBA pixels to RGB565");
  Expect(cache.LoadPng((temp / "missing.png").string(), error) == nullptr &&
             !error.empty(),
         "missing artwork falls back without an image");

  if (image != nullptr) {
    gb::render::Surface240 surface;
    gb::render::BlitImageFit(surface, *image, gb::render::Rect{10, 10, 20, 20}, 0);
    Expect(surface.Pixels()[15 * 240 + 10] == gb::render::Rgb565(255, 0, 0),
           "cover is aspect-fitted and centered in the destination box");
  }

  const auto transparent_path = temp / "transparent.png";
  Expect(WritePng(transparent_path, true), "write transparent PNG fixture");
  image = cache.LoadPng(transparent_path.string(), error);
  Expect(image && image->alpha == std::vector<std::uint8_t>({0, 128}),
         "PNG alpha survives decoding");
  if (image) {
    gb::render::Surface240 surface;
    const auto background = gb::render::Rgb565(0, 0, 255);
    const auto blended = gb::render::Rgb565(0, 128, 120);
    gb::render::BlitImageFit(surface, *image, {10, 10, 20, 20}, background);
    Expect(surface.Pixels()[15 * 240 + 10] == background,
           "transparent pixels retain the artwork background");
    Expect(surface.Pixels()[15 * 240 + 20] == blended,
           "partial alpha blends against the artwork background");
    gb::render::BlitImageFit(surface, *image, {10, 10, 20, 20}, background, true);
    Expect(surface.Pixels()[10 * 240 + 10] == blended,
           "optional trimming fits visible artwork without transparent padding");
    const auto red = gb::render::Rgb565(255, 0, 0);
    surface.Clear(red);
    surface.FillRect(20, 15, 10, 10, background);
    gb::render::BlitImageFit(surface, *image, {10, 10, 20, 20}, std::nullopt);
    Expect(surface.Pixels()[15 * 240 + 10] == red &&
               surface.Pixels()[15 * 240 + 20] == blended &&
               surface.Pixels()[10 * 240 + 20] == red,
           "overlay alpha blends per destination pixel and preserves surrounding wallpaper");
  }

  // The store cache names every image .png, so signature detection is required.
  const auto jpeg_path = temp / "jpeg-named-as-png.png";
  Expect(WriteJpeg(jpeg_path, 3, 2), "write JPEG fixture");
  image = cache.LoadImage(jpeg_path.string(), error);
  Expect(image && error.empty() && image->width == 3 && image->height == 2,
         "JPEG signature is decoded despite PNG filename");
  if (image) {
    const auto pixel = image->pixels.front();
    Expect(((pixel >> 11) & 31) >= 28 && ((pixel >> 5) & 63) <= 14 && (pixel & 31) <= 5,
           "JPEG pixels convert to RGB565 with expected color");
  }
  Expect(cache.LoadPng(jpeg_path.string(), error) != nullptr,
         "existing LoadPng callers gain JPEG compatibility");
  const auto large_jpeg = temp / "large.jpg";
  Expect(WriteJpeg(large_jpeg, 800, 400), "write larger JPEG fixture");
  image = cache.LoadImage(large_jpeg.string(), error);
  Expect(image && image->width == 240 && image->height == 120 && image->pixels.size() == 240 * 120,
         "JPEG decode stores a bounded aspect-preserving cache image");
  const auto malformed = temp / "broken.jpg";
  const unsigned char malformed_bytes[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x03, 0x00};
  std::ofstream(malformed, std::ios::binary).write(reinterpret_cast<const char*>(malformed_bytes), sizeof(malformed_bytes));
  Expect(cache.LoadImage(malformed.string(), error) == nullptr && !error.empty(),
         "malformed JPEG returns an error without terminating the process");
  const auto oversized = temp / "oversized.jpg";
  Expect(WriteJpeg(oversized, 4097, 1), "write oversized JPEG fixture");
  Expect(cache.LoadImage(oversized.string(), error) == nullptr && error.find("limits") != std::string::npos,
         "JPEG dimensions enforce the same bounds as PNG");
  const auto unsupported = temp / "unknown.png";
  std::ofstream(unsupported) << "not an image";
  Expect(cache.LoadImage(unsupported.string(), error) == nullptr && !error.empty(),
         "unsupported artwork has a graceful fallback");

  const auto repaired = temp / "repaired.png";
  Expect(cache.LoadImage(repaired.string(), error) == nullptr, "cache the missing artwork state");
  Expect(WritePng(repaired), "create artwork after a missing-art render");
  image = cache.LoadImage(repaired.string(), error);
  Expect(image && error.empty() && image->width == 2, "new artwork replaces a cached NO ART result");
  const auto previous_time = std::filesystem::last_write_time(repaired);
  std::filesystem::copy_file(jpeg_path, repaired, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::last_write_time(repaired, previous_time + std::chrono::seconds(1));
  image = cache.LoadImage(repaired.string(), error);
  Expect(image && image->width == 3 && image->height == 2,
         "replacing an existing cover refreshes cached pixels at the same library path");

  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
  if (failures != 0) {
    std::cerr << failures << " image cache assertion(s) failed\n";
    return 1;
  }
  std::cout << "image cache tests passed\n";
  return 0;
}
