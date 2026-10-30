#include <chrono>
#include <algorithm>
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
#include "nest/package.h"
#include "nest/json.h"
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

void TestPackages(const fs::path& root, const std::string& helper) {
  const auto config=TestConfig(root);
  gb::nest::Service service(config);
  const std::string manifest=R"JSON({"format_version":1,"runtime":"python3","entrypoint":"main.py","args":["space argument","literal;$(touch NEVER)","{not_a_template}",""]})JSON";
  const auto python=MakePackage(config,"drop-grid","Drop Grid!",{
    {"game.json",manifest},
    {"main.py", R"(import os, pathlib, sys
assert pathlib.Path.cwd() == pathlib.Path(os.environ['GAMEBIRD_GAME_DIR'])
assert pathlib.Path('assets/data.txt').read_text() == 'relative asset'
assert sys.argv[1:] == ['space argument', 'literal;$(touch NEVER)', '{not_a_template}', '']
saves = pathlib.Path(os.environ['GAMEBIRD_SAVE_DIR'])
save = saves / 'progress.txt'
save.write_text(str(int(save.read_text()) + 1) if save.exists() else '1')
print('package stdout')
print('package stderr', file=sys.stderr)
)"}, {"assets/data.txt","relative asset"}, {"helper.sh","not a library entry"},
  });
  service.Install(python);
  const auto installed=service.LoadInstalled().at("drop-grid");
  const auto descriptor=service.LaunchPath(installed);
  const auto launch=gb::nest::ReadPackageLaunch(descriptor);
  Expect(launch.directory==config.games_dir/"drop-grid","package has an isolated game directory");
  Expect(launch.argv.size()==7 && launch.argv[0]=="python3" && launch.argv[1]=="-u" &&
         launch.argv[4]=="literal;$(touch NEVER)" && launch.argv.back().empty(),
         "manifest arguments retain spaces, shell characters, braces and empty strings");
  Expect(launch.save_directory==config.data_dir/"saves/drop-grid","save directory is outside package files");

  auto native=MakePackage(config,"native-game","Native Game",{
    {"game.json",R"({"format_version":1,"runtime":"native","entrypoint":"bin/probe","args":[]})"},
    {"bin/probe","#!/bin/sh\nprintf 'native stdout\\n'\nexit 0\n"},
  });
  service.Install(native);
  Expect((fs::status(config.games_dir/"native-game/bin/probe").permissions()&fs::perms::owner_exec)!=fs::perms::none,
         "installer makes the declared native entrypoint executable");
  Expect(gb::nest::ReadPackageLaunch(service.LaunchPath(service.LoadInstalled().at("native-game"))).argv.front()==
         (config.games_dir/"native-game/bin/probe").string(),"native entrypoint runs directly");

  const auto systems=root/"systems.d";
  WriteText(systems/"apps.json",R"({"id":"apps","name":"GAMEBIRD","rom_extensions":[".gbgame"],"launch_type":"exec","launch_template":"{rom_path}"})");
  gb::db::Database db;
  const auto database=root/"catalog.db";
  Expect(db.Open(database.string())&&db.InitSchema(),"initialize package catalog");
  gb::db::ScanConfig scan;
  scan.systems_dir=systems.string(); scan.override_roots={config.launchers_dir.string()};
  gb::db::ScanStats stats;
  Expect(gb::db::RunIncrementalScan(db,scan,stats)&&stats.present_games==2&&stats.files_seen==2,
         "each package appears once; payload and helper files are not scanned");
  std::vector<gb::db::LaunchInfo> games;
  db.ListPresentLaunchInfos(games);
  const auto game=std::find_if(games.begin(),games.end(),[](const auto& value){return value.title=="Drop Grid!";});
  Expect(game!=games.end(),"library reads publisher title from the generated data record");
  if (!helper.empty()&&game!=games.end()) {
    const auto run=[&](const int expected){
      const auto result=gb::platform::RunProcessCapture({helper,"--db",database.string(),"--game-id",std::to_string(game->game_id)});
      Expect(result.process.exit_code==expected,"real helper returns the expected package exit status: "+result.output);
      return result.output;
    };
    const auto dry=gb::platform::RunProcessCapture({helper,"--db",database.string(),"--game-id",std::to_string(game->game_id),"--dry-run"});
    Expect(dry.process.exit_code==0&&!fs::exists(launch.save_directory),"dry run validates without creating saves or executing the game");
    run(0);
    Expect(ReadText(launch.save_directory/"progress.txt")=="1","game receives a writable persistent save directory");
    Expect(ReadText(launch.log_path).find("package stdout")!=std::string::npos&&
           ReadText(launch.log_path).find("package stderr")!=std::string::npos,"game log captures stdout and stderr");
    run(0);
    Expect(ReadText(launch.save_directory/"progress.txt")=="2","save data survives consecutive launches");
    Expect(!fs::exists(launch.directory/"NEVER"),"literal args never execute shell syntax");
    gb::platform::ProcessOptions no_runtime;
    no_runtime.environment={{"PATH","/no-gamebird-runtimes"}};
    const auto missing_runtime=gb::platform::RunProcessBlocking(
        {helper,"--db",database.string(),"--game-id",std::to_string(game->game_id)},no_runtime);
    Expect(missing_runtime.exit_code==1&&
           ReadText(root/"continue"/("result-"+std::to_string(game->game_id))).find("python3")!=std::string::npos,
           "missing Python runtime produces a useful shell error");
    service.Install(python);
    run(0);
    Expect(ReadText(launch.save_directory/"progress.txt")=="3","package replacement preserves persistent progress");
    WriteText(launch.directory/"main.py","import sys\nprint('failure reason', file=sys.stderr)\nsys.exit(7)\n");
    run(7);
    Expect(ReadText(launch.log_path).find("failure reason")!=std::string::npos,"failed game keeps its error in the log");
    Expect(ReadText(root/"continue"/("result-"+std::to_string(game->game_id))).find("code 7")!=std::string::npos,
           "shell receipt explains the package exit failure");
    WriteText(launch.directory/"main.py","import os, signal\nos.kill(os.getpid(), signal.SIGTERM)\n");
    run(143);
    Expect(ReadText(root/"continue"/("result-"+std::to_string(game->game_id))).find("signal 15")!=std::string::npos,
           "signal failures return to the shell with an explanation");
    WriteText(launch.directory/"game.json",R"({"format_version":1,"runtime":"native","entrypoint":"missing"})");
    run(1);
    Expect(ReadText(root/"continue"/("result-"+std::to_string(game->game_id))).find("Missing package entrypoint")!=std::string::npos,
           "missing entrypoint is reported before launching");
    service.Uninstall("drop-grid");
    Expect(ReadText(launch.save_directory/"progress.txt")=="3","removal preserves save data");
    service.Install(python);
    run(0);
    Expect(ReadText(launch.save_directory/"progress.txt")=="4","reinstallation reuses the same save data");

    const auto native_game=std::find_if(games.begin(),games.end(),[](const auto& value){return value.title=="Native Game";});
    if (native_game!=games.end()) {
      const auto result=gb::platform::RunProcessCapture({helper,"--db",database.string(),"--game-id",std::to_string(native_game->game_id)});
      Expect(result.process.exit_code==0,"real helper launches a native entrypoint directly");
    }
  }
  // Renaming a helper or setting a different cwd never affects the parent.
  const auto before=fs::current_path();
  gb::platform::ProcessOptions options;
  options.working_directory=(root/"does-not-exist").string();
  const auto failed=gb::platform::RunProcessBlocking({"/bin/true"},options);
  Expect(!failed.launched&&failed.error.find("directory")!=std::string::npos&&fs::current_path()==before,
         "child setup failures are reported without changing the parent working directory");
  const auto missing=gb::platform::RunProcessBlocking({"/definitely-missing-gamebird-runtime"});
  Expect(!missing.launched&&!missing.error.empty(),"missing executable is a launch failure with an explanation");
  service.Uninstall("drop-grid");
  Expect(fs::exists(config.games_dir/"native-game/bin/probe"),"uninstalling one package preserves the other game");
}

void TestOldPackages(const fs::path& root) {
  const auto config=TestConfig(root);
  WriteText(config.legacy_games_dir/"Old Game.sh","#!/bin/sh\nexit 0\n");
  WriteText(config.legacy_data_dir/"installed_games.json",
    "{\"games\":[{\"id\":\"old-game\",\"title\":\"Old Game\",\"installed_version\":\"1.0\",\"install_path\":\""+
    config.legacy_games_dir.string()+"\",\"installed_files\":[\"Old Game.sh\"]}]}");
  gb::nest::Service service(config);
  WriteText(config.launchers_dir/"nest-old-game.sh","#!/bin/sh\n# Managed by Game Bird Nest.\nexit 0\n");
  WriteText(config.launchers_dir/"unrelated.sh","user-owned script");
  service.RefreshLaunchers();
  const auto installed=service.LoadInstalled();
  Expect(installed.contains("old-game")&&service.LaunchPath(installed.at("old-game")).empty(),
         "legacy metadata can be replaced but never creates an old shell launcher");
  Expect(!fs::exists(config.launchers_dir/"nest-old-game.sh") &&
         ReadText(config.launchers_dir/"unrelated.sh")=="user-owned script",
         "retire only Nest's old generated script without touching unrelated apps");
  Expect(fs::exists(config.legacy_games_dir/"Old Game.sh"),"conversion does not delete old user payloads");
  const auto converted=MakePackage(config,"old-game","Old Game",{
    {"game.json",R"({"format_version":1,"runtime":"python3","entrypoint":"main.py"})"},{"main.py","print('converted')\n"}
  });
  service.Install(converted);
  Expect(service.LoadInstalled().at("old-game").package_format_version==1&&
         service.LoadInstalled().at("old-game").install_path==config.games_dir/"old-game",
         "replacing legacy metadata installs the new isolated package");
}
}  // namespace

int main(int argc,char** argv) {
  TempTree temp;
  try {
    TestPackages(temp.path/"native install",argc>1?argv[1]:"");
    TestOldPackages(temp.path/"old packages");
  } catch (const std::exception& error) {
    std::cerr<<"FAIL: unexpected exception: "<<error.what()<<'\n';
    ++failures;
  }
  if (failures) return 1;
  std::cout<<"nest package tests passed\n";
  return 0;
}
