#include "core/settings.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace gb::core {

namespace {

std::string ReadFileText(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

bool ExtractBool(const std::string& json,
                 const std::string& key,
                 bool& out_value) {
  const std::regex re("\\\"" + key + "\\\"\\s*:\\s*(true|false)",
                      std::regex::icase);
  std::smatch match;
  if (!std::regex_search(json, match, re) || match.size() < 2) {
    return false;
  }

  const std::string value = match[1].str();
  out_value = (value == "true" || value == "TRUE" || value == "True");
  return true;
}

bool ExtractString(const std::string& json,
                   const std::string& key,
                   std::string& out_value) {
  const std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"",
                      std::regex::icase);
  std::smatch match;
  if (!std::regex_search(json, match, re) || match.size() < 2) {
    return false;
  }
  out_value = match[1].str();
  return true;
}

std::string EscapeJsonString(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

}  // namespace

bool LoadRuntimeSettings(const std::string& path,
                         RuntimeSettings& out,
                         std::string& error) {
  error.clear();
  out = RuntimeSettings{};

  if (!std::filesystem::exists(path)) {
    return true;
  }

  const std::string text = ReadFileText(path);
  if (text.empty()) {
    error = "settings file exists but could not be read: " + path;
    return false;
  }

  bool v = false;
  if (ExtractBool(text, "show_diagnostics", v)) {
    out.show_diagnostics = v;
  }
  if (ExtractBool(text, "show_hidden_games", v)) {
    out.show_hidden_games = v;
  }
  if (ExtractBool(text, "enable_bluetooth_gamepads", v)) {
    out.enable_bluetooth_gamepads = v;
  }
  std::string s;
  if (ExtractString(text, "preferred_input_device", s)) {
    out.preferred_input_device = s;
  }
  if (ExtractString(text, "input_profiles", s)) {
    out.input_profiles = s;
  }

  return true;
}

bool SaveRuntimeSettings(const std::string& path,
                         const RuntimeSettings& settings,
                         std::string& error) {
  error.clear();

  std::error_code ec;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
  }

  const std::string tmp_path = path + ".tmp";
  std::ofstream out(tmp_path, std::ios::trunc);
  if (!out) {
    error = "failed to open settings file for write: " + tmp_path;
    return false;
  }

  out << "{\n";
  out << "  \"show_diagnostics\": "
      << (settings.show_diagnostics ? "true" : "false") << ",\n";
  out << "  \"show_hidden_games\": "
      << (settings.show_hidden_games ? "true" : "false") << ",\n";
  out << "  \"enable_bluetooth_gamepads\": "
      << (settings.enable_bluetooth_gamepads ? "true" : "false") << ",\n";
  out << "  \"preferred_input_device\": \""
      << EscapeJsonString(settings.preferred_input_device) << "\",\n";
  out << "  \"input_profiles\": \""
      << EscapeJsonString(settings.input_profiles) << "\"\n";
  out << "}\n";

  if (!out.good()) {
    error = "failed to write settings file: " + tmp_path;
    return false;
  }

  out.close();
  if (!out) {
    error = "failed to close settings file: " + tmp_path;
    return false;
  }

  std::filesystem::rename(tmp_path, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
      error = "failed to replace settings file: " + path + " (" + ec.message() + ")";
      return false;
    }
  }

  return true;
}

}  // namespace gb::core
