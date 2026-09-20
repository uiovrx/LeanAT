#include "leanat/descriptor.hpp"
#include "leanat/interpreter.hpp"
#include "test_support.hpp"
#include <fstream>
#include <iterator>
using namespace leanat;
using namespace leanat::exec;
static Project hierarchy() {
  Project p;
  p.schema_major = 4;
  p.types = {{TypeKind::Bits, 64}};
  p.state_types = {0, 0};
  p.initial_state = {Value{std::uint64_t{0}}, Value{std::uint64_t{0}}};
  ComponentDesc c;
  c.id = 3;
  c.parameters.push_back({5, 0, Value{std::uint64_t{7}}, {}});
  c.states.push_back({0, 0, Value{std::uint64_t{0}}});
  EndpointDesc source;
  source.id = 0;
  source.role = EndpointRole::Initiator;
  source.bus_width = 64;
  source.max_bindings = 1;
  source.max_outstanding = 2;
  source.max_payload_bytes = 64;
  source.max_byte_enable_bytes = 8;
  source.protocol_ref = "base4";
  auto target = source;
  target.id = 1;
  target.role = EndpointRole::Target;
  c.endpoints = {source, target};
  p.components = {c};
  for (std::uint32_t n = 0; n < 2; ++n) {
    Program pr;
    pr.id = n;
    pr.effect_mask = Effect::StateWrite;
    pr.result_types = {0};
    Block b;
    Instruction literal;
    literal.op = Op::Const;
    literal.dest = Reg{0, 0};
    literal.value = Value{std::uint64_t{42}};
    Instruction write;
    write.op = Op::BufferStateWrite;
    write.args = {{0, 0}};
    write.immediate = n;
    b.instructions = {literal, write};
    b.terminator.values = {{0, 0}};
    pr.blocks = {b};
    p.programs.push_back(pr);
    InstanceDesc instance;
    instance.id = n ? 20 : 10;
    instance.definition = 3;
    instance.state_base = n;
    instance.state_count = 1;
    instance.handlers.push_back({0, n, ContextKind::Timed, "timer", {}, {}, ""});
    instance.resolved_config = {{5, Value{std::uint64_t{7}}}};
    p.instances.push_back(instance);
  }
  SystemMetadata system;
  system.id = 9;
  system.runtime_domain = 7;
  for (auto &i : p.instances)
    system.original_instances.push_back({i.id, i.definition, i.resolved_config, {}});
  system.bindings.push_back({0, {10, 0, 0}, {20, 1, 0}, {}});
  system.address_maps.push_back({0, 10, 0, 0, 0, 16, 0, false, {}});
  p.system_metadata = system;
  ExternalContract external;
  external.id = 1;
  external.cpp_type = "Native";
  external.header = "native.hpp";
  MetadataLiteral bits;
  bits.tag = 2;
  bits.width = 8;
  bits.value = Value{std::uint64_t{7}};
  MetadataLiteral vec;
  vec.tag = 5;
  vec.children = {bits};
  external.constructor_mapping = {{"small", bits}, {"vector", vec}};
  p.external_contracts = {external};
  return p;
}
int main(int argc, char **argv) {
  if (argc == 2) {
    std::ifstream file(argv[1], std::ios::binary);
    LEANAT_CHECK(file);
    Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    auto loaded = load_descriptor(bytes);
    LEANAT_CHECK(loaded);
    std::vector<VersionedCell> state;
    for (auto &v : loaded.value().get().initial_state)
      state.push_back(VersionedCell{v});
    std::vector<VersionedCell *> refs;
    for (auto &cell : state)
      refs.push_back(&cell);
    ExecutionContext ctx;
    ctx.domain = DomainId{7};
    ctx.instance = InstanceId{20};
    Interpreter vm(loaded.value(), refs);
    EventTxn txn({}, ctx);
    FuelCounter fuel{20};
    auto result = vm.execute_segment(1, ctx, {}, txn, fuel);
    if (!result)
      std::cerr << result.error().message << '\n';
    LEANAT_CHECK(result && txn.commit() && state.size() == 2 &&
                 state[0].value == Value{std::uint64_t{0}} &&
                 state[1].value == Value{std::uint64_t{42}});
    return 0;
  }
  auto p = hierarchy();
  auto v = validate(p);
  if (!v)
    std::cerr << v.error().message << '\n';
  LEANAT_CHECK(v);
  auto encoded = serialize(v.value());
  LEANAT_CHECK(encoded);
  auto loaded = load_descriptor(encoded.value());
  if (!loaded)
    std::cerr << loaded.error().message << '\n';
  LEANAT_CHECK(loaded);
  auto again = serialize(loaded.value());
  LEANAT_CHECK(again && again.value() == encoded.value());
  auto invalid = p;
  invalid.instances[1].state_base = 0;
  LEANAT_CHECK(!validate(invalid));
  invalid = p;
  invalid.programs[0].blocks[0].instructions[1].immediate = 1;
  LEANAT_CHECK(!validate(invalid));
  invalid = p;
  invalid.system_metadata->original_instances[0].resolved_config[0].second =
      Value{std::uint64_t{8}};
  LEANAT_CHECK(!validate(invalid));
  invalid = p;
  invalid.system_metadata->bindings[0].sink_endpoint.binding_index = 1;
  LEANAT_CHECK(!validate(invalid));
  invalid = p;
  invalid.system_metadata->address_maps[0].source_start = UINT64_MAX;
  LEANAT_CHECK(!validate(invalid));
  invalid = p;
  invalid.external_contracts[0].constructor_mapping[0].second.width = 0;
  LEANAT_CHECK(!validate(invalid));
  invalid = p;
  MetadataLiteral deep;
  for (unsigned i = 0; i < 70; ++i) {
    MetadataLiteral next;
    next.tag = 3;
    next.children.push_back(std::move(deep));
    deep = std::move(next);
  }
  invalid.external_contracts[0].constructor_mapping[0].second = std::move(deep);
  LEANAT_CHECK(!validate(invalid));
  auto capabilities = p;
  capabilities.capabilities = {"test.feature"};
  auto cv = validate(capabilities);
  LEANAT_CHECK(cv);
  auto bytes = serialize(cv.value());
  LEANAT_CHECK(bytes && !load_descriptor(bytes.value()));
  LoadPolicy permitted;
  permitted.allowed_capabilities = {"test.feature"};
  LEANAT_CHECK(load_descriptor(bytes.value(), permitted));
  Limits budget;
  budget.max_work = 20;
  LEANAT_CHECK(!validate(p, budget));
  VersionedCell first{p.initial_state[0]}, second{p.initial_state[1]};
  Interpreter vm(v.value(), {&first, &second});
  ExecutionContext ctx;
  ctx.domain = DomainId{7};
  ctx.instance = InstanceId{20};
  EventTxn txn({}, ctx);
  FuelCounter fuel{3};
  auto result = vm.execute_segment(1, ctx, {}, txn, fuel);
  LEANAT_CHECK(result && txn.commit() && first.value == Value{std::uint64_t{0}} &&
               second.value == Value{std::uint64_t{42}});
  EventTxn wrong({}, ctx);
  FuelCounter wrongfuel{3};
  auto denied = vm.execute_segment(0, ctx, {}, wrong, wrongfuel);
  LEANAT_CHECK(!denied && denied.error().code == ErrorCode::WrongOwner && wrongfuel.remaining == 3);
  ctx.domain = DomainId{9};
  EventTxn domain({}, ctx);
  FuelCounter domainfuel{3};
  auto denied_domain = vm.execute_segment(1, ctx, {}, domain, domainfuel);
  LEANAT_CHECK(!denied_domain && denied_domain.error().code == ErrorCode::WrongDomain);
}
