#pragma once
#include <fstream>
#include <functional>
#include <leanat/common.hpp>
#include <stdexcept>
#include <string>
namespace conformance {
inline std::string json(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else if (c < 32)
      out += '?';
    else
      out += char(c);
  }
  return out + '"';
}
inline void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T> T take(leanat::Expected<T> value) {
  if (!value)
    throw std::runtime_error(value.error().message);
  return std::move(value.value());
}
inline void take(leanat::Expected<void> value) {
  if (!value)
    throw std::runtime_error(value.error().message);
}
class Report {
  std::ofstream output_;

public:
  unsigned failures{};
  explicit Report(const char *path) : output_(path) {
    if (!output_)
      throw std::runtime_error("report path cannot be opened");
  }
  void run(const std::string &id, const std::string &input, const std::string &expected,
           const std::function<std::string()> &body, const std::string &coverage = "Pass") {
    std::string status = coverage, actual,
                stop = coverage == "Pass" ? "ScenarioCompleted" : "ImplementationGap";
    try {
      actual = body();
    } catch (const std::exception &e) {
      status = "Fail";
      actual = e.what();
      stop = "AssertionOrImplementationFailure";
      ++failures;
    } catch (...) {
      status = "Fail";
      actual = "nonstandard exception";
      stop = "ImplementationFailure";
      ++failures;
    }
    output_ << "{\"id\":" << json(id) << ",\"input\":" << json(input)
            << ",\"expected\":" << json(expected) << ",\"actual\":" << json(actual)
            << ",\"status\":" << json(status) << ",\"stop\":" << json(stop) << "}\n";
    output_.flush();
  }
};
} // namespace conformance
