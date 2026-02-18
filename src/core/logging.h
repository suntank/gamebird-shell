#pragma once

#include <string_view>

namespace gb::core {

enum class LogLevel {
  Info,
  Warn,
  Error,
};

void Log(LogLevel level, std::string_view msg);

}  // namespace gb::core
