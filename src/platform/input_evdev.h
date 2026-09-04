#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
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
  int joypad_index = -1;
};

class EvdevInput {
 public:
  EvdevInput() = default;
  ~EvdevInput();

  bool Init(const std::string& device_path, bool grab_input = true);
  bool WaitAndPoll(InputFrame& out, int timeout_ms);
  void Shutdown();
  void ReleaseDeviceGrabs();
  void AcquireDeviceGrabs();
  bool RefreshDevices(bool force = false);
  bool ConsumeDevicesChanged();

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
    int joypad_index = -1;
    std::array<std::uint64_t, KEY_MAX + 1> key_down_ms {};
    AxisState abs_x;
    AxisState abs_y;
    AxisState abs_rx;
    AxisState abs_ry;
    AxisState abs_z;
    AxisState abs_rz;
    AxisState abs_brake;
    AxisState abs_gas;
    std::unordered_map<unsigned short, int> js_button_index;
    std::unordered_map<unsigned short, int> js_axis_index;
  };

  static bool MapKeyCode(unsigned short code, Button& out_button, bool& out_quit);
  static std::string RetroArchKeyName(unsigned short code);
  bool OpenDevicePath(const std::string& path, bool grab_input);
  void PopulateJoystickMaps(Device& dev);
  void RebuildDevicePath();
  void RemoveDevices(const std::vector<std::string>& paths);
  std::string RetroArchBindingForEvent(const Device& dev,
                                       const input_event& event) const;

  std::vector<Device> devices_;
  std::string device_path_;
  std::string requested_path_;
  bool auto_mode_ = false;
  bool grab_input_ = true;
  bool devices_changed_ = false;
  std::uint64_t last_refresh_ms_ = 0;
};

}  // namespace gb::platform
