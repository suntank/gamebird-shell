#include "scrape/providers/provider_local_dat.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace gb::scrape::providers {

namespace {

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

int GuessYear(const std::string& text) {
  const std::regex year_re("(19[0-9]{2}|20[0-9]{2})");
  std::smatch m;
  if (std::regex_search(text, m, year_re) && m.size() > 1) {
    return std::atoi(m[1].str().c_str());
  }
  return 0;
}

int GuessPlayers(const std::string& lower) {
  if (lower.find("2p") != std::string::npos ||
      lower.find("multiplayer") != std::string::npos ||
      lower.find("coop") != std::string::npos ||
      lower.find("co-op") != std::string::npos) {
    return 2;
  }
  if (lower.find("4p") != std::string::npos) {
    return 4;
  }
  return 1;
}

std::string GuessGenre(const std::string& lower) {
  if (lower.find("racing") != std::string::npos ||
      lower.find("kart") != std::string::npos ||
      lower.find("f-zero") != std::string::npos) {
    return "Racing";
  }
  if (lower.find("soccer") != std::string::npos ||
      lower.find("football") != std::string::npos ||
      lower.find("nba") != std::string::npos) {
    return "Sports";
  }
  if (lower.find("puzzle") != std::string::npos ||
      lower.find("tetris") != std::string::npos) {
    return "Puzzle";
  }
  if (lower.find("rpg") != std::string::npos ||
      lower.find("quest") != std::string::npos ||
      lower.find("chrono") != std::string::npos) {
    return "RPG";
  }
  if (lower.find("metroid") != std::string::npos ||
      lower.find("mario") != std::string::npos ||
      lower.find("zelda") != std::string::npos ||
      lower.find("sonic") != std::string::npos ||
      lower.find("mega man") != std::string::npos) {
    return "Action";
  }
  if (lower.find("shooter") != std::string::npos ||
      lower.find("contra") != std::string::npos ||
      lower.find("gradius") != std::string::npos) {
    return "Shooter";
  }
  return "Unknown";
}

}  // namespace

db::MetadataUpdate BuildHeuristicMetadata(const db::MetadataCandidate& game) {
  const std::string merged = game.title + " " + game.filename + " " + game.system_id;
  const std::string lower = ToLower(merged);

  db::MetadataUpdate out;
  out.game_id = game.game_id;
  out.release_year = GuessYear(merged);
  out.genre = GuessGenre(lower);
  out.players = GuessPlayers(lower);
  out.description = "Auto-generated metadata from local heuristics.";
  out.source = "local_heuristic";
  return out;
}

}  // namespace gb::scrape::providers
