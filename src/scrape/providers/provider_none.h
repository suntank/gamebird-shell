#pragma once

#include "db/db.h"

namespace gb::scrape::providers {

inline db::MetadataUpdate BuildNoneMetadata(const db::MetadataCandidate& game) {
  db::MetadataUpdate out;
  out.game_id = game.game_id;
  out.release_year = 0;
  out.genre = "Unknown";
  out.players = 1;
  out.description = "No metadata provider configured.";
  out.source = "none";
  return out;
}

}  // namespace gb::scrape::providers
