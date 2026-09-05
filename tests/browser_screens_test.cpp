#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <png.h>
#include <unistd.h>

#include "render/surface_240.h"
#include "render/theme.h"
#include "ui/screens/home.h"
#include "ui/widgets/list.h"
#include "ui/widgets/chrome.h"

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool WriteCover(const std::filesystem::path& path) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr,
                                             nullptr);
  png_infop info = png == nullptr ? nullptr : png_create_info_struct(png);
  if (png == nullptr || info == nullptr || setjmp(png_jmpbuf(png)) != 0) {
    if (png != nullptr) {
      png_destroy_write_struct(&png, info == nullptr ? nullptr : &info);
    }
    std::fclose(file);
    return false;
  }
  png_init_io(png, file);
  png_set_IHDR(png, info, 2, 3, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  png_byte row[] = {240, 48, 48, 255, 240, 48, 48, 255};
  png_bytep rows[] = {row, row, row};
  png_write_image(png, rows);
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  std::fclose(file);
  return true;
}

}  // namespace

int main() {
  const auto temp = std::filesystem::temp_directory_path() /
                    ("gamebird-browser-screens-" + std::to_string(::getpid()) + "-" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::filesystem::create_directories(temp);
  const auto cover = temp / "cover.png";
  Expect(WriteCover(cover), "write cover fixture");

  const auto theme = gb::render::DefaultTheme();
  gb::render::Surface240 surface;
  gb::ui::screens::DrawSystems(
      surface, theme,
      {{.name = "Super Nintendo", .game_count = 3, .icon_path = "", .logo_path = ""},
       {.name = "Game Boy", .game_count = 5, .icon_path = "", .logo_path = ""}},
      0, "");
  Expect(surface.Pixels()[52 * 240 + 66] == theme.panel_border,
         "missing system icon draws console fallback");

  gb::ui::screens::DrawGameBrowser(
      surface, theme, "Super Nintendo",
      {"Chrono Trigger", "Super Metroid", "Zelda"}, 1, cover.string(), "");
  Expect(surface.Pixels()[100 * 240 + 120] == gb::render::Rgb565(240, 48, 48),
         "cover art occupies the browser artwork area");
  Expect(surface.Pixels()[182 * 240 + 16] == theme.accent,
         "selected game row is highlighted");

  surface.Clear(theme.bg);
  gb::ui::widgets::DrawList(surface, 16, 40, 208, 48, 24,
                            {std::string(100, 'W'), "Second", "Third"}, 0, theme);
  bool margin_clean = true;
  for (int y = 40; y < 64; ++y) {
    for (int x = 224; x < 227; ++x) {
      margin_clean &= surface.Pixels()[y * 240 + x] == theme.bg;
    }
  }
  Expect(margin_clean, "long list labels stay inside their row");
  Expect(surface.Pixels()[40 * 240 + 227] == theme.accent,
         "overflowing list displays scroll thumb");
  surface.Clear(theme.bg);
  gb::ui::widgets::DrawList(surface, 16, 40, 208, 48, 0, {"Invalid"}, 0, theme);
  Expect(surface.Pixels()[40 * 240 + 16] == theme.bg,
         "zero-height rows do not draw or divide by zero");
  Expect(gb::ui::widgets::FitLabel("Long title", 36) == "Lon...",
         "bounded labels reserve room for ellipsis");

  std::error_code ec;
  std::filesystem::remove_all(temp, ec);
  if (failures != 0) {
    std::cerr << failures << " browser screen assertion(s) failed\n";
    return 1;
  }
  std::cout << "browser screen tests passed\n";
  return 0;
}
