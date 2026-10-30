#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/sha.h>
#include <unistd.h>

#include "db/db.h"
#include "db/queries.h"
#include "nest/service.h"
#include "platform/proc.h"

namespace {
namespace fs = std::filesystem;
int failures = 0;

void Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct TempTree {
  fs::path path = fs::temp_directory_path() /
      ("gamebird nest compatibility " + std::to_string(::getpid()) + " " +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TempTree() { fs::create_directories(path); }
  ~TempTree() { std::error_code ec; fs::remove_all(path, ec); }
};

void WriteText(const fs::path& path, const std::string& value) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << value;
  if (!out) throw std::runtime_error("could not write test fixture");
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string Sha256(const std::string& value) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
  std::ostringstream out;
  for (const auto byte : digest) out << std::hex << std::setw(2) << std::setfill('0') << int(byte);
  return out.str();
}

void LittleEndian(std::string& bytes, std::uint32_t value, int count) {
  for (int i = 0; i < count; ++i) bytes.push_back(static_cast<char>((value >> (8 * i)) & 255));
}

std::uint32_t Crc32(const std::string& bytes) {
  std::uint32_t crc = 0xffffffff;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
  }
  return ~crc;
}

// A tiny stored ZIP writer keeps these tests independent of Python and zip tools.
std::string Zip(const std::map<std::string, std::string>& files) {
  std::string result, directory;
  for (const auto& [name, contents] : files) {
    const auto offset = static_cast<std::uint32_t>(result.size());
    const auto crc = Crc32(contents);
    LittleEndian(result, 0x04034b50, 4);
    LittleEndian(result, 20, 2); LittleEndian(result, 0, 2); LittleEndian(result, 0, 2);
    LittleEndian(result, 0, 2); LittleEndian(result, 0, 2); LittleEndian(result, crc, 4);
    LittleEndian(result, contents.size(), 4); LittleEndian(result, contents.size(), 4);
    LittleEndian(result, name.size(), 2); LittleEndian(result, 0, 2);
    result += name; result += contents;
    LittleEndian(directory, 0x02014b50, 4);
    LittleEndian(directory, 0x0314, 2); LittleEndian(directory, 20, 2);
    LittleEndian(directory, 0, 2); LittleEndian(directory, 0, 2);
    LittleEndian(directory, 0, 2); LittleEndian(directory, 0, 2);
    LittleEndian(directory, crc, 4); LittleEndian(directory, contents.size(), 4);
    LittleEndian(directory, contents.size(), 4); LittleEndian(directory, name.size(), 2);
    LittleEndian(directory, 0, 2); LittleEndian(directory, 0, 2); LittleEndian(directory, 0, 2);
    LittleEndian(directory, 0, 2); LittleEndian(directory, 0100644u << 16, 4);
    LittleEndian(directory, offset, 4); directory += name;
  }
  const auto directory_offset = result.size();
  result += directory;
  LittleEndian(result, 0x06054b50, 4); LittleEndian(result, 0, 2); LittleEndian(result, 0, 2);
  LittleEndian(result, files.size(), 2); LittleEndian(result, files.size(), 2);
  LittleEndian(result, directory.size(), 4); LittleEndian(result, directory_offset, 4);
  LittleEndian(result, 0, 2);
  return result;
}

gb::nest::Config TestConfig(const fs::path& root) {
  gb::nest::Config config;
  config.api_base_url = "file://" + (root / "api").string();
  config.cdn_base_url = "file://" + (root / "cdn").string();
  config.data_dir = root / "data" / "nest";
  config.games_dir = config.data_dir / "games";
  config.launchers_dir = root / "library" / "apps";
  config.legacy_data_dir = root / "old store";
  config.legacy_games_dir = root / "old games";
  return config;
}

gb::nest::Manifest MakePackage(const gb::nest::Config& config,
                              const std::string& slug, const std::string& title,
                              const std::map<std::string, std::string>& files) {
  const auto bytes = Zip(files);
  WriteText(fs::path(config.cdn_base_url.substr(7)) / (slug + ".zip"), bytes);
  gb::nest::Manifest manifest;
  manifest.id = manifest.slug = slug;
  manifest.title = title;
  manifest.version = manifest.download.version = "1.0.0";
  manifest.download.path = slug + ".zip";
  manifest.download.size_bytes = bytes.size();
  manifest.download.sha256 = Sha256(bytes);
  return manifest;
}

void CheckProbe(const gb::nest::Service& service,
                const gb::nest::InstalledGame& installed,
                const fs::path& expected_directory) {
  const auto path = service.LaunchPath(installed);
  Expect(fs::is_regular_file(path), "installed game has a native library launcher");
  // This is a generated shell-only probe that prints PWD; no downloaded game,
  // interpreter, or external application is executed by this compatibility test.
  const auto result = gb::platform::RunProcessCapture({path.string()});
  Expect(result.process.exited_normally && result.process.exit_code == 0,
         "legacy-style launcher runs with a configured path containing spaces: " + result.output);
  Expect(result.output == expected_directory.string() + "\n",
         "legacy launcher resolves its payload directory correctly: " + result.output);
}

void TestArchiveLaunchCompatibility(const fs::path& root) {
  const auto config = TestConfig(root);
  gb::nest::Service service(config);
  // Matches the top-level launcher + camel-case payload folder seen in the
  // shipped Drop Grid and Hungry Hatchling archives, without copying the games.
  const auto first = MakePackage(config, "drop-grid", "Drop Grid", {
      {"Drop Grid.sh", "#!/bin/sh\n# The game's working directory contains its assets.\ncd /home/pi/gamebird/games/dropGrid || exit 1\nprintf '%s\\n' \"$PWD\"\n"},
      {"dropGrid/main.py", "# compatibility fixture only\n"},
      {"dropGrid/helper.sh", "# support file, not an app\n"},
      {"dropGrid/data.bin", "payload data"},
  });
  service.Install(first);
  auto installed = service.LoadInstalled();
  Expect(installed.count(first.slug) == 1, "first archive is registered by catalog slug");
  if (installed.count(first.slug)) CheckProbe(service, installed.at(first.slug), config.games_dir / "dropGrid");
  auto damaged_update = first;
  damaged_update.download.sha256 = std::string(64, '0');
  bool rejected = false;
  try { service.Install(damaged_update); } catch (const std::exception&) { rejected = true; }
  Expect(rejected, "a damaged update is rejected");
  installed = service.LoadInstalled();
  if (installed.count(first.slug)) CheckProbe(service, installed.at(first.slug), config.games_dir / "dropGrid");

  const auto second = MakePackage(config, "hungry-hatchling", "Hungry Hatchling", {
      {"game/Hungry Hatchling.sh", "#!/bin/sh\nGAME=hungryHatchling\ncd \"${HOME}/gamebird/games/${GAME}\" || exit 1\nprintf '%s\\n' \"$PWD\"\n"},
      {"game/hungryHatchling/main.py", "# compatibility fixture only\n"},
  });
  service.Install(second);
  installed = service.LoadInstalled();
  Expect(installed.count(second.slug) == 1, "wrapper-folder archive is registered");
  if (installed.count(second.slug)) CheckProbe(service, installed.at(second.slug), config.games_dir / "hungryHatchling");

  const auto systems = root / "systems.d";
  WriteText(systems / "apps.json", R"({"id":"apps","name":"GAMEBIRD","rom_extensions":[".sh",".elf",".bin"],"launch_type":"exec","launch_template":"{rom_path}"})");
  gb::db::Database db;
  Expect(db.Open((root / "catalog.db").string()) && db.InitSchema(), "initialize scan catalog");
  gb::db::ScanConfig scan;
  scan.systems_dir = systems.string();
  scan.override_roots = {config.launchers_dir.string()};
  gb::db::ScanStats stats;
  Expect(gb::db::RunIncrementalScan(db, scan, stats), "installed launchers are scanned");
  Expect(stats.present_games == 2 && stats.files_seen == 2,
         "each package appears once, without nested helper scripts or binary data");
  service.RefreshLaunchers();
  Expect(gb::db::RunIncrementalScan(db, scan, stats) && stats.present_games == 2,
         "refreshing launchers preserves a single library entry per package");
  service.Uninstall(first.slug);
  Expect(fs::exists(config.games_dir / "hungryHatchling" / "main.py"),
         "uninstalling one package preserves the other payload");
}

std::string LegacyRecord(const std::string& id, const fs::path& path) {
  return "{\"id\":\"" + id + "\",\"title\":\"Legacy Game\",\"installed_version\":\"1.2\",\"install_path\":\"" +
      path.string() + "\",\"installed_files\":[\"Legacy Game.sh\",\"legacyGame/main.py\"]}";
}

void TestSharedLegacyUpdate(const fs::path& root) {
  const auto config = TestConfig(root);
  WriteText(config.games_dir / "One.sh", "#!/bin/sh\nexit 0\n");
  WriteText(config.games_dir / "Two.sh", "#!/bin/sh\nexit 0\n");
  WriteText(config.games_dir / "shared.txt", "asset still used by Two");
  const auto shared_record = [&](const std::string& id, const std::string& title) {
    return "{\"id\":\"" + id + "\",\"title\":\"" + title +
        "\",\"installed_version\":\"1.0\",\"install_path\":\"" + config.games_dir.string() +
        "\",\"installed_files\":[\"" + title + ".sh\",\"shared.txt\"]}";
  };
  WriteText(config.data_dir / "installed_games.json", "{\"games\":[" +
      shared_record("one", "One") + "," + shared_record("two", "Two") + "]}");
  gb::nest::Service service(config);
  service.RefreshLaunchers();
  auto update = MakePackage(config, "one", "One", {
      {"One.sh", "#!/bin/sh\nexit 0\n"}, {"one-data.txt", "new private asset"},
  });
  update.download.version = "2.0";
  try { service.Install(update); } catch (const std::exception&) {
    // Rejecting an ambiguous old ownership record is also a valid safe policy.
  }
  Expect(ReadText(config.games_dir / "shared.txt") == "asset still used by Two",
         "updating a legacy game cannot remove a file also owned by another game");
  Expect(service.LoadInstalled().contains("two"), "shared-file update preserves the other game registry entry");
}

void TestIdentifierMigration(const fs::path& root) {
  const auto config = TestConfig(root);
  WriteText(config.games_dir / "Renamed Game.sh", "#!/bin/sh\nexit 0\n");
  WriteText(config.data_dir / "installed_games.json",
      "{\"games\":[{\"id\":\"catalog-id\",\"title\":\"Renamed Game\",\"installed_version\":\"1.0\","
      "\"install_path\":\"" + config.games_dir.string() +
      "\",\"installed_files\":[\"Renamed Game.sh\"]}]}");
  gb::nest::Service service(config);
  service.RefreshLaunchers();
  auto update = MakePackage(config, "friendly-slug", "Renamed Game", {
      {"Renamed Game.sh", "#!/bin/sh\nexit 0\n"},
  });
  update.id = "catalog-id";
  update.download.version = "2.0";
  service.Install(update);
  const auto installed = service.LoadInstalled();
  Expect(installed.contains("friendly-slug") && !installed.contains("catalog-id"),
         "an older catalog ID registry entry migrates to its API slug");
  int launchers = 0;
  for (const auto& entry : fs::directory_iterator(config.launchers_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".sh") ++launchers;
  }
  Expect(launchers == 1, "migrating a catalog ID to a slug leaves one discoverable launcher");
}

void TestLegacyImport(const fs::path& root) {
  const auto config = TestConfig(root / "valid import");
  WriteText(config.legacy_games_dir / "Legacy Game.sh", "#!/bin/sh\nexit 0\n");
  WriteText(config.legacy_games_dir / "legacyGame" / "main.py", "legacy payload");
  WriteText(config.legacy_data_dir / "installed_games.json",
            "{\"games\":[" + LegacyRecord("legacy-game", config.legacy_games_dir) + "]}");
  WriteText(config.legacy_data_dir / "ratings.json", "{\"ratings\":{\"legacy-game\":4}}");
  WriteText(config.legacy_data_dir / "parental_pin.txt", Sha256("1234"));
  gb::nest::Service service(config);
  service.RefreshLaunchers();
  const auto installed = service.LoadInstalled();
  Expect(installed.count("legacy-game") == 1, "legacy installed-game registry is preserved");
  Expect(service.LoadRatings().contains("legacy-game") && service.LoadRatings().at("legacy-game") == 4,
         "existing personal ratings survive migration");
  Expect(service.ParentalLocked() && service.VerifyPin("1234"), "existing parental PIN stays enabled and locked");
  if (installed.count("legacy-game")) {
    Expect(fs::is_regular_file(service.LaunchPath(installed.at("legacy-game"))),
           "legacy installed game gains a discoverable launcher");
  }

  const auto unsafe_config = TestConfig(root / "unsafe import");
  const auto outside = root / "outside data";
  WriteText(outside / "Legacy Game.sh", "do not remove");
  WriteText(outside / "legacyGame" / "main.py", "do not remove either");
  WriteText(unsafe_config.legacy_data_dir / "installed_games.json",
            "{\"games\":[" + LegacyRecord("outside-game", outside) + "]}");
  try {
    gb::nest::Service unsafe(unsafe_config);
    Expect(!unsafe.LoadInstalled().contains("outside-game"), "legacy import does not trust arbitrary installation roots");
  } catch (const std::exception&) {
    // Rejecting the entire untrusted registry is also a safe import policy.
  }
  Expect(ReadText(outside / "Legacy Game.sh") == "do not remove" &&
             ReadText(outside / "legacyGame" / "main.py") == "do not remove either",
         "rejecting unsafe legacy state preserves external files");

  const auto symlink_config = TestConfig(root / "symlink import");
  fs::create_directories(symlink_config.legacy_games_dir);
  const auto alias = symlink_config.legacy_games_dir / "external";
  fs::create_directory_symlink(outside, alias);
  WriteText(symlink_config.legacy_data_dir / "installed_games.json",
            "{\"games\":[" + LegacyRecord("symlink-game", alias) + "]}");
  try {
    gb::nest::Service unsafe(symlink_config);
    Expect(!unsafe.LoadInstalled().contains("symlink-game"),
           "legacy import rejects installation paths escaping through a symlink");
  } catch (const std::exception&) {
  }
  Expect(ReadText(outside / "Legacy Game.sh") == "do not remove",
         "rejecting symlinked legacy state preserves external files");
}
}  // namespace

int main() {
  TempTree temp;
  try {
    TestArchiveLaunchCompatibility(temp.path / "native install");
    TestSharedLegacyUpdate(temp.path / "shared ownership");
    TestIdentifierMigration(temp.path / "identifier migration");
    TestLegacyImport(temp.path / "migration");
  } catch (const std::exception& error) {
    std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
    ++failures;
  }
  if (failures) return 1;
  std::cout << "nest launch compatibility tests passed\n";
  return 0;
}
