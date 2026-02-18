#include "core/time.h"

#include <chrono>
#include <thread>

namespace gb::core {

std::uint64_t NowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void SleepMs(const std::uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace gb::core
