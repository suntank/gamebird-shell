#include "core/battery_status.h"

#include <algorithm>
#include <cmath>

namespace gb::core {

BatteryStatus ClassifyBattery(const float voltage, const float shunt_millivolts) {
  BatteryStatus status;
  status.voltage = voltage;
  status.shunt_millivolts = shunt_millivolts;
  // GamePi13 leaves the INA219 bus-voltage input sitting near 4.07 V while
  // powered over USB even when no cell is installed. A real cell supplying or
  // accepting current produces a measurable shunt drop; the empty holder only
  // shows only converter noise. Requiring 0.5 mV is still far below normal
  // GamePi13 charge/discharge current, but rejects empty-holder transients.
  status.available = voltage > 0.0F && std::abs(shunt_millivolts) >= 0.5F;
  if (!status.available) {
    return status;
  }

  // The original overlay used INA219 current polarity. Shunt voltage retains
  // that polarity without requiring us to write a calibration register.
  status.charging = shunt_millivolts > 1.0F;
  const float minimum = status.charging ? 3.9F : 3.3F;
  const float maximum = status.charging ? 4.3F : 4.0F;
  const float normalized = std::clamp((voltage - minimum) / (maximum - minimum),
                                      0.0F, 1.0F);
  status.percent = static_cast<int>(std::lround(normalized * 100.0F));
  status.critical = !status.charging && voltage <= 3.3F;
  return status;
}

}  // namespace gb::core
