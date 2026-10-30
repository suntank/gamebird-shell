#include "nest/package.h"

#include <fstream>
#include <stdexcept>

#include "nest/json.h"

namespace gb::nest {
namespace {
namespace fs = std::filesystem;
using detail::Json;

Json Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Missing " + path.filename().string());
  std::string contents(65537, '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  contents.resize(static_cast<std::size_t>(input.gcount()));
  if (contents.size() > 65536) throw std::runtime_error("Package metadata exceeds 64 KiB");
  try {
    auto value = Json::Parse(contents);
    if (!value.IsObject()) throw std::runtime_error("expected an object");
    return value;
  } catch (const std::exception& error) {
    throw std::runtime_error("Invalid " + path.filename().string() + ": " + error.what());
  }
}

std::string String(const Json& value, const std::string& field) {
  if (!value.IsString() || value.String().empty() || value.String().size() > 4096 ||
      value.String().find('\0') != std::string::npos)
    throw std::runtime_error("Invalid package " + field);
  return value.String();
}

fs::path AbsolutePath(const Json& value, const std::string& field) {
  const fs::path path = String(value, field);
  if (!path.is_absolute()) throw std::runtime_error("Package " + field + " must be absolute");
  return path.lexically_normal();
}
}  // namespace

Package ReadPackage(const fs::path& directory) {
  if (fs::is_symlink(directory / "game.json"))
    throw std::runtime_error("game.json cannot be a symbolic link");
  const auto manifest = Read(directory / "game.json");
  if (manifest["format_version"].Number(-1) != 1)
    throw std::runtime_error("game.json requires format_version 1");
  Package package;
  package.runtime = String(manifest["runtime"], "runtime");
  if (package.runtime != "python3" && package.runtime != "native")
    throw std::runtime_error("Unsupported game runtime: " + package.runtime + " (use python3 or native)");
  const auto entrypoint = String(manifest["entrypoint"], "entrypoint");
  package.entrypoint = fs::path(entrypoint);
  if (package.entrypoint.is_absolute() || entrypoint.find('\\') != std::string::npos ||
      entrypoint.find(':') != std::string::npos)
    throw std::runtime_error("Package entrypoint must be a relative file path");
  auto current = directory;
  for (const auto& part : package.entrypoint) {
    if (part == "..") throw std::runtime_error("Package entrypoint cannot contain ..");
    current /= part;
    if (fs::is_symlink(current)) throw std::runtime_error("Package entrypoint cannot use symbolic links");
  }
  if (!fs::is_regular_file(current)) throw std::runtime_error("Missing package entrypoint: " + entrypoint);
  const auto& args = manifest["args"];
  if (manifest.Members().contains("args") && !args.IsArray())
    throw std::runtime_error("Package args must be an array of strings");
  if (args.Items().size() > 64) throw std::runtime_error("Package args exceeds 64 arguments");
  for (const auto& arg : args.Items()) {
    if (!arg.IsString() || arg.String().size() > 4096 || arg.String().find('\0') != std::string::npos)
      throw std::runtime_error("Package args must contain strings without NUL bytes");
    package.args.push_back(arg.String());
  }
  return package;
}

std::vector<std::string> PackageCommand(const fs::path& directory, const Package& package) {
  const auto entrypoint = fs::absolute(directory / package.entrypoint).lexically_normal().string();
  std::vector<std::string> argv;
  if (package.runtime == "python3") argv = {"python3", "-u", entrypoint};
  else argv = {entrypoint};
  argv.insert(argv.end(), package.args.begin(), package.args.end());
  return argv;
}

PackageLaunch ReadPackageLaunch(const fs::path& descriptor) {
  const auto data = Read(descriptor);
  if (data["managed_by"].String() != "gamebird-nest" || data["format_version"].Number(-1) != 1)
    throw std::runtime_error("Invalid Nest library record; reinstall this game");
  PackageLaunch launch;
  launch.id = String(data["id"], "id");
  launch.title = String(data["title"], "title");
  launch.directory = AbsolutePath(data["package_dir"], "package_dir");
  launch.save_directory = AbsolutePath(data["save_dir"], "save_dir");
  launch.log_path = AbsolutePath(data["log_path"], "log_path");
  launch.argv = PackageCommand(launch.directory, ReadPackage(launch.directory));
  return launch;
}

}  // namespace gb::nest
