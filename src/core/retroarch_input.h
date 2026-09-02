#pragma once

#include <string>
#include <utility>
#include <vector>

namespace gb::core {

// Each binding value is encoded as key:<name>, btn:<index-or-hat>, or
// axis:<signed-index>. Control names use RetroPad names such as "up" and "a".
std::string BuildRetroArchInputConfig(
    const std::string& device_name,
    const std::vector<std::pair<std::string, std::string>>& bindings,
    int joypad_index = -1);

bool WriteRetroArchInputConfig(
    const std::string& path,
    const std::string& device_name,
    const std::vector<std::pair<std::string, std::string>>& bindings,
    int joypad_index,
    std::string& error);

}  // namespace gb::core
