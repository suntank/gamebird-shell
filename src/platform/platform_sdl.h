#pragma once

#include "platform/platform.h"

#include "render/surface_240.h"
#include <SDL2/SDL.h>

namespace gb::platform {

class SDLPresenter {
 public:
  SDLPresenter() = default;
  ~SDLPresenter();

  bool Init(const PlatformOptions& options);
 bool WaitAndPoll(InputFrame& out, int timeout_ms);
  void Present(const render::Surface240& surface);
  void Shutdown();

 private:
  static bool MapFromScancode(int scancode, Button& out_button, bool& out_quit);
  static bool MapFromController(int button, Button& out_button);

  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  SDL_GameController* controller_ = nullptr;
};

}  // namespace gb::platform
