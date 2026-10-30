#include "nest/service.h"
#include "nest/archive_api.h"
#include "nest/json.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <unistd.h>
namespace gb::nest {
namespace {
using detail::Json;
constexpr std::uint64_t kMaxDownload = 1024ULL*1024*1024;
constexpr std::uint64_t kMaxUnpacked = 2ULL*1024*1024*1024;
constexpr std::size_t kMaxMetadata = 16*1024*1024;
std::atomic<unsigned> sequence{0};
[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error(message);
}

std::string Lower(std::string s) {
  for (char& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

fs::path Home() {
  const char* h=std::getenv("HOME");
  return h?fs::path(h):fs::path("/home/pi");
}

fs::path Normal(fs::path p) {
  auto s=p.string();
  if (s=="~")p=Home();
  else if (s.rfind("~/",0)==0)p=Home()/s.substr(2);
  return fs::weakly_canonical(fs::absolute(p));
}

bool Under(const fs::path& path,const fs::path& root) {
  if (root.empty())return false;
  auto p=Normal(path),r=Normal(root);
  auto a=p.begin(),b=r.begin();
  for (;b!=r.end();++a,++b)if (a==p.end()||*a!=*b)return false;
  return true;
}

std::string ReadText(const fs::path& p,std::size_t limit=kMaxMetadata) {
  std::ifstream f(p,std::ios::binary);
  if (!f)Fail("Could not read "+p.string());
  std::string out;
  std::array<char,8192>b{};
  while (f) {
    f.read(b.data(),b.size());
    auto n=f.gcount();
    if (out.size()+std::size_t(n)>limit)Fail("File exceeds size limit");
    out.append(b.data(),std::size_t(n));
  }
  if (!f.eof())Fail("Could not read "+p.string());
  return out;
}

Json ReadJson(const fs::path& p) {
  return Json::Parse(ReadText(p));
}

fs::path Unique(const fs::path& dir,const std::string& prefix) {
  for (int i=0;i<100;++i) {
    auto p=dir/(prefix+std::to_string(getpid())+"-"+std::to_string(++sequence));
    std::error_code e;
    if (fs::create_directory(p,e))return p;
    if (e)Fail("Could not create temporary folder: "+e.message());
  }
  Fail("Could not create temporary folder");
}

void AtomicText(const fs::path& p,const std::string& text) {
  fs::create_directories(p.parent_path());
  auto temp=Unique(p.parent_path(),".nest-write-");
  try {
    auto file=temp/"value";
    {
      std::ofstream f(file,std::ios::binary);
      if (!f)Fail("Could not save "+p.string());
      f.write(text.data(),std::streamsize(text.size()));
      f.flush();
      if (!f)Fail("Could not save "+p.string());
    }
    fs::rename(file,p);
    fs::remove(temp);
  } catch (...) {
    std::error_code e;
    fs::remove_all(temp,e);
    throw;
  }
}

void WriteJson(const fs::path& p,const Json& j) {
  AtomicText(p,j.Dump()+"\n");
}

std::vector<std::string> Strings(const Json& j) {
  std::vector<std::string> out;
  for (const auto& v:j.Items()) {
    auto s=v.String();
    if (!s.empty())out.push_back(s);
  }
  return out;
}

Json StringArray(const std::vector<std::string>& values) {
  Json::Array a;
  for (const auto& s:values)a.emplace_back(s);
  return Json(a);
}

std::uint64_t Count(const Json& j) {
  auto n=j.Number();
  return n>=0&&n<=double(std::numeric_limits<std::uint64_t>::max()/2)?std::uint64_t(n):0;
}

Entry ParseEntry(const Json& j) {
  Entry e;
  e.id=j["id"].String();
  e.slug=j["slug"].String(e.id);
  e.title=j["title"].String();
  if (e.id.empty()||e.slug.empty()||e.title.empty())Fail("Game metadata is missing its ID or title");
  e.description=j["description"].String();
  e.version=j["version"].String(j["current_version"].String());
  e.icon_url=j["icon_url"].String();
  e.screenshot_urls=Strings(j["screenshot_urls"]);
  e.tags=Strings(j["tags"]);
  if (!j["rating"].IsNull())e.rating=j["rating"].Number();
  e.mature_content=j["mature_content"].Bool(j["mature_content"].Number()!=0);
  e.size_bytes=Count(j["size_bytes"]);
  e.sha256=j["sha256"].String();
  e.download_path=j["download_path"].String();
  e.release_date=j["release_date"].String();
  return e;
}

Json EntryJson(const Entry& e) {
  return Json::Object{
    {
      "id",e.id
    },
    {
      "slug",e.slug
    },
    {
      "title",e.title
    },
    {
      "description",e.description
    },
    {
      "version",e.version
    },
    {
      "icon_url",e.icon_url
    },
    {
      "screenshot_urls",StringArray(e.screenshot_urls)
    },
    {
      "tags",StringArray(e.tags)
    },
    {
      "rating",e.rating?Json(*e.rating):Json()
    },
    {
      "mature_content",e.mature_content
    },
    {
      "size_bytes",e.size_bytes
    },
    {
      "sha256",e.sha256
    },
    {
      "download_path",e.download_path
    },
    {
      "release_date",e.release_date
    }
  };
}

std::vector<Entry> Entries(const Json& j) {
  std::vector<Entry> entries;
  for (const auto& v:j["games"].Items()) {
    try {
      entries.push_back(ParseEntry(v));
    } catch (const std::exception&) {}
  }
  return entries;
}

std::string HashBytes(const void* data,std::size_t size) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned len=0;
  if (!EVP_Digest(data,size,digest,&len,EVP_sha256(),nullptr))Fail("SHA-256 failed");
  std::string out;
  const char* h="0123456789abcdef";
  for (unsigned i=0;i<len;++i) {
    out+=h[digest[i]>>4];
    out+=h[digest[i]&15];
  }
  return out;
}

std::string Hash(const std::string& s) {
  return HashBytes(s.data(),s.size());
}

std::string FileHash(const fs::path& p) {
  std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
  if (!ctx||!EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr))Fail("SHA-256 failed");
  std::ifstream f(p,std::ios::binary);
  if (!f)Fail("Could not read download");
  std::array<char,65536>b{};
  while (f) {
    f.read(b.data(),b.size());
    if (!EVP_DigestUpdate(ctx.get(),b.data(),std::size_t(f.gcount())))Fail("SHA-256 failed");
  }
  if (!f.eof())Fail("Could not read download");
  unsigned char d[EVP_MAX_MD_SIZE];
  unsigned n=0;
  EVP_DigestFinal_ex(ctx.get(),d,&n);
  std::string out;
  const char* h="0123456789abcdef";
  for (unsigned i=0;i<n;++i) {
    out+=h[d[i]>>4];
    out+=h[d[i]&15];
  }
  return out;
}

bool ValidSlug(const std::string& s) {
  if (s.empty()||s.size()>160||s=="."||s=="..")return false;
  for (unsigned char c:s)if (!std::isalnum(c)&&c!='-'&&c!='_'&&c!='.')return false;
  return true;
}

std::string Escape(const std::string& s) {
  std::string out;
  const char* h="0123456789ABCDEF";
  for (unsigned char c:s) {
    if (std::isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=char(c);
    else{
      out+='%';
      out+=h[c>>4];
      out+=h[c&15];
    }
  }
  return out;
}

std::string JoinUrl(const std::string& base,const std::string& relative) {
  if (relative.rfind("https://",0)==0)return relative;
  if (relative.find("://")!=std::string::npos) {
    if (base.rfind("file://",0)==0&&relative.rfind("file://",0)==0)return relative;
    Fail("Nest requires HTTPS URLs");
  }
  auto b=base;
  while (!b.empty()&&b.back()=='/')b.pop_back();
  auto p=relative;
  while (!p.empty()&&p.front()=='/')p.erase(0,1);
  return b+"/"+p;
}

void Report(const ProgressCallback& cb,const std::string& stage,std::uint64_t n=0,std::uint64_t total=0) {
  if (cb&&!cb(Progress{
    stage,n,total
  }))Fail("Operation canceled");
}

struct Transfer {
  std::string text;
  std::ofstream file;
  std::uint64_t count=0,limit=kMaxMetadata;
  ProgressCallback progress;
  bool canceled=false;
  std::string retry_after;
};
size_t Receive(char* ptr,size_t size,size_t n,void* opaque) {
  auto& t=*static_cast<Transfer*>(opaque);
  auto bytes=size*n;
  if (bytes>t.limit-t.count)return 0;
  t.count+=bytes;
  if (t.file.is_open()) {
    t.file.write(ptr,std::streamsize(bytes));
    if (!t.file)return 0;
  } else t.text.append(ptr,bytes);
  return bytes;
}

size_t ReceiveHeader(char* ptr,size_t size,size_t count,void* opaque) {
  auto bytes=size*count;
  std::string line(ptr,bytes);
  if (Lower(line).rfind("retry-after:",0)==0) {
    auto value=line.substr(12,64);
    value.erase(std::remove_if(value.begin(),value.end(),[](unsigned char c) {
      return c=='\r'||c=='\n';
    }),value.end());
    static_cast<Transfer*>(opaque)->retry_after=value;
  }
  return bytes;
}

int CurlProgress(void* opaque,curl_off_t total,curl_off_t now,curl_off_t,curl_off_t) {
  auto& t=*static_cast<Transfer*>(opaque);
  if (t.progress) {
    try {
      if (!t.progress(Progress{
        "Downloading",std::uint64_t(std::max<curl_off_t>(0,now)),std::uint64_t(std::max<curl_off_t>(0,total))
      })) {
        t.canceled=true;
        return 1;
      }
    } catch (...) {
      t.canceled=true;
      return 1;
    }
  }
  return 0;
}

std::string Request(const Config& cfg,const std::string& url,const std::string& method="GET",const std::string& body={},
                    const fs::path& dest={},
                    const ProgressCallback& cb={},
                    std::uint64_t max_bytes=kMaxDownload) {
  static const bool ready=[]() {
    return curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK;
  }();
  if (!ready)Fail("Could not initialize network client");
  // file:// exists only for explicitly configured offline fixtures. Server-provided
  // URLs cannot opt into local filesystem reads in a normal HTTPS configuration.
  if (url.rfind("file://",0)==0) {
    if (cfg.api_base_url.rfind("file://",0)!=0&&cfg.cdn_base_url.rfind("file://",0)!=0)Fail("Local file URLs are disabled");
    if (method!="GET")Fail("Offline fixture has no write endpoint");
    auto path=url.substr(7);
    auto q=path.find('?');
    if (q!=std::string::npos)path.resize(q);
    if (dest.empty())return ReadText(path);
    Report(cb,"Downloading",0,fs::file_size(path));
    std::ifstream in(path,std::ios::binary);
    std::ofstream out(dest,std::ios::binary);
    std::array<char,65536>b{};
    std::uint64_t n=0;
    while (in) {
      in.read(b.data(),b.size());
      n+=std::uint64_t(in.gcount());
      if (n>max_bytes)Fail("Download exceeds size limit");
      out.write(b.data(),in.gcount());
      Report(cb,"Downloading",n,fs::file_size(path));
    }
    if (!in.eof()||!out)Fail("Could not copy download");
    return {};
  }
  if (url.rfind("https://",0)!=0)Fail("Nest requires HTTPS URLs");
  std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
  if (!curl)Fail("Could not initialize network request");
  Transfer t;
  t.progress=cb;
  if (!dest.empty()) {
    t.file.open(dest,std::ios::binary);
    if (!t.file)Fail("Could not save download");
    t.limit=max_bytes;
  }
  curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());
  curl_easy_setopt(curl.get(),CURLOPT_FOLLOWLOCATION,1L);
  curl_easy_setopt(curl.get(),CURLOPT_MAXREDIRS,3L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS_STR,"https");
  curl_easy_setopt(curl.get(),CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
  curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS,CURLPROTO_HTTPS);
  curl_easy_setopt(curl.get(),CURLOPT_REDIR_PROTOCOLS,CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT,long(cfg.timeout_seconds));
  curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT,dest.empty()?long(cfg.timeout_seconds):600L);
  curl_easy_setopt(curl.get(),CURLOPT_LOW_SPEED_LIMIT,128L);
  curl_easy_setopt(curl.get(),CURLOPT_LOW_SPEED_TIME,30L);
  curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(curl.get(),CURLOPT_USERAGENT,"GameBirdShell-Nest/1");
  curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,Receive);
  curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&t);
  curl_easy_setopt(curl.get(),CURLOPT_HEADERFUNCTION,ReceiveHeader);
  curl_easy_setopt(curl.get(),CURLOPT_HEADERDATA,&t);
  curl_easy_setopt(curl.get(),CURLOPT_XFERINFOFUNCTION,CurlProgress);
  curl_easy_setopt(curl.get(),CURLOPT_XFERINFODATA,&t);
  curl_easy_setopt(curl.get(),CURLOPT_NOPROGRESS,0L);
  std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> headers(nullptr,curl_slist_free_all);
  if (method!="GET") {
    headers.reset(curl_slist_append(nullptr,"Content-Type: application/json"));
    curl_easy_setopt(curl.get(),CURLOPT_HTTPHEADER,headers.get());
    curl_easy_setopt(curl.get(),CURLOPT_CUSTOMREQUEST,method.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDS,body.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDSIZE,long(body.size()));
  }
  CURLcode result=CURLE_OK;
  long status=0;
  const int retries=method=="GET"&&dest.empty()?std::clamp(cfg.max_retries,0,3):0;
  for (int attempt=0;;++attempt) {
    t.text.clear();
    t.count=0;
    t.retry_after.clear();
    result=curl_easy_perform(curl.get());
    curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
    if (t.canceled)Fail("Operation canceled");
    // A rate limit is surfaced immediately so the user can retry later; never
    // turn Retry-After into a long uninterruptible wait inside the shell.
    if (status==429)Fail("Store is busy. Retry later"+(t.retry_after.empty()?std::string("."):std::string(" (Retry-After:")+t.retry_after+")."));
    if ((result==CURLE_OK&&status<500)||attempt>=retries||result==CURLE_WRITE_ERROR)break;
    Report(cb,"Connecting");
    std::this_thread::sleep_for(std::chrono::milliseconds(150*(1<<attempt)));
  }
  if (result!=CURLE_OK)Fail(std::string("Store connection failed: ")+curl_easy_strerror(result));
  if (status<200||status>=300)Fail("Store returned HTTP "+std::to_string(status));
  return t.text;
}

Json Api(const Config& c,const std::string& path,const std::string& method="GET",const Json& body={}) {
  auto j=Json::Parse(Request(c,JoinUrl(c.api_base_url,path),method,method=="GET"?"":body.Dump()));
  if (!j.IsObject())Fail("Invalid store response");
  return j;
}

std::string ShellQuote(const std::string& s) {
  std::string out="'";
  for (char c:s)out+=c=='\''?"'\\''":std::string(1,c);
  return out+"'";
}

std::string RewriteLauncher(const std::string& source,const fs::path& root) {
  const std::vector<std::string> prefixes={
    "/home/pi/gamebird/games","/home/gamebird/gamebird/games","${HOME}/gamebird/games","$HOME/gamebird/games"
  };
  std::string result;
  char quote=0;
  bool escaped=false;
  bool comment=false;
  for (std::size_t i=0;i<source.size();) {
    if (comment) {
      char c=source[i++];
      result+=c;
      if (c=='\n')comment=false;
      continue;
    }
    if (!quote&&!escaped&&source[i]=='#'&&(i==0||source[i-1]=='\n'||std::isspace(static_cast<unsigned char>(source[i-1])))) {
      comment=true;
      continue;
    }
    bool matched=false;
    if (!escaped)for (const auto& p:prefixes)if (source.compare(i,p.size(),p)==0&&(i+p.size()==source.size()||source[i+p.size()]=='/'||source[i+p.size()]=='\''||source[i+p.size()]=='"'||std::isspace(static_cast<unsigned char>(source[i+p.size()])))) {
      auto replacement=root.string();
      if (!quote)replacement=ShellQuote(replacement);
      else if (quote=='"') {
        std::string x;
        for (char c:replacement) {
          if (c=='\\'||c=='"'||c=='$'||c=='`')x+='\\';
          x+=c;
        }
        replacement=x;
      } else {
        std::string x;
        for (char c:replacement)x+=c=='\''?"'\\''":std::string(1,c);
        replacement=x;
      }
      result+=replacement;
      i+=p.size();
      matched=true;
      break;
    }
    if (matched)continue;
    char c=source[i++];
    result+=c;
    if (escaped) {
      escaped=false;
      continue;
    }
    if (c=='\\'&&quote!='\'') {
      escaped=true;
      continue;
    }
    if (c=='\''||c=='"') {
      if (!quote)quote=c;
      else if (quote==c)quote=0;
    }
  }
  return result;
}

fs::path SafeRelative(const std::string& name,bool flatten=false) {
  if (name.empty()||name.size()>4096||name.find('\\')!=std::string::npos||name.find(':')!=std::string::npos||name.find('\0')!=std::string::npos)Fail("Unsafe package path");
  fs::path p(name);
  if (p.is_absolute()||p.has_root_name())Fail("Unsafe absolute package path");
  for (const auto& c:p)if (c=="..")Fail("Unsafe parent path in package");
  p=p.lexically_normal();
  if (flatten) {
    auto it=p.begin();
    if (it!=p.end()&&*it=="game") {
      ++it;
      fs::path q;
      for (;it!=p.end();++it)q/=*it;
      p=q;
    }
  }
  if (p.empty()||p==".")return {};
  for (const auto& c:p)if (c=="."||c==".."||c.string().rfind(".nest-",0)==0)Fail("Reserved package path");
  return p;
}

void NoSymlinks(const fs::path& root,const fs::path& relative) {
  auto p=root;
  for (const auto& c:relative) {
    p/=c;
    std::error_code ec;
    auto st=fs::symlink_status(p,ec);
    if (ec&&ec!=std::errc::no_such_file_or_directory)Fail("Could not inspect install path");
    if (fs::is_symlink(st))Fail("Symbolic link in install destination");
  }
}

bool AllowedInstall(const Config& c,const fs::path& p) {
  return !p.empty()&&(Under(p,c.games_dir)||Under(p,c.legacy_games_dir));
}

bool ManagedLauncher(const fs::path& path) {
  if (!fs::is_regular_file(path)) return false;
  try {
    return ReadText(path, 4096).find("# Managed by Game Bird Nest.\n") != std::string::npos;
  } catch (const std::exception&) {
    return false;
  }
}

void ValidateLauncherDestination(const Config& config, const std::string& slug) {
  const auto launcher = config.launchers_dir / ("nest-" + slug + ".sh");
  auto icon = launcher;
  icon.replace_extension(".png");
  NoSymlinks(config.launchers_dir, launcher.filename());
  NoSymlinks(config.launchers_dir, icon.filename());
  const bool managed = ManagedLauncher(launcher);
  if (fs::exists(launcher) && !managed)
    Fail("A file outside Nest already uses this launcher name: " + launcher.string());
  if (fs::exists(icon) && !managed)
    Fail("A file outside Nest already uses this artwork name: " + icon.string());
}

fs::path GameScript(const InstalledGame& game) {
  const auto expected=game.title+".sh";
  for (const auto& s:game.installed_files)if (fs::path(s).filename()==expected&&fs::path(s).parent_path().empty()&&fs::is_regular_file(game.install_path/s))return game.install_path/s;
  for (const auto& s:game.installed_files)if (fs::path(s).parent_path().empty()&&fs::path(s).extension()==".sh"&&fs::is_regular_file(game.install_path/s))return game.install_path/s;
  // The oldest registry did not track files, but still used <title>.sh.
  if (fs::path(expected).filename()==expected&&fs::is_regular_file(game.install_path/expected))return game.install_path/expected;
  return {};
}

void CopyLegacy(const Config& c) {
  if (c.legacy_data_dir.empty()||!fs::is_directory(c.legacy_data_dir)||Normal(c.legacy_data_dir)==Normal(c.data_dir))return;
  for (const auto& file:{
    "installed_games.json","ratings.json","parental_pin.txt","cache/catalog.json"
  }) {
    auto src=c.legacy_data_dir/file,dst=c.data_dir/file;
    if (fs::is_regular_file(src)&&!fs::exists(dst))AtomicText(dst,ReadText(src));
  }
}

std::string Serial() {
  try {
    auto s=ReadText("/sys/firmware/devicetree/base/serial-number",256);
    s.erase(std::remove_if(s.begin(),s.end(),[](unsigned char c) {
      return c==0||std::isspace(c);
    }),s.end());
    s=Lower(s);
    if (s.size()==16&&s.find_first_not_of("0123456789abcdef")==std::string::npos)return s;
  } catch (...) {}
  return {};
}
}  // namespace
Config DefaultConfig() {
  Config c;
  c.data_dir=Home()/"gamebird/store";
  c.games_dir=Home()/"gamebird/games";
  c.legacy_data_dir=c.data_dir;
  c.legacy_games_dir=c.games_dir;
  return c;
}

Config LoadConfig(const fs::path& path,const fs::path& data,const fs::path& games,const fs::path& launchers) {
  auto c=DefaultConfig();
  c.data_dir=data;
  c.games_dir=games;
  c.launchers_dir=launchers;
  if (!path.empty()&&fs::exists(path)) {
    auto j=ReadJson(path);
    if (!j.IsObject())Fail("Nest config must be a JSON object");
    c.api_base_url=j["api_base_url"].String(c.api_base_url);
    c.cdn_base_url=j["cdn_base_url"].String(c.cdn_base_url);
    c.data_dir=j["data_dir"].String(c.data_dir.string());
    c.games_dir=j["games_dir"].String(c.games_dir.string());
    c.launchers_dir=j["launchers_dir"].String(c.launchers_dir.string());
    c.timeout_seconds=int(std::clamp(j["timeout_seconds"].Number(j["http_timeout"].Number(c.timeout_seconds)),1.0,120.0));
    c.max_retries=int(std::clamp(j["max_retries"].Number(c.max_retries),0.0,3.0));
    c.legacy_data_dir=j["legacy_data_dir"].String(c.legacy_data_dir.string());
    c.legacy_games_dir=j["legacy_games_dir"].String(c.legacy_games_dir.string());
  }
  c.data_dir=Normal(c.data_dir);
  c.games_dir=Normal(c.games_dir);
  if (!c.launchers_dir.empty())c.launchers_dir=Normal(c.launchers_dir);
  c.timeout_seconds=std::clamp(c.timeout_seconds,1,120);
  return c;
}

Service::Service(Config config):config_(std::move(config)) {
  if (config_.data_dir.empty()||config_.games_dir.empty())Fail("Nest storage paths are not configured");
  config_.data_dir=Normal(config_.data_dir);
  config_.games_dir=Normal(config_.games_dir);
  if (config_.launchers_dir.empty())config_.launchers_dir=config_.games_dir;
  else config_.launchers_dir=Normal(config_.launchers_dir);
  if (config_.legacy_data_dir.empty())config_.legacy_data_dir=Home()/"gamebird/store";
  if (config_.legacy_games_dir.empty())config_.legacy_games_dir=Home()/"gamebird/games";
  config_.legacy_games_dir=Normal(config_.legacy_games_dir);
  if (config_.device_id.empty())config_.device_id=Serial();
  config_.timeout_seconds=std::clamp(config_.timeout_seconds,1,120);
  fs::create_directories(config_.data_dir/"cache");
  CopyLegacy(config_);
}

std::vector<Entry> Service::CachedCatalog() const {
  try {
    return Entries(ReadJson(config_.data_dir/"cache/catalog.json"));
  } catch (...) {
    return {};
  }
}

CatalogPage Service::FetchCatalog(const CatalogQuery& query) {
  CatalogQuery q=query;
  q.page=std::max(1,q.page);
  q.per_page=std::clamp(q.per_page,1,100);
  if (q.sort_by!="title"&&q.sort_by!="rating"&&q.sort_by!="release_date")q.sort_by="title";
  const bool mature=q.include_mature&&!ParentalLocked();
  std::string path="api/catalog?page="+std::to_string(q.page)+"&per_page="+std::to_string(q.per_page)+"&sort="+q.sort_by;
  for (const auto& t:q.tags)path+="&tag="+Escape(t);
  if (q.min_rating)path+="&min_rating="+std::to_string(*q.min_rating);
  if (!mature)path+="&mature=false";
  CatalogPage page;
  page.page=q.page;
  page.per_page=q.per_page;
  try {
    auto j=Api(config_,path);
    page.entries=Entries(j);
    page.page=std::max(1,int(j["page"].Number(q.page)));
    page.per_page=std::clamp(int(j["per_page"].Number(q.per_page)),1,100);
    page.total=std::max(0,int(j["total"].Number(double(page.entries.size()))));
    page.total_pages=std::max(1,int(j["total_pages"].Number(1)));
    auto cached=CachedCatalog();
    std::map<std::string,Entry> merged;
    for (const auto& e:cached)merged[e.slug]=e;
    for (const auto& e:page.entries)merged[e.slug]=e;
    Json::Array all;
    for (const auto& [id,e]:merged) {
      (void)id;
      all.push_back(EntryJson(e));
    }
    WriteJson(config_.data_dir/"cache/catalog.json",Json::Object{
      {
        "games",Json(all)
      }
    });
    WriteJson(config_.data_dir/"cache"/("page-"+Hash(path)+".json"),j);
  } catch (const std::exception&) {
    auto cached=CachedCatalog();
    if (cached.empty())throw;
    page.offline=true;
    std::vector<Entry> filtered;
    for (const auto& e:cached) {
      if (!mature&&e.mature_content)continue;
      if (q.min_rating&&(!e.rating||*e.rating<*q.min_rating))continue;
      if (!q.tags.empty()&&std::none_of(q.tags.begin(),q.tags.end(),[&](const auto& t) {
        return std::find(e.tags.begin(),e.tags.end(),t)!=e.tags.end();
      }))continue;
      filtered.push_back(e);
    }
    std::stable_sort(filtered.begin(),filtered.end(),[&](const Entry& a,const Entry& b) {
      if (q.sort_by=="rating") return a.rating.value_or(0)>b.rating.value_or(0);
      if (q.sort_by=="release_date") return a.release_date>b.release_date;
      return Lower(a.title)<Lower(b.title);
    });
    page.total=int(filtered.size());
    page.total_pages=std::max(1,(page.total+q.per_page-1)/q.per_page);
    auto start=std::min(filtered.size(),std::size_t(q.page-1)*std::size_t(q.per_page));
    auto stop=std::min(filtered.size(),start+std::size_t(q.per_page));
    page.entries.assign(filtered.begin()+start,filtered.begin()+stop);
  }
  if (!mature)std::erase_if(page.entries,[](const Entry& e) {
    return e.mature_content;
  });
  return page;
}

std::vector<Entry> Service::FetchAllCatalog() {
  CatalogQuery q;
  q.per_page=100;
  std::vector<Entry> out;
  std::set<std::string> seen;
  for (int n=1;n<=1000;++n) {
    q.page=n;
    auto p=FetchCatalog(q);
    for (auto& e:p.entries)if (seen.insert(e.slug).second)out.push_back(std::move(e));
    if (n>=p.total_pages)break;
  }
  return out;
}

std::vector<Tag> Service::FetchTags() {
  Json j;
  try {
    j=Api(config_,"api/tags");
    WriteJson(config_.data_dir/"cache/tags.json",j);
  } catch (...) {
    try {
      j=ReadJson(config_.data_dir/"cache/tags.json");
    } catch (...) {
      std::set<std::string> ids;
      for (const auto& e:CachedCatalog())for (const auto& t:e.tags)ids.insert(t);
      std::vector<Tag> out;
      for (const auto& t:ids)out.push_back(Tag{
        t,t,"Tags"
      });
      return out;
    }
  }
  std::vector<Tag> out;
  for (const auto& [group,values]:j["grouped"].Members())for (const auto& value:values.Items()) {
    auto id=value["id"].String();
    if (!id.empty())out.push_back(Tag{
      id,value["name"].String(id),group
    });
  }
  return out;
}

Manifest Service::FetchManifest(const std::string& slug) {
  if (!ValidSlug(slug))Fail("Invalid game identifier");
  auto cache=config_.data_dir/"cache"/("manifest-"+Hash(slug)+".json");
  Json j;
  try {
    j=Api(config_,"api/game/"+Escape(slug));
    WriteJson(cache,j);
  } catch (...) {
    if (!fs::exists(cache))throw;
    j=ReadJson(cache);
  }
  Manifest m;
  static_cast<Entry&>(m)=ParseEntry(j);
  if (ParentalLocked()&&m.mature_content)Fail("This game is hidden by parental controls");
  auto d=j["download"];
  m.download={
    d["version"].String(m.version),d["sha256"].String(),d["path"].String(),Count(d["size_bytes"])
  };
  m.rating_count=int(j["rating_count"].Number());
  m.author=j["author"].String();
  m.engine_version=j["engine_version"].String();
  m.min_firmware_version=j["min_firmware_version"].String();
  for (const auto& c:j["changelog"].Items())m.changelog.push_back({
    c["version"].String(),c["date"].String(),Strings(c["notes"])
  });
  return m;
}

fs::path Service::CacheImage(const std::string& url) {
  if (url.empty())return {};
  auto file=config_.data_dir/"cache/images"/(Hash(url)+".png");
  if (fs::is_regular_file(file))return file;
  fs::create_directories(file.parent_path());
  auto tmp=Unique(file.parent_path(),".nest-image-");
  try {
    Request(config_,JoinUrl(config_.cdn_base_url,url),"GET",{},
    tmp/"image",{},
    8*1024*1024);
    if (fs::file_size(tmp/"image")>8*1024*1024)Fail("Image exceeds size limit");
    fs::rename(tmp/"image",file);
    fs::remove(tmp);
    return file;
  } catch (...) {
    std::error_code e;
    fs::remove_all(tmp,e);
    throw;
  }
}

std::map<std::string,InstalledGame> Service::LoadInstalled() const {
  const auto file=config_.data_dir/"installed_games.json";
  if (!fs::exists(file))return {};
  auto j=ReadJson(file);
  if (!j.IsObject())Fail("Installed game registry is invalid");
  std::map<std::string,InstalledGame> out;
  for (const auto& v:j["games"].Items()) {
    InstalledGame g;
    g.id=v["id"].String();
    g.title=v["title"].String();
    g.installed_version=v["installed_version"].String();
    auto path=v["install_path"].String();
    if (!ValidSlug(g.id)||g.title.empty()||path.empty())continue;
    g.install_path=Normal(path);
    if (!AllowedInstall(config_,g.install_path))continue;
    for (const auto& s:Strings(v["installed_files"])) {
      try {
        auto rel=SafeRelative(s);
        if (!rel.empty())g.installed_files.push_back(rel.generic_string());
      } catch (...) {
        Fail("Unsafe installed file registry");
      }
    }
    auto launcher=v["launcher_path"].String();
    if (!launcher.empty()) {
      auto p=Normal(launcher);
      if (Under(p,config_.launchers_dir)&&p.parent_path()==config_.launchers_dir&&p.filename()=="nest-"+g.id+".sh")g.launcher_path=p;
    }
    out[g.id]=std::move(g);
  }
  return out;
}

void Service::SaveInstalled(const std::map<std::string,InstalledGame>& games) const {
  Json::Array all;
  for (const auto& [id,g]:games) {
    (void)id;
    all.emplace_back(Json::Object{
      {
        "id",g.id
      },
      {
        "title",g.title
      },
      {
        "installed_version",g.installed_version
      },
      {
        "install_path",g.install_path.string()
      },
      {
        "installed_files",StringArray(g.installed_files)
      },
      {
        "launcher_path",g.launcher_path.string()
      }
    });
  }
  WriteJson(config_.data_dir/"installed_games.json",Json::Object{
    {
      "games",Json(all)
    }
  });
}

fs::path Service::LaunchPath(const InstalledGame& g) const {
  if (!AllowedInstall(config_,g.install_path)||!ValidSlug(g.id))return {};
  auto p=config_.launchers_dir/("nest-"+g.id+".sh");
  if (ManagedLauncher(p))return p;
  return GameScript(g);
}

void Service::WriteLauncher(InstalledGame& g) const {
  if (!AllowedInstall(config_,g.install_path)||!ValidSlug(g.id))Fail("Unsafe installed game root");
  ValidateLauncherDestination(config_, g.id);
  auto script=GameScript(g);
  if (script.empty())Fail("The game package has no top-level launcher");
  NoSymlinks(g.install_path,script.lexically_relative(g.install_path));
  const auto source=ReadText(script);
  const auto rewritten=RewriteLauncher(source,g.install_path);
  // Existing games may live on the old read-only image. Keep their payload
  // untouched and place a compatible executable copy in writable Nest storage.
  const bool legacy = !Under(g.install_path, config_.games_dir);
  if (legacy && (rewritten!=source || (fs::status(script).permissions() & fs::perms::owner_exec)==fs::perms::none)) {
    script=config_.data_dir/"cache/launchers"/(g.id+".sh");
    bool same=fs::is_regular_file(script)&&ReadText(script)==rewritten;
    if (!same)AtomicText(script,rewritten);
    fs::permissions(script,fs::perms::owner_exec|fs::perms::group_exec|fs::perms::others_exec,fs::perm_options::add);
  } else {
    if (rewritten!=source)AtomicText(script,rewritten);
    if (!legacy)fs::permissions(script,fs::perms::owner_exec|fs::perms::group_exec|fs::perms::others_exec,fs::perm_options::add);
  }
  fs::create_directories(config_.launchers_dir);
  g.launcher_path=config_.launchers_dir/("nest-"+g.id+".sh");
  NoSymlinks(config_.launchers_dir,g.launcher_path.filename());
  std::string title=g.title;
  std::replace_if(title.begin(),title.end(),[](unsigned char c) {
    return c<32||c==127;
  },
  ' ');
  const auto text="#!/bin/sh\n# GameBird Nest title: "+title+"\n# Managed by Game Bird Nest.\ncd "+ShellQuote(g.install_path.string())+" || exit 1\nexec "+ShellQuote(script.string())+" \"$@\"\n";
  bool same=false;
  if (fs::is_regular_file(g.launcher_path))same=ReadText(g.launcher_path)==text;
  if (!same)AtomicText(g.launcher_path,text);
  fs::permissions(g.launcher_path,fs::perms::owner_exec|fs::perms::group_exec|fs::perms::others_exec,fs::perm_options::add);
  for (const auto& file:g.installed_files) {
    auto p=fs::path(file);
    if (p.filename()=="icon.png"||p.filename().string().ends_with("-icon.png")||p.filename().string().ends_with("_icon.png")) {
      NoSymlinks(g.install_path,p);
      if (fs::is_regular_file(g.install_path/p)) {
        auto dest=g.launcher_path;
        dest.replace_extension(".png");
        AtomicText(dest,ReadText(g.install_path/p,8*1024*1024));
        break;
      }
    }
  }
}

bool Service::RefreshLaunchers() {
  auto games=LoadInstalled();
  bool changed=false;
  for (auto& [id,g]:games) {
    (void)id;
    if (GameScript(g).empty())continue;
    auto before=g.launcher_path;
    auto expected=config_.launchers_dir/("nest-"+g.id+".sh");
    std::string old;
    if (fs::is_regular_file(expected))old=ReadText(expected);
    WriteLauncher(g);
    if (before!=g.launcher_path||old!=ReadText(g.launcher_path))changed=true;
  }
  if (changed)SaveInstalled(games);
  return changed;
}

std::vector<UpdateInfo> Service::FindUpdates(const std::vector<Entry>& entries) const {
  std::vector<UpdateInfo> out;
  for (const auto& [id,g]:LoadInstalled())for (const auto& e:entries)if ((e.id==id||e.slug==id)&&IsNewer(e.version,g.installed_version)) {
    out.push_back({
      e.slug,g.title,g.installed_version,e.version
    });
    break;
  }
  return out;
}

bool Service::IsNewer(const std::string& latest,const std::string& current) {
  if (latest.empty()||current.empty())return false;
  auto a=latest,b=current;
  while (a.starts_with('v'))a.erase(0,1);
  while (b.starts_with('v'))b.erase(0,1);
  auto parts=[](const std::string& s) {
    std::vector<unsigned long long> p;
    std::size_t pos=0;
    while (pos<=s.size()) {
      auto end=s.find('.',pos);
      if (end==std::string::npos)end=s.size();
      auto part=s.substr(pos,end-pos);
      if (part.empty()||part.find_first_not_of("0123456789")!=std::string::npos)throw std::invalid_argument("version");
      p.push_back(std::stoull(part));
      if (end==s.size())break;
      pos=end+1;
    }
    return p;
  };
  try {
    auto x=parts(a),y=parts(b);
    x.resize(std::max(x.size(),y.size()),0);
    y.resize(x.size(),0);
    return x>y;
  } catch (...) {
    return a!=b&&a>b;
  }
}

void Service::Install(const Manifest& m,const ProgressCallback& progress) {
  if (ParentalLocked()&&m.mature_content)Fail("This game is hidden by parental controls");
  const auto slug=m.slug.empty()?m.id:m.slug;
  if (!ValidSlug(slug)||m.title.empty())Fail("Invalid game metadata");
  if (m.download.size_bytes==0||m.download.size_bytes>kMaxDownload||m.download.sha256.size()!=64||Lower(m.download.sha256).find_first_not_of("0123456789abcdef")!=std::string::npos)Fail("Package is missing valid size or SHA-256 integrity metadata");
  auto games=LoadInstalled();
  auto previous=games.find(slug);
  if (previous==games.end())for (auto it=games.begin();it!=games.end();++it)if (it->first==m.id) {
    previous=it;
    break;
  }
  const std::string old_id=previous==games.end()?std::string():previous->first;
  InstalledGame game;
  game.id=slug;
  game.title=m.title;
  game.installed_version=m.download.version.empty()?m.version:m.download.version;
  game.install_path=previous==games.end()?config_.games_dir:previous->second.install_path;
  if (!AllowedInstall(config_,game.install_path))Fail("Unsafe installed game root");
  ValidateLauncherDestination(config_, slug);
  fs::create_directories(game.install_path);
  auto txn=Unique(game.install_path,".nest-txn-");
  std::vector<fs::path> moved_old,moved_new;
  bool registered=false;
  auto launcher=config_.launchers_dir/("nest-"+slug+".sh");
  auto icon=launcher;
  icon.replace_extension(".png");
  std::optional<std::string> old_launcher,old_icon;
  if (fs::is_regular_file(launcher))old_launcher=ReadText(launcher);
  if (fs::is_regular_file(icon))old_icon=ReadText(icon,8*1024*1024);
  try {
    auto zip=txn/"package.zip";
    Report(progress,"Downloading");
    Request(config_,JoinUrl(config_.cdn_base_url,m.download.path.empty()?"download/"+slug:m.download.path),"GET",{},
    zip,progress,m.download.size_bytes);
    Report(progress,"Verifying");
    if (fs::file_size(zip)!=m.download.size_bytes)Fail("Package size does not match store metadata");
    if (FileHash(zip)!=Lower(m.download.sha256))Fail("Package SHA-256 verification failed");
    Report(progress,"Extracting");
    auto fresh=txn/"new";
    fs::create_directory(fresh);
    std::unique_ptr<archive,decltype(&archive_read_free)> reader(archive_read_new(),archive_read_free);
    if (!reader)Fail("Could not initialize package reader");
    archive_read_support_format_zip(reader.get());
    archive_read_support_filter_all(reader.get());
    if (archive_read_open_filename(reader.get(),zip.c_str(),65536)!=ARCHIVE_OK)Fail("Could not open game ZIP");
    archive_entry* entry=nullptr;
    std::set<std::string> names;
    std::uint64_t expanded=0;
    int status=0;
    while ((status=archive_read_next_header(reader.get(),&entry))==ARCHIVE_OK) {
      const auto* name=archive_entry_pathname(entry);
      if (!name)Fail("Package entry has no filename");
      auto relative=SafeRelative(name,true);
      auto type=archive_entry_filetype(entry);
      if (archive_entry_symlink(entry)||archive_entry_hardlink(entry)||(type!=AE_IFREG&&type!=AE_IFDIR))Fail("Package contains a link or unsupported file type");
      if (relative.empty()) {
        if (type==AE_IFDIR)continue;
        Fail("Empty file path in package");
      }
      if (names.size()>=20000||!names.insert(relative.generic_string()).second)Fail("Package contains duplicate paths or too many entries");
      NoSymlinks(fresh,relative);
      if (type==AE_IFDIR) {
        fs::create_directories(fresh/relative);
        continue;
      }
      auto size=archive_entry_size(entry);
      if (size<0||std::uint64_t(size)>kMaxUnpacked-expanded)Fail("Package exceeds unpacked size limit");
      fs::create_directories((fresh/relative).parent_path());
      std::ofstream file(fresh/relative,std::ios::binary);
      if (!file)Fail("Could not extract game file");
      std::array<char,65536> b{};
      std::uint64_t count=0;
      for (;;) {
        auto n=archive_read_data(reader.get(),b.data(),b.size());
        if (n<0)Fail("Game ZIP is damaged");
        if (n==0)break;
        count+=std::uint64_t(n);
        expanded+=std::uint64_t(n);
        if (expanded>kMaxUnpacked)Fail("Package exceeds unpacked size limit");
        file.write(b.data(),n);
        if (!file)Fail("Could not extract game file");
        Report(progress,"Extracting",expanded,0);
      }
      if (count!=std::uint64_t(size))Fail("Extracted game file has an invalid size");
      file.close();
      fs::permissions(fresh/relative,(archive_entry_perm(entry)&0111)?fs::perms::owner_all|fs::perms::group_read|fs::perms::group_exec|fs::perms::others_read|fs::perms::others_exec:fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read|fs::perms::others_read,fs::perm_options::replace);
      game.installed_files.push_back(relative.generic_string());
    }
    if (status!=ARCHIVE_EOF)Fail(std::string("Game ZIP is damaged: ")+(archive_error_string(reader.get())?archive_error_string(reader.get()):"unknown error"));
    if (game.installed_files.empty())Fail("Game ZIP is empty");
    auto staged=game;
    staged.install_path=fresh;
    auto script=GameScript(staged);
    if (script.empty())Fail("Game package has no top-level .sh launcher");
    auto source=ReadText(script);
    AtomicText(script,RewriteLauncher(source,game.install_path));
    fs::permissions(script,fs::perms::owner_exec|fs::perms::group_exec|fs::perms::others_exec,fs::perm_options::add);
    std::set<std::string> old_files;
    if (previous!=games.end())old_files.insert(previous->second.installed_files.begin(),previous->second.installed_files.end());
    std::set<std::string> new_files(game.installed_files.begin(),game.installed_files.end());
    for (const auto& f:new_files) {
      NoSymlinks(game.install_path,fs::path(f));
      auto dest=game.install_path/f;
      if (fs::exists(dest)&&(!old_files.count(f)||!fs::is_regular_file(dest)))Fail("Package would overwrite an existing unowned file: "+f);
      for (const auto& [other_id,other]:games)if (other_id!=slug&&previous!=games.find(other_id)&&Normal(other.install_path)==Normal(game.install_path)&&std::find(other.installed_files.begin(),other.installed_files.end(),f)!=other.installed_files.end())Fail("Package conflicts with another installed game");
    }
    Report(progress,"Installing",0,game.installed_files.size());
    // Refuse ambiguous legacy ownership before touching either game. New
    // packages never acquire shared files, but old Python registries could.
    for (const auto& f:old_files) for (const auto& [other_id,other]:games) if (previous!=games.find(other_id)&&Normal(other.install_path)==Normal(game.install_path)&&std::find(other.installed_files.begin(),other.installed_files.end(),f)!=other.installed_files.end()) Fail("Update cannot safely replace a file shared with another game: "+f);
    // Back up all previous tracked files, including files removed by the update.
    fs::create_directory(txn/"old");
    for (const auto& f:old_files) {
      auto rel=SafeRelative(f);
      NoSymlinks(game.install_path,rel);
      auto original=game.install_path/rel;
      if (!fs::exists(original))continue;
      if (!fs::is_regular_file(original))Fail("Previous installed file is no longer a regular file");
      fs::create_directories((txn/"old"/rel).parent_path());
      fs::rename(original,txn/"old"/rel);
      moved_old.push_back(rel);
    }
    std::uint64_t done=0;
    for (const auto& f:game.installed_files) {
      auto rel=fs::path(f);
      fs::create_directories((game.install_path/rel).parent_path());
      fs::rename(fresh/rel,game.install_path/rel);
      moved_new.push_back(rel);
      Report(progress,"Installing",++done,game.installed_files.size());
    }
    WriteLauncher(game);
    if (previous!=games.end()&&previous->first!=slug)games.erase(previous);
    games[slug]=game;
    SaveInstalled(games);
    registered=true;
    std::error_code ec;
    if (!old_id.empty()&&old_id!=slug) {
      auto old_path=config_.launchers_dir/("nest-"+old_id+".sh");
      if (ManagedLauncher(old_path)) {
        fs::remove(old_path,ec);
        old_path.replace_extension(".png");
        fs::remove(old_path,ec);
      }
    }
    fs::remove_all(txn,ec);
  } catch (...) {
    if (!registered) {
      std::error_code ec;
      for (auto it=moved_new.rbegin();it!=moved_new.rend();++it)fs::remove(game.install_path/ *it,ec);
      for (auto it=moved_old.rbegin();it!=moved_old.rend();++it) {
        fs::create_directories((game.install_path/ *it).parent_path(),ec);
        fs::rename(txn/"old"/ *it,game.install_path/ *it,ec);
      }
      try {
        if (old_launcher) {
          AtomicText(launcher,*old_launcher);
          fs::permissions(launcher,fs::perms::owner_exec|fs::perms::group_exec|fs::perms::others_exec,fs::perm_options::add);
        } else fs::remove(launcher,ec);
        if (old_icon)AtomicText(icon,*old_icon);
        else fs::remove(icon,ec);
      } catch (...) {}
    fs::remove_all(txn,ec);
    }
    throw;
  }
}

void Service::Uninstall(const std::string& id) {
  auto games=LoadInstalled();
  auto it=games.find(id);
  if (it==games.end())Fail("This game is not installed");
  auto game=it->second;
  if (!AllowedInstall(config_,game.install_path))Fail("Unsafe installed game root");
  ValidateLauncherDestination(config_, id);
  fs::create_directories(game.install_path);
  auto txn=Unique(game.install_path,".nest-uninstall-");
  std::vector<fs::path> moved;
  std::set<std::string> files(game.installed_files.begin(),game.installed_files.end());
  if (files.empty()) {
    auto script=GameScript(game);
    if (!script.empty())files.insert(script.lexically_relative(game.install_path).generic_string());
    auto folder=game.install_path/id;
    if (fs::is_directory(folder)&&!fs::is_symlink(folder))for (const auto& e:fs::recursive_directory_iterator(folder))if (e.is_regular_file()&&!e.is_symlink())files.insert(e.path().lexically_relative(game.install_path).generic_string());
  }
  auto launcher=config_.launchers_dir/("nest-"+id+".sh"),icon=launcher;
  icon.replace_extension(".png");
  std::optional<std::string> old_launcher,old_icon;
  if (fs::is_regular_file(launcher))old_launcher=ReadText(launcher);
  if (fs::is_regular_file(icon))old_icon=ReadText(icon,8*1024*1024);
  try {
    for (const auto& f:files) {
      auto rel=SafeRelative(f);
      if (rel.empty())continue;
      bool shared=false;
      for (const auto& [other_id,other]:games)if (other_id!=id&&Normal(other.install_path)==Normal(game.install_path)&&std::find(other.installed_files.begin(),other.installed_files.end(),f)!=other.installed_files.end())shared=true;
      if (shared)continue;
      NoSymlinks(game.install_path,rel);
      auto src=game.install_path/rel;
      if (!fs::exists(src))continue;
      if (!fs::is_regular_file(src))Fail("Installed path is no longer a regular file");
      fs::create_directories((txn/rel).parent_path());
      fs::rename(src,txn/rel);
      moved.push_back(rel);
    }
    NoSymlinks(config_.launchers_dir,launcher.filename());
    NoSymlinks(config_.launchers_dir,icon.filename());
    fs::remove(launcher);
    fs::remove(icon);
    games.erase(it);
    SaveInstalled(games);
  } catch (...) {
    std::error_code ec;
    for (auto r=moved.rbegin();r!=moved.rend();++r)fs::rename(txn/ *r,game.install_path/ *r,ec);
    try {
      if (old_launcher) {
        AtomicText(launcher,*old_launcher);
        fs::permissions(launcher,fs::perms::owner_exec|fs::perms::group_exec|fs::perms::others_exec,fs::perm_options::add);
      }
      if (old_icon)AtomicText(icon,*old_icon);
    } catch (...) {}
    fs::remove_all(txn,ec);
    throw;
  }
  std::error_code ec;
  fs::remove_all(txn,ec);
  std::set<fs::path> dirs;
  for (const auto& f:files) {
    auto p=(game.install_path/SafeRelative(f)).parent_path();
    while (p!=game.install_path&&Under(p,game.install_path)) {
      dirs.insert(p);
      p=p.parent_path();
    }
  }
  std::vector<fs::path> sorted(dirs.begin(),dirs.end());
  std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b) {
    return a.native().size()>b.native().size();
  });
  for (const auto& d:sorted)fs::remove(d,ec);
}

std::map<std::string,int> Service::LoadRatings() const {
  auto file=config_.data_dir/"ratings.json";
  if (!fs::exists(file))return {};
  auto j=ReadJson(file);
  std::map<std::string,int> out;
  for (const auto& [slug,v]:j["ratings"].Members()) {
    const auto number = v.Number();
    if (ValidSlug(slug) && number >= 1 && number <= 5 && number == std::floor(number))
      out[slug] = int(number);
  }
  return out;
}

void Service::SaveRatings(const std::map<std::string,int>& values) const {
  Json::Object ratings;
  for (const auto& [id,n]:values)ratings[id]=n;
  WriteJson(config_.data_dir/"ratings.json",Json::Object{
    {
      "ratings",Json(ratings)
    }
  });
}

namespace {
// Zero is a pending deletion. Other values are unsynchronized local stars.
std::map<std::string, int> PendingRatings(const Config& config) {
  const auto file = config.data_dir / "ratings_pending.json";
  if (!fs::exists(file)) return {};
  std::map<std::string, int> pending;
  const auto saved = ReadJson(file);
  for (const auto& [slug, value] : saved["ratings"].Members()) {
    const auto number = value.Number(-1);
    if (ValidSlug(slug) && number >= 0 && number <= 5 && number == std::floor(number))
      pending[slug] = int(number);
  }
  return pending;
}

void SavePendingRatings(const Config& config, const std::map<std::string, int>& pending) {
  Json::Object ratings;
  for (const auto& [slug, number] : pending) ratings[slug] = number;
  WriteJson(config.data_dir / "ratings_pending.json", Json::Object{{"ratings", Json(ratings)}});
}

bool SubmitRating(const Config& config, const std::string& slug, int number) {
  if (config.device_id.empty()) return false;
  Json body = Json::Object{{"device_id", config.device_id}, {"game_slug", slug}};
  if (number) body["rating"] = number;
  try {
    return Api(config, "api/device/ratings", number ? "POST" : "DELETE", body)["ok"].Bool();
  } catch (const std::exception&) {
    return false;
  }
}
}  // namespace

std::map<std::string, int> Service::SyncRatings() {
  const auto local = LoadRatings();
  if (config_.device_id.empty()) return local;
  auto pending = PendingRatings(config_);
  for (auto it = pending.begin(); it != pending.end();) {
    if (SubmitRating(config_, it->first, it->second)) it = pending.erase(it);
    else ++it;
  }
  SavePendingRatings(config_, pending);
  try {
    const auto response = Api(config_, "api/device/ratings?device_id=" + Escape(config_.device_id));
    if (!response["ratings"].IsObject()) return local;
    std::map<std::string, int> ratings;
    for (const auto& [slug, value] : response["ratings"].Members()) {
      const auto number = value.Number();
      if (ValidSlug(slug) && number >= 1 && number <= 5 && number == std::floor(number))
        ratings[slug] = int(number);
    }
    // An unavailable server must never overwrite stars saved offline. Retain
    // old local-only ratings too, so importing an unlinked device loses none.
    for (const auto& [slug, number] : local)
      if (!ratings.count(slug)) ratings[slug] = number;
    for (const auto& [slug, number] : pending) {
      if (number) ratings[slug] = number;
      else ratings.erase(slug);
    }
    SaveRatings(ratings);
    return ratings;
  } catch (const std::exception&) {
    return local;
  }
}

bool Service::SetRating(const std::string& slug, int number) {
  if (!ValidSlug(slug) || number < 1 || number > 5)
    Fail("Ratings must be between one and five stars");
  auto ratings = LoadRatings();
  ratings[slug] = number;
  SaveRatings(ratings);
  auto pending = PendingRatings(config_);
  pending[slug] = number;
  SavePendingRatings(config_, pending);
  if (!SubmitRating(config_, slug, number)) return false;
  pending.erase(slug);
  SavePendingRatings(config_, pending);
  return true;
}

bool Service::RemoveRating(const std::string& slug) {
  if (!ValidSlug(slug)) Fail("Invalid game identifier");
  auto ratings = LoadRatings();
  ratings.erase(slug);
  SaveRatings(ratings);
  auto pending = PendingRatings(config_);
  pending[slug] = 0;
  SavePendingRatings(config_, pending);
  if (!SubmitRating(config_, slug, 0)) return false;
  pending.erase(slug);
  SavePendingRatings(config_, pending);
  return true;
}

std::string Service::RequestDeveloperCode() {
  if (config_.device_id.empty())Fail("Device ID is only available on a Game Bird device");
  auto j=Api(config_,"api/device/request-code","POST",Json::Object{
    {
      "device_id",config_.device_id
    }
  });
  auto code=j["code"].String();
  if (code.empty())Fail(j["error"].String("The store did not return a developer code"));
  return code;
}

bool Service::ParentalEnabled() const {
  return fs::exists(config_.data_dir/"parental_pin.txt");
}

bool Service::ParentalLocked() const {
  return ParentalEnabled()&&!unlocked_;
}

bool Service::SetPin(const std::string& pin) {
  if (pin.size()!=4||pin.find_first_not_of("0123456789")!=std::string::npos)return false;
  if (ParentalLocked())return false;
  AtomicText(config_.data_dir/"parental_pin.txt",Hash(pin));
  unlocked_=false;
  return true;
}

bool Service::VerifyPin(const std::string& pin) const {
  if (pin.size()!=4||pin.find_first_not_of("0123456789")!=std::string::npos||!ParentalEnabled())return false;
  try {
    auto stored=ReadText(config_.data_dir/"parental_pin.txt",256);
    auto end=stored.find_last_not_of(" \t\r\n");
    if (end==std::string::npos)return false;
    stored.resize(end+1);
    auto first=stored.find_first_not_of(" \t\r\n");
    stored.erase(0,first);
    return stored==Hash(pin);
  } catch (...) {
    return false;
  }
}

bool Service::Unlock(const std::string& pin) {
  if (!VerifyPin(pin))return false;
  unlocked_=true;
  return true;
}

void Service::Lock() {
  unlocked_=false;
}

bool Service::RemovePin(const std::string& pin) {
  if (!VerifyPin(pin))return false;
  fs::remove(config_.data_dir/"parental_pin.txt");
  unlocked_=false;
  return true;
}
}  // namespace gb::nest
