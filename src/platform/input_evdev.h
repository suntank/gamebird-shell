#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <linux/input.h>

#include "platform/platform.h"

namespace gb::platform {

struct EvdevDeviceInfo {
  std::string path;
  std::string name;
  std::string bus;
  std::uint16_t bus_type = 0;
  std::uint16_t vendor = 0;
  std::uint16_t product = 0;
  bool is_bluetooth = false;
  bool is_gamepad = false;
  bool is_keyboard = false;
};

class EvdevInput {
 public:
  EvdevInput() = default;
  ~EvdevInput();

  bool Init(const std::string& device_path);
  bool WaitAndPoll(InputFrame& out, int timeout_ms);
  void Shutdown();
  void ReleaseDeviceGrabs();
  void AcquireDeviceGrabs();

  [[nodiscard]] const std::string& DevicePath() const;
  [[nodiscard]] std::size_t DeviceCount() const;
  [[nodiscard]] std::vector<EvdevDeviceInfo> ConnectedDevices() const;

  static std::string AutoDetectPath();

 private:
  struct Device {
    struct AxisState {
      bool present = false;
      int min = 0;
      int max = 0;
      int center = 0;
      int deadzone = 0;
      int last_dir = 0;
    };

    int fd = -1;
    std::string path;
    std::string name;
    std::string bus;
    std::uint16_t bus_type = 0;
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    bool is_bluetooth = false;
    bool is_gamepad = false;
    bool is_keyboard = false;
    bool grabbed = false;
    std::array<std::uint64_t, KEY_MAX + 1> key_down_ms {};
    AxisState abs_x;
    AxisState abs_y;
  };

  static bool MapKeyCode(unsigned short code, Button& out_button, bool& out_quit);
  bool OpenDevicePath(const std::string& path, bool grab_input);

  std::vector<Device> devices_;
  std::string device_path_;
};

}  // namespace gb::platform
