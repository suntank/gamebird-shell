#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gb::platform {

enum class Button {
  Up,
  Down,
  Left,
  Right,
  A,
  B,
  X,
  Y,
  L,
  R,
  Start,
  Select,
};

struct InputEvent {
  Button button = Button::A;
  bool mapped_button = true;
  std::string device_name;
  std::string device_path;
  std::uint16_t bus_type = 0;
  std::uint16_t vendor = 0;
  std::uint16_t product = 0;
  bool is_bluetooth = false;
  bool is_keyboard = false;
  std::uint16_t raw_type = 0;
  std::uint16_t raw_code = 0;
  int raw_value = 0;
  int hold_ms = 0;
};

struct InputFrame {
  bool quit_requested = false;
  std::vector<Button> pressed;
  std::vector<InputEvent> events;
};

struct PlatformOptions {
  std::string title = "GameBird Shell";
  int scale = 3;
};

}  // namespace gb::platform
