#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

#include "render/surface_240.h"
#include "ui/widgets/text.h"

namespace {
using gb::render::Surface240;
constexpr auto ink = gb::render::Rgb565(255, 255, 255);
int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool SamePixels(const Surface240& left, const Surface240& right) {
  return std::equal(left.Pixels(), left.Pixels() + 240 * 240, right.Pixels());
}

gb::render::Rect InkBounds(const Surface240& surface) {
  int left = 240, top = 240, right = -1, bottom = -1;
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 240; ++x) {
      if (surface.Pixels()[y * 240 + x] != ink) continue;
      left = std::min(left, x);
      top = std::min(top, y);
      right = std::max(right, x);
      bottom = std::max(bottom, y);
    }
  }
  return {left, top, right - left + 1, bottom - top + 1};
}

bool MatchesClippedTranslation(const Surface240& reference,
                               const Surface240& clipped,
                               const int dx, const int dy) {
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 240; ++x) {
      const int source_x = x - dx, source_y = y - dy;
      const auto expected = source_x >= 0 && source_x < 240 &&
                            source_y >= 0 && source_y < 240
          ? reference.Pixels()[source_y * 240 + source_x] : 0;
      if (clipped.Pixels()[y * 240 + x] != expected) return false;
    }
  }
  return true;
}
}  // namespace

int main() {
  using namespace gb::ui::widgets;
  Expect(MeasureReadableTextWidth("") == 0, "empty readable text has zero width");
  Expect(MeasureReadableTextWidth("AB") == 18, "readable text reserves nine pixels per character");

  Surface240 readable;
  DrawReadableText(readable, 18, 29, "A", ink);
  const auto bounds = InkBounds(readable);
  Expect(bounds.x == 18 && bounds.y == 29 && bounds.w == 8 && bounds.h == 11,
         "readable letters occupy eight by eleven pixels");

  Surface240 truncated, expected;
  DrawReadableText(truncated, 204, 228, "ABCDEFG", ink);
  DrawReadableText(expected, 204, 228, "A...", ink);
  Expect(SamePixels(truncated, expected), "overflow uses ellipsis within the available width");
  const auto truncation_bounds = InkBounds(truncated);
  Expect(truncation_bounds.x >= 204 && truncation_bounds.y >= 228 &&
         truncation_bounds.x + truncation_bounds.w <= 240 &&
         truncation_bounds.y + truncation_bounds.h <= 240,
         "truncated text stays within its surface corner");

  Surface240 reference, top_left, bottom_right;
  DrawReadableText(reference, 10, 10, "A", ink);
  DrawReadableText(top_left, -3, -4, "A", ink);
  Expect(MatchesClippedTranslation(reference, top_left, -13, -14),
         "letters clip correctly at negative coordinates");
  DrawReadableText(bottom_right, 225, 234, "A", ink);
  Expect(MatchesClippedTranslation(reference, bottom_right, 215, 224),
         "letters clip correctly at the lower edge");
  Surface240 outside, empty;
  DrawReadableText(outside, 240, 0, "EDGE", ink);
  DrawReadableText(outside, 0, 240, "EDGE", ink);
  Expect(SamePixels(outside, empty), "text beyond the surface draws no pixels");

  Surface240 legacy, legacy_expected;
  DrawText(legacy, 18, 29, "A", ink);
  constexpr std::array<std::string_view, 7> legacy_a{
      ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"};
  for (int row = 0; row < 7; ++row)
    for (int column = 0; column < 5; ++column)
      if (legacy_a[row][column] == '#')
        legacy_expected.FillRect(18 + column, 29 + row, 1, 1, ink);
  Expect(SamePixels(legacy, legacy_expected), "the existing scale-one bitmap remains unchanged");
  Expect(MeasureTextWidth("AB") == 12 && MeasureTextWidth("AB", 2) == 24,
         "existing font measurements remain unchanged");

  if (failures) return 1;
  std::cout << "Readable text passed\n";
}
