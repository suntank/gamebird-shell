#include "render/surface_240.h"

#include <algorithm>

namespace gb::render {

Surface240::Surface240() : pixels_(kSurfaceWidth * kSurfaceHeight, 0) {}

int Surface240::Width() const { return kSurfaceWidth; }

int Surface240::Height() const { return kSurfaceHeight; }

const std::uint16_t* Surface240::Pixels() const { return pixels_.data(); }

std::uint16_t* Surface240::Pixels() { return pixels_.data(); }

void Surface240::Clear(const std::uint16_t color) {
  std::fill(pixels_.begin(), pixels_.end(), color);
  MarkDirty(0, 0, kSurfaceWidth, kSurfaceHeight);
}

void Surface240::FillRect(int x,
                          int y,
                          int w,
                          int h,
                          const std::uint16_t color) {
  if (w <= 0 || h <= 0) {
    return;
  }

  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(kSurfaceWidth, x + w);
  const int y1 = std::min(kSurfaceHeight, y + h);

  if (x0 >= x1 || y0 >= y1) {
    return;
  }

  for (int row = y0; row < y1; ++row) {
    auto* dst = &pixels_[row * kSurfaceWidth + x0];
    std::fill(dst, dst + (x1 - x0), color);
  }

  MarkDirty(x0, y0, x1 - x0, y1 - y0);
}

void Surface240::StrokeRect(const int x,
                            const int y,
                            const int w,
                            const int h,
                            const std::uint16_t color) {
  FillRect(x, y, w, 1, color);
  FillRect(x, y + h - 1, w, 1, color);
  FillRect(x, y, 1, h, color);
  FillRect(x + w - 1, y, 1, h, color);
}

void Surface240::ClearDirtyRects() { dirty_rects_.clear(); }

const std::vector<Rect>& Surface240::DirtyRects() const { return dirty_rects_; }

void Surface240::MarkDirty(const int x, const int y, const int w, const int h) {
  dirty_rects_.push_back(Rect{x, y, w, h});
}

}  // namespace gb::render
