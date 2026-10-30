#pragma once

#include <string>
#include <map>
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

struct ProcessOptions {
  std::string working_directory;
  std::string log_path;
  std::map<std::string, std::string> environment;
};

ProcessResult RunProcessBlocking(const std::vector<std::string>& argv,
                                 const ProcessOptions& options = {});

struct ProcessCaptureResult {
  ProcessResult process;
  std::string output;
};

ProcessCaptureResult RunProcessCapture(const std::vector<std::string>& argv);

}  // namespace gb::platform
