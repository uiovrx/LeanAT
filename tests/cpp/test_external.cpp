#include "leanat/external.hpp"
#include "test_support.hpp"
using namespace leanat;
static int calls;
extern "C" std::int32_t native_flip(const std::uint8_t *input, std::uint32_t n,
                                    std::uint8_t *output, std::uint32_t cap, std::uint32_t *size) {
  ++calls;
  if (n != 1 || cap != 1) {
    return 7;
  }
  output[0] = input[0] ? 0 : 1;
  *size = 1;
  return 0;
}
extern "C" std::int32_t native_bad(const std::uint8_t *, std::uint32_t, std::uint8_t *,
                                   std::uint32_t cap, std::uint32_t *size) {
  *size = cap + 1;
  return 0;
}
extern "C" std::int32_t native_error(const std::uint8_t *, std::uint32_t, std::uint8_t *,
                                     std::uint32_t, std::uint32_t *) {
  try {
    throw 1;
  } catch (...) {
    return 9;
  }
}
static ExternCallDesc desc(unsigned id, bool native = true) {
  ExternCallDesc d;
  d.id = id;
  d.logical_id = "flip";
  d.reference_hash = "reference1";
  d.symbol_id = "flip";
  d.build_id = "build1";
  d.contract_hash = "contract1";
  d.input = {ExternalLayoutKind::Bool, 1};
  d.output = d.input;
  d.select_native = native;
  d.effects = {true, true, true, true, true, true};
  return d;
}
int main() {
  auto reference = [](ExternalCallGate &, const Value &v) -> Expected<Value> {
    return Value{!std::get<bool>(v.data)};
  };
  auto pre = [](const Value &) { return true; };
  ExternalCallGate g(true);
  LEANAT_CHECK(g.register_reference(desc(1), reference, pre));
  LEANAT_CHECK(!g.register_native(1, "flip", "wrong", 1, native_flip));
  LEANAT_CHECK(g.register_native(1, "flip", "build1", 1, native_flip));
  auto parent = desc(2, false);
  parent.dependencies = {1};
  LEANAT_CHECK(g.register_reference(
      parent, [](ExternalCallGate &gate, const Value &v) { return gate.call_pure(1, v); }, pre));
  LEANAT_CHECK(g.seal());
  LEANAT_CHECK(g.compare(1, Value{true}));
  LEANAT_CHECK(calls == 1);
  LEANAT_CHECK(g.call_reference(2, Value{true}));
  LEANAT_CHECK(calls == 1);
  LEANAT_CHECK(!g.call_pure(1, Value{Bytes{0}}));
  LEANAT_CHECK(!g.register_reference(desc(3), reference, pre));
  ExternalCallGate disabled;
  LEANAT_CHECK(disabled.register_reference(desc(1, false), reference, pre));
  LEANAT_CHECK(!disabled.register_native(1, "flip", "build1", 1, native_flip));
  LEANAT_CHECK(disabled.seal());
  LEANAT_CHECK(disabled.call_pure(1, Value{false}).value() == Value{true});
  ExternalCallGate invalid(true);
  LEANAT_CHECK(invalid.register_reference(desc(1), reference, pre));
  LEANAT_CHECK(invalid.register_native(1, "flip", "build1", 1, native_bad));
  LEANAT_CHECK(invalid.seal());
  LEANAT_CHECK(invalid.call_pure(1, Value{false}).error().code == ErrorCode::Overflow);
  ExternalCallGate error(true);
  LEANAT_CHECK(error.register_reference(desc(1), reference, pre));
  LEANAT_CHECK(error.register_native(1, "flip", "build1", 1, native_error));
  LEANAT_CHECK(error.seal());
  LEANAT_CHECK(error.call_pure(1, Value{false}).error().code == ErrorCode::ExternalFailure);
  ExternalCallGate precondition(true);
  LEANAT_CHECK(
      precondition.register_reference(desc(1), reference, [](const Value &) { return false; }));
  LEANAT_CHECK(precondition.register_native(1, "flip", "build1", 1, native_flip));
  LEANAT_CHECK(precondition.seal());
  LEANAT_CHECK(!precondition.call_pure(1, Value{true}));
  LEANAT_CHECK(calls == 1);
  ExternalCallGate missing;
  auto d = desc(1, false);
  d.dependencies = {42};
  LEANAT_CHECK(missing.register_reference(d, reference, pre));
  LEANAT_CHECK(!missing.seal());
  ExternalCallGate different(true);
  LEANAT_CHECK(different.register_reference(
      desc(1), [](ExternalCallGate &, const Value &v) -> Expected<Value> { return v; }, pre));
  LEANAT_CHECK(different.register_native(1, "flip", "build1", 1, native_flip));
  LEANAT_CHECK(different.seal());
  LEANAT_CHECK(!different.compare(1, Value{true}));
  LEANAT_CHECK(different.last_comparison());
  LEANAT_CHECK(different.last_comparison()->native_result == Value{false});
  LEANAT_CHECK(different.last_comparison()->reference_result == Value{true});
  LEANAT_CHECK(!disabled.compare(1, Value{true}));
  ExternalCallGate bytes;
  auto buffer = desc(1, false);
  buffer.input = {ExternalLayoutKind::Bytes, 4};
  buffer.output = buffer.input;
  LEANAT_CHECK(bytes.register_reference(
      buffer, [](ExternalCallGate &, const Value &v) -> Expected<Value> { return v; }, pre));
  LEANAT_CHECK(bytes.seal());
  LEANAT_CHECK(bytes.call_pure(1, Value{Bytes{}}));
  LEANAT_CHECK(bytes.call_pure(1, Value{Bytes{1, 2, 3, 4}}));
  LEANAT_CHECK(bytes.call_pure(1, Value{Bytes{1, 2, 3, 4, 5}}).error().code == ErrorCode::Capacity);
  return 0;
}
