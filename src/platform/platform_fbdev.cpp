#include "platform/platform_fbdev.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "core/logging.h"

namespace gb::platform {

FbdevPresenter::~FbdevPresenter() { Shutdown(); }

bool FbdevPresenter::Init(const std::string& fbdev_path) {
  fbdev_path_ = fbdev_path;
  SetConsoleGraphicsMode();
  SetConsoleCursorVisible(false);

  fd_ = open(fbdev_path_.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    core::Log(core::LogLevel::Error,
              "fbdev open failed for " + fbdev_path_ + ": " +
                  std::strerror(errno));
    return false;
  }

  fb_fix_screeninfo finfo {};
  fb_var_screeninfo vinfo {};

  if (ioctl(fd_, FBIOGET_FSCREENINFO, &finfo) < 0) {
    core::Log(core::LogLevel::Error,
              "FBIOGET_FSCREENINFO failed: " + std::string(std::strerror(errno)));
    Shutdown();
    return false;
  }
  if (ioctl(fd_, FBIOGET_VSCREENINFO, &vinfo) < 0) {
    core::Log(core::LogLevel::Error,
              "FBIOGET_VSCREENINFO failed: " + std::string(std::strerror(errno)));
    Shutdown();
    return false;
  }

  map_len_ = finfo.smem_len;
  map_ = static_cast<std::uint8_t*>(
      mmap(nullptr, map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
  if (map_ == MAP_FAILED) {
    map_ = nullptr;
    core::Log(core::LogLevel::Error,
              "mmap failed for " + fbdev_path_ + ": " + std::strerror(errno));
    Shutdown();
    return false;
  }

  line_length_ = static_cast<int>(finfo.line_length);
  bytes_per_pixel_ = static_cast<int>(vinfo.bits_per_pixel / 8);
  xres_ = static_cast<int>(vinfo.xres);
  yres_ = static_cast<int>(vinfo.yres);
  xoffset_ = static_cast<int>(vinfo.xoffset);
  yoffset_ = static_cast<int>(vinfo.yoffset);

  core::Log(core::LogLevel::Info,
            "fbdev ready " + fbdev_path_ + " " + std::to_string(xres_) + "x" +
                std::to_string(yres_) + " bpp=" +
                std::to_string(vinfo.bits_per_pixel) +
                " line=" + std::to_string(line_length_));

  if (bytes_per_pixel_ != 2 && bytes_per_pixel_ != 4) {
    core::Log(core::LogLevel::Warn,
              "Unsupported fbdev pixel format. Expected 16bpp or 32bpp.");
  }

  return true;
}

render::Rect FbdevPresenter::ClampRect(const render::Rect& rect,
                                       const int max_w,
                                       const int max_h) const {
  const int x0 = std::clamp(rect.x, 0, max_w);
  const int y0 = std::clamp(rect.y, 0, max_h);
  const int x1 = std::clamp(rect.x + rect.w, 0, max_w);
  const int y1 = std::clamp(rect.y + rect.h, 0, max_h);
  return render::Rect{.x = x0, .y = y0, .w = std::max(0, x1 - x0), .h = std::max(0, y1 - y0)};
}

void FbdevPresenter::BlitRgb565(const render::Surface240& surface,
                                const render::Rect& rect) {
  const auto* src_base = surface.Pixels();

  for (int row = 0; row < rect.h; ++row) {
    const int src_y = rect.y + row;
    const int dst_y = src_y + yoffset_;

    const auto* src = src_base + src_y * surface.Width() + rect.x;
    auto* dst = map_ + (dst_y * line_length_) + ((rect.x + xoffset_) * bytes_per_pixel_);

    std::memcpy(dst, src, static_cast<std::size_t>(rect.w) * sizeof(std::uint16_t));
  }
}

void FbdevPresenter::BlitXrgb8888(const render::Surface240& surface,
                                  const render::Rect& rect) {
  const auto* src_base = surface.Pixels();

  for (int row = 0; row < rect.h; ++row) {
    const int src_y = rect.y + row;
    const int dst_y = src_y + yoffset_;

    const auto* src = src_base + src_y * surface.Width() + rect.x;
    auto* dst = reinterpret_cast<std::uint32_t*>(
        map_ + (dst_y * line_length_) + ((rect.x + xoffset_) * bytes_per_pixel_));

    for (int col = 0; col < rect.w; ++col) {
      const std::uint16_t p = src[col];
      const std::uint8_t r5 = static_cast<std::uint8_t>((p >> 11) & 0x1F);
      const std::uint8_t g6 = static_cast<std::uint8_t>((p >> 5) & 0x3F);
      const std::uint8_t b5 = static_cast<std::uint8_t>(p & 0x1F);

      const std::uint8_t r8 = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
      const std::uint8_t g8 = static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4));
      const std::uint8_t b8 = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));

      dst[col] = 0xFF000000u | (static_cast<std::uint32_t>(r8) << 16) |
                 (static_cast<std::uint32_t>(g8) << 8) |
                 static_cast<std::uint32_t>(b8);
    }
  }
}

void FbdevPresenter::BlitScaled(const render::Surface240& surface) {
  if (bytes_per_pixel_ != 2 && bytes_per_pixel_ != 4) {
    return;
  }

  // TV Mode uses the DRM framebuffer, which is normally much larger than the
  // 240x240 shell canvas. Keep the square design intact, center it, and use
  // nearest-neighbour scaling so the interface remains crisp on a television.
  const int target = std::min(xres_, yres_);
  const int origin_x = (xres_ - target) / 2;
  const int origin_y = (yres_ - target) / 2;
  const auto* source = surface.Pixels();

  for (int y = 0; y < yres_; ++y) {
    auto* row = map_ + ((y + yoffset_) * line_length_) + xoffset_ * bytes_per_pixel_;
    for (int x = 0; x < xres_; ++x) {
      const bool inside = x >= origin_x && x < origin_x + target &&
                          y >= origin_y && y < origin_y + target;
      const std::uint16_t pixel = inside
          ? source[((y - origin_y) * surface.Height() / target) * surface.Width() +
                   ((x - origin_x) * surface.Width() / target)]
          : 0;
      if (bytes_per_pixel_ == 2) {
        reinterpret_cast<std::uint16_t*>(row)[x] = pixel;
      } else {
        const std::uint8_t r5 = static_cast<std::uint8_t>((pixel >> 11) & 0x1F);
        const std::uint8_t g6 = static_cast<std::uint8_t>((pixel >> 5) & 0x3F);
        const std::uint8_t b5 = static_cast<std::uint8_t>(pixel & 0x1F);
        reinterpret_cast<std::uint32_t*>(row)[x] =
            0xFF000000u | (static_cast<std::uint32_t>((r5 << 3) | (r5 >> 2)) << 16) |
            (static_cast<std::uint32_t>((g6 << 2) | (g6 >> 4)) << 8) |
            static_cast<std::uint32_t>((b5 << 3) | (b5 >> 2));
      }
    }
  }
}

void FbdevPresenter::Present(const render::Surface240& surface) {
  if (!map_) {
    return;
  }

  if (xres_ != surface.Width() || yres_ != surface.Height()) {
    BlitScaled(surface);
    return;
  }

  const int target_w = std::min(surface.Width(), xres_);
  const int target_h = std::min(surface.Height(), yres_);

  const auto& dirty = surface.DirtyRects();
  if (dirty.empty()) {
    const auto full = ClampRect(render::Rect{0, 0, target_w, target_h}, target_w, target_h);
    if (full.w > 0 && full.h > 0) {
      if (bytes_per_pixel_ == 2) {
        BlitRgb565(surface, full);
      } else if (bytes_per_pixel_ == 4) {
        BlitXrgb8888(surface, full);
      }
    }
    return;
  }

  for (const auto& rect : dirty) {
    const auto clipped = ClampRect(rect, target_w, target_h);
    if (clipped.w <= 0 || clipped.h <= 0) {
      continue;
    }

    if (bytes_per_pixel_ == 2) {
      BlitRgb565(surface, clipped);
      continue;
    }

    if (bytes_per_pixel_ == 4) {
      BlitXrgb8888(surface, clipped);
      continue;
    }

    if (!warned_unsupported_format_) {
      warned_unsupported_format_ = true;
      core::Log(core::LogLevel::Warn,
                "Skipping present: unsupported framebuffer bytes_per_pixel=" +
                    std::to_string(bytes_per_pixel_));
    }
    return;
  }
}

void FbdevPresenter::Shutdown() {
  if (map_) {
    munmap(map_, map_len_);
    map_ = nullptr;
    map_len_ = 0;
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  SetConsoleCursorVisible(true);
  RestoreConsoleTextMode();
}

bool FbdevPresenter::IsReady() const { return map_ != nullptr && fd_ >= 0; }

void FbdevPresenter::SetConsoleGraphicsMode() {
  constexpr const char* kTtyCandidates[] = {"/dev/tty1", "/dev/tty0"};
  for (const char* tty_path : kTtyCandidates) {
    const int tty_fd = open(tty_path, O_RDWR | O_CLOEXEC);
    if (tty_fd < 0) {
      continue;
    }

    if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) == 0) {
      tty_fd_ = tty_fd;
      tty_graphics_mode_ = true;
      core::Log(core::LogLevel::Info, std::string("tty graphics mode enabled on ") + tty_path);
      return;
    }

    core::Log(core::LogLevel::Warn,
              std::string("KDSETMODE(KD_GRAPHICS) failed on ") + tty_path + ": " +
                  std::strerror(errno));
    close(tty_fd);
  }
}

void FbdevPresenter::SetConsoleCursorVisible(const bool visible) {
  constexpr const char* kTtyCandidates[] = {"/dev/tty1", "/dev/tty0"};
  const char* seq = visible ? "\x1b[?25h" : "\x1b[?25l";
  const std::size_t seq_len = std::strlen(seq);
  bool wrote_any = false;

  for (const char* tty_path : kTtyCandidates) {
    const int tty_fd = open(tty_path, O_WRONLY | O_CLOEXEC);
    if (tty_fd < 0) {
      continue;
    }
    const ssize_t wrote = write(tty_fd, seq, seq_len);
    if (wrote == static_cast<ssize_t>(seq_len)) {
      wrote_any = true;
    }
    close(tty_fd);
  }

  if (!wrote_any) {
    core::Log(core::LogLevel::Warn,
              std::string("Failed to set console cursor ") +
                  (visible ? "visible" : "hidden"));
  }
}

void FbdevPresenter::RestoreConsoleTextMode() {
  if (tty_fd_ < 0) {
    return;
  }

  if (tty_graphics_mode_) {
    if (ioctl(tty_fd_, KDSETMODE, KD_TEXT) < 0) {
      core::Log(core::LogLevel::Warn,
                "KDSETMODE(KD_TEXT) failed during shutdown: " +
                    std::string(std::strerror(errno)));
    }
  }

  close(tty_fd_);
  tty_fd_ = -1;
  tty_graphics_mode_ = false;
}

}  // namespace gb::platform
