#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/time.h"
#include "platform/input_evdev.h"

namespace {

std::atomic<bool> running{true};

void Stop(int) { running = false; }

std::vector<pid_t> RetroArchPids() {
  std::vector<pid_t> result;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
    if (error) break;
    const std::string pid = entry.path().filename().string();
    if (pid.empty() ||
        !std::all_of(pid.begin(), pid.end(), [](const unsigned char c) {
          return c >= '0' && c <= '9';
        })) {
      continue;
    }
    std::ifstream comm(entry.path() / "comm");
    std::string name;
    if (std::getline(comm, name) && name == "retroarch") {
      try {
        result.push_back(static_cast<pid_t>(std::stol(pid)));
      } catch (...) {
      }
    }
  }
  return result;
}

bool RetroArchRunning() { return !RetroArchPids().empty(); }

void StopRetroArch() {
  for (const auto pid : RetroArchPids()) ::kill(pid, SIGTERM);
}

bool SendRetroArchCommand(const std::string& command) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;

  sockaddr_in address {};
  address.sin_family = AF_INET;
  address.sin_port = htons(55355);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const std::string payload = command + "\n";
  const auto sent = ::sendto(fd, payload.data(), payload.size(), 0,
                             reinterpret_cast<sockaddr*>(&address), sizeof(address));
  ::close(fd);
  return sent == static_cast<ssize_t>(payload.size());
}

bool RuntimeOverlayIsCritical() {
  std::ifstream config("/tmp/gamebird-battery-overlay.cfg");
  std::string line;
  while (std::getline(config, line)) {
    if (line.find("battery-0.png") != std::string::npos ||
        line.find("battery-5.png") != std::string::npos) {
      return true;
    }
  }
  return false;
}

void SetOverlayVisible(bool& visible, const bool desired) {
  if (visible == desired) return;
  // The runtime overlay always has exactly two pages: hidden and one fixed
  // battery image. One command is therefore an unambiguous show/hide toggle.
  if (SendRetroArchCommand("OVERLAY_NEXT")) visible = desired;
}

}  // namespace

int main() {
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);

  gb::platform::EvdevInput input;
  // Observe only; gbshell and RetroArch remain the owners of controller input.
  input.Init("auto", false);

  std::set<std::string> start_devices;
  std::set<std::string> select_devices;
  std::uint64_t start_hold_started_ms = 0;
  std::uint64_t last_volume_adjust_ms = 0;
  std::uint64_t last_process_check_ms = 0;
  bool retroarch_active = false;
  bool overlay_visible = false;
  bool critical_overlay = false;

  while (running) {
    gb::platform::InputFrame frame;
    input.WaitAndPoll(frame, 100);
    const auto now = gb::core::NowMs();

    if (last_process_check_ms == 0 || now - last_process_check_ms >= 1000) {
      const bool was_active = retroarch_active;
      retroarch_active = RetroArchRunning();
      last_process_check_ms = now;
      if (!was_active && retroarch_active) {
        // RetroArch always loads overlay page zero (hidden) for a new process.
        overlay_visible = false;
        critical_overlay = RuntimeOverlayIsCritical();
      }
      if (was_active && !retroarch_active) {
        start_devices.clear();
        select_devices.clear();
        start_hold_started_ms = 0;
        last_volume_adjust_ms = 0;
        overlay_visible = false;
        critical_overlay = false;
      }
    }

    for (const auto& event : frame.events) {
      if (!event.mapped_button) {
        continue;
      }
      if (event.button == gb::platform::Button::Start && event.raw_type == EV_KEY) {
        if (event.raw_value > 0) {
          if (start_devices.empty()) start_hold_started_ms = now;
          start_devices.insert(event.device_path);
        } else {
          start_devices.erase(event.device_path);
          if (start_devices.empty()) start_hold_started_ms = 0;
        }
        if (retroarch_active && !start_devices.empty() && !select_devices.empty()) {
          StopRetroArch();
        }
        continue;
      }
      if (event.button == gb::platform::Button::Select && event.raw_type == EV_KEY) {
        if (event.raw_value > 0) {
          select_devices.insert(event.device_path);
        } else {
          select_devices.erase(event.device_path);
        }
        if (retroarch_active && !start_devices.empty() && !select_devices.empty()) {
          StopRetroArch();
        }
        continue;
      }

      const bool direction_press =
          (event.raw_type == EV_KEY && event.raw_value > 0) ||
          (event.raw_type == EV_ABS && event.raw_value != 0);
      const bool volume_up = event.button == gb::platform::Button::Up ||
                             event.button == gb::platform::Button::LeftStickUp ||
                             event.button == gb::platform::Button::RightStickUp;
      const bool volume_down = event.button == gb::platform::Button::Down ||
                               event.button == gb::platform::Button::LeftStickDown ||
                               event.button == gb::platform::Button::RightStickDown;
      if (retroarch_active && !start_devices.empty() && direction_press &&
          (volume_up || volume_down) &&
          (last_volume_adjust_ms == 0 || now - last_volume_adjust_ms >= 140)) {
        SendRetroArchCommand(volume_up ? "VOLUME_UP" : "VOLUME_DOWN");
        last_volume_adjust_ms = now;
      }
    }

    if (retroarch_active) {
      const bool flash_on = !critical_overlay || ((now / 400) % 2 == 0);
      constexpr std::uint64_t kBatteryHoldDelayMs = 1000;
      const bool held_long_enough = !start_devices.empty() &&
                                    start_hold_started_ms != 0 &&
                                    now - start_hold_started_ms >=
                                        kBatteryHoldDelayMs;
      const bool show = held_long_enough || (critical_overlay && flash_on);
      SetOverlayVisible(overlay_visible, show);
    }
  }

  input.Shutdown();
  return 0;
}
