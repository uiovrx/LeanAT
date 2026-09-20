#include "../../tools/cpp/opcode_object_fixture.hpp"
#include "leanat/descriptor.hpp"
#include "leanat/memory.hpp"
#include "leanat/object_services.hpp"
#include "leanat/pipeline.hpp"
#include "leanat/queue.hpp"
#include "leanat/register_bank.hpp"
#include "leanat/resource.hpp"
#include "test_support.hpp"
using namespace leanat;
struct ObjectHost : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return fail(ErrorCode::ExternalFailure, "unexpected host transport");
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
exec::Program program(const exec::ServiceSignature &s, std::uint32_t id) {
  exec::Program p;
  p.id = id;
  p.input_types = s.input_types;
  p.result_types = s.result_types;
  p.context = s.op == exec::Op::DebugTransfer ? ContextKind::Debug : ContextKind::Timed;
  p.effect_mask = s.effect_mask;
  exec::Block block;
  block.id = 0;
  exec::Instruction call;
  call.op = s.op;
  call.immediate = s.id;
  for (std::uint32_t i = 0; i < s.input_types.size(); ++i) {
    exec::Reg reg{i, s.input_types[i]};
    block.parameters.push_back(reg);
    call.args.push_back(reg);
  }
  if (!s.result_types.empty()) {
    call.dest = exec::Reg{static_cast<std::uint32_t>(s.input_types.size()), s.result_types[0]};
    block.terminator.values = {*call.dest};
  }
  block.instructions.push_back(call);
  p.blocks.push_back(block);
  return p;
}
int main() {
  {
    // A forged source generation numerically equal to the native live generation stays stale.
    ResultStore store(4, 4, 4, DomainId{1}, 20);
    Handle source{HandleKind::Transaction, DomainId{1}, 50, 0, 1, 7};
    auto first = store.reserve({source, TypeId{0}, 128, 7, 7});
    auto second = store.reserve({source, TypeId{0}, 128, 7, 7});
    LEANAT_CHECK(first && second);
    const auto actual = second.value().consumer.result;
    auto logical = actual;
    logical.slot = 0;
    logical.generation = actual.generation - 1;
    Value forged(actual);
    opcode_test::object_fixture_detail::remap_identity(forged, logical, actual);
    const auto mapped = std::get<Handle>(forged.data);
    LEANAT_CHECK(mapped.generation == actual.generation + 1 && mapped.slot == actual.slot + 1 &&
                 mapped != actual);
    LEANAT_CHECK(store.inspect_ownership({actual}) && !store.inspect_ownership({mapped}));
    Value wrong_owner(logical);
    std::get<Handle>(wrong_owner.data).owner = 8;
    opcode_test::object_fixture_detail::remap_identity(wrong_owner, logical, actual);
    LEANAT_CHECK(std::get<Handle>(wrong_owner.data).owner == 8);
  }
  // Adding service schemas must preserve selected-system metadata and newer versions.
  for (std::uint16_t version : {1, 2, 3, 4}) {
    exec::Project catalog;
    catalog.schema_major = version;
    if (version == 4) {
      catalog.system_metadata = exec::SystemMetadata{};
      catalog.system_metadata->id = 17;
    }
    LEANAT_CHECK(exec::validate(catalog));
    LEANAT_CHECK(ObjectServices::create(catalog, {}));
    LEANAT_CHECK(catalog.schema_major == (version < 2 ? 2 : version));
    LEANAT_CHECK(exec::validate(catalog));
    if (version == 4)
      LEANAT_CHECK(catalog.system_metadata->id == 17);
    auto before = exec::serialize(exec::validate(catalog).value()).value();
    // Duplicate object IDs fail after building a temporary registry; input stays intact.
    auto mem = std::make_shared<Memory>(std::move(Memory::make(4).value()));
    StandardObjectBinding binding{{ObjectId{1}, 35, {}, {}}, mem};
    LEANAT_CHECK(!ObjectServices::create(catalog, {binding, binding}));
    LEANAT_CHECK(exec::serialize(exec::validate(catalog).value()).value() == before);
    LEANAT_CHECK(ObjectServices::create(catalog, {binding}));
    LEANAT_CHECK(catalog.schema_major == (version < 2 ? 2 : version));
    LEANAT_CHECK(exec::validate(catalog));
    before = exec::serialize(exec::validate(catalog).value()).value();
    // A service-ID conflict is discovered after constructing the draft type catalog.
    LEANAT_CHECK(!ObjectServices::create(catalog, {binding}));
    LEANAT_CHECK(exec::serialize(exec::validate(catalog).value()).value() == before);
  }
  ObjectHost host;
  RuntimeConfig config;
  config.domain = DomainId{1};
  config.instance = InstanceId{1};
  config.connections = {ConnectionId{1}};
  config.descriptor_identity = "objects-v2";
  Runtime runtime(config, host);
  LEANAT_CHECK(runtime.start({DomainId{1}, "objects-v2", {ConnectionId{1}}, true, true}));
  {
    // Scope admission and drain transfer share a validator; each must reach its own store.
    Runtime isolated(config, host);
    LEANAT_CHECK(isolated.start({DomainId{1}, "objects-v2", {ConnectionId{1}}, true, true}));
    exec::Project project;
    project.types = {{exec::TypeKind::Bits, 64, {}, {}}};
    ObjectServiceConfig settings;
    settings.max_bytes = 16;
    settings.max_entries = 4;
    settings.queue_element_types[ObjectId{4}] = 0;
    auto declaration = std::make_shared<BoundedQueue>(1, 128, 4, DomainId{1});
    auto catalog = ObjectServices::create(
        project, {{{ObjectId{4}, 38, DomainId{1}, InstanceId{1}}, declaration}}, settings);
    LEANAT_CHECK(catalog);
    auto transfer = catalog.value()->find(ObjectId{4}, StandardIntrinsic::TransferDrain);
    LEANAT_CHECK(transfer);
    project.programs = {program(transfer.value(), 0)};
    auto validated = exec::validate(project);
    LEANAT_CHECK(validated);
    using A = Value::Array;
    auto u = [](std::uint64_t n) { return Value(n); };
    auto some = [](Value value) { return Value(A{Value(std::uint64_t(1)), Value(A{value})}); };
    Handle scope{HandleKind::Scope, DomainId{1}, 130, 0, 1, 0};
    Handle transaction{HandleKind::Transaction, DomainId{1}, 12, 0, 1, 7};
    Handle drain{HandleKind::Drain, DomainId{1}, 15, 0, 1, 7};
    opcode_test::FixtureConfig source;
    source.context.domain = DomainId{1};
    source.context.instance = InstanceId{1};
    source.context.connection = ConnectionId{1};
    source.context.owner = 7;
    source.inputs = {Value(scope), Value(drain), u(1)};
    source.environment = {
        {"objects.kind", u(3)},
        {"objects.id", u(4)},
        {"objects.maxBytes", u(16)},
        {"objects.maxEntries", u(4)},
        {"objects.elementType", u(0)},
        {"objects.valueNodeBytes", u(sizeof(Value))},
        {"objects.scopeSeed", Value(scope)},
        {"objects.drainSeed",
         Value(A{u(1), u(1), Value(Bytes{4, 9}), Value(transaction), Value(drain)})},
        {"objects.record", Value(A{u(1), u(1), u(128), u(4), u(99), u(2),
                                   Value(A{Value(A{u(1), some(u(42)), some(Value(scope)),
                                                   Value(true), Value(A{u(0), Value(A{})})})})})}};
    CoreRuntimeBackend provider(isolated, 32);
    auto fixture = opcode_test::make_object_fixture(isolated, provider, project, source);
    LEANAT_CHECK(fixture && provider.freeze());
    auto concrete = std::static_pointer_cast<opcode_test::object_fixture_detail::Owned>(
        fixture.value().lifetime[0]);
    auto queue = std::get<std::shared_ptr<BoundedQueue>>(concrete->object);
    EventTxn before({}, source.context);
    LEANAT_CHECK(queue->inspect_entries(before).value()[0].owner == concrete->scopes->root());
    exec::Interpreter vm(validated.value(), {}, &provider);
    exec::FuelCounter fuel{10000};
    auto stale_inputs = fixture.value().inputs;
    ++std::get<Handle>(stale_inputs[1].data).generation;
    EventTxn rejected({}, source.context);
    auto denied = vm.execute_segment(0, source.context, stale_inputs, rejected, fuel);
    // Provider errors use Expected<SegmentResult>::error; Kind::Failed is the DSL Fail terminator.
    if (!denied)
      std::cerr << "stale drain rejection: code=" << static_cast<unsigned>(denied.error().code)
                << " message=" << denied.error().message << '\n';
    else
      std::cerr << "stale drain unexpectedly returned segment kind="
                << static_cast<unsigned>(denied.value().kind) << '\n';
    LEANAT_CHECK(!denied);
    LEANAT_CHECK(denied.error().code == ErrorCode::StaleHandle);
    LEANAT_CHECK(denied.error().message.find("drain receipt") == 0);
    EventTxn unchanged({}, source.context);
    LEANAT_CHECK(queue->inspect_entries(unchanged).value()[0].owner == concrete->scopes->root());
    EventTxn transfer_tx({}, source.context);
    auto result = vm.execute_segment(0, source.context, fixture.value().inputs, transfer_tx, fuel);
    LEANAT_CHECK(result && result.value().kind != exec::SegmentResult::Kind::Failed);
    LEANAT_CHECK(transfer_tx.commit());
    EventTxn after({}, source.context);
    LEANAT_CHECK(queue->inspect_entries(after).value()[0].owner == concrete->drain_seed->receipt);
    LEANAT_CHECK(isolated.drains().inspect(concrete->drain_seed->receipt));
  }
  {
    Runtime isolated(config, host);
    LEANAT_CHECK(isolated.start({DomainId{1}, "objects-v2", {ConnectionId{1}}, true, true}));
    EventQueue declaration_events(4, DomainId{1});
    auto declaration =
        Pipeline::make(Duration{}, Duration{}, 1, declaration_events, 4, DomainId{1});
    LEANAT_CHECK(declaration);
    auto declared = std::make_shared<Pipeline>(std::move(declaration.value()));
    exec::Project project;
    ObjectServiceConfig settings;
    settings.max_bytes = 16;
    settings.max_entries = 4;
    auto services = ObjectServices::create(
        project, {{{ObjectId{5}, 39, DomainId{1}, InstanceId{1}}, declared}}, settings);
    LEANAT_CHECK(services);
    auto ready = services.value()->find(ObjectId{5}, StandardIntrinsic::Ready);
    LEANAT_CHECK(ready);
    project.programs = {program(ready.value(), 0)};
    auto validated = exec::validate(project);
    LEANAT_CHECK(validated);
    using A = Value::Array;
    auto u = [](std::uint64_t n) { return Value(n); };
    Handle source_handle{HandleKind::Transaction, DomainId{1}, 50, 0, 1, 7};
    Handle logical_ticket{HandleKind::ResourceTicket, DomainId{1}, 315, 0, 1, 7};
    Handle logical_event{HandleKind::Event, DomainId{1}, 1, 0, 1, 7};
    Value logical(A{Value(A{u(0), u(0), u(0), Value(logical_ticket), u(0)}), Value(source_handle),
                    Value(logical_event), u(0)});
    opcode_test::FixtureConfig source;
    source.context.domain = DomainId{1};
    source.context.instance = InstanceId{1};
    source.context.connection = ConnectionId{1};
    source.context.owner = 7;
    source.context.ready = {Tick{}, 1};
    source.inputs = {logical, Value(logical_event)};
    source.environment = {
        {"objects.kind", u(4)},
        {"objects.id", u(5)},
        {"objects.maxBytes", u(16)},
        {"objects.maxEntries", u(4)},
        {"objects.valueNodeBytes", u(sizeof(Value))},
        {"objects.record", Value(A{u(1), u(2), u(0), u(0), u(1), u(4), u(315), u(0), u(0), u(0),
                                   Value(A{u(0)}), Value(A{})})},
        {"objects.pipelineSeed", Value(A{Value(source_handle), u(0), u(0), u(0), logical})}};
    CoreRuntimeBackend provider(isolated, 32);
    auto fixture = opcode_test::make_object_fixture(isolated, provider, project, source);
    LEANAT_CHECK(fixture && provider.freeze());
    LEANAT_CHECK(isolated.queue().active_event(std::get<Handle>(fixture.value().inputs[1].data)));
    exec::Interpreter vm(validated.value(), {}, &provider);
    EventTxn tx({}, source.context);
    exec::FuelCounter fuel{10000};
    auto completed = vm.execute_segment(0, source.context, fixture.value().inputs, tx, fuel);
    LEANAT_CHECK(completed && completed.value().values == std::vector<Value>{Value(true)});
    auto before = fixture.value().snapshot().dump();
    LEANAT_CHECK(tx.commit());
    LEANAT_CHECK(fixture.value().snapshot().dump() != before);
  }
  {
    // The native opcode fixture binds the descriptor to independently constructed real storage.
    exec::Project fixture_project;
    auto catalog_memory = std::make_shared<Memory>(std::move(Memory::make(4).value()));
    ObjectServiceConfig fixture_settings;
    fixture_settings.max_bytes = 16;
    fixture_settings.max_entries = 4;
    auto catalog = ObjectServices::create(
        fixture_project, {{{ObjectId{1}, 35, DomainId{1}, InstanceId{1}}, catalog_memory}},
        fixture_settings);
    LEANAT_CHECK(catalog);
    auto write_signature = catalog.value()->find(ObjectId{1}, StandardIntrinsic::WriteBytes);
    LEANAT_CHECK(write_signature);
    fixture_project.programs.push_back(program(write_signature.value(), 0));
    auto reset_signature = catalog.value()->find(ObjectId{1}, StandardIntrinsic::Reset);
    LEANAT_CHECK(reset_signature);
    auto reset_twice = program(reset_signature.value(), 1);
    reset_twice.blocks[0].instructions.push_back(reset_twice.blocks[0].instructions[0]);
    fixture_project.programs.push_back(reset_twice);
    auto valid_fixture = exec::validate(fixture_project);
    LEANAT_CHECK(valid_fixture);
    opcode_test::FixtureConfig source;
    source.context.domain = DomainId{1};
    source.context.instance = InstanceId{1};
    source.context.connection = ConnectionId{1};
    source.environment = {
        {"objects.kind", Value(std::uint64_t(0))},
        {"objects.id", Value(std::uint64_t(1))},
        {"objects.maxBytes", Value(std::uint64_t(16))},
        {"objects.maxEntries", Value(std::uint64_t(4))},
        {"objects.valueNodeBytes", Value(std::uint64_t(sizeof(Value)))},
        {"objects.record",
         Value(Value::Array{Value(std::uint64_t(1)), Value(Bytes{1, 2, 3, 4}),
                            Value(Bytes{1, 2, 3, 4}), Value(std::uint64_t(0)), Value(false)})}};
    CoreRuntimeBackend fixture_backend(runtime, 32);
    auto fixture =
        opcode_test::make_object_fixture(runtime, fixture_backend, fixture_project, source);
    LEANAT_CHECK(fixture && fixture_backend.freeze());
    auto concrete = std::static_pointer_cast<opcode_test::object_fixture_detail::Owned>(
        fixture.value().lifetime[0]);
    auto actual_memory = std::get<std::shared_ptr<Memory>>(concrete->object);
    LEANAT_CHECK(actual_memory->version() == 0);
    auto before = fixture.value().snapshot().dump();
    exec::Interpreter fixture_vm(valid_fixture.value(), {}, &fixture_backend);
    EventTxn fixture_tx({}, source.context);
    exec::FuelCounter fixture_fuel{10000};
    LEANAT_CHECK(fixture_vm.execute_segment(
        0, source.context, {Value(std::uint64_t(1)), Value(Bytes{9}), Value(Bytes{})}, fixture_tx,
        fixture_fuel));
    LEANAT_CHECK(fixture.value().snapshot().dump() == before);
    LEANAT_CHECK(fixture_tx.commit());
    LEANAT_CHECK(actual_memory->version() == 1);
    LEANAT_CHECK(fixture.value().snapshot().dump() != before);
    LEANAT_CHECK(catalog_memory->data()[1] == 0);
    auto after = fixture.value().snapshot().dump();
    EventTxn rejected({}, source.context);
    LEANAT_CHECK(!fixture_vm.execute_segment(
        0, source.context, {Value(std::uint64_t(8)), Value(Bytes{9}), Value(Bytes{})}, rejected,
        fixture_fuel));
    LEANAT_CHECK(fixture.value().snapshot().dump() == after);
    for (std::uint64_t version : {2, 3}) {
      EventTxn reset_tx({}, source.context);
      LEANAT_CHECK(fixture_vm.execute_segment(1, source.context, {}, reset_tx, fixture_fuel));
      LEANAT_CHECK(actual_memory->version() == version - 1);
      LEANAT_CHECK(reset_tx.commit());
      LEANAT_CHECK(actual_memory->version() == version);
      LEANAT_CHECK(Bytes(actual_memory->data(), actual_memory->data() + actual_memory->size()) ==
                   Bytes({1, 2, 3, 4}));
    }
  }
  CoreRuntimeBackend backend(runtime, 128);
  auto memory = std::make_shared<Memory>(std::move(Memory::make(8).value()));
  auto resource = std::make_shared<Resource>(std::move(
      Resource::make({ResourceKind::Serial, 1, Duration{5}, Duration{}, 4}, DomainId{1}).value()));
  auto queue = std::make_shared<BoundedQueue>(4, 512, 16, DomainId{1});
  RegisterSpec reg;
  reg.width_bits = 8;
  reg.allowed_access_bytes = {1};
  reg.fields = {{FieldId{1}, 0, 8, RegisterAccess::RC, {9}}};
  auto registers = std::make_shared<RegisterBank>(std::move(RegisterBank::make({reg}).value()));
  auto pipe = std::make_shared<Pipeline>(std::move(
      Pipeline::make(Duration{3}, Duration{1}, 1, runtime.queue(), 4, DomainId{1}).value()));
  exec::Project project;
  project.types = {{exec::TypeKind::Bits, 64, {}, {}}};
  ObjectServiceConfig settings;
  settings.max_bytes = 64;
  settings.max_entries = 8;
  settings.queue_element_types[ObjectId{3}] = 0;
  settings.queue_element_hashes[ObjectId{3}] = ObjectServices::type_hash(project, 0).value();
  auto services =
      ObjectServices::create(project,
                             {{{ObjectId{1}, 35, DomainId{1}, InstanceId{1}}, memory},
                              {{ObjectId{2}, 37, DomainId{1}, InstanceId{1}}, resource},
                              {{ObjectId{3}, 38, DomainId{1}, InstanceId{1}}, queue},
                              {{ObjectId{4}, 36, DomainId{1}, InstanceId{1}}, registers},
                              {{ObjectId{5}, 39, DomainId{1}, InstanceId{1}}, pipe}},
                             settings);
  LEANAT_CHECK(services);
  std::vector<std::pair<ObjectId, StandardIntrinsic>> calls = {
      {ObjectId{1}, StandardIntrinsic::WriteBytes},
      {ObjectId{1}, StandardIntrinsic::ReadBytes},
      {ObjectId{2}, StandardIntrinsic::ReserveDefault},
      {ObjectId{3}, StandardIntrinsic::Push},
      {ObjectId{3}, StandardIntrinsic::Pop},
      {ObjectId{4}, StandardIntrinsic::Debug},
      {ObjectId{5}, StandardIntrinsic::Submit}};
  for (std::uint32_t i = 0; i < calls.size(); ++i) {
    auto signature = services.value()->find(calls[i].first, calls[i].second);
    LEANAT_CHECK(signature);
    project.programs.push_back(program(signature.value(), i));
  }
  auto failing = project.programs[0];
  failing.id = 7;
  failing.blocks[0].terminator.kind = exec::TermKind::Fail;
  failing.blocks[0].terminator.error = "rollback";
  project.programs.push_back(failing);
  auto valid = exec::validate(project);
  LEANAT_CHECK(valid);
  LEANAT_CHECK(services.value()->register_into(backend));
  LEANAT_CHECK(backend.freeze());
  exec::Interpreter vm(valid.value(), {}, &backend);
  ExecutionContext context;
  context.domain = DomainId{1};
  context.instance = InstanceId{1};
  context.owner = 7;
  EventTxn tx({}, context);
  exec::FuelCounter fuel{10000};
  auto write = vm.execute_segment(
      0, context, {Value(std::uint64_t(0)), Value(Bytes{4, 5}), Value(Bytes{})}, tx, fuel);
  LEANAT_CHECK(write);
  auto read =
      vm.execute_segment(1, context, {Value(std::uint64_t(0)), Value(std::uint64_t(2))}, tx, fuel);
  LEANAT_CHECK(read && std::get<Bytes>(read.value().values[0].data) == Bytes({4, 5}));
  LEANAT_CHECK(memory->data()[0] == 0);
  Handle owner{HandleKind::Transaction, DomainId{1}, 1, 0, 1, 7};
  auto grant = vm.execute_segment(2, context, {Value(owner), Value(std::uint64_t(10))}, tx, fuel);
  LEANAT_CHECK(grant && std::get<std::uint64_t>(
                            std::get<Value::Array>(grant.value().values[0].data)[0].data) == 10);
  LEANAT_CHECK(vm.execute_segment(3, context, {Value(std::uint64_t(42))}, tx, fuel));
  auto popped = vm.execute_segment(4, context, {}, tx, fuel);
  LEANAT_CHECK(popped);
  auto outer = std::get<Value::Array>(popped.value().values[0].data);
  LEANAT_CHECK(std::get<std::uint64_t>(outer[0].data) == 1);
  LEANAT_CHECK(tx.commit());
  LEANAT_CHECK(memory->data()[0] == 4);
  EventTxn aborted({}, context);
  auto fail = vm.execute_segment(
      7, context, {Value(std::uint64_t(0)), Value(Bytes{99}), Value(Bytes{})}, aborted, fuel);
  LEANAT_CHECK(fail && fail.value().kind == exec::SegmentResult::Kind::Failed);
  LEANAT_CHECK(memory->data()[0] == 4);
  context.kind = ContextKind::Debug;
  EventTxn debug({}, context);
  auto peek = vm.execute_segment(
      5, context, {Value(std::uint64_t(Command::Read)), Value(std::uint64_t(0)), Value(Bytes{0})},
      debug, fuel);
  LEANAT_CHECK(peek);
  LEANAT_CHECK(debug.commit());
  context.kind = ContextKind::Timed;
  EventTxn unchanged({}, context);
  LEANAT_CHECK(registers->read_field(unchanged, FieldId{1}).value() == Bytes{9});
  EventTxn pipeline({}, context);
  LEANAT_CHECK(
      vm.execute_segment(6, context, {Value(owner), Value(std::uint64_t(0))}, pipeline, fuel));
  LEANAT_CHECK(pipeline.commit());
  LEANAT_CHECK(runtime.queue().occupied() == 1);
  auto wrong = project.services[0];
  wrong.abi_hash[0] ^= 1;
  LEANAT_CHECK(!backend.check_signature(wrong));
  exec::Project subset;
  subset.types = project.types;
  subset.schema_major = 2;
  subset.services = {services.value()->find(ObjectId{1}, StandardIntrinsic::ReadBytes).value()};
  auto rebound = ObjectServices::bind_existing(
      subset, {{{ObjectId{1}, 35, DomainId{1}, InstanceId{1}}, memory}}, settings);
  LEANAT_CHECK(rebound && rebound.value()->entries().size() == 1);
  auto altered = subset;
  altered.services[0].effect_mask ^= exec::StateWrite;
  LEANAT_CHECK(!ObjectServices::bind_existing(
      altered, {{{ObjectId{1}, 35, DomainId{1}, InstanceId{1}}, memory}}, settings));
  auto queue_subset = subset;
  queue_subset.services = {services.value()->find(ObjectId{3}, StandardIntrinsic::Push).value()};
  LEANAT_CHECK(ObjectServices::bind_existing(
      queue_subset, {{{ObjectId{3}, 38, DomainId{1}, InstanceId{1}}, queue}}, settings));
  queue_subset.types[0].bound = 32;
  LEANAT_CHECK(!ObjectServices::bind_existing(
      queue_subset, {{{ObjectId{3}, 38, DomainId{1}, InstanceId{1}}, queue}}, settings));
  auto read_signature = services.value()->find(ObjectId{1}, StandardIntrinsic::ReadBytes).value();
  EventTxn oversized({}, context);
  LEANAT_CHECK(!backend.invoke(read_signature, {Value(std::uint64_t(0)), Value(std::uint64_t(65))},
                               context, oversized));
  // A failed catalog installation must not leave the first provider behind.
  CoreRuntimeBackend too_small(runtime, 1);
  LEANAT_CHECK(!services.value()->register_into(too_small));
  LEANAT_CHECK(too_small.register_provider(read_signature,
                                           [](const std::vector<Value> &, const ExecutionContext &,
                                              EventTxn &) -> Expected<std::vector<Value>> {
                                             return std::vector<Value>{Value(Bytes{})};
                                           }));
  // Real ledger activity upgrades an entry even when its business publication flag lagged.
  exec::Project protocol_project;
  protocol_project.types = {
      {exec::TypeKind::Handle, std::uint64_t(HandleKind::Transaction), {}, {}},
      {exec::TypeKind::Handle, std::uint64_t(HandleKind::Hop), {}, {}},
      {exec::TypeKind::Record, 0, {0, 1}, {}}};
  auto protocol_queue = std::make_shared<BoundedQueue>(4, 1024, 16, DomainId{1});
  CancelScopeStore scopes(DomainId{1}, 50, 4, 8, 8);
  protocol_queue->set_scope_store(scopes);
  ObjectServiceConfig protocol_config;
  protocol_config.queue_element_types[ObjectId{9}] = 2;
  protocol_config.queue_protocol_fields[ObjectId{9}] = {0, 1};
  auto protocol_services = ObjectServices::create(
      protocol_project, {{{ObjectId{9}, 38, DomainId{1}, InstanceId{1}}, protocol_queue}},
      protocol_config);
  LEANAT_CHECK(protocol_services);
  LEANAT_CHECK(!protocol_services.value()->find(ObjectId{9}, StandardIntrinsic::RemoveOwned));
  auto hop = runtime.protocol().create_ledger(ConnectionId{1}, TransportId{77});
  LEANAT_CHECK(hop);
  Handle transaction{HandleKind::Transaction, DomainId{1}, 70, 0, 1, 7};
  LEANAT_CHECK(
      runtime.drains().register_responsibility({transaction,
                                                InstanceId{1},
                                                0,
                                                {},
                                                {{hop.value(), false, false, false, false}},
                                                false,
                                                false}));
  EventTxn push({}, context);
  LEANAT_CHECK(protocol_queue
                   ->try_push(push, Value(Value::Array{Value(transaction), Value(hop.value())}),
                              scopes.root())
                   .value());
  LEANAT_CHECK(push.commit());
  WireCall call;
  call.id = CallId{999};
  call.connection = ConnectionId{1};
  call.transport = TransportId{77};
  call.phase = begin_req;
  call.request.data = {1};
  auto pending = runtime.protocol().begin_call(call);
  LEANAT_CHECK(pending);
  EventTxn observe({}, context);
  LEANAT_CHECK(protocol_services.value()->observe_queue_publication(ObjectId{9}, runtime, observe));
  LEANAT_CHECK(protocol_queue->inspect_entries(observe).value()[0].publication ==
               QueuePublication::PublishedProtocol);
  LEANAT_CHECK(observe.commit());
  EventTxn no_drain({}, context);
  auto denied_cancel = protocol_services.value()->cancel_queue_scope(ObjectId{9}, runtime, no_drain,
                                                                     scopes.root(), Handle{}, 0);
  LEANAT_CHECK(!denied_cancel);
  LEANAT_CHECK(protocol_queue->inspect_entries(no_drain).value().size() == 1);
  LEANAT_CHECK(no_drain.commit());
  LEANAT_CHECK(!runtime.drains().is_cancelled(transaction));
  // An idle queued request cancels its actual Runtime responsibility before removal.
  auto idle = runtime.protocol().create_ledger(ConnectionId{1}, TransportId{78});
  LEANAT_CHECK(idle);
  Handle idle_transaction{HandleKind::Transaction, DomainId{1}, 70, 1, 1, 7};
  LEANAT_CHECK(
      runtime.drains().register_responsibility({idle_transaction,
                                                InstanceId{1},
                                                0,
                                                {},
                                                {{idle.value(), false, false, false, false}},
                                                false,
                                                false}));
  auto scope = scopes.create(scopes.root());
  LEANAT_CHECK(scope);
  EventTxn idle_push({}, context);
  LEANAT_CHECK(protocol_queue
                   ->try_push(idle_push,
                              Value(Value::Array{Value(idle_transaction), Value(idle.value())}),
                              scope.value())
                   .value());
  LEANAT_CHECK(idle_push.commit());
  auto outgoing = runtime.allocate_call_id(CallOrigin::Outgoing);
  LEANAT_CHECK(outgoing);
  SendIntent intent;
  intent.txn = idle_transaction;
  intent.connection = ConnectionId{1};
  intent.transport = TransportId{78};
  intent.call_id = outgoing.value();
  intent.not_before = Tick{20};
  intent.payload.data = {1};
  LEANAT_CHECK(runtime.publish(intent));
  LEANAT_CHECK(runtime.has_unstarted_intent(idle_transaction));
  EventTxn cancel({}, context);
  auto cleanup = protocol_services.value()->cancel_queue_scope(ObjectId{9}, runtime, cancel,
                                                               scope.value(), Handle{}, 0);
  LEANAT_CHECK(cleanup && cleanup.value().removed.size() == 1);
  LEANAT_CHECK(runtime.commit_segment(cancel));
  LEANAT_CHECK(runtime.drains().is_cancelled(idle_transaction));
  runtime.set_handler([](const QueuedEvent &, Runtime &) -> Expected<void> { return {}; });
  for (int n = 0; n < 8 && runtime.next_wakeup(); ++n)
    LEANAT_CHECK(runtime.pump_batch(Tick{20}, 16));
  LEANAT_CHECK(!runtime.stopped());
}
