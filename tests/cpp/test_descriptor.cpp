#include "leanat/descriptor.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
using namespace leanat;
using namespace leanat::exec;
static void resign(Bytes &b) {
  std::fill(b.begin() + 72, b.begin() + 104, 0);
  auto h = sha256(b);
  std::copy(h.begin(), h.end(), b.begin() + 72);
}
int main(int argc, char **argv) {
  if (argc == 2) {
    std::ifstream file(argv[1], std::ios::binary);
    LEANAT_CHECK(file);
    Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    auto loaded = load_descriptor(bytes);
    if (!loaded)
      std::cerr << loaded.error().message << '\n';
    LEANAT_CHECK(loaded);
    auto canonical = serialize(loaded.value());
    LEANAT_CHECK(canonical && canonical.value() == bytes);
    return 0;
  }
  auto h = sha256(Bytes{'a', 'b', 'c'});
  std::ostringstream hex;
  for (auto x : h)
    hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(x);
  LEANAT_CHECK(hex.str() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  Project p;
  p.types = {{TypeKind::Bits, 8}};
  Program pr;
  pr.input_types = {0};
  pr.result_types = {0};
  Block b;
  b.parameters = {{0, 0}};
  b.terminator.values = {{0, 0}};
  pr.blocks = {b};
  p.programs = {pr};
  auto v = validate(p);
  LEANAT_CHECK(v);
  auto enc = serialize(v.value());
  LEANAT_CHECK(enc);
  auto dec = load_descriptor(enc.value());
  LEANAT_CHECK(dec);
  auto enc2 = serialize(dec.value());
  LEANAT_CHECK(enc2 && enc2.value() == enc.value());
  for (std::size_t n = 0; n < enc.value().size(); ++n) {
    Bytes truncated(enc.value().begin(), enc.value().begin() + n);
    LEANAT_CHECK(!load_descriptor(truncated));
  }
  auto corrupt = enc.value();
  corrupt.back() ^= 1;
  auto c = load_descriptor(corrupt);
  LEANAT_CHECK(!c && c.error().code == ErrorCode::Integrity);
  corrupt = enc.value();
  corrupt[4] = 2;
  resign(corrupt);
  LEANAT_CHECK(!load_descriptor(corrupt));
  Project v2;
  v2.schema_major = 2;
  v2.types = {{TypeKind::Bytes, 16}, {TypeKind::Handle, static_cast<unsigned>(HandleKind::Task)}};
  v2.state_types = {0, 1};
  v2.initial_state = {Value{Bytes{0, 1, 255}},
                      Value{Handle{HandleKind::Task, DomainId{3}, 4, 5, 6, 7}}};
  auto typed = validate(v2);
  LEANAT_CHECK(typed);
  auto typed_bytes = serialize(typed.value());
  LEANAT_CHECK(typed_bytes);
  auto typed_loaded = load_descriptor(typed_bytes.value());
  LEANAT_CHECK(typed_loaded && typed_loaded.value().get().initial_state == v2.initial_state);
  auto typed_again = serialize(typed_loaded.value());
  LEANAT_CHECK(typed_again && typed_again.value() == typed_bytes.value());
  corrupt = enc.value();
  corrupt[24] = 0;
  resign(corrupt);
  LEANAT_CHECK(!load_descriptor(corrupt));
  corrupt = enc.value();
  corrupt.push_back(0);
  resign(corrupt);
  LEANAT_CHECK(!load_descriptor(corrupt));
  corrupt = enc.value();
  for (unsigned n = 0; n < 4; ++n)
    corrupt[104 + n] = 255;
  resign(corrupt);
  LEANAT_CHECK(!load_descriptor(corrupt));
  LoadPolicy policy;
  policy.max_file_bytes = enc.value().size() - 1;
  LEANAT_CHECK(!load_descriptor(enc.value(), policy));
  policy = {};
  policy.expected_profile = "AT-Ext";
  LEANAT_CHECK(!load_descriptor(enc.value(), policy));
  policy = {};
  policy.limits.max_work = 1;
  LEANAT_CHECK(!load_descriptor(enc.value(), policy));
  // Re-signed invalid schema must still fail, independent of checksum authenticity.
  corrupt = enc.value();
  auto type_start = 104 + 4 + p.profile.size() + 4;
  corrupt[type_start + 1] = 0;
  resign(corrupt);
  LEANAT_CHECK(!load_descriptor(corrupt));
}
