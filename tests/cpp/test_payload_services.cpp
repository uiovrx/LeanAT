#include "leanat/payload_services.hpp"
#include "test_support.hpp"
using namespace leanat;
struct Host : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return WireReturn{};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
int main() {
  exec::Project project;
  project.schema_major = 2;
  project.types = {{exec::TypeKind::Unit},
                   {exec::TypeKind::Bits, 64},
                   {exec::TypeKind::Bytes, 8},
                   {exec::TypeKind::Handle, static_cast<unsigned>(HandleKind::Transaction)},
                   {exec::TypeKind::Bool},
                   {exec::TypeKind::Variant, 0, {}, {{}, {2}}}};
  auto write = payload_service_signature(1, "leanat.payload.data.write", project.types, 3, 2, 0);
  auto read = payload_service_signature(2, "leanat.payload.data.get", project.types, 3, 2);
  auto extension = payload_service_signature(3, "leanat.extension.meta.get", project.types, 3, 5);
  LEANAT_CHECK(write && read && extension);
  project.services = {write.value(), read.value(), extension.value()};
  exec::Program program;
  program.id = 0;
  program.input_types = {3, 2};
  program.result_types = {2};
  program.effect_mask = exec::Transaction;
  exec::Block block;
  block.id = 0;
  block.parameters = {{0, 3}, {1, 2}};
  exec::Instruction put;
  put.op = exec::Op::BufferPayloadWrite;
  put.immediate = 1;
  put.args = {{0, 3}, {1, 2}};
  put.dest = exec::Reg{2, 0};
  exec::Instruction get;
  get.op = exec::Op::PayloadGet;
  get.immediate = 2;
  get.args = {{0, 3}};
  get.dest = exec::Reg{3, 2};
  block.instructions = {put, get};
  block.terminator.values = {{3, 2}};
  program.blocks = {block};
  project.programs = {program};
  auto validated = exec::validate(project);
  LEANAT_CHECK(validated);
  Host host;
  RuntimeConfig config;
  config.descriptor_identity = "payload-services";
  Runtime runtime(config, host);
  CoreRuntimeBackend backend(runtime);
  PayloadShadow shadow(8, 64);
  LEANAT_CHECK(shadow.register_extension("meta", {8, true, true, true, false, false}));
  PayloadServiceContext scope(2);
  LEANAT_CHECK(register_payload_services(backend, shadow, scope, project));
  LEANAT_CHECK(backend.freeze());
  ExecutionContext context;
  PayloadViewKey key{Handle{HandleKind::Transaction, {}, 1, 0, 1, 0},
                     Handle{HandleKind::Hop, {}, 2, 0, 1, 0},
                     {},
                     0};
  PayloadSnapshot request;
  request.command = Command::Read;
  request.data = {1, 2};
  request.byte_enable = {255, 0};
  LEANAT_CHECK(shadow.create(key, request, context, PayloadRole::Target, true));
  PayloadAccessContext access;
  access.hop = key.hop;
  access.local_side = 0;
  access.validated = true;
  access.response_write_permit = true;
  exec::Interpreter vm(validated.value(), {}, &backend);
  EventTxn unbound({}, context);
  exec::FuelCounter no_scope_fuel{100};
  LEANAT_CHECK(!vm.execute_segment(0, context, {Value(key.txn), Value(Bytes{9, 9})}, unbound,
                                   no_scope_fuel));
  {
    auto bound = scope.bind(key, access);
    LEANAT_CHECK(bound);
    auto other = key;
    other.txn.generation = 2;
    LEANAT_CHECK(shadow.create(other, request, context, PayloadRole::Target, true));
    {
      auto nested = scope.bind(other, access);
      LEANAT_CHECK(nested);
      LEANAT_CHECK(scope.current().value().key.txn == other.txn);
      LEANAT_CHECK(!scope.bind(other, access));
    }
    LEANAT_CHECK(scope.current().value().key.txn == key.txn);
    EventTxn segment({}, context);
    exec::FuelCounter fuel{100};
    auto result =
        vm.execute_segment(0, context, {Value(key.txn), Value(Bytes{9, 9})}, segment, fuel);
    LEANAT_CHECK(result && result.value().values.size() == 1);
    LEANAT_CHECK(std::get<Bytes>(result.value().values[0].data) == Bytes({9, 2}));
    EventTxn outside({}, context);
    LEANAT_CHECK(shadow.snapshot(key, outside).value().data == Bytes({1, 2}));
    LEANAT_CHECK(segment.commit());
    EventTxn observe({}, context);
    LEANAT_CHECK(shadow.snapshot(key, observe).value().data == Bytes({9, 2}));
    auto none = backend.invoke(extension.value(), {Value(key.txn)}, context, observe);
    LEANAT_CHECK(none);
    auto variant = std::get<Value::Array>(none.value()[0].data);
    LEANAT_CHECK(std::get<std::uint64_t>(variant[0].data) == 0);
    EventTxn wrong({}, context);
    exec::FuelCounter wrong_fuel{100};
    LEANAT_CHECK(
        !vm.execute_segment(0, context, {Value(other.txn), Value(Bytes{8, 8})}, wrong, wrong_fuel));
    auto transport = context;
    transport.kind = ContextKind::Transport;
    EventTxn callback({}, transport);
    LEANAT_CHECK(backend.invoke(read.value(), {Value(key.txn)}, transport, callback));
  }
  LEANAT_CHECK(!scope.current());
  CoreRuntimeBackend tampered(runtime);
  auto broken = project;
  broken.services[0].abi_hash[0] ^= 1;
  LEANAT_CHECK(!register_payload_services(tampered, shadow, scope, broken));
}
