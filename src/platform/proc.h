#pragma once

#include <string>
#include <vector>

namespace gb::platform {

struct ProcessResult {
  bool launched = false;
  bool exited_normally = false;
  int exit_code = -1;
  bool signaled = false;
  int signal = 0;
  std::string error;
};

ProcessResult RunProcessBlocking(const std::vector<std::string>& argv);

struct ProcessCaptureResult {
  ProcessResult process;
  std::string output;
};

ProcessCaptureResult RunProcessCapture(const std::vector<std::string>& argv);

}  // namespace gb::platform
