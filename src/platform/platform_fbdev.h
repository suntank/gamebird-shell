#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "render/surface_240.h"

namespace gb::platform {

class FbdevPresenter {
 public:
  FbdevPresenter() = default;
  ~FbdevPresenter();

  bool Init(const std::string& fbdev_path);
  void Present(const render::Surface240& surface);
  void Shutdown();

  [[nodiscard]] bool IsReady() const;

 private:
  void SetConsoleGraphicsMode();
  void RestoreConsoleTextMode();
  void SetConsoleCursorVisible(bool visible);
  void BlitRgb565(const render::Surface240& surface, const render::Rect& rect);
  void BlitXrgb8888(const render::Surface240& surface, const render::Rect& rect);
  render::Rect ClampRect(const render::Rect& rect,
                         int max_w,
                         int max_h) const;

  int fd_ = -1;
  std::string fbdev_path_;
  std::uint8_t* map_ = nullptr;
  std::size_t map_len_ = 0;

  int line_length_ = 0;
  int bytes_per_pixel_ = 0;
  int xres_ = 0;
  int yres_ = 0;
  int xoffset_ = 0;
  int yoffset_ = 0;
  int tty_fd_ = -1;
  bool tty_graphics_mode_ = false;

  bool warned_unsupported_format_ = false;
};

}  // namespace gb::platform
