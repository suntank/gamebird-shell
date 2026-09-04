#include <iostream>
#include <string>

#include "core/battery_status.h"

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  const auto empty = gb::core::ClassifyBattery(0.0F, 0.0F);
  Expect(!empty.available, "zero voltage reports unavailable");

  const auto powered_without_cell = gb::core::ClassifyBattery(4.072F, 0.20F);
  Expect(!powered_without_cell.available,
         "powered battery rail without cell current reports unavailable");

  const auto mid = gb::core::ClassifyBattery(3.65F, -2.0F);
  Expect(mid.available && !mid.charging && !mid.critical && mid.percent == 50,
         "discharging voltage maps to midpoint");

  const auto full = gb::core::ClassifyBattery(4.1F, -1.0F);
  Expect(full.percent == 100 && !full.charging, "discharging voltage clamps full");

  const auto charging = gb::core::ClassifyBattery(4.1F, 2.0F);
  Expect(charging.charging && charging.percent == 50,
         "positive shunt voltage uses charging curve");

  const auto critical = gb::core::ClassifyBattery(3.29F, -1.0F);
  Expect(critical.critical && critical.percent == 0,
         "low discharging voltage is critical");

  if (failures != 0) {
    std::cerr << failures << " battery assertion(s) failed\n";
    return 1;
  }
  std::cout << "battery status tests passed\n";
  return 0;
}
