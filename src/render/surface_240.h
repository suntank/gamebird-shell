#pragma once

#include <cstdint>
#include <vector>

namespace gb::render {

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

constexpr int kSurfaceWidth = 240;
constexpr int kSurfaceHeight = 240;

constexpr std::uint16_t Rgb565(const std::uint8_t r,
                               const std::uint8_t g,
                               const std::uint8_t b) {
  return static_cast<std::uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) |
                                    ((b & 0xF8u) >> 3));
}

class Surface240 {
 public:
  Surface240();

  [[nodiscard]] int Width() const;
  [[nodiscard]] int Height() const;

  [[nodiscard]] const std::uint16_t* Pixels() const;
  [[nodiscard]] std::uint16_t* Pixels();

  void Clear(std::uint16_t color);
  void FillRect(int x, int y, int w, int h, std::uint16_t color);
  void StrokeRect(int x, int y, int w, int h, std::uint16_t color);

  void ClearDirtyRects();
  [[nodiscard]] const std::vector<Rect>& DirtyRects() const;

 private:
  void MarkDirty(int x, int y, int w, int h);

  std::vector<std::uint16_t> pixels_;
  std::vector<Rect> dirty_rects_;
};

}  // namespace gb::render
