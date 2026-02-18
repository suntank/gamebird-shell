#include "core/logging.h"

#include <ctime>
#include <iostream>

namespace gb::core {

namespace {

const char* LevelName(const LogLevel level) {
  switch (level) {
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warn:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
  }
  return "UNKNOWN";
}

}  // namespace

void Log(const LogLevel level, const std::string_view msg) {
  std::time_t now = std::time(nullptr);
  char ts[32] = {};
  std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&now));
  std::cerr << '[' << ts << "] [" << LevelName(level) << "] " << msg << '\n';
}

}  // namespace gb::core
