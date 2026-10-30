#pragma once

#include <filesystem>
#include <string>

namespace gb::render {

// Services run from the checkout; local tools may run from its build directory.
inline std::filesystem::path BundledAsset(const std::filesystem::path& path) {
  const auto relative = std::filesystem::path("assets") / path;
  std::error_code error;
  if (std::filesystem::is_regular_file(relative, error)) return relative;
  const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error) return executable.parent_path().parent_path() / relative;
  return relative;
}

inline std::filesystem::path BundledSystemArt(const std::string& filename) {
  return BundledAsset(std::filesystem::path("system-art") / filename);
}

}  // namespace gb::render
