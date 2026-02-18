#pragma once

#include <cstdint>

namespace gb::core {

std::uint64_t NowMs();
void SleepMs(std::uint32_t ms);

}  // namespace gb::core
