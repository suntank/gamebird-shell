#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include <png.h>
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

bool WritePng(const std::filesystem::path& path) {
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
  png_bytep row = pixels;
  png_write_row(png, row);
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
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

  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
  if (failures != 0) {
    std::cerr << failures << " image cache assertion(s) failed\n";
    return 1;
  }
  std::cout << "image cache tests passed\n";
  return 0;
}
