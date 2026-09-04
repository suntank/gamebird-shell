#include <iostream>
#include <string>

#include "scrape/providers/provider_libretro.h"

namespace {
int failures = 0;

void Expect(const bool value, const std::string& message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}
}  // namespace

int main() {
  const std::string html = R"HTML(
    <a href="Super%20Metroid%20(Europe)%20(En,Fr,De).png">EU</a>
    <a href="Super%20Metroid%20(Japan,%20USA)%20(En,Ja).png">JU</a>
    <a href="Super%20Metroid%20-%20Redux%20(USA).png">Hack</a>
    <a href="Unrelated%20Game%20(USA).png">Other</a>
  )HTML";
  const auto candidates =
      gb::scrape::providers::ParseLibretroThumbnailIndex(html);
  Expect(candidates.size() == 4, "parse every PNG link");
  const auto best = gb::scrape::providers::SelectBestLibretroArtwork(
      "Super Metroid (JU) [!]", candidates);
  Expect(best.title == "Super Metroid (Japan, USA) (En,Ja)",
         "prefer the matching Japan/USA retail cover");
  Expect(best.score >= 1000, "exact normalized title has high confidence");

  const std::string dat = R"DAT(
game (
  comment "Super Metroid (Japan, USA) (En,Ja)"
  genre "Adventure"
  rom ( crc D63ED5F8 )
)
game (
  comment "Unrelated Game (USA)"
  genre "Puzzle"
  rom ( crc 00000000 )
)
)DAT";
  Expect(gb::scrape::providers::ParseLibretroMetadataValue(
             dat, best.title, "genre") == "Adventure",
         "metadata field is read from the selected game's block");
  Expect(gb::scrape::providers::ParseLibretroMetadataValue(
             dat, best.title, "publisher").empty(),
         "missing metadata field stays empty");

  if (failures != 0) {
    std::cerr << failures << " scraper assertion(s) failed\n";
    return 1;
  }
  std::cout << "Libretro scraper tests passed\n";
  return 0;
}
