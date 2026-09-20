#pragma once
#include <charconv>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace leanat::opcode_test {
// A bounded JSON reader for the opcode runner's integer-only configuration schema.
// No third-party parser or shell interpretation is involved.
struct InputJson {
  struct Number {
    std::string text;
  };
  using Array = std::vector<InputJson>;
  using Object = std::map<std::string, InputJson>;
  std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> value{nullptr};
  template <class T> explicit InputJson(T v) : value(std::move(v)) {}
  InputJson() = default;
  const InputJson &at(const std::string &key) const {
    const auto *o = std::get_if<Object>(&value);
    if (!o || !o->count(key))
      throw std::runtime_error("missing input JSON field: " + key);
    return o->at(key);
  }
  const InputJson *find(const std::string &key) const {
    const auto *o = std::get_if<Object>(&value);
    if (!o)
      return nullptr;
    auto i = o->find(key);
    return i == o->end() ? nullptr : &i->second;
  }
  const std::string &string() const {
    auto *p = std::get_if<std::string>(&value);
    if (!p)
      throw std::runtime_error("JSON string required");
    return *p;
  }
  std::string integer_text() const {
    if (auto *p = std::get_if<Number>(&value))
      return p->text;
    return string();
  }
  uint64_t natural(uint64_t limit = UINT64_MAX) const {
    auto text = integer_text();
    uint64_t n{};
    auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (text.empty() || (text.size() > 1 && text.front() == '0') || r.ec != std::errc{} ||
        r.ptr != text.data() + text.size() || n > limit)
      throw std::runtime_error("canonical bounded natural required");
    return n;
  }
  bool boolean() const {
    auto *p = std::get_if<bool>(&value);
    if (!p)
      throw std::runtime_error("JSON boolean required");
    return *p;
  }
  const Array &array() const {
    auto *p = std::get_if<Array>(&value);
    if (!p)
      throw std::runtime_error("JSON array required");
    return *p;
  }
  const Object &object() const {
    auto *p = std::get_if<Object>(&value);
    if (!p)
      throw std::runtime_error("JSON object required");
    return *p;
  }
};

class InputJsonReader {
  const std::string &text_;
  size_t cursor_{}, nodes_{};
  [[noreturn]] void bad(const char *m) const {
    throw std::runtime_error(std::string(m) + " at JSON byte " + std::to_string(cursor_));
  }
  void space() {
    while (cursor_ < text_.size() && (text_[cursor_] == ' ' || text_[cursor_] == '\n' ||
                                      text_[cursor_] == '\r' || text_[cursor_] == '\t'))
      ++cursor_;
  }
  char take() {
    if (cursor_ >= text_.size())
      bad("unexpected end");
    return text_[cursor_++];
  }
  bool accept(char c) {
    space();
    if (cursor_ < text_.size() && text_[cursor_] == c) {
      ++cursor_;
      return true;
    }
    return false;
  }
  unsigned hex4() {
    unsigned n = 0;
    for (unsigned i = 0; i < 4; ++i) {
      auto c = take();
      n *= 16;
      if (c >= '0' && c <= '9')
        n += c - '0';
      else if (c >= 'a' && c <= 'f')
        n += c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        n += c - 'A' + 10;
      else
        bad("invalid unicode escape");
    }
    return n;
  }
  static void utf8(std::string &o, unsigned c) {
    if (c < 128)
      o += char(c);
    else if (c < 2048) {
      o += char(192 | (c >> 6));
      o += char(128 | (c & 63));
    } else if (c < 65536) {
      o += char(224 | (c >> 12));
      o += char(128 | ((c >> 6) & 63));
      o += char(128 | (c & 63));
    } else {
      o += char(240 | (c >> 18));
      o += char(128 | ((c >> 12) & 63));
      o += char(128 | ((c >> 6) & 63));
      o += char(128 | (c & 63));
    }
  }
  std::string string() {
    if (take() != '"')
      bad("string required");
    std::string out;
    while (true) {
      unsigned char c = take();
      if (c == '"')
        return out;
      if (c < 32)
        bad("unescaped control");
      if (c != '\\') {
        out += char(c);
        continue;
      }
      char e = take();
      switch (e) {
      case '"':
      case '\\':
      case '/':
        out += e;
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        unsigned n = hex4();
        if (n >= 0xd800 && n <= 0xdbff) {
          if (take() != '\\' || take() != 'u')
            bad("missing low surrogate");
          unsigned lo = hex4();
          if (lo < 0xdc00 || lo > 0xdfff)
            bad("invalid low surrogate");
          n = 0x10000 + ((n - 0xd800) << 10) + (lo - 0xdc00);
        } else if (n >= 0xdc00 && n <= 0xdfff)
          bad("unpaired low surrogate");
        utf8(out, n);
        break;
      }
      default:
        bad("invalid string escape");
      }
    }
  }
  InputJson read(size_t depth) {
    space();
    if (depth > 160 || ++nodes_ > 4 * 1024 * 1024)
      bad("JSON structural bound");
    if (cursor_ == text_.size())
      bad("missing value");
    char c = text_[cursor_];
    if (c == '"')
      return InputJson(string());
    if (c == '{') {
      ++cursor_;
      InputJson::Object out;
      if (accept('}'))
        return InputJson(out);
      do {
        space();
        auto key = string();
        if (out.size() >= 128)
          bad("object field bound");
        if (!accept(':'))
          bad("missing colon");
        auto v = read(depth + 1);
        if (!out.emplace(std::move(key), std::move(v)).second)
          bad("duplicate object key");
        if (accept('}'))
          return InputJson(out);
      } while (accept(','));
      bad("missing object delimiter");
    }
    if (c == '[') {
      ++cursor_;
      InputJson::Array out;
      if (accept(']'))
        return InputJson(out);
      do {
        out.push_back(read(depth + 1));
        if (accept(']'))
          return InputJson(out);
      } while (accept(','));
      bad("missing array delimiter");
    }
    for (const auto &word : {std::string("null"), std::string("true"), std::string("false")})
      if (text_.compare(cursor_, word.size(), word) == 0) {
        cursor_ += word.size();
        if (word == "null")
          return InputJson();
        return InputJson(word == "true");
      }
    const auto start = cursor_;
    if (c == '-')
      bad("unsigned JSON metadata required");
    if (cursor_ == text_.size() || text_[cursor_] < '0' || text_[cursor_] > '9')
      bad("invalid integer");
    if (text_[cursor_] == '0')
      ++cursor_;
    else
      while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9')
        ++cursor_;
    if (cursor_ - start > 20)
      bad("JSON integer token bound");
    return InputJson(InputJson::Number{text_.substr(start, cursor_ - start)});
  }

public:
  explicit InputJsonReader(const std::string &text) : text_(text) {}
  InputJson parse() {
    for (size_t i = 0; i < text_.size();) {
      unsigned char c = text_[i++];
      if (c < 128)
        continue;
      unsigned count = 0, code = 0, min = 0;
      if (c >= 194 && c <= 223) {
        count = 1;
        code = c & 31;
        min = 128;
      } else if (c >= 224 && c <= 239) {
        count = 2;
        code = c & 15;
        min = 2048;
      } else if (c >= 240 && c <= 244) {
        count = 3;
        code = c & 7;
        min = 65536;
      } else
        bad("invalid UTF8");
      if (i + count > text_.size())
        bad("truncated UTF8");
      while (count--) {
        unsigned char b = text_[i++];
        if ((b & 192) != 128)
          bad("invalid UTF8 continuation");
        code = (code << 6) | (b & 63);
      }
      if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        bad("invalid UTF8 scalar");
    }
    auto v = read(0);
    space();
    if (cursor_ != text_.size())
      bad("trailing JSON data or unsupported noninteger number");
    return v;
  }
};
inline InputJson read_input_json(const std::string &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("input JSON unavailable");
  auto size = in.tellg();
  if (size < 0 || size > 4 * 1024 * 1024)
    throw std::runtime_error("input JSON byte bound");
  std::string s(size_t(size), '\0');
  in.seekg(0);
  if (!s.empty() && !in.read(s.data(), size))
    throw std::runtime_error("input JSON short read");
  return InputJsonReader(s).parse();
}
} // namespace leanat::opcode_test
