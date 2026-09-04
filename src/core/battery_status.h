#pragma once

namespace gb::core {

struct BatteryStatus {
  bool available = false;
  bool charging = false;
  bool critical = false;
  int percent = 0;
  float voltage = 0.0F;
  float shunt_millivolts = 0.0F;
};

BatteryStatus ClassifyBattery(float voltage, float shunt_millivolts);

}  // namespace gb::core
