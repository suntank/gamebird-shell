#include "platform/input_evdev.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "core/logging.h"
#include "core/time.h"

namespace gb::platform {

namespace {

struct ProbeResult {
  std::string path;
  std::string name;
  std::string bus;
  std::uint16_t bus_type = 0;
  std::uint16_t vendor = 0;
  std::uint16_t product = 0;
  bool is_bluetooth = false;
  bool is_gamepad = false;
  bool is_keyboard = false;
  bool is_mouse = false;
  int gamepad_score = 0;
  int keyboard_score = 0;
};

std::string BusTypeName(unsigned short bustype) {
  switch (bustype) {
    case BUS_BLUETOOTH:
      return "bluetooth";
    case BUS_USB:
      return "usb";
    case BUS_I2C:
      return "i2c";
    case BUS_SPI:
      return "spi";
    case BUS_HOST:
      return "host";
    default:
      return "other";
  }
}

bool TestBit(const std::vector<unsigned char>& bits, const int bit) {
  if (bit < 0) {
    return false;
  }
  const std::size_t idx = static_cast<std::size_t>(bit / 8);
  if (idx >= bits.size()) {
    return false;
  }
  return (bits[idx] & static_cast<unsigned char>(1u << (bit % 8))) != 0;
}

std::vector<unsigned char> ReadEventBits(const int fd, const int ev_type, const int max_code) {
  std::vector<unsigned char> bits(static_cast<std::size_t>((max_code + 8) / 8), 0);
  if (ioctl(fd, EVIOCGBIT(ev_type, bits.size()), bits.data()) < 0) {
    bits.clear();
  }
  return bits;
}

std::string ReadDeviceNameFromFd(const int fd) {
  char name[256] = {};
  if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) <= 0) {
    return {};
  }
  return std::string(name);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

ProbeResult ProbeEventDevice(const std::string& event_path) {
  ProbeResult out;
  out.path = event_path;

  const int fd = open(event_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return out;
  }

  out.name = ReadDeviceNameFromFd(fd);
  const std::string lower_name = ToLower(out.name);

  input_id devid {};
  if (ioctl(fd, EVIOCGID, &devid) == 0) {
    out.bus = BusTypeName(devid.bustype);
    out.bus_type = devid.bustype;
    out.vendor = devid.vendor;
    out.product = devid.product;
    out.is_bluetooth = (devid.bustype == BUS_BLUETOOTH);
  }

  const auto ev_bits = ReadEventBits(fd, 0, EV_MAX);
  const bool has_ev_key = TestBit(ev_bits, EV_KEY);
  const bool has_ev_abs = TestBit(ev_bits, EV_ABS);
  const bool has_ev_rel = TestBit(ev_bits, EV_REL);

  const auto key_bits = has_ev_key ? ReadEventBits(fd, EV_KEY, KEY_MAX) : std::vector<unsigned char>{};
  const auto abs_bits = has_ev_abs ? ReadEventBits(fd, EV_ABS, ABS_MAX) : std::vector<unsigned char>{};
  const auto rel_bits = has_ev_rel ? ReadEventBits(fd, EV_REL, REL_MAX) : std::vector<unsigned char>{};

  close(fd);

  const bool has_mouse_btn = TestBit(key_bits, BTN_MOUSE);
  const bool has_rel_xy = TestBit(rel_bits, REL_X) && TestBit(rel_bits, REL_Y);
  out.is_mouse = has_mouse_btn || (has_ev_rel && has_rel_xy);

  const bool has_btn_south = TestBit(key_bits, BTN_SOUTH);
  const bool has_btn_east = TestBit(key_bits, BTN_EAST);
  const bool has_btn_start = TestBit(key_bits, BTN_START);
  const bool has_btn_select = TestBit(key_bits, BTN_SELECT);
  const bool has_btn_gamepad = TestBit(key_bits, BTN_GAMEPAD);
  const bool has_dpad_buttons = TestBit(key_bits, BTN_DPAD_UP) || TestBit(key_bits, BTN_DPAD_DOWN) ||
                                TestBit(key_bits, BTN_DPAD_LEFT) || TestBit(key_bits, BTN_DPAD_RIGHT);
  const bool has_hat_axes = TestBit(abs_bits, ABS_HAT0X) || TestBit(abs_bits, ABS_HAT0Y);

  out.gamepad_score = 0;
  if (has_btn_gamepad || (has_btn_south && has_btn_east)) {
    out.gamepad_score += 6;
  }
  if (has_btn_start || has_btn_select) {
    out.gamepad_score += 2;
  }
  if (has_dpad_buttons) {
    out.gamepad_score += 3;
  }
  if (has_hat_axes) {
    out.gamepad_score += 2;
  }
  if (lower_name.find("gamepad") != std::string::npos || lower_name.find("joystick") != std::string::npos ||
      lower_name.find("joypad") != std::string::npos || lower_name.find("controller") != std::string::npos) {
    out.gamepad_score += 3;
  }
  if (out.is_bluetooth) {
    // Prefer Bluetooth pads over generic keyboards when both exist.
    out.gamepad_score += 1;
  }

  const bool has_arrow_keys =
      TestBit(key_bits, KEY_UP) && TestBit(key_bits, KEY_DOWN) && TestBit(key_bits, KEY_LEFT) && TestBit(key_bits, KEY_RIGHT);
  const bool has_confirm_keys = TestBit(key_bits, KEY_ENTER) || TestBit(key_bits, KEY_SPACE) || TestBit(key_bits, KEY_Z);
  const bool has_letter_keys = TestBit(key_bits, KEY_A) || TestBit(key_bits, KEY_S) || TestBit(key_bits, KEY_X);

  out.keyboard_score = 0;
  if (has_arrow_keys) {
    out.keyboard_score += 4;
  }
  if (has_confirm_keys) {
    out.keyboard_score += 2;
  }
  if (has_letter_keys) {
    out.keyboard_score += 1;
  }
  if (lower_name.find("keyboard") != std::string::npos) {
    out.keyboard_score += 2;
  }
  if (out.is_mouse) {
    out.keyboard_score -= 2;
  }

  out.is_gamepad = out.gamepad_score >= 6;
  out.is_keyboard = out.keyboard_score >= 5;
  return out;
}

std::vector<ProbeResult> ProbeAllEventDevices() {
  std::vector<ProbeResult> probes;
  try {
    for (const auto& entry : std::filesystem::directory_iterator("/dev/input")) {
      const std::string path = entry.path().string();
      if (path.find("/dev/input/event") != 0) {
        continue;
      }
      probes.push_back(ProbeEventDevice(path));
    }
  } catch (const std::exception&) {
    return {};
  }

  std::sort(probes.begin(), probes.end(), [](const ProbeResult& a, const ProbeResult& b) {
    return a.path < b.path;
  });
  return probes;
}

std::string SelectPrimaryPath(const std::vector<ProbeResult>& probes) {
  const ProbeResult* best_gamepad = nullptr;
  const ProbeResult* best_keyboard = nullptr;
  const ProbeResult* first_non_mouse = nullptr;
  const ProbeResult* first_any = nullptr;

  for (const auto& probe : probes) {
    if (first_any == nullptr) {
      first_any = &probe;
    }
    if (!probe.is_mouse && first_non_mouse == nullptr) {
      first_non_mouse = &probe;
    }
    if (probe.gamepad_score > 0 &&
        (best_gamepad == nullptr || probe.gamepad_score > best_gamepad->gamepad_score)) {
      best_gamepad = &probe;
    }
    if (probe.keyboard_score > 0 &&
        (best_keyboard == nullptr || probe.keyboard_score > best_keyboard->keyboard_score)) {
      best_keyboard = &probe;
    }
  }

  if (best_gamepad != nullptr) {
    return best_gamepad->path;
  }
  if (best_keyboard != nullptr) {
    return best_keyboard->path;
  }
  if (first_non_mouse != nullptr) {
    return first_non_mouse->path;
  }
  return first_any != nullptr ? first_any->path : std::string{};
}

std::string SelectBestKeyboardPath(const std::vector<ProbeResult>& probes,
                                   const std::string& exclude_path) {
  const ProbeResult* best_keyboard = nullptr;
  for (const auto& probe : probes) {
    if (probe.path == exclude_path || probe.keyboard_score <= 0) {
      continue;
    }
    if (best_keyboard == nullptr || probe.keyboard_score > best_keyboard->keyboard_score) {
      best_keyboard = &probe;
    }
  }
  return best_keyboard != nullptr ? best_keyboard->path : std::string{};
}

}  // namespace

EvdevInput::~EvdevInput() { Shutdown(); }

std::string EvdevInput::AutoDetectPath() {
  const auto probes = ProbeAllEventDevices();
  return SelectPrimaryPath(probes);
}

bool EvdevInput::OpenDevicePath(const std::string& path, const bool grab_input) {
  const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    core::Log(core::LogLevel::Error,
              "evdev open failed for " + path + ": " + std::strerror(errno));
    return false;
  }

  Device dev;
  dev.fd = fd;
  dev.path = path;
  const ProbeResult probe = ProbeEventDevice(path);
  dev.name = probe.name;
  dev.bus = probe.bus;
  dev.bus_type = probe.bus_type;
  dev.vendor = probe.vendor;
  dev.product = probe.product;
  dev.is_bluetooth = probe.is_bluetooth;
  dev.is_gamepad = probe.is_gamepad;
  dev.is_keyboard = probe.is_keyboard;

  // Some low-cost USB pads expose D-pad as ABS_X/ABS_Y instead of HAT axes.
  const auto abs_bits = ReadEventBits(fd, EV_ABS, ABS_MAX);
  const bool has_hat_axes = TestBit(abs_bits, ABS_HAT0X) || TestBit(abs_bits, ABS_HAT0Y);
  if (!has_hat_axes) {
    input_absinfo abs_x_info {};
    if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x_info) == 0) {
      dev.abs_x.present = true;
      dev.abs_x.min = abs_x_info.minimum;
      dev.abs_x.max = abs_x_info.maximum;
      dev.abs_x.center = (abs_x_info.minimum + abs_x_info.maximum) / 2;
      const int span = std::max(0, abs_x_info.maximum - abs_x_info.minimum);
      dev.abs_x.deadzone = std::max(abs_x_info.flat, std::max(1, span / 6));
    }

    input_absinfo abs_y_info {};
    if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y_info) == 0) {
      dev.abs_y.present = true;
      dev.abs_y.min = abs_y_info.minimum;
      dev.abs_y.max = abs_y_info.maximum;
      dev.abs_y.center = (abs_y_info.minimum + abs_y_info.maximum) / 2;
      const int span = std::max(0, abs_y_info.maximum - abs_y_info.minimum);
      dev.abs_y.deadzone = std::max(abs_y_info.flat, std::max(1, span / 6));
    }
  }

  if (grab_input) {
    if (ioctl(fd, EVIOCGRAB, 1) == 0) {
      dev.grabbed = true;
    } else {
      core::Log(core::LogLevel::Warn,
                "evdev grab failed for " + path + ": " + std::strerror(errno));
    }
  }

  std::string msg = "evdev ready " + dev.path;
  if (!dev.name.empty()) {
    msg += " name=\"" + dev.name + "\"";
  }
  if (!dev.bus.empty()) {
    msg += " bus=" + dev.bus;
  }
  if (dev.is_bluetooth) {
    msg += " bt";
  }
  if (dev.grabbed) {
    msg += " grabbed";
  }
  core::Log(core::LogLevel::Info, msg);

  devices_.push_back(std::move(dev));
  return true;
}

bool EvdevInput::Init(const std::string& device_path) {
  Shutdown();
  device_path_ = device_path;

  if (device_path == "auto") {
    const auto probes = ProbeAllEventDevices();
    const std::string primary = SelectPrimaryPath(probes);
    if (primary.empty()) {
      core::Log(core::LogLevel::Error, "No /dev/input/event* devices found for evdev input.");
      return false;
    }

    std::set<std::string> selected;
    selected.insert(primary);

    const std::string keyboard_companion = SelectBestKeyboardPath(probes, primary);
    if (!keyboard_companion.empty()) {
      selected.insert(keyboard_companion);
    }

    bool opened_primary = false;
    for (const auto& path : selected) {
      const bool required = (path == primary);
      if (!OpenDevicePath(path, true)) {
        if (required) {
          Shutdown();
          return false;
        }
        core::Log(core::LogLevel::Warn, "Optional evdev companion skipped: " + path);
        continue;
      }
      if (required) {
        opened_primary = true;
      }
    }

    if (!opened_primary || devices_.empty()) {
      Shutdown();
      return false;
    }

    device_path_.clear();
    for (std::size_t i = 0; i < devices_.size(); ++i) {
      if (i > 0) {
        device_path_ += ",";
      }
      device_path_ += devices_[i].path;
    }
    return true;
  }

  if (!OpenDevicePath(device_path, true)) {
    return false;
  }
  device_path_ = device_path;
  return true;
}

bool EvdevInput::MapKeyCode(const unsigned short code,
                            Button& out_button,
                            bool& out_quit) {
  out_quit = false;

  switch (code) {
    case KEY_UP:
    case BTN_DPAD_UP:
    case BTN_TRIGGER_HAPPY3:
      out_button = Button::Up;
      return true;

    case KEY_DOWN:
    case BTN_DPAD_DOWN:
    case BTN_TRIGGER_HAPPY4:
      out_button = Button::Down;
      return true;

    case KEY_LEFT:
    case BTN_DPAD_LEFT:
    case BTN_TRIGGER_HAPPY1:
      out_button = Button::Left;
      return true;

    case KEY_RIGHT:
    case BTN_DPAD_RIGHT:
    case BTN_TRIGGER_HAPPY2:
      out_button = Button::Right;
      return true;

    case BTN_SOUTH:
    case BTN_TRIGGER:
    case BTN_BASE:
    case BTN_0:
    case KEY_D:
      out_button = Button::A;
      return true;

    case BTN_EAST:
    case BTN_THUMB:
    case BTN_BASE2:
    case BTN_1:
    case KEY_S:
      out_button = Button::B;
      return true;

    case BTN_WEST:
    case BTN_THUMB2:
    case BTN_TOP:
    case BTN_2:
    case KEY_W:
      out_button = Button::X;
      return true;

    case BTN_NORTH:
    case BTN_TOP2:
    case BTN_3:
    case KEY_A:
      out_button = Button::Y;
      return true;

    case BTN_TL:
    case BTN_TL2:
    case BTN_PINKIE:
    case BTN_4:
    case KEY_Q:
      out_button = Button::L;
      return true;

    case BTN_TR:
    case BTN_TR2:
    case BTN_BASE5:
    case BTN_5:
    case KEY_E:
      out_button = Button::R;
      return true;

    case BTN_START:
    case BTN_BASE4:
    case BTN_7:
    case KEY_ENTER:
      out_button = Button::Start;
      return true;

    case BTN_SELECT:
    case BTN_BASE3:
    case BTN_6:
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
      out_button = Button::Select;
      return true;

    case KEY_ESC:
      out_button = Button::B;
      return true;

    default:
      return false;
  }
}

bool EvdevInput::WaitAndPoll(InputFrame& out, const int timeout_ms) {
  out.pressed.clear();
  out.events.clear();
  out.quit_requested = false;

  if (devices_.empty()) {
    return false;
  }

  std::vector<pollfd> pfds(devices_.size());
  for (std::size_t i = 0; i < devices_.size(); ++i) {
    pfds[i].fd = devices_[i].fd;
    pfds[i].events = POLLIN;
    pfds[i].revents = 0;
  }

  const int pr = poll(pfds.data(), pfds.size(), timeout_ms);
  if (pr <= 0) {
    return false;
  }

  bool has_input = false;
  for (std::size_t i = 0; i < devices_.size(); ++i) {
    const auto revents = pfds[i].revents;
    if (revents == 0) {
      continue;
    }

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
      core::Log(core::LogLevel::Error,
                "evdev poll error/hangup on " + devices_[i].path);
      out.quit_requested = true;
      return true;
    }

    while (true) {
      input_event ev {};
      const ssize_t n = read(devices_[i].fd, &ev, sizeof(ev));
      if (n == static_cast<ssize_t>(sizeof(ev))) {
        auto push_event = [&](const Button button,
                              const input_event& raw_ev,
                              const bool pressed,
                              const int hold_ms,
                              const bool mapped_button = true) {
          if (pressed) {
            out.pressed.push_back(button);
          }
          out.events.push_back(InputEvent{
              .button = button,
              .mapped_button = mapped_button,
              .device_name = devices_[i].name.empty() ? devices_[i].path : devices_[i].name,
              .device_path = devices_[i].path,
              .bus_type = devices_[i].bus_type,
              .vendor = devices_[i].vendor,
              .product = devices_[i].product,
              .is_bluetooth = devices_[i].is_bluetooth,
              .is_keyboard = devices_[i].is_keyboard,
              .raw_type = raw_ev.type,
              .raw_code = raw_ev.code,
              .raw_value = raw_ev.value,
              .hold_ms = hold_ms,
          });
          has_input = true;
        };

        if (ev.type == EV_KEY) {
          Button button = Button::A;
          bool quit = false;
          int hold_ms = 0;
          if (ev.code <= KEY_MAX) {
            if (ev.value == 1) {
              devices_[i].key_down_ms[ev.code] = gb::core::NowMs();
            } else if (ev.value == 0) {
              const auto down_ms = devices_[i].key_down_ms[ev.code];
              devices_[i].key_down_ms[ev.code] = 0;
              if (down_ms > 0) {
                hold_ms = static_cast<int>(gb::core::NowMs() - down_ms);
              }
            }
          }

          if (MapKeyCode(ev.code, button, quit)) {
            if (ev.value == 0) {
              push_event(button, ev, false, hold_ms);
            }

            if (ev.value == 1 || ev.value == 2) {
              push_event(button, ev, true, 0);
            }
          } else if (ev.code != KEY_ESC) {
            if (ev.value == 0) {
              push_event(Button::A, ev, false, hold_ms, false);
            }
            if (ev.value == 1 || ev.value == 2) {
              push_event(Button::A, ev, true, 0, false);
            }
          } else if (quit) {
            if (ev.value == 1 || ev.value == 2) {
              out.quit_requested = true;
              has_input = true;
            }
          }
        }

        if (ev.type == EV_ABS) {
          if (ev.code == ABS_HAT0X) {
            if (ev.value < 0) {
              push_event(Button::Left, ev, true, 0);
            } else if (ev.value > 0) {
              push_event(Button::Right, ev, true, 0);
            }
          } else if (ev.code == ABS_HAT0Y) {
            if (ev.value < 0) {
              push_event(Button::Up, ev, true, 0);
            } else if (ev.value > 0) {
              push_event(Button::Down, ev, true, 0);
            }
          } else if (ev.code == ABS_X && devices_[i].abs_x.present) {
            auto& axis = devices_[i].abs_x;
            int dir = 0;
            if (ev.value < (axis.center - axis.deadzone)) {
              dir = -1;
            } else if (ev.value > (axis.center + axis.deadzone)) {
              dir = 1;
            }
            if (dir != axis.last_dir) {
              axis.last_dir = dir;
              if (dir < 0) {
                push_event(Button::Left, ev, true, 0);
              } else if (dir > 0) {
                push_event(Button::Right, ev, true, 0);
              }
            }
          } else if (ev.code == ABS_Y && devices_[i].abs_y.present) {
            auto& axis = devices_[i].abs_y;
            int dir = 0;
            if (ev.value < (axis.center - axis.deadzone)) {
              dir = -1;
            } else if (ev.value > (axis.center + axis.deadzone)) {
              dir = 1;
            }
            if (dir != axis.last_dir) {
              axis.last_dir = dir;
              if (dir < 0) {
                push_event(Button::Up, ev, true, 0);
              } else if (dir > 0) {
                push_event(Button::Down, ev, true, 0);
              }
            }
          }
        }

        continue;
      }

      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        break;
      }

      if (n < 0) {
        core::Log(core::LogLevel::Error,
                  "evdev read failed on " + devices_[i].path + ": " +
                      std::string(std::strerror(errno)));
        out.quit_requested = true;
        return true;
      }

      break;
    }
  }

  return has_input;
}

void EvdevInput::Shutdown() {
  for (auto& dev : devices_) {
    if (dev.fd < 0) {
      continue;
    }
    if (dev.grabbed) {
      ioctl(dev.fd, EVIOCGRAB, 0);
      dev.grabbed = false;
    }
    close(dev.fd);
    dev.fd = -1;
  }
  devices_.clear();
}

void EvdevInput::ReleaseDeviceGrabs() {
  for (auto& dev : devices_) {
    if (dev.fd < 0 || !dev.grabbed) {
      continue;
    }
    if (ioctl(dev.fd, EVIOCGRAB, 0) == 0) {
      dev.grabbed = false;
    } else {
      core::Log(core::LogLevel::Warn,
                "evdev ungrab failed for " + dev.path + ": " +
                    std::string(std::strerror(errno)));
    }
  }
}

void EvdevInput::AcquireDeviceGrabs() {
  for (auto& dev : devices_) {
    if (dev.fd < 0 || dev.grabbed) {
      continue;
    }
    if (ioctl(dev.fd, EVIOCGRAB, 1) == 0) {
      dev.grabbed = true;
    } else {
      core::Log(core::LogLevel::Warn,
                "evdev re-grab failed for " + dev.path + ": " +
                    std::string(std::strerror(errno)));
    }
  }
}

const std::string& EvdevInput::DevicePath() const { return device_path_; }

std::size_t EvdevInput::DeviceCount() const { return devices_.size(); }

std::vector<EvdevDeviceInfo> EvdevInput::ConnectedDevices() const {
  std::vector<EvdevDeviceInfo> out;
  out.reserve(devices_.size());
  for (const auto& dev : devices_) {
    out.push_back(EvdevDeviceInfo{
        .path = dev.path,
        .name = dev.name,
        .bus = dev.bus,
        .bus_type = dev.bus_type,
        .vendor = dev.vendor,
        .product = dev.product,
        .is_bluetooth = dev.is_bluetooth,
        .is_gamepad = dev.is_gamepad,
        .is_keyboard = dev.is_keyboard,
    });
  }
  return out;
}

}  // namespace gb::platform
