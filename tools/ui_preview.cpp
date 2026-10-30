// Local visual review: compile with the screen, widget, surface, theme and image-cache sources.
#include <filesystem>
#include <fstream>
#include "ui/screens/home.h"
#include "ui/screens/update.h"
#include "render/assets.h"

int main(int argc, char** argv) {
  const std::filesystem::path out = argc > 1 ? argv[1] : "/tmp/gamebird-ui";
  std::filesystem::create_directories(out);
  gb::render::Surface240 s;
  const auto t = gb::render::DefaultTheme();
  auto save = [&](const char* name) {
    std::ofstream f(out / (std::string(name) + ".ppm"), std::ios::binary);
    f << "P6\n240 240\n255\n";
    for (int i = 0; i < 240 * 240; ++i) {
      auto p = s.Pixels()[i];
      const char rgb[] = {static_cast<char>(((p >> 11) & 31) * 255 / 31),
                          static_cast<char>(((p >> 5) & 63) * 255 / 63),
                          static_cast<char>((p & 31) * 255 / 31)};
      f.write(rgb, 3);
    }
  };
  using namespace gb::ui::screens;
  gb::ui::UIState state;
  state.home_selected = gb::ui::HomeIndex(gb::ui::HomeItem::Nest);
  DrawHome(s, state, t); save("01-home");
  std::vector<SystemCarouselItem> systems;
  for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{
           {"apps", "GAMEBIRD"}, {"snes", "Super Nintendo"},
           {"n64", "Nintendo 64"}, {"psx", "Sony PlayStation"}}) {
    systems.push_back({name, 42, gb::render::BundledSystemArt(id + "-icon.png").string(),
                      gb::render::BundledSystemArt(id + "-logo.png").string()});
  }
  DrawSystems(s,t,systems,1,""); save("02-systems");
  for (int i = 0; i < static_cast<int>(systems.size()); ++i) {
    DrawSystems(s,t,systems,i,"");
    const auto filename = "02-system-" + std::to_string(i);
    save(filename.c_str());
  }
  DrawGameBrowser(s,t,"Super Nintendo",{"Chrono Trigger","Super Metroid","The Legend of Zelda"},1,"",""); save("03-browser");
  DrawGameList(s,t,"FAVORITES",{"Chrono Trigger","Super Metroid","The Legend of Zelda: A Link to the Past"},2,""); save("04-favorites");
  DrawSettings(s,t,{"Volume: 75%","Brightness: 80%","Theme: Default","Show hidden: OFF","Wi-Fi: Connected","Bluetooth: ON","Save settings"},4,"Settings saved"); save("05-settings");
  DrawTools(s,t,{"Wi-Fi","Bluetooth","Input setup","Live input test","Scrape library","System update"},5,""); save("06-tools");
  DrawDetails(s,t,"The Legend of Zelda: A Link to the Past","Super Nintendo","zelda-a-link-to-the-past.sfc",1991,"Action adventure",1,"Libretro","",true,false,""); save("07-details");
  DrawScrapeProgress(s,t,24,42,20,2,2,false,"Super Metroid","Downloading artwork"); save("08-scrape");
  DrawUpdate(s,t,"INSTALLING",62,12,true,true,"Installing system packages. Please wait.",
             "c8cf121789", "c8cf121789", "a123456789", 600); save("09-update");
  DrawWifi(s,t,WifiView::Overview,"GameBird Home",80,true,"US",{},0,"",0,""); save("10-wifi");
  DrawWifi(s,t,WifiView::Networks,"GameBird Home",80,true,"US",{{"GameBird Home",80,true,true},{"A very long wireless network name",40,true,false}},1,"",0,""); save("11-networks");
  DrawWifi(s,t,WifiView::Password,"GameBird Home",80,true,"US",{},8,"password",0,""); save("12-password");
  DrawWifi(s,t,WifiView::Country,"",0,true,"US",{},8,"GB",0,""); save("13-country");
}
