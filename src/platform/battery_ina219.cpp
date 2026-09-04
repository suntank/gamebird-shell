#include "platform/battery_ina219.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

namespace gb::platform {

namespace {

constexpr std::uint8_t kBusVoltageRegister = 0x02;
constexpr std::uint8_t kShuntVoltageRegister = 0x01;
constexpr std::size_t kHistoryLimit = 15;

bool ReadRegister(const int fd, const std::uint8_t reg, std::uint16_t& value) {
  if (::write(fd, &reg, 1) != 1) {
    return false;
  }
  std::array<std::uint8_t, 2> bytes {};
  if (::read(fd, bytes.data(), bytes.size()) !=
      static_cast<ssize_t>(bytes.size())) {
    return false;
  }
  value = static_cast<std::uint16_t>((bytes[0] << 8) | bytes[1]);
  return true;
}

float Median(const std::deque<float>& values) {
  std::vector<float> sorted(values.begin(), values.end());
  std::sort(sorted.begin(), sorted.end());
  const auto middle = sorted.size() / 2;
  if ((sorted.size() % 2) != 0) {
    return sorted[middle];
  }
  return (sorted[middle - 1] + sorted[middle]) * 0.5F;
}

}  // namespace

Ina219Battery::Ina219Battery(std::string device_path, const std::uint8_t address)
    : device_path_(std::move(device_path)), address_(address) {}

bool Ina219Battery::Read(core::BatteryStatus& out, std::string& error) {
  error.clear();
  out = core::BatteryStatus{};
  const int fd = ::open(device_path_.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    error = "battery I2C unavailable: " + std::string(std::strerror(errno));
    return false;
  }
  const auto close_fd = [&] { ::close(fd); };
  if (::ioctl(fd, I2C_SLAVE, address_) < 0) {
    error = "battery I2C address unavailable: " + std::string(std::strerror(errno));
    close_fd();
    return false;
  }

  std::uint16_t bus_raw = 0;
  std::uint16_t shunt_raw = 0;
  if (!ReadRegister(fd, kBusVoltageRegister, bus_raw) ||
      !ReadRegister(fd, kShuntVoltageRegister, shunt_raw)) {
    error = "battery read failed: " + std::string(std::strerror(errno));
    close_fd();
    return false;
  }
  close_fd();

  // INA219 bus voltage has 4 mV LSBs in bits 15:3. Shunt voltage is signed,
  // with 10 microvolt LSBs, represented here in millivolts.
  const float voltage = static_cast<float>(bus_raw >> 3) * 0.004F;
  const auto shunt_signed = static_cast<std::int16_t>(shunt_raw);
  const float shunt_millivolts = static_cast<float>(shunt_signed) * 0.01F;
  voltage_history_.push_back(voltage);
  if (voltage_history_.size() > kHistoryLimit) {
    voltage_history_.pop_front();
  }
  // Current reacts immediately so inserting a cell can trigger the HUD at
  // once. Only voltage needs the longer median used for a stable percentage.
  out = core::ClassifyBattery(Median(voltage_history_), shunt_millivolts);
  return true;
}

}  // namespace gb::platform
