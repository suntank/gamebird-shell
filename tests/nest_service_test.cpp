#include "nest/service.h"
#include "nest/json.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <openssl/sha.h>
namespace {
  namespace fs=std::filesystem;
  using gb::nest::detail::Json;
  int failures=0;
  void Check(bool value,const std::string& message){
    if(!value){
      std::cerr<<"FAIL: "<<message<<'\n';
      ++failures;
    }
  }
  template<class F> void Reject(F f,const std::string& message){
    try{
      f();
      Check(false,message);
    } catch(const std::exception&) {}
  }
  void Write(const fs::path& p,const std::string& s){
    fs::create_directories(p.parent_path());
    std::ofstream f(p,std::ios::binary);
    f<<s;
    if(!f)throw std::runtime_error("fixture write failed");
  }
  std::string Read(const fs::path& p){
    std::ifstream f(p,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),{});
  }
  std::string Hash(const std::string& bytes){
    unsigned char d[32];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size(),d);
    std::string out;
    const char* h="0123456789abcdef";
    for(auto c:d){
      out+=h[c>>4];
      out+=h[c&15];
    }
    return out;
  }
  void LE(std::string& s,std::uint32_t n,int bytes){
    for(int i=0;i<bytes;++i)s+=char(n>>(8*i));
  }
  std::uint32_t CRC(const std::string& s){
    std::uint32_t n=~0U;
    for(unsigned char c:s){
      n^=c;
      for(int i=0;i<8;++i)n=(n>>1)^((n&1)?0xedb88320:0);
    }
    return ~n;
  }
  std::string Zip(const std::map<std::string,std::string>& files, bool symlink=false){
    std::string out,central;
    for(const auto& [name,data]:files){
      auto offset=out.size();
      auto crc=CRC(data);
      LE(out,0x04034b50,4);
      LE(out,20,2);
      LE(out,0,2);
      LE(out,0,2);
      LE(out,0,2);
      LE(out,0,2);
      LE(out,crc,4);
      LE(out,data.size(),4);
      LE(out,data.size(),4);
      LE(out,name.size(),2);
      LE(out,0,2);
      out+=name;
      out+=data;
      LE(central,0x02014b50,4);
      LE(central,0x0314,2);
      LE(central,20,2);
      LE(central,0,2);
      LE(central,0,2);
      LE(central,0,2);
      LE(central,0,2);
      LE(central,crc,4);
      LE(central,data.size(),4);
      LE(central,data.size(),4);
      LE(central,name.size(),2);
      LE(central,0,2);
      LE(central,0,2);
      LE(central,0,2);
      LE(central,0,2);
      LE(central,(symlink?0120777u:0100644u)<<16,4);
      LE(central,offset,4);
      central+=name;
    }
    auto offset=out.size();
    out+=central;
    LE(out,0x06054b50,4);
    LE(out,0,2);
    LE(out,0,2);
    LE(out,files.size(),2);
    LE(out,files.size(),2);
    LE(out,central.size(),4);
    LE(out,offset,4);
    LE(out,0,2);
    return out;
  }
  gb::nest::Manifest Package(const gb::nest::Config& c,const std::string& version,const std::map<std::string,std::string>& files,bool link=false){
    auto bytes=Zip(files,link);
    Write(fs::path(c.cdn_base_url.substr(7))/"game.zip",bytes);
    gb::nest::Manifest m;
    m.id=m.slug="fixture";
    m.title="Fixture";
    m.version=m.download.version=version;
    m.download.path="game.zip";
    m.download.size_bytes=bytes.size();
    m.download.sha256=Hash(bytes);
    return m;
  }
  void JsonTests(){
    Check(Json::Parse("{\"name\":\"caf\\u00e9 \\ud83d\\udc26\",\"line\":\"a\\nb\"}")["name"].String()=="caf\xc3\xa9 \xf0\x9f\x90\xa6","parse JSON Unicode and surrogate pairs");
    Reject([]{
      Json::Parse("{\"a\":1,\"a\":2}");
    },
    "reject ambiguous duplicate JSON keys");
    Reject([]{
      Json::Parse("[01]");
    },
    "reject invalid JSON number");
    Reject([]{
      Json::Parse("\"\\ud800\"");
    },
    "reject unpaired Unicode surrogate");
    Check(Json::Parse(Json(Json::Object{
      {
        "path","a\"\\\nb"
      }
    } ).Dump())["path"].String()=="a\"\\\nb","roundtrip JSON escaping");
  }
  void CatalogTests(gb::nest::Service& service,const gb::nest::Config& c){
    const auto api=fs::path(c.api_base_url.substr(7));
    Write(api/"api/catalog",R"({"games":[{"id":"one","slug":"one","title":"Alpha","version":"1.0","tags":["arcade"],"rating":4.5},{"id":"two","slug":"two","title":"Beta","tags":["puzzle"],"rating":3,"mature_content":1}],"page":1,"per_page":25,"total":2,"total_pages":1})");
    auto page=service.FetchCatalog();
    Check(!page.offline&&page.entries.size()==2,"load and cache catalog response");
    Write(api/"api/tags",R"({"grouped":{"Genre":[{"id":"arcade","name":"Arcade"}]}})");
    Check(service.FetchTags().at(0).name=="Arcade","load grouped tags");
    Write(api/"api/game/one",R"({"id":"one","slug":"one","title":"Alpha","version":"1.0","description":"Full description","download":{"version":"1.0","size_bytes":20,"sha256":"abcd","path":"game.zip"},"changelog":[{"version":"1.0","notes":["Hello"]}]})");
    Check(service.FetchManifest("one").changelog.size()==1,"read full details and changelog");
    fs::remove(api/"api/catalog");
    fs::remove(api/"api/game/one");
    gb::nest::CatalogQuery q;
    q.tags={
      "arcade"
    } ;
    q.min_rating=4;
    page=service.FetchCatalog(q);
    Check(page.offline&&page.entries.size()==1&&page.entries[0].slug=="one","offline cache applies tag and rating filters");
    Check(service.FetchManifest("one").description=="Full description","details available offline");
    Check(service.SetPin("1234")&&service.ParentalLocked(),"new PIN locks content");
    page=service.FetchCatalog();
    Check(page.entries.size()==1&&!page.entries[0].mature_content,"cached mature content stays hidden when locked");
    Check(!service.Unlock("0000")&&service.Unlock("1234"),"PIN unlock requires matching digits");
    Check(service.RemovePin("1234"),"correct PIN removes controls");
    Check(!service.SetRating("one",5)&&service.LoadRatings().at("one")==5,"rating saved locally without device serial");
    Check(!service.RemoveRating("one")&&service.LoadRatings().empty(),"remove local rating offline");
    auto linked_config = c;
    linked_config.device_id = "0000000000000001";
    gb::nest::Service linked(linked_config);
    Write(api/"api/device/ratings", R"({"ratings":{"one":1}})");
    Check(!linked.SetRating("one",5), "offline rating submission remains pending");
    Check(linked.SyncRatings().at("one")==5, "pending local stars override stale server stars");
    Check(!linked.RemoveRating("one") && !linked.SyncRatings().contains("one"), "pending removal stays removed during offline sync");

  }
  void InstallTests(gb::nest::Service& service,const gb::nest::Config& c){
    auto m=Package(c,"1.0",{
      {
        "game/Fixture.sh","#!/bin/sh\nexit 0\n"
      },
      {
        "game/payload/data.txt","original"
      },
      {
        "game/obsolete.txt","old"
      }
    } );
    service.Install(m);
    auto installed=service.LoadInstalled();
    Check(installed.at("fixture").installed_version=="1.0","install registers version");
    Check(Read(c.games_dir/"payload/data.txt")=="original"&&fs::exists(c.games_dir/"Fixture.sh"),"flatten optional game wrapper into payload root");
    auto launcher=service.LaunchPath(installed.at("fixture"));
    Check(fs::exists(launcher)&&(fs::status(launcher).permissions()&fs::perms::owner_exec)!=fs::perms::none,"native wrapper is executable");
    auto changed=Package(c,"2.0",{
      {
        "Fixture.sh","#!/bin/sh\nexit 0\n"
      },
      {
        "payload/data.txt","updated"
      },
      {
        "new.txt","new"
      }
    } );
    Reject([&]{
      service.Install(changed,[](const gb::nest::Progress& p){
        return p.stage!="Installing"||p.completed==0;
      } );
    },
    "cancel during commit rolls back");
    Check(Read(c.games_dir/"payload/data.txt")=="original"&&fs::exists(c.games_dir/"obsolete.txt")&&!fs::exists(c.games_dir/"new.txt"),"canceled commit restores old files and removes new files");
    Check(service.LoadInstalled().at("fixture").installed_version=="1.0"&&(fs::status(launcher).permissions()&fs::perms::owner_exec)!=fs::perms::none,"rollback preserves version and executable launcher");
    auto wrong=changed;
    wrong.download.sha256=std::string(64,'0');
    Reject([&]{
      service.Install(wrong);
    },
    "reject incorrect checksum");
    Check(Read(c.games_dir/"payload/data.txt")=="original","integrity failure leaves installed version untouched");
    service.Install(changed);
    Check(Read(c.games_dir/"payload/data.txt")=="updated"&&!fs::exists(c.games_dir/"obsolete.txt"),"successful update replaces payload and removes obsolete tracked files");
    Write(c.games_dir/"payload/save.dat","user save");
    service.Uninstall("fixture");
    Check(!fs::exists(launcher)&&!fs::exists(c.games_dir/"payload/data.txt")&&fs::exists(c.games_dir/"payload/save.dat"),"uninstall removes owned files and preserves user saves");
    auto traversal=Package(c,"1.0",{
      {
        "../escape.txt","bad"
      },
      {
        "Fixture.sh","#!/bin/sh\n"
      }
    } );
    Reject([&]{
      service.Install(traversal);
    },
    "reject ZIP path traversal");
    Check(!fs::exists(c.games_dir.parent_path()/"escape.txt"),"archive never escapes staging root");
    auto link=Package(c,"1.0",{
      {
        "Fixture.sh","/etc/passwd"
      }
    },
    true);
    Reject([&]{
      service.Install(link);
    },
    "reject archive symbolic links");
    Write(c.games_dir/"untouched.txt","unowned");
    auto collision=Package(c,"1.0",{
      {
        "Fixture.sh","#!/bin/sh\n"
      },
      {
        "untouched.txt","replace"
      }
    } );
    Reject([&]{
      service.Install(collision);
    },
    "reject overwriting unowned files");
    Check(Read(c.games_dir/"untouched.txt")=="unowned","collision preserves unrelated files");
    fs::create_directory_symlink(c.data_dir,c.games_dir/"link");
    auto destination=Package(c,"1.0",{
      {
        "Fixture.sh","#!/bin/sh\n"
      },
      {
        "link/attack.txt","bad"
      }
    } );
    Reject([&]{
      service.Install(destination);
    },
    "reject symlink in install destination");
    Check(!fs::exists(c.data_dir/"attack.txt"),"symlink rejection preserves destination outside payload");
    const auto wrapper = c.launchers_dir/"nest-fixture.sh";
    Write(wrapper, "unrelated user script");
    const auto valid = Package(c,"1.0",{{"Fixture.sh","#!/bin/sh\nexit 0\n"}});
    Reject([&]{service.Install(valid);}, "refuse to overwrite an unowned launcher");
    Check(Read(wrapper)=="unrelated user script", "launcher collision leaves user script intact");
    fs::remove(wrapper);
    const auto outside = c.data_dir/"outside-script";
    Write(outside, "keep outside file");
    fs::create_symlink(outside,wrapper);
    Reject([&]{service.Install(valid);}, "refuse a launcher symlink before touching installed files");
    Check(fs::is_symlink(wrapper)&&Read(outside)=="keep outside file", "launcher validation leaves external file and symlink untouched");
    fs::remove(wrapper);

  }
}
int main(){
  auto root=fs::temp_directory_path()/("nest-service-test-"+std::to_string(getpid()));
  fs::remove_all(root);
  try{
    JsonTests();
    gb::nest::Config c;
    c.api_base_url="file://"+(root/"server").string();
    c.cdn_base_url="file://"+(root/"cdn").string();
    c.data_dir=root/"store";
    c.games_dir=root/"payload";
    c.launchers_dir=root/"apps";
    c.legacy_data_dir=root/"missing-old-store";
    c.legacy_games_dir=root/"missing-old-games";
    const auto settings = root/"nest.json";
    Write(settings, Json(Json::Object{{"legacy_data_dir",c.legacy_data_dir.string()}, {"legacy_games_dir",c.legacy_games_dir.string()}, {"timeout_seconds",2}, {"max_retries",1}}).Dump());
    const auto loaded = gb::nest::LoadConfig(settings,c.data_dir,c.games_dir,c.launchers_dir);
    Check(loaded.legacy_data_dir==c.legacy_data_dir && loaded.legacy_games_dir==c.legacy_games_dir && loaded.timeout_seconds==2 && loaded.max_retries==1, "config preserves account migration storage overrides");
    gb::nest::Service service(c);
    CatalogTests(service,c);
    InstallTests(service,c);
    Check(gb::nest::Service::IsNewer("v1.10","1.2")&&!gb::nest::Service::IsNewer("1.0.0","1"),"version comparison pads missing components");
  } catch(const std::exception& e){
    std::cerr<<"FAIL: unexpected exception: "<<e.what()<<'\n';
    ++failures;
  }
  std::error_code ec;
  fs::remove_all(root,ec);
  if(failures)return 1;
  std::cout<<"nest service tests passed\n";
  return 0;
}
