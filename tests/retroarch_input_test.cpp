#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/retroarch_input.h"

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
  }
}

bool Contains(const std::string& text, const std::string& expected) {
  return text.find(expected) != std::string::npos;
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::string>> bindings = {
      {"up", "btn:h0up"},
      {"a", "btn:1"},
      {"b", "key:d"},
      {"left", "axis:-0"},
      {"ignored", "unknown:4"},
  };
  const std::string config =
      gb::core::BuildRetroArchInputConfig("Test Pad", bindings, 2);
  Expect(Contains(config, "# Active device: Test Pad"), "write device comment");
  Expect(Contains(config, "input_player1_joypad_index = \"2\""),
         "select active joypad index");
  Expect(Contains(config, "input_player1_up_btn = \"h0up\""),
         "write hat binding");
  Expect(Contains(config, "input_player1_a_btn = \"1\""),
         "write button binding");
  Expect(Contains(config, "input_player1_b = \"d\""),
         "write keyboard binding");
  Expect(Contains(config, "input_player1_left_axis = \"-0\""),
         "write axis binding");
  Expect(!Contains(config, "input_player1_ignored"), "ignore invalid encoding");
  Expect(Contains(config, "input_player1_a = \"nul\""),
         "clear inherited keyboard binding");
  Expect(Contains(config, "input_player1_b_btn = \"nul\""),
         "clear inherited joypad binding");

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = std::filesystem::temp_directory_path() /
                   ("gamebird-ra-input-test-" + std::to_string(stamp));
  const auto path = dir / "retroarch-input.cfg";
  std::string error;
  Expect(gb::core::WriteRetroArchInputConfig(path.string(), "Test Pad", bindings,
                                              2, error),
         "atomically write config: " + error);
  std::ifstream in(path);
  std::ostringstream saved;
  saved << in.rdbuf();
  Expect(saved.str() == config, "saved config matches generated config");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  if (failures == 0) {
    std::cout << "retroarch_input_test: PASS\n";
  }
  return failures == 0 ? 0 : 1;
}
