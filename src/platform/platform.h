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
  L2,
  R2,
  L3,
  R3,
  Start,
  Select,
  Guide,
  LeftStickUp,
  LeftStickDown,
  LeftStickLeft,
  LeftStickRight,
  RightStickUp,
  RightStickDown,
  RightStickLeft,
  RightStickRight,
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
  int retroarch_joypad_index = -1;
  // Encoded as "key:<name>", "btn:<index-or-hat>", or "axis:<signed-index>".
  // Empty when the raw device event cannot be represented by RetroArch.
  std::string retroarch_binding;
};

struct InputFrame {
  bool quit_requested = false;
  bool devices_changed = false;
  std::vector<Button> pressed;
  std::vector<InputEvent> events;
};

struct PlatformOptions {
  std::string title = "GameBird Shell";
  int scale = 3;
};

}  // namespace gb::platform
