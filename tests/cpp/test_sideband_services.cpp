#include "../../tools/cpp/opcode_sideband_fixture.hpp"
#include "leanat/sideband_services.hpp"
#include "test_support.hpp"
using namespace leanat;
struct InputHost : RuntimeHost {
  unsigned outputs{}, traces{};
  Expected<WireReturn> transport(const SendIntent &) override {
    return WireReturn{};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {
    ++outputs;
  }
  void emit_trace(const TraceEvent &) override {
    ++traces;
  }
};
static Value n(std::uint64_t value) {
  return Value{value};
}
int main() {
  exec::Project p;
  p.schema_major = 2;
  p.types = {{exec::TypeKind::Unit},
             {exec::TypeKind::Bits, 64},
             {exec::TypeKind::Bool},
             {exec::TypeKind::Bits, 8}};
  auto boolean = input_service_signature(1, p.types, 1, 2),
       bits = input_service_signature(2, p.types, 1, 3);
  LEANAT_CHECK(boolean && bits);
  p.services = {boolean.value(), bits.value()};
  InputHost host;
  RuntimeConfig config;
  Runtime runtime(config, host);
  CoreRuntimeBackend backend(runtime);
  SidebandInputStore store(DomainId{7}, 5);
  LEANAT_CHECK(store.define_port(InstanceId{1}, 11, 0, exec::PortDirection::Input, p.types[2],
                                 Value{false}));
  LEANAT_CHECK(
      store.define_port(InstanceId{2}, 22, 0, exec::PortDirection::Input, p.types[2], Value{true}));
  LEANAT_CHECK(
      store.define_port(InstanceId{1}, 11, 4, exec::PortDirection::Input, p.types[3], n(255)));
  LEANAT_CHECK(store.define_port(InstanceId{1}, 11, 5, exec::PortDirection::Output, p.types[2],
                                 Value{true}));
  LEANAT_CHECK(store.define_port(InstanceId{1}, 11, 6, exec::PortDirection::Input, p.types[2]));
  LEANAT_CHECK(!store.define_port(InstanceId{1}, 11, 7, exec::PortDirection::Input, p.types[2]));
  LEANAT_CHECK(!register_sideband_services(backend, store, p));
  LEANAT_CHECK(store.freeze());
  LEANAT_CHECK(!store.define_port(InstanceId{3}, 33, 0, exec::PortDirection::Input, p.types[2]));
  LEANAT_CHECK(register_sideband_services(backend, store, p));
  LEANAT_CHECK(backend.freeze());
  ExecutionContext ctx;
  ctx.domain = DomainId{7};
  ctx.instance = InstanceId{1};
  ctx.owner = 11;
  auto invoke = [&](const exec::ServiceSignature &s, Value index, const ExecutionContext &c) {
    EventTxn tx({}, c);
    return backend.invoke(s, {index}, c, tx);
  };
  LEANAT_CHECK(invoke(boolean.value(), n(0), ctx).value()[0] == Value{false});
  LEANAT_CHECK(store.update(InstanceId{1}, 0, Value{true}));
  LEANAT_CHECK(invoke(boolean.value(), n(0), ctx).value()[0] == Value{true});
  LEANAT_CHECK(store.update(InstanceId{1}, 0, Value{false}));
  auto other = ctx;
  other.instance = InstanceId{2};
  other.owner = 22;
  LEANAT_CHECK(invoke(boolean.value(), n(0), other).value()[0] == Value{true});
  LEANAT_CHECK(invoke(bits.value(), n(4), ctx).value()[0] == n(255));
  LEANAT_CHECK(!store.update(InstanceId{1}, 4, n(256)));
  LEANAT_CHECK(invoke(bits.value(), n(4), ctx).value()[0] == n(255));
  LEANAT_CHECK(!store.update(InstanceId{1}, 5, Value{false}));
  LEANAT_CHECK(!invoke(boolean.value(), n(5), ctx));
  LEANAT_CHECK(!invoke(boolean.value(), n(6), ctx));
  LEANAT_CHECK(!invoke(boolean.value(), n(4), ctx));
  LEANAT_CHECK(!invoke(bits.value(), n(0), ctx));
  LEANAT_CHECK(!invoke(boolean.value(), n(UINT64_MAX), ctx));
  LEANAT_CHECK(!invoke(boolean.value(), Value{true}, ctx));
  LEANAT_CHECK(!invoke(boolean.value(), n(99), ctx));
  auto wrong = ctx;
  wrong.owner = 22;
  LEANAT_CHECK(!invoke(boolean.value(), n(0), wrong));
  wrong = ctx;
  wrong.instance = InstanceId{9};
  LEANAT_CHECK(!invoke(boolean.value(), n(0), wrong));
  wrong = ctx;
  wrong.domain = DomainId{8};
  LEANAT_CHECK(!invoke(boolean.value(), n(0), wrong));
  for (auto kind : {ContextKind::Transport, ContextKind::Debug, ContextKind::Dmi}) {
    wrong = ctx;
    wrong.kind = kind;
    LEANAT_CHECK(!invoke(boolean.value(), n(0), wrong));
  }
  wrong = ctx;
  wrong.kind = ContextKind::Process;
  LEANAT_CHECK(invoke(boolean.value(), n(0), wrong));
  EventTxn mismatched({}, ctx);
  wrong = ctx;
  wrong.owner++;
  LEANAT_CHECK(!backend.invoke(boolean.value(), {n(0)}, wrong, mismatched));
  auto malformed = p;
  malformed.services[1].abi_hash[0] ^= 1;
  CoreRuntimeBackend invalid(runtime);
  LEANAT_CHECK(!register_sideband_services(invalid, store, malformed));
  LEANAT_CHECK(register_sideband_services(invalid, store, p)); // failed batch registered nothing
  LEANAT_CHECK(!input_service_signature(9, p.types, 3, 2));
  LEANAT_CHECK(!input_service_signature(9, p.types, 1, 0));
  // Execute the real VM opcode, including abort after a staged state write.
  p.state_types = {3};
  p.initial_state = {n(0)};
  exec::Program program;
  program.id = 0;
  program.input_types = {1};
  program.result_types = {2};
  program.effect_mask = exec::StateWrite;
  exec::Block block;
  block.parameters = {{0, 1}};
  exec::Instruction constant;
  constant.op = exec::Op::Const;
  constant.dest = exec::Reg{1, 3};
  constant.value = n(42);
  exec::Instruction write;
  write.op = exec::Op::BufferStateWrite;
  write.args = {{1, 3}};
  exec::Instruction read;
  read.op = exec::Op::LoadInput;
  read.immediate = 1;
  read.args = {{0, 1}};
  read.dest = exec::Reg{2, 2};
  block.instructions = {constant, write, read};
  block.terminator.values = {{2, 2}};
  program.blocks = {block};
  p.programs = {program};
  auto validated = exec::validate(p);
  LEANAT_CHECK(validated);
  VersionedCell cell{n(0)};
  exec::Interpreter vm(validated.value(), {&cell}, &backend);
  EventTxn good({}, ctx);
  exec::FuelCounter fuel{100};
  auto result = vm.execute_segment(0, ctx, {n(0)}, good, fuel);
  LEANAT_CHECK(result);
  LEANAT_CHECK(cell.value == n(0));
  LEANAT_CHECK(good.commit());
  LEANAT_CHECK(cell.value == n(42));
  cell.value = n(0);
  EventTxn bad({}, ctx);
  exec::FuelCounter bad_fuel{100};
  LEANAT_CHECK(!vm.execute_segment(0, ctx, {n(5)}, bad, bad_fuel));
  LEANAT_CHECK(cell.value == n(0));
  LEANAT_CHECK(host.outputs == 0 && host.traces == 0);
  // Hierarchical metadata independently crosschecks host direction and exact type.
  auto hierarchy = p;
  exec::ComponentDesc component;
  component.id = 3;
  component.sidebands = {{0, exec::PortDirection::Input, 2, {}, {}}};
  hierarchy.components = {component};
  exec::InstanceDesc instance;
  instance.id = 1;
  instance.definition = 3;
  hierarchy.instances = {instance};
  CoreRuntimeBackend mapped(runtime);
  LEANAT_CHECK(register_sideband_services(mapped, store, hierarchy));
  hierarchy.components[0].sidebands[0].type_id = 3;
  CoreRuntimeBackend wrong_type(runtime);
  LEANAT_CHECK(!register_sideband_services(wrong_type, store, hierarchy));
  opcode_test::FixtureConfig setup;
  setup.context = ctx;
  setup.inputs = {n(0)};
  setup.environment["runtime.owners"] = Value{Value::Array{Value{Value::Array{n(1), n(11)}}}};
  setup.environment["runtime.inputPorts"] =
      Value{Value::Array{Value{Value::Array{n(1), n(0), n(2)}}}};
  setup.environment["runtime.inputs"] =
      Value{Value::Array{Value{Value::Array{n(1), n(0), Value{true}}}}};
  CoreRuntimeBackend fixture_backend(runtime);
  auto fixture = opcode_test::bind_sideband_fixture(runtime, fixture_backend, p, setup);
  LEANAT_CHECK(fixture && fixture_backend.freeze());
  EventTxn sampled({}, ctx);
  LEANAT_CHECK(fixture_backend.invoke(boolean.value(), {n(0)}, ctx, sampled).value()[0] ==
               Value{true});
  LEANAT_CHECK(fixture.value().snapshot().dump().find("true") != std::string::npos);
}
