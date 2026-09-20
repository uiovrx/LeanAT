#include "../../tools/cpp/opcode_payload_fixture.hpp"
#include "test_support.hpp"
using namespace leanat;
using namespace leanat::opcode_test;
struct FixtureHost : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return WireReturn{};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
int main() {
  FixtureHost host;
  Runtime runtime({}, host);
  CoreRuntimeBackend backend(runtime);
  exec::Project project;
  project.types = {
      {exec::TypeKind::Unit},     {exec::TypeKind::Bool},
      {exec::TypeKind::Bits, 64}, {exec::TypeKind::Handle, unsigned(HandleKind::Transaction)},
      {exec::TypeKind::Bytes, 4}, {exec::TypeKind::Variant, 0, {}, {{}, {4}}}};
  auto get = payload_service_signature(0, "leanat.payload.data.get", project.types, 3, 4, 0);
  auto write = payload_service_signature(1, "leanat.payload.data.write", project.types, 3, 4, 0);
  auto ext_get = payload_service_signature(2, "leanat.extension.trace.get", project.types, 3, 5, 0);
  auto ext_write =
      payload_service_signature(3, "leanat.extension.trace.write", project.types, 3, 4, 0);
  LEANAT_CHECK(get && write && ext_get && ext_write);
  project.services = {get.value(), write.value(), ext_get.value(), ext_write.value()};
  FixtureConfig config;
  config.context.domain = DomainId{1};
  config.context.instance = InstanceId{3};
  config.context.owner = 9;
  config.context.ready = {Tick{5}, 2};
  Handle transaction{HandleKind::Transaction, DomainId{1}, 40, 0, 1, 9};
  Handle hop{HandleKind::Hop, DomainId{1}, 41, 0, 1, 9};
  auto n = [](std::uint64_t x) { return Value(x); };
  Value::Array extension{
      Value(Bytes{'t', 'r', 'a', 'c', 'e'}), n(4), Value(true), Value(true), Value(true), Value{}};
  Value::Array record{Value(transaction),
                      Value(hop),
                      n(3),
                      n(7),
                      Value(true),
                      Value(true),
                      n(0),
                      n(4096),
                      n(4),
                      Value(Bytes{1, 2, 3, 4}),
                      Value(Bytes{1, 2, 3, 4}),
                      Value(Bytes{255, 0}),
                      n(0),
                      Value(false),
                      n(65536),
                      Value(Value::Array{Value(extension)}),
                      n(0)};
  config.environment["storage.payload.record"] = Value(record);
  config.environment["storage.payload.access"] = Value(Value::Array{
      Value(hop), n(7), Value(false), Value(true), n(1), n(0), n(0), Value(true), Value(true)});
  config.inputs = {Value(transaction)};
  auto made = make_payload_fixture(runtime, backend, project, config);
  LEANAT_CHECK(made);
  auto fixture = std::move(made.value());
  LEANAT_CHECK(backend.freeze());
  LEANAT_CHECK(fixture.inputs == config.inputs);
  auto before = fixture.snapshot().dump();
  LEANAT_CHECK(
      before ==
      Json::object({{"payload", observe(Value(record))}, {"events", Json::array({})}}).dump());
  {
    EventTxn discarded({}, config.context);
    LEANAT_CHECK(backend.invoke(write.value(), {Value(transaction), Value(Bytes{9, 9, 9, 9})},
                                config.context, discarded));
    LEANAT_CHECK(fixture.snapshot().dump() == before);
  }
  LEANAT_CHECK(fixture.snapshot().dump() == before);
  EventTxn committed({}, config.context);
  LEANAT_CHECK(backend.invoke(write.value(), {Value(transaction), Value(Bytes{9, 9, 9, 9})},
                              config.context, committed));
  LEANAT_CHECK(backend.invoke(ext_write.value(), {Value(transaction), Value(Bytes{8, 7})},
                              config.context, committed));
  auto current = backend.invoke(get.value(), {Value(transaction)}, config.context, committed);
  LEANAT_CHECK(current && current.value() == std::vector<Value>{Value(Bytes{9, 2, 9, 4})});
  auto current_ext =
      backend.invoke(ext_get.value(), {Value(transaction)}, config.context, committed);
  LEANAT_CHECK(current_ext);
  auto variant = std::get<Value::Array>(current_ext.value()[0].data);
  LEANAT_CHECK(variant == Value::Array({n(1), Value(Value::Array{Value(Bytes{8, 7})})}));
  LEANAT_CHECK(committed.commit());
  record[10] = Value(Bytes{9, 2, 9, 4});
  extension[5] = Value(Bytes{8, 7});
  record[15] = Value(Value::Array{Value(extension)});
  LEANAT_CHECK(
      fixture.snapshot().dump() ==
      Json::object({{"payload", observe(Value(record))}, {"events", Json::array({})}}).dump());
  auto wrong_connection = config.context;
  wrong_connection.connection = ConnectionId{1};
  EventTxn connection_txn({}, wrong_connection);
  LEANAT_CHECK(
      !backend.invoke(get.value(), {Value(transaction)}, wrong_connection, connection_txn));
  auto stale = transaction;
  ++stale.generation;
  EventTxn failed({}, config.context);
  LEANAT_CHECK(!backend.invoke(get.value(), {Value(stale)}, config.context, failed));
  LEANAT_CHECK(
      fixture.snapshot().dump() ==
      Json::object({{"payload", observe(Value(record))}, {"events", Json::array({})}}).dump());
}
