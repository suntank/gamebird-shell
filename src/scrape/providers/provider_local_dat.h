#pragma once

#include "db/db.h"

namespace gb::scrape::providers {

db::MetadataUpdate BuildHeuristicMetadata(const db::MetadataCandidate& game);

}  // namespace gb::scrape::providers
