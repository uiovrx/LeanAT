#include "leanat/descriptor.hpp"
#include "leanat/optional_services.hpp"
#include "test_support.hpp"
using namespace leanat;
using namespace leanat::exec;
namespace {
struct Host final : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return fail(ErrorCode::Unsupported, "no transport");
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
Project types() {
  Project p;
  p.schema_major = 2;
  p.profile = "AT-Ext-1.1-draft";
  p.types = {{TypeKind::Unit, 0, {}, {}},
             {TypeKind::Bits, 64, {}, {}},
             {TypeKind::Bytes, 8, {}, {}},
             {TypeKind::Handle, static_cast<unsigned>(HandleKind::Lease), {}, {}},
             {TypeKind::Handle, static_cast<unsigned>(HandleKind::Access), {}, {}},
             {TypeKind::Variant, 0, {}, {{1}, {3}}},
             {TypeKind::Variant, 0, {}, {{1}, {4}}},
             {TypeKind::Record, 0, {1, 1, 2}, {}},
             {TypeKind::Bool, 0, {}, {}},
             {TypeKind::Variant, 0, {}, {{1}, {8}}}};
  return p;
}
Instruction service(Op op, std::uint32_t id, std::vector<Reg> args, Reg dest) {
  Instruction i;
  i.op = op;
  i.immediate = id;
  i.args = std::move(args);
  i.dest = dest;
  return i;
}
Instruction project_ok(Reg input, Reg dest) {
  Instruction i;
  i.op = Op::VariantGet;
  i.immediate = 1u << 16;
  i.args = {input};
  i.dest = dest;
  return i;
}
ValidatedProject roundtrip(Project p) {
  auto v = validate(std::move(p));
  LEANAT_CHECK(v);
  auto bytes = serialize(v.value());
  LEANAT_CHECK(bytes);
  LoadPolicy policy;
  policy.expected_profile = "AT-Ext-1.1-draft";
  auto loaded = load_descriptor(bytes.value(), policy);
  LEANAT_CHECK(loaded);
  return std::move(loaded.value());
}
extern "C" std::int32_t flip(const std::uint8_t *input, std::uint32_t count, std::uint8_t *out,
                             std::uint32_t capacity, std::uint32_t *size) {
  if (count != 1 || capacity != 1) {
    return 1;
  }
  out[0] = input[0] ? 0 : 1;
  *size = 1;
  return 0;
}
extern "C" std::int32_t failing(const std::uint8_t *, std::uint32_t, std::uint8_t *, std::uint32_t,
                                std::uint32_t *) {
  return 7;
}
ExternCallDesc external_desc() {
  ExternCallDesc d;
  d.id = 1;
  d.logical_id = "flip";
  d.reference_hash = "ref-v1";
  d.symbol_id = "flip";
  d.build_id = "fixture";
  d.contract_hash = "contract-v1";
  d.input = {ExternalLayoutKind::Bool, 1};
  d.output = d.input;
  d.select_native = true;
  d.effects = {true, true, true, true, true, true};
  return d;
}
} // namespace
int main() {
  Host host;
  RuntimeConfig config;
  config.domain = DomainId{1};
  config.descriptor_identity = "optional-v2";
  config.result_capacity = 16;
  config.consumer_capacity = 32;
  Runtime rt(config, host);
  HostBindingManifest manifest;
  manifest.domain = DomainId{1};
  manifest.descriptor_identity = "optional-v2";
  manifest.clock_grid_valid = true;
  manifest.capabilities_valid = true;
  LEANAT_CHECK(rt.start(manifest));
  ManagedLimits limits;
  limits.leases = 8;
  limits.operations = 8;
  ManagedAccessManager manager(rt.results(), DomainId{1}, true, limits);
  auto memory = std::make_shared<Bytes>(8, 0);
  ManagedRegionDesc region;
  region.id = RegionId{1};
  region.end = 7;
  region.backing = memory;
  region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{2}; };
  LEANAT_CHECK(manager.add_region(region));
  auto p = types();
  auto request = managed_service_signature(p, 1, Op::RequestManaged, {1, 1, 1, 1}, 5);
  LEANAT_CHECK(request);
  auto write = managed_service_signature(p, 2, Op::BeginManagedWrite, {3, 1, 2, 1}, 6);
  LEANAT_CHECK(write);
  auto get = managed_service_signature(p, 3, Op::ResultGet, {4}, 7);
  LEANAT_CHECK(get);
  auto release = managed_service_signature(p, 4, Op::ResultRelease, {4}, 0);
  LEANAT_CHECK(release);
  p.services = {request.value(), write.value(), get.value(), release.value()};
  Program begin;
  begin.id = 0;
  begin.input_types = {1, 1, 1, 1, 1, 2, 1};
  begin.result_types = {4};
  begin.effect_mask = Managed;
  Block b;
  b.id = 0;
  for (unsigned i = 0; i < begin.input_types.size(); ++i) {
    b.parameters.push_back({i, begin.input_types[i]});
  }
  b.instructions = {service(Op::RequestManaged, 1, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, {7, 5}),
                    project_ok({7, 5}, {8, 3}),
                    service(Op::BeginManagedWrite, 2, {{8, 3}, {4, 1}, {5, 2}, {6, 1}}, {9, 6}),
                    project_ok({9, 6}, {10, 4})};
  b.terminator.values = {{10, 4}};
  begin.blocks = {b};
  Program consume;
  consume.id = 1;
  consume.input_types = {4};
  consume.result_types = {7};
  consume.effect_mask = Result;
  Block cb;
  cb.id = 0;
  cb.parameters = {{0, 4}};
  cb.instructions = {service(Op::ResultGet, 3, {{0, 4}}, {1, 7}),
                     service(Op::ResultRelease, 4, {{0, 4}}, {2, 0})};
  cb.terminator.values = {{1, 7}};
  consume.blocks = {cb};
  p.programs = {begin, consume};
  auto validated = roundtrip(p);
  CoreRuntimeBackend backend(rt);
  for (auto &signature : p.services) {
    LEANAT_CHECK(register_managed_service(backend, p, signature, manager, rt.results()));
  }
  LEANAT_CHECK(backend.freeze());
  Interpreter vm(validated, {}, &backend);
  ExecutionContext context;
  context.domain = DomainId{1};
  context.owner = 9;
  SegmentBudget budget;
  budget.bytes = 4 * 1024 * 1024;
  std::vector<Value> args{Value{std::uint64_t{1}}, Value{std::uint64_t{0}}, Value{std::uint64_t{7}},
                          Value{std::uint64_t{3}}, Value{std::uint64_t{1}}, Value{Bytes{7, 8}},
                          Value{std::uint64_t{3}}};
  EventTxn discarded(budget, context);
  FuelCounter fuel{64};
  auto tentative = vm.execute_segment(0, context, args, discarded, fuel);
  if (!tentative) {
    std::cerr << tentative.error().message << "\\n";
  }
  LEANAT_CHECK(tentative);
  LEANAT_CHECK(!manager.next_ready());
  LEANAT_CHECK(discarded.discard());
  LEANAT_CHECK(!manager.next_ready());
  LEANAT_CHECK(rt.results().occupied() == 0);
  EventTxn admitted(budget, context);
  fuel = {64};
  auto started = vm.execute_segment(0, context, args, admitted, fuel);
  LEANAT_CHECK(started);
  LEANAT_CHECK(!manager.next_ready());
  LEANAT_CHECK(admitted.commit());
  auto access = std::get<Handle>(started.value().values[0].data);
  LEANAT_CHECK(manager.next_ready()->time == Tick{3});
  LEANAT_CHECK(manager.advance(Tick{4}));
  LEANAT_CHECK((*memory)[1] == 0);
  LEANAT_CHECK(manager.advance(Tick{5}));
  LEANAT_CHECK((*memory)[1] == 7);
  context.ready = {Tick{5}, 0};
  EventTxn read_release(budget, context);
  fuel = {32};
  auto result = vm.execute_segment(1, context, {Value{access}}, read_release, fuel);
  LEANAT_CHECK(result);
  LEANAT_CHECK(read_release.commit());
  LEANAT_CHECK(
      std::get<std::uint64_t>(std::get<Value::Array>(result.value().values[0].data)[1].data) ==
      static_cast<unsigned>(CommitDisposition::WriteCommitted));
  LEANAT_CHECK(!manager.result_handle(access));
  auto foreign = access;
  foreign.owner = 8;
  EventTxn wrong_owner(budget, context);
  fuel = {32};
  LEANAT_CHECK(!vm.execute_segment(1, context, {Value{foreign}}, wrong_owner, fuel));
  // Extern ABI success unwraps to source Bool; native failure discards earlier state writes.
  Project reference_project;
  reference_project.schema_major = 2;
  reference_project.types = {{TypeKind::Bool, 0, {}, {}}};
  Program flip_program;
  flip_program.id = 2;
  flip_program.input_types = {0};
  flip_program.result_types = {0};
  Block fb;
  fb.id = 0;
  fb.parameters = {{0, 0}};
  Instruction ffalse;
  ffalse.op = Op::Const;
  ffalse.dest = Reg{1, 0};
  ffalse.value = Value{false};
  Instruction ftrue = ffalse;
  ftrue.dest = Reg{2, 0};
  ftrue.value = Value{true};
  Instruction select;
  select.op = Op::SelectValue;
  select.args = {{0, 0}, {1, 0}, {2, 0}};
  select.dest = Reg{3, 0};
  fb.instructions = {ffalse, ftrue, select};
  fb.terminator.values = {{3, 0}};
  flip_program.blocks = {fb};
  Program reference_program;
  reference_program.id = 0;
  reference_program.input_types = {0};
  reference_program.result_types = {0};
  Block rb;
  rb.id = 0;
  rb.parameters = {{0, 0}};
  Instruction call;
  call.op = Op::CallPure;
  call.immediate = 2;
  call.args = {{0, 0}};
  call.dest = Reg{1, 0};
  rb.instructions = {call};
  rb.terminator.values = {{1, 0}};
  reference_program.blocks = {rb};
  Program precondition_program = reference_program;
  precondition_program.id = 1;
  precondition_program.blocks[0].instructions = {ffalse};
  precondition_program.blocks[0].instructions[0].value = Value{true};
  reference_project.programs = {reference_program, precondition_program, flip_program};
  auto validated_reference = validate(reference_project);
  LEANAT_CHECK(validated_reference);
  auto reference_artifact =
      std::make_shared<const ValidatedProject>(std::move(validated_reference.value()));
  ExternalCallGate gate(true);
  auto descriptor = external_desc();
  descriptor.reference_hash.clear();
  LEANAT_CHECK(register_external_reference_program(gate, descriptor, reference_artifact,
                                                   ProgramId{0}, ProgramId{1}));
  LEANAT_CHECK(gate.register_native(1, "flip", "fixture", 1, flip));
  LEANAT_CHECK(gate.seal());
  LEANAT_CHECK(gate.compare(1, Value{true}));
  auto actual_descriptor = *gate.descriptor(1).value();
  LEANAT_CHECK(actual_descriptor.reference_hash.size() > 64);
  ExternalCallGate wrong_reference;
  descriptor.reference_hash = "wrong-artifact";
  LEANAT_CHECK(!register_external_reference_program(wrong_reference, descriptor, reference_artifact,
                                                    ProgramId{0}, ProgramId{1}));
  ExternalCallGate reference_only;
  descriptor.reference_hash.clear();
  descriptor.select_native = false;
  LEANAT_CHECK(register_external_reference_program(reference_only, descriptor, reference_artifact,
                                                   ProgramId{0}, ProgramId{1}));
  LEANAT_CHECK(reference_only.seal());
  LEANAT_CHECK(reference_only.call_pure(1, Value{false}).value() == Value{true});
  ExternalCallGate exhausted;
  LEANAT_CHECK(register_external_reference_program(exhausted, descriptor, reference_artifact,
                                                   ProgramId{0}, ProgramId{1}, 1));
  LEANAT_CHECK(exhausted.seal());
  LEANAT_CHECK(exhausted.call_pure(1, Value{false}).error().code == ErrorCode::FuelExhausted);
  auto ep = types();
  auto es = external_service_signature(ep, 10, 8, 9, gate, 1);
  LEANAT_CHECK(es);
  ep.services = {es.value()};
  ep.state_types = {1};
  ep.initial_state = {Value{std::uint64_t{0}}};
  Program external;
  external.id = 0;
  external.input_types = {8};
  external.result_types = {8};
  external.effect_mask = External | StateWrite;
  Block eb;
  eb.id = 0;
  eb.parameters = {{0, 8}};
  Instruction constant;
  constant.op = Op::Const;
  constant.dest = Reg{1, 1};
  constant.value = Value{std::uint64_t{99}};
  Instruction buffer;
  buffer.op = Op::BufferStateWrite;
  buffer.args = {{1, 1}};
  buffer.immediate = 0;
  eb.instructions = {constant, buffer, service(Op::CallExternPure, 10, {{0, 8}}, {2, 9}),
                     project_ok({2, 9}, {3, 8})};
  eb.terminator.values = {{3, 8}};
  external.blocks = {eb};
  ep.programs = {external};
  auto ev = roundtrip(ep);
  CoreRuntimeBackend ext_backend(rt);
  LEANAT_CHECK(register_external_service(ext_backend, ep, es.value(), gate, 1));
  LEANAT_CHECK(ext_backend.freeze());
  VersionedCell state{Value{std::uint64_t{0}}};
  Interpreter evm(ev, {&state}, &ext_backend);
  EventTxn success(budget, context);
  fuel = {32};
  auto flipped = evm.execute_segment(0, context, {Value{true}}, success, fuel);
  LEANAT_CHECK(flipped && flipped.value().values[0] == Value{false});
  LEANAT_CHECK(state.value == Value{std::uint64_t{0}});
  LEANAT_CHECK(success.commit());
  LEANAT_CHECK(state.value == Value{std::uint64_t{99}});
  ExternalCallGate bad_gate(true);
  LEANAT_CHECK(bad_gate.register_reference(
      actual_descriptor, [](ExternalCallGate &, const Value &v) -> Expected<Value> { return v; },
      [](const Value &) { return true; }));
  LEANAT_CHECK(bad_gate.register_native(1, "flip", "fixture", 1, failing));
  LEANAT_CHECK(bad_gate.seal());
  CoreRuntimeBackend bad_backend(rt);
  LEANAT_CHECK(register_external_service(bad_backend, ep, es.value(), bad_gate, 1));
  LEANAT_CHECK(bad_backend.freeze());
  VersionedCell clean{Value{std::uint64_t{0}}};
  Interpreter badvm(ev, {&clean}, &bad_backend);
  EventTxn failure(budget, context);
  fuel = {32};
  auto failed = badvm.execute_segment(0, context, {Value{true}}, failure, fuel);
  LEANAT_CHECK(!failed && failed.error().code == ErrorCode::ExternalFailure);
  LEANAT_CHECK(clean.value == Value{std::uint64_t{0}});
  LEANAT_CHECK(!failure.commit());
  auto mismatch = es.value();
  mismatch.provider_version = "different";
  CoreRuntimeBackend mismatch_backend(rt);
  LEANAT_CHECK(!register_external_service(mismatch_backend, ep, mismatch, gate, 1));
  return 0;
}
