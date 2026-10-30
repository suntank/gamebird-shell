#pragma once
// Small, strict JSON value/parser used for the Nest wire format and local state.
// Keeps the shell independent of an additional JSON runtime on the appliance.
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace gb::nest::detail {
class Json {
 public:
  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;
  using Value = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
  Json() : value_(nullptr) {}
  Json(bool v) : value_(v) {}
  Json(double v) : value_(v) {}
  Json(int v) : value_(double(v)) {}
  Json(std::uint64_t v) : value_(double(v)) {}
  Json(std::string v) : value_(std::move(v)) {}
  Json(const char* v) : value_(std::string(v)) {}
  Json(Array v) : value_(std::move(v)) {}
  Json(Object v) : value_(std::move(v)) {}
  bool IsNull() const {
    return std::holds_alternative<std::nullptr_t>(value_);
  }
  bool IsObject() const {
    return std::holds_alternative<Object>(value_);
  }
  const Array& Items() const {
    static const Array empty;
    auto p=std::get_if<Array>(&value_);
    return p?*p:empty;
  }
  const Object& Members() const {
    static const Object empty;
    auto p=std::get_if<Object>(&value_);
    return p?*p:empty;
  }
  const Json& operator[](const std::string& k) const {
    static const Json empty;
    const auto& o=Members();
    auto it=o.find(k);
    return it==o.end()?empty:it->second;
  }
  Json& operator[](const std::string& k) {
    if (!IsObject()) value_=Object{};
    return std::get<Object>(value_)[k];
  }
  std::string String(const std::string& fallback={}) const {
    auto p=std::get_if<std::string>(&value_);
    return p?*p:fallback;
  }
  double Number(double fallback=0) const {
    auto p=std::get_if<double>(&value_);
    return p?*p:fallback;
  }
  bool Bool(bool fallback=false) const {
    auto p=std::get_if<bool>(&value_);
    return p?*p:fallback;
  }
  std::string Dump() const {
    if (IsNull()) return "null";
    if (auto p=std::get_if<bool>(&value_)) return *p?"true":"false";
    if (auto p=std::get_if<double>(&value_)) {
      if (!std::isfinite(*p)) throw std::runtime_error("Invalid JSON number");
      std::ostringstream o;
      o<<std::setprecision(17)<<*p;
      return o.str();
    }
    if (auto p=std::get_if<std::string>(&value_)) return Quote(*p);
    if (auto p=std::get_if<Array>(&value_)) {
      std::string s="[";
      for (const auto& x:*p) {
        if (s.size()>1) s+=',';
        s+=x.Dump();
      }
      return s+"]";
    }
    std::string s="{";
    for (const auto& [k,v]:std::get<Object>(value_)) {
      if (s.size()>1) s+=',';
      s+=Quote(k)+":"+v.Dump();
    }
    return s+"}";
  }
  static Json Parse(std::string_view text) {
    Parser p{text};
    Json out=p.Read(0);
    p.Space();
    if (p.pos!=text.size()) p.Fail();
    return out;
  }
 private:
  Value value_;
  static std::string Quote(const std::string& s) {
    std::string out="\"";
    const char* h="0123456789abcdef";
    for (unsigned char c:s) {
      switch (c) {
        case '"': out+="\\\"";
        break;
        case '\\': out+="\\\\";
        break;
        case '\n':out+="\\n";
        break;
        case '\r':out+="\\r";
        break;
        case '\t':out+="\\t";
        break;
        default: if (c<32) {
          out+="\\u00";
          out+=h[c>>4];
          out+=h[c&15];
        } else out+=char(c);
      }
    }
    return out+'"';
  }
  struct Parser {
    std::string_view s;
    std::size_t pos=0;
    [[noreturn]] void Fail() const {
      throw std::runtime_error("Invalid JSON at byte "+std::to_string(pos));
    }
    void Space() {
      while (pos<s.size()&&(s[pos]==' '||s[pos]=='\n'||s[pos]=='\r'||s[pos]=='\t')) ++pos;
    }
    bool Take(char c) {
      Space();
      if (pos<s.size()&&s[pos]==c) {
        ++pos;
        return true;
      }
      return false;
    }
    unsigned Hex() {
      if (pos+4>s.size()) Fail();
      unsigned v=0;
      for (int i=0;i<4;++i) {
        char c=s[pos++];
        v<<=4;
        if (c>='0'&&c<='9')v+=c-'0';
        else if (c>='a'&&c<='f')v+=c-'a'+10;
        else if (c>='A'&&c<='F')v+=c-'A'+10;
        else Fail();
      }
      return v;
    }
    static void Utf8(std::string& o,unsigned c) {
      if (c<0x80)o+=char(c);
      else if (c<0x800) {
        o+=char(0xc0|(c>>6));
        o+=char(0x80|(c&63));
      } else if (c<0x10000) {
        o+=char(0xe0|(c>>12));
        o+=char(0x80|((c>>6)&63));
        o+=char(0x80|(c&63));
      } else{
        o+=char(0xf0|(c>>18));
        o+=char(0x80|((c>>12)&63));
        o+=char(0x80|((c>>6)&63));
        o+=char(0x80|(c&63));
      }
    }
    std::string Str() {
      Space();
      if (pos>=s.size()||s[pos++]!='"')Fail();
      std::string o;
      while (pos<s.size()) {
        unsigned char c=s[pos++];
        if (c=='"')return o;
        if (c<32)Fail();
        if (c!='\\') {
          o+=char(c);
          continue;
        }
        if (pos>=s.size())Fail();
        switch (s[pos++]) {
          case '"':o+='"';
          break;
          case '\\':o+='\\';
          break;
          case '/':o+='/';
          break;
          case 'b':o+='\b';
          break;
          case 'f':o+='\f';
          break;
          case 'n':o+='\n';
          break;
          case 'r':o+='\r';
          break;
          case 't':o+='\t';
          break;
          case 'u':{
            unsigned n=Hex();
            if (n>=0xd800&&n<=0xdbff) {
              if (pos+2>s.size()||s[pos++]!='\\'||s[pos++]!='u')Fail();
              unsigned low=Hex();
              if (low<0xdc00||low>0xdfff)Fail();
              n=0x10000+((n-0xd800)<<10)+(low-0xdc00);
            } else if (n>=0xdc00&&n<=0xdfff)Fail();
            Utf8(o,n);
            break;
          }
          default:Fail();
        }
      }
      Fail();
    }
    Json Read(unsigned depth) {
      if (depth>96)Fail();
      Space();
      if (pos>=s.size())Fail();
      char c=s[pos];
      if (c=='"')return Json(Str());
      if (Take('{')) {
        Object o;
        if (Take('}'))return Json(o);
        do{
          std::string k=Str();
          if (!Take(':'))Fail();
          if (o.count(k))Fail();
          o.emplace(k,Read(depth+1));
        }
        while (Take(','));
        if (!Take('}'))Fail();
        return Json(o);
      }
      if (Take('[')) {
        Array a;
        if (Take(']'))return Json(a);
        do{
          a.push_back(Read(depth+1));
        }
        while (Take(','));
        if (!Take(']'))Fail();
        return Json(a);
      }
      for (auto token:{
        std::string_view("true"),std::string_view("false"),std::string_view("null")
      })if (s.substr(pos,token.size())==token) {
        pos+=token.size();
        if (token=="null")return Json();
        return Json(token=="true");
      }
      const auto start=pos;
      if (s[pos]=='-')++pos;
      if (pos>=s.size())Fail();
      if (s[pos]=='0')++pos;
      else{
        if (s[pos]<'1'||s[pos]>'9')Fail();
        while (pos<s.size()&&s[pos]>='0'&&s[pos]<='9')++pos;
      }
      if (pos<s.size()&&s[pos]=='.') {
        ++pos;
        auto b=pos;
        while (pos<s.size()&&s[pos]>='0'&&s[pos]<='9')++pos;
        if (pos==b)Fail();
      }
      if (pos<s.size()&&(s[pos]=='e'||s[pos]=='E')) {
        ++pos;
        if (pos<s.size()&&(s[pos]=='+'||s[pos]=='-'))++pos;
        auto b=pos;
        while (pos<s.size()&&s[pos]>='0'&&s[pos]<='9')++pos;
        if (pos==b)Fail();
      }
      std::string n(s.substr(start,pos-start));
      char* end=nullptr;
      double d=std::strtod(n.c_str(),&end);
      if (!end||*end||!std::isfinite(d))Fail();
      return Json(d);
    }
  };
};
}
// namespace gb::nest::detail
