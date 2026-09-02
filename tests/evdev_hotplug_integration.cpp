#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "platform/input_evdev.h"

namespace {

constexpr const char* kDeviceName = "GameBird Hotplug Test Pad";

int CreateTestPad() {
  const int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, BTN_SOUTH) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, BTN_EAST) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, BTN_START) < 0 ||
      ioctl(fd, UI_SET_KEYBIT, BTN_SELECT) < 0) {
    close(fd);
    return -1;
  }

  uinput_setup setup {};
  std::strncpy(setup.name, kDeviceName, UINPUT_MAX_NAME_SIZE - 1);
  setup.id.bustype = BUS_USB;
  setup.id.vendor = 0x1209;
  setup.id.product = 0x4742;
  setup.id.version = 1;
  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    close(fd);
    return -1;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  return fd;
}

void DestroyTestPad(const int fd) {
  if (fd >= 0) {
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
  }
}

bool EmitKey(const int fd, const unsigned short code, const int value) {
  input_event key {};
  key.type = EV_KEY;
  key.code = code;
  key.value = value;
  input_event sync {};
  sync.type = EV_SYN;
  sync.code = SYN_REPORT;
  return write(fd, &key, sizeof(key)) == static_cast<ssize_t>(sizeof(key)) &&
         write(fd, &sync, sizeof(sync)) == static_cast<ssize_t>(sizeof(sync));
}

bool HasTestPad(const gb::platform::EvdevInput& input) {
  for (const auto& device : input.ConnectedDevices()) {
    if (device.name == kDeviceName) {
      return true;
    }
  }
  return false;
}

bool WaitForPresence(gb::platform::EvdevInput& input, const bool present) {
  for (int i = 0; i < 30; ++i) {
    input.RefreshDevices(true);
    if (HasTestPad(input) == present) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

}  // namespace

int main() {
  int pad_fd = CreateTestPad();
  if (pad_fd < 0) {
    std::cerr << "FAIL: create uinput test pad (run as root)\n";
    return 1;
  }

  gb::platform::EvdevInput input;
  if (!input.Init("auto") || !WaitForPresence(input, true)) {
    std::cerr << "FAIL: discover initial test pad\n";
    DestroyTestPad(pad_fd);
    return 1;
  }

  EmitKey(pad_fd, BTN_SOUTH, 1);
  EmitKey(pad_fd, BTN_SOUTH, 0);
  bool mapped_event = false;
  for (int i = 0; i < 20 && !mapped_event; ++i) {
    gb::platform::InputFrame frame;
    input.WaitAndPoll(frame, 100);
    for (const auto& event : frame.events) {
      if (event.device_name == kDeviceName &&
          event.retroarch_binding.rfind("btn:", 0) == 0) {
        mapped_event = true;
      }
    }
  }
  if (!mapped_event) {
    std::cerr << "FAIL: virtual button did not produce RetroArch binding\n";
    input.Shutdown();
    DestroyTestPad(pad_fd);
    return 1;
  }

  DestroyTestPad(pad_fd);
  pad_fd = -1;
  if (!WaitForPresence(input, false)) {
    std::cerr << "FAIL: disconnected pad was not removed\n";
    input.Shutdown();
    return 1;
  }

  pad_fd = CreateTestPad();
  if (pad_fd < 0 || !WaitForPresence(input, true)) {
    std::cerr << "FAIL: reconnected pad was not rediscovered\n";
    input.Shutdown();
    DestroyTestPad(pad_fd);
    return 1;
  }

  input.Shutdown();
  DestroyTestPad(pad_fd);
  std::cout << "evdev_hotplug_integration: PASS\n";
  return 0;
}
