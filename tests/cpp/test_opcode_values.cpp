#include "../../tools/cpp/opcode_values.hpp"
#include <cassert>
using namespace leanat;
using namespace leanat::opcode_test;
int main() {
  exec::Project p;
  p.types = {{exec::TypeKind::Unit},
             {exec::TypeKind::Bits, 64},
             {exec::TypeKind::Variant, 0, {}, {{}, {1}, {2}}}};
  for (
      const auto &wire :
      {R"({"kind":"variant","tag":0,"fields":[]})",
       R"({"kind":"variant","tag":1,"fields":[{"kind":"bits","width":64,"value":"18446744073709551615"}]})",
       R"({"kind":"variant","tag":2,"fields":[{"kind":"variant","tag":0,"fields":[]}]})"}) {
    auto value = decode_value(InputJsonReader(wire).parse());
    assert(exec::conforms(p, 2, value));
    assert(typed_value(p, value, 2) == wire);
    assert(std::get<Value::Array>(value.data).size() == 2);
  }
  for (const auto &bad :
       {R"({"kind":"bits","width":8,"value":"256"})", R"({"kind":"bits","width":64,"value":"01"})",
        R"({"kind":"bits","width":64,"value":7})", R"({"kind":"unit","hidden":true})",
        R"({"kind":"unit","kind":"bool"})"}) {
    bool rejected = false;
    try {
      (void)decode_value(InputJsonReader(bad).parse());
    } catch (const std::exception &) {
      rejected = true;
    }
    assert(rejected);
  }
  bool invalid_utf8 = false;
  try {
    std::string invalid = "\"";
    invalid += char(0xc0);
    invalid += char(0x80);
    invalid += '"';
    (void)InputJsonReader(invalid).parse();
  } catch (const std::exception &) {
    invalid_utf8 = true;
  }
  assert(invalid_utf8);
  std::string nested = R"({"kind":"unit"})";
  for (unsigned n = 0; n < 40; ++n)
    nested = R"({"kind":"vec","values":[)" + nested + "]}";
  (void)decode_value(InputJsonReader(nested).parse());
}
