#pragma once

#include <cstdint>
#include <deque>
#include <string>

#include "core/battery_status.h"

namespace gb::platform {

class Ina219Battery {
 public:
  Ina219Battery(std::string device_path, std::uint8_t address);

  bool Read(core::BatteryStatus& out, std::string& error);

 private:
  std::string device_path_;
  std::uint8_t address_;
  std::deque<float> voltage_history_;
};

}  // namespace gb::platform
