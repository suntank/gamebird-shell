#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gb::nest {

struct Package {
  std::string runtime;
  std::filesystem::path entrypoint;
  std::vector<std::string> args;
};

struct PackageLaunch {
  std::string id, title;
  std::filesystem::path directory, save_directory, log_path;
  std::vector<std::string> argv;
};

// Package paths stay relative to the game directory; failures explain the field.
Package ReadPackage(const std::filesystem::path& directory);
std::vector<std::string> PackageCommand(const std::filesystem::path& directory,
                                      const Package& package);
PackageLaunch ReadPackageLaunch(const std::filesystem::path& descriptor);

}  // namespace gb::nest
