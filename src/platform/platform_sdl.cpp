#include "platform/platform_sdl.h"

#include "core/logging.h"
#include "render/surface_240.h"

#include <SDL2/SDL.h>

namespace gb::platform {

namespace {

constexpr std::uint32_t kTextureFormat = SDL_PIXELFORMAT_RGB565;

}  // namespace

SDLPresenter::~SDLPresenter() { Shutdown(); }

bool SDLPresenter::Init(const PlatformOptions& options) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    core::Log(core::LogLevel::Error,
              std::string("SDL_Init failed: ") + SDL_GetError());
    return false;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

  window_ = SDL_CreateWindow(options.title.c_str(), SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED,
                             render::kSurfaceWidth * options.scale,
                             render::kSurfaceHeight * options.scale,
                             SDL_WINDOW_SHOWN);
  if (!window_) {
    core::Log(core::LogLevel::Error,
              std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, -1,
                                 SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) {
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  }

  if (!renderer_) {
    core::Log(core::LogLevel::Error,
              std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
    return false;
  }

  texture_ = SDL_CreateTexture(renderer_, kTextureFormat, SDL_TEXTUREACCESS_STREAMING,
                               render::kSurfaceWidth, render::kSurfaceHeight);
  if (!texture_) {
    core::Log(core::LogLevel::Error,
              std::string("SDL_CreateTexture failed: ") + SDL_GetError());
    return false;
  }

  if (SDL_NumJoysticks() > 0 && SDL_IsGameController(0)) {
    controller_ = SDL_GameControllerOpen(0);
  }

  return true;
}

bool SDLPresenter::MapFromScancode(const int scancode,
                                   Button& out_button,
                                   bool& out_quit) {
  out_quit = false;
  switch (scancode) {
    case SDL_SCANCODE_UP:
      out_button = Button::Up;
      return true;
    case SDL_SCANCODE_DOWN:
      out_button = Button::Down;
      return true;
    case SDL_SCANCODE_LEFT:
      out_button = Button::Left;
      return true;
    case SDL_SCANCODE_RIGHT:
      out_button = Button::Right;
      return true;
    case SDL_SCANCODE_D:
      out_button = Button::A;
      return true;
    case SDL_SCANCODE_S:
      out_button = Button::B;
      return true;
    case SDL_SCANCODE_W:
      out_button = Button::X;
      return true;
    case SDL_SCANCODE_A:
      out_button = Button::Y;
      return true;
    case SDL_SCANCODE_Q:
      out_button = Button::L;
      return true;
    case SDL_SCANCODE_E:
      out_button = Button::R;
      return true;
    case SDL_SCANCODE_RETURN:
      out_button = Button::Start;
      return true;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
      out_button = Button::Select;
      return true;
    case SDL_SCANCODE_ESCAPE:
      out_quit = true;
      return true;
    default:
      return false;
  }
}

bool SDLPresenter::MapFromController(const int button, Button& out_button) {
  switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
      out_button = Button::Up;
      return true;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
      out_button = Button::Down;
      return true;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
      out_button = Button::Left;
      return true;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
      out_button = Button::Right;
      return true;
    case SDL_CONTROLLER_BUTTON_A:
      out_button = Button::A;
      return true;
    case SDL_CONTROLLER_BUTTON_B:
      out_button = Button::B;
      return true;
    case SDL_CONTROLLER_BUTTON_X:
      out_button = Button::X;
      return true;
    case SDL_CONTROLLER_BUTTON_Y:
      out_button = Button::Y;
      return true;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
      out_button = Button::L;
      return true;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
      out_button = Button::R;
      return true;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
      out_button = Button::L3;
      return true;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
      out_button = Button::R3;
      return true;
    case SDL_CONTROLLER_BUTTON_START:
      out_button = Button::Start;
      return true;
    case SDL_CONTROLLER_BUTTON_BACK:
      out_button = Button::Select;
      return true;
    case SDL_CONTROLLER_BUTTON_GUIDE:
      out_button = Button::Guide;
      return true;
    default:
      return false;
  }
}

bool SDLPresenter::WaitAndPoll(InputFrame& out, const int timeout_ms) {
  out.pressed.clear();
  out.events.clear();
  out.quit_requested = false;

  SDL_Event event;
  const int has_event = SDL_WaitEventTimeout(&event, timeout_ms);
  if (!has_event) {
    return false;
  }

  auto consume = [&](const SDL_Event& ev) {
    switch (ev.type) {
      case SDL_QUIT:
        out.quit_requested = true;
        break;
      case SDL_KEYDOWN:
        if (ev.key.repeat == 0) {
          Button button = Button::A;
          bool quit = false;
          if (MapFromScancode(ev.key.keysym.scancode, button, quit)) {
            if (quit) {
              out.quit_requested = true;
            } else {
              out.pressed.push_back(button);
              out.events.push_back(
                  InputEvent{.button = button,
                             .mapped_button = true,
                             .device_name = "SDL Keyboard",
                             .device_path = "sdl://keyboard",
                             .is_keyboard = true,
                             .raw_type = static_cast<std::uint16_t>(SDL_KEYDOWN),
                             .raw_code = static_cast<std::uint16_t>(ev.key.keysym.scancode),
                             .raw_value = ev.key.repeat == 0 ? 1 : 2,
                             .retroarch_joypad_index = -1,
                             .retroarch_binding = {}});
            }
          } else {
            out.events.push_back(
                InputEvent{.button = Button::A,
                           .mapped_button = false,
                           .device_name = "SDL Keyboard",
                           .device_path = "sdl://keyboard",
                           .is_keyboard = true,
                           .raw_type = static_cast<std::uint16_t>(SDL_KEYDOWN),
                           .raw_code = static_cast<std::uint16_t>(ev.key.keysym.scancode),
                           .raw_value = ev.key.repeat == 0 ? 1 : 2,
                           .retroarch_joypad_index = -1,
                           .retroarch_binding = {}});
          }
        }
        break;
      case SDL_CONTROLLERBUTTONDOWN: {
        Button button = Button::A;
        if (MapFromController(ev.cbutton.button, button)) {
          out.pressed.push_back(button);
          out.events.push_back(
              InputEvent{.button = button,
                         .mapped_button = true,
                         .device_name = "SDL Controller",
                         .device_path = "sdl://controller",
                         .raw_type = static_cast<std::uint16_t>(SDL_CONTROLLERBUTTONDOWN),
                         .raw_code = static_cast<std::uint16_t>(ev.cbutton.button),
                         .raw_value = 1,
                         .retroarch_joypad_index = 0,
                         .retroarch_binding = {}});
        }
        break;
      }
      default:
        break;
    }
  };

  consume(event);
  while (SDL_PollEvent(&event)) {
    consume(event);
  }

  return out.quit_requested || !out.events.empty() || !out.pressed.empty();
}

void SDLPresenter::Present(const render::Surface240& surface) {
  SDL_UpdateTexture(texture_, nullptr, surface.Pixels(),
                    render::kSurfaceWidth * sizeof(std::uint16_t));
  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

void SDLPresenter::Shutdown() {
  if (controller_) {
    SDL_GameControllerClose(controller_);
    controller_ = nullptr;
  }
  if (texture_) {
    SDL_DestroyTexture(texture_);
    texture_ = nullptr;
  }
  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
    renderer_ = nullptr;
  }
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  SDL_Quit();
}

}  // namespace gb::platform
