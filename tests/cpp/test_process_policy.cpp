#include "leanat/descriptor.hpp"
#include "leanat/interpreter.hpp"
#include "test_support.hpp"
#include <fstream>
using namespace leanat;
using namespace leanat::exec;

static void metadata(Project &p) {
  if (p.schema_major < 4)
    return;
  ComponentDesc component;
  component.id = 1;
  for (std::uint32_t i = 0; i < p.state_types.size(); ++i)
    component.states.push_back({i, p.state_types[i], p.initial_state[i]});
  p.components = {component};
  InstanceDesc instance;
  instance.id = 1;
  instance.definition = 1;
  instance.state_count = static_cast<std::uint32_t>(p.state_types.size());
  for (const auto &program : p.programs) {
    HandlerBinding h;
    h.local_id = program.id;
    h.program_id = program.id;
    h.context = program.context;
    h.trigger = "manual";
    if (program.context == ContextKind::Process) {
      ProcessCapacity capacity;
      capacity.max_instances = 2;
      capacity.result_capacity = 2;
      capacity.frame_bytes_limit = 4096;
      h.process_capacity = capacity;
    }
    instance.handlers.push_back(h);
  }
  p.instances = {instance};
  SystemMetadata system;
  system.id = 1;
  system.runtime_domain = 9;
  system.original_instances = {{1, 1, {}, {}}};
  p.system_metadata = system;
}
static Project writing(unsigned version = 5, std::uint64_t fuel = 3) {
  Project p;
  p.schema_major = static_cast<std::uint16_t>(version);
  p.types = {{TypeKind::Bits, 64}};
  p.state_types = {0};
  p.initial_state = {Value{std::uint64_t{7}}};
  Program program;
  program.context = version == 1 ? ContextKind::Timed : ContextKind::Process;
  program.effect_mask = version == 1 ? 0 : Effect::StateWrite;
  program.result_types = {0};
  if (version >= 5) {
    program.instruction_fuel = fuel;
    program.owner_policy = "caller";
    program.result_lifetime_policy = "until-release";
  }
  Block block;
  Instruction literal;
  literal.op = Op::Const;
  literal.dest = Reg{0, 0};
  literal.value = Value{std::uint64_t{42}};
  Instruction write;
  write.op = Op::BufferStateWrite;
  write.args = {{0, 0}};
  write.immediate = 0;
  block.instructions = {literal, write};
  block.terminator.values = {{0, 0}};
  program.blocks = {block};
  p.programs = {program};
  metadata(p);
  return p;
}
static ExecutionContext execution(const Project &p) {
  ExecutionContext c;
  c.kind = p.programs.front().context;
  c.owner = 7;
  if (p.schema_major >= 4) {
    c.domain = DomainId{9};
    c.instance = InstanceId{1};
  }
  return c;
}
static void resign(Bytes &bytes) {
  std::fill(bytes.begin() + 72, bytes.begin() + 104, 0);
  auto hash = sha256(bytes);
  std::copy(hash.begin(), hash.end(), bytes.begin() + 72);
}
static ValidatedProject valid(Project p) {
  auto checked = validate(std::move(p));
  if (!checked)
    std::cerr << checked.error().message << '\n';
  LEANAT_CHECK(checked);
  return std::move(checked.value());
}
static void schema() {
  for (unsigned version = 1; version <= 5; ++version) {
    auto project = valid(writing(version));
    auto bytes = serialize(project);
    LEANAT_CHECK(bytes);
    auto loaded = load_descriptor(bytes.value());
    LEANAT_CHECK(loaded);
    auto again = serialize(loaded.value());
    LEANAT_CHECK(again && again.value() == bytes.value());
    const auto &p = loaded.value().get().programs[0];
    LEANAT_CHECK(p.instruction_fuel == (version == 5 ? 3 : 0));
    LEANAT_CHECK(p.owner_policy == (version == 5 ? "caller" : ""));
    LEANAT_CHECK(p.result_lifetime_policy == (version == 5 ? "until-release" : ""));
  }
  for (const auto &bad_owner : {"", "scope-id", "Caller"}) {
    auto p = writing();
    p.programs[0].owner_policy = bad_owner;
    LEANAT_CHECK(!validate(p));
  }
  for (const auto &bad_lifetime : {"", "owning-return", "forever"}) {
    auto p = writing();
    p.programs[0].result_lifetime_policy = bad_lifetime;
    LEANAT_CHECK(!validate(p));
  }
  auto zero = writing(5, 0);
  LEANAT_CHECK(!validate(zero));
  auto legacy = writing(4);
  legacy.programs[0].instruction_fuel = 3;
  LEANAT_CHECK(!validate(legacy));
  auto wrong_context = writing();
  wrong_context.programs[0].context = ContextKind::Timed;
  metadata(wrong_context);
  LEANAT_CHECK(!validate(wrong_context));
  auto encoded = serialize(valid(writing())).value();
  const Bytes needle{'c', 'a', 'l', 'l', 'e', 'r'};
  auto at = std::search(encoded.begin(), encoded.end(), needle.begin(), needle.end());
  LEANAT_CHECK(at != encoded.end());
  *at = 'x';
  resign(encoded);
  auto rejected = load_descriptor(encoded);
  LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::Unsupported);
  auto plain = writing();
  plain.programs[0].instruction_fuel = 0;
  plain.programs[0].owner_policy.clear();
  plain.programs[0].result_lifetime_policy.clear();
  LEANAT_CHECK(validate(plain));
}
static void budgets() {
  for (std::uint64_t cap : {2u, 3u}) {
    auto checked = valid(writing(5, cap));
    VersionedCell cell{Value{std::uint64_t{7}}};
    Interpreter vm(checked, {&cell});
    auto c = execution(checked.get());
    EventTxn tx({}, c);
    FuelCounter caller{100};
    auto result = vm.execute_segment(0, c, {}, tx, caller);
    if (cap == 2) {
      LEANAT_CHECK(!result && result.error().code == ErrorCode::FuelExhausted);
      LEANAT_CHECK(!tx.commit() && cell.value == Value{std::uint64_t{7}} && caller.remaining == 98);
    } else {
      LEANAT_CHECK(result && result.value().fuel_used == 3 && caller.remaining == 97);
      LEANAT_CHECK(tx.commit() && cell.value == Value{std::uint64_t{42}});
      EventTxn second({}, c);
      auto again = vm.execute_segment(0, c, {}, second, caller);
      LEANAT_CHECK(again && again.value().fuel_used == 3 && caller.remaining == 94 &&
                   second.commit());
    }
  }
  auto checked = valid(writing());
  VersionedCell cell{Value{std::uint64_t{7}}};
  Interpreter vm(checked, {&cell});
  auto c = execution(checked.get());
  EventTxn tx({}, c);
  FuelCounter caller{2};
  auto result = vm.execute_segment(0, c, {}, tx, caller);
  LEANAT_CHECK(!result && result.error().code == ErrorCode::FuelExhausted &&
               caller.remaining == 0 && cell.value == Value{std::uint64_t{7}});
  for (std::uint64_t cap : {4u, 5u}) {
    auto p = writing(5, cap);
    Program pure;
    pure.id = 1;
    pure.result_types = {0};
    Block body;
    body.instructions = {p.programs[0].blocks[0].instructions[0]};
    body.terminator.values = {{0, 0}};
    pure.blocks = {body};
    p.programs.push_back(pure);
    auto &call = p.programs[0].blocks[0].instructions[0];
    call.op = Op::CallPure;
    call.immediate = 1;
    call.value = Value{};
    metadata(p);
    auto project = valid(p);
    VersionedCell state{Value{std::uint64_t{7}}};
    Interpreter nested(project, {&state});
    auto ctx = execution(p);
    EventTxn segment({}, ctx);
    FuelCounter outer{100};
    auto r = nested.execute_segment(0, ctx, {}, segment, outer);
    if (cap == 4) {
      LEANAT_CHECK(!r && r.error().code == ErrorCode::FuelExhausted && outer.remaining == 96 &&
                   state.value == Value{std::uint64_t{7}});
    } else {
      LEANAT_CHECK(r && r.value().fuel_used == 5 && outer.remaining == 95 && segment.commit() &&
                   state.value == Value{std::uint64_t{42}});
    }
  }
}
static void flat_metadata() {
  auto p = writing();
  p.components.clear();
  p.instances.clear();
  p.system_metadata.reset();
  auto encoded = serialize(valid(p));
  LEANAT_CHECK(encoded);
  auto loaded = load_descriptor(encoded.value());
  LEANAT_CHECK(loaded);
  VersionedCell state{Value{std::uint64_t{7}}};
  Interpreter vm(loaded.value(), {&state});
  auto ctx = execution(p);
  EventTxn txn({}, ctx);
  FuelCounter fuel{100};
  auto result = vm.execute_segment(0, ctx, {}, txn, fuel);
  LEANAT_CHECK(result && fuel.remaining == 97 && txn.commit());
  auto legacy = p;
  legacy.schema_major = 4;
  legacy.programs[0].instruction_fuel = 0;
  legacy.programs[0].owner_policy.clear();
  legacy.programs[0].result_lifetime_policy.clear();
  LEANAT_CHECK(!validate(legacy));
  auto partial = p;
  partial.components.push_back(ComponentDesc{});
  LEANAT_CHECK(!validate(partial));
  partial = p;
  partial.instances.push_back(InstanceDesc{});
  LEANAT_CHECK(!validate(partial));
}
struct Continuation : InterpreterBackend {
  VersionedCell saved{Value{std::uint64_t{0}}};
  Expected<void> check_signature(const ServiceSignature &) const override {
    return {};
  }
  Expected<std::vector<Value>> invoke(const ServiceSignature &, const std::vector<Value> &,
                                      const ExecutionContext &, EventTxn &) override {
    return fail(ErrorCode::Unsupported, "no service");
  }
  Expected<void> prepare_suspend(Handle, BlockId, std::vector<Value> live, TypeId,
                                 const ExecutionContext &, EventTxn &tx) override {
    return tx.buffer(saved, live.at(0));
  }
  Expected<ResumeInput> prepare_resume(const SuspensionToken &, const ExecutionContext &,
                                       EventTxn &tx) override {
    auto value = tx.read(saved);
    if (!value)
      return value.error();
    auto reset = tx.buffer(saved, Value{std::uint64_t{0}});
    if (!reset)
      return reset.error();
    return ResumeInput{ProgramId{0}, BlockId{1}, {Value{true}, value.value()}};
  }
};
static void resume_policy() {
  for (bool extra : {false, true}) {
    Project p;
    p.schema_major = 5;
    p.types = {{TypeKind::Handle, static_cast<unsigned>(HandleKind::Wait)},
               {TypeKind::Bits, 64},
               {TypeKind::Bool}};
    Program program;
    program.context = ContextKind::Process;
    program.instruction_fuel = 1;
    program.owner_policy = "caller";
    program.result_lifetime_policy = "until-release";
    program.effect_mask = Effect::Wait;
    program.input_types = {0, 1};
    program.result_types = {1};
    program.frame = {{1, 0, 8, 1}};
    program.frame_bytes = 64;
    Block before;
    before.parameters = {{0, 0}, {1, 1}};
    before.terminator.kind = TermKind::Suspend;
    before.terminator.value = {0, 0};
    before.terminator.yes.target = 1;
    before.terminator.values = {{1, 1}};
    Block after;
    after.id = 1;
    after.parameters = {{2, 2}, {3, 1}};
    after.terminator.values = {{3, 1}};
    if (extra) {
      Instruction move;
      move.op = Op::Move;
      move.args = {{3, 1}};
      move.dest = Reg{4, 1};
      after.instructions = {move};
      after.terminator.values = {{4, 1}};
    }
    program.blocks = {before, after};
    p.programs = {program};
    metadata(p);
    auto checked = valid(p);
    Continuation bridge;
    Interpreter vm(checked, {}, &bridge);
    auto c = execution(p);
    Handle wait{HandleKind::Wait, c.domain, 1, 0, 1, c.owner};
    EventTxn suspend({}, c);
    FuelCounter caller{20};
    auto started =
        vm.execute_segment(0, c, {Value{wait}, Value{std::uint64_t{7}}}, suspend, caller);
    LEANAT_CHECK(started && started.value().kind == SegmentResult::Kind::Suspended &&
                 caller.remaining == 19 && suspend.commit());
    SuspensionToken token{{HandleKind::Process, c.domain, 2, 0, 1, c.owner}, wait, 1};
    EventTxn resume({}, c);
    auto result = vm.resume_segment(token, c, resume, caller);
    LEANAT_CHECK(caller.remaining == 18);
    if (extra) {
      LEANAT_CHECK(!result && result.error().code == ErrorCode::FuelExhausted &&
                   bridge.saved.value == Value{std::uint64_t{7}} && !resume.commit());
    } else {
      LEANAT_CHECK(result && result.value().values == std::vector<Value>{Value{std::uint64_t{7}}} &&
                   resume.commit() && bridge.saved.value == Value{std::uint64_t{0}});
    }
  }
}
static void fin_and_indices() {
  Project p;
  p.schema_major = 5;
  p.types = {{TypeKind::Fin, 0}, {TypeKind::Bool}};
  Program program;
  program.input_types = {0, 0};
  program.result_types = {1};
  Block block;
  block.parameters = {{0, 0}, {1, 0}};
  Instruction less;
  less.op = Op::Binary;
  less.binary = Binary::Lt;
  less.args = {{0, 0}, {1, 0}};
  less.dest = Reg{2, 1};
  block.instructions = {less};
  block.terminator.values = {{2, 1}};
  program.blocks = {block};
  p.programs = {program};
  metadata(p);
  auto checked = valid(p);
  auto encoded = serialize(checked);
  LEANAT_CHECK(encoded && load_descriptor(encoded.value()));
  LEANAT_CHECK(conforms(p, 0, Value{UINT64_MAX}));
  Interpreter vm(checked);
  auto c = execution(p);
  EventTxn tx({}, c);
  FuelCounter fuel{2};
  auto result = vm.execute_segment(0, c, {Value{std::uint64_t{0}}, Value{UINT64_MAX}}, tx, fuel);
  LEANAT_CHECK(result && result.value().values == std::vector<Value>{Value{true}});
  p.schema_major = 4;
  LEANAT_CHECK(!validate(p));
  for (auto index :
       std::vector<Type>{{TypeKind::Bits, 8}, {TypeKind::Fin, 3}, {TypeKind::Fin, 0}}) {
    Project v;
    v.schema_major = 5;
    v.types = {{TypeKind::Bits, 64}, index, {TypeKind::Vec, 2, {0}}};
    Program get;
    get.input_types = {2, 1};
    get.result_types = {0};
    Block b;
    b.parameters = {{0, 2}, {1, 1}};
    Instruction access;
    access.op = Op::VecGet;
    access.args = {{0, 2}, {1, 1}};
    access.dest = Reg{2, 0};
    b.instructions = {access};
    b.terminator.values = {{2, 0}};
    get.blocks = {b};
    v.programs = {get};
    metadata(v);
    auto validated = valid(v);
    Interpreter interpreter(validated);
    auto ctx = execution(v);
    for (unsigned n : {1u, 2u}) {
      EventTxn segment({}, ctx);
      FuelCounter budget{2};
      auto outcome = interpreter.execute_segment(
          0, ctx,
          {Value{Value::Array{Value{std::uint64_t{10}}, Value{std::uint64_t{20}}}},
           Value{std::uint64_t{n}}},
          segment, budget);
      if (n == 1)
        LEANAT_CHECK(outcome &&
                     outcome.value().values == std::vector<Value>{Value{std::uint64_t{20}}});
      else
        LEANAT_CHECK(!outcome && outcome.error().code == ErrorCode::InvalidArgument);
    }
    v.schema_major = 4;
    LEANAT_CHECK(!validate(v));
  }
}
static void failed_trace_prefix() {
  struct Observer final : InterpreterObserver {
    std::vector<Op> completed;
    std::vector<Value> trace_values;
    bool on_opcode(const OpcodeObservation &o) override {
      if (o.stage == OpcodeObservation::Stage::Completed) {
        completed.push_back(o.instruction.op);
        if (o.instruction.op == Op::Trace)
          trace_values.push_back(o.argument(0));
      }
      return true;
    }
  } observer;
  auto p = writing(5, 4);
  Instruction trace;
  trace.op = Op::Trace;
  trace.text = "before-explicit-failure";
  trace.args = {{0, 0}};
  auto &block = p.programs[0].blocks[0];
  block.instructions.push_back(trace);
  block.terminator.kind = TermKind::Fail;
  block.terminator.error = "expected failure";
  block.terminator.values.clear();
  auto checked = valid(p);
  VersionedCell state{Value{std::uint64_t{7}}};
  Interpreter vm(checked, {&state}, nullptr, &observer);
  auto ctx = execution(p);
  ctx.ready = {Tick{23}, 4};
  ctx.connection = ConnectionId{5};
  EventTxn txn({}, ctx);
  FuelCounter fuel{100};
  auto result = vm.execute_segment(0, ctx, {}, txn, fuel);
  LEANAT_CHECK(result && result.value().kind == SegmentResult::Kind::Failed);
  LEANAT_CHECK(result.value().error == "expected failure" && result.value().fuel_used == 4 &&
               fuel.remaining == 96 && result.value().values.empty());
  LEANAT_CHECK(!txn.is_open() && !txn.commit() && state.value == Value{std::uint64_t{7}});
  LEANAT_CHECK(observer.complete() && observer.completed ==
      (std::vector<Op>{Op::Const, Op::BufferStateWrite, Op::Trace}));
  LEANAT_CHECK(result.value().traces.size() == 1);
  const auto &retained = result.value().traces.front();
  LEANAT_CHECK(retained.kind == trace.text && retained.values == observer.trace_values &&
      retained.values == std::vector<Value>{Value{std::uint64_t{42}}} &&
      retained.ready == ctx.ready && retained.instance == ctx.instance &&
      retained.connection == ctx.connection);
}
static void lean_fixture(const char *path) {
  std::ifstream file(path, std::ios::binary);
  LEANAT_CHECK(file);
  Bytes bytes;
  char ch;
  while (file.get(ch)) {
    LEANAT_CHECK(bytes.size() < LoadPolicy{}.max_file_bytes);
    bytes.push_back(static_cast<unsigned char>(ch));
  }
  LEANAT_CHECK(file.eof());
  auto loaded = load_descriptor(bytes);
  LEANAT_CHECK(loaded);
  const auto &p = loaded.value().get();
  LEANAT_CHECK(p.schema_major == 5 && p.programs.size() == 1 && p.system_metadata &&
               p.instances.size() == 1 && p.initial_state.empty());
  const auto &program = p.programs.front();
  LEANAT_CHECK(program.context == ContextKind::Process && program.instruction_fuel == 2 &&
               program.owner_policy == "caller" &&
               program.result_lifetime_policy == "until-release");
  auto encoded = serialize(loaded.value());
  LEANAT_CHECK(encoded && encoded.value() == bytes);
  ExecutionContext ctx;
  ctx.kind = program.context;
  ctx.domain = DomainId{p.system_metadata->runtime_domain};
  ctx.instance = InstanceId{p.instances.front().id};
  Interpreter vm(loaded.value());
  for (std::uint64_t available : {100u, 1u}) {
    EventTxn txn({}, ctx);
    FuelCounter fuel{available};
    auto result = vm.execute_segment(program.id, ctx, {}, txn, fuel);
    if (available == 100) {
      LEANAT_CHECK(result && result.value().kind == SegmentResult::Kind::Returned &&
                   result.value().values == std::vector<Value>{Value{std::uint64_t{42}}} &&
                   result.value().fuel_used == 2 && fuel.remaining == 98 && txn.commit());
    } else {
      LEANAT_CHECK(!result && result.error().code == ErrorCode::FuelExhausted &&
                   fuel.remaining == 0 && !txn.is_open());
    }
  }
  auto lowered_cap = p;
  lowered_cap.programs.front().instruction_fuel = 1;
  auto low = valid(std::move(lowered_cap));
  Interpreter capped(low);
  EventTxn txn({}, ctx);
  FuelCounter fuel{100};
  auto rejected = capped.execute_segment(program.id, ctx, {}, txn, fuel);
  LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::FuelExhausted &&
               fuel.remaining == 99 && !txn.is_open());
  std::cout << "Lean v5 cross-codec: canonical bytes, caller/until-release, result42, "
               "exact fuel2, caller fuel1 refusal, policy fuel1 refusal passed\n";
}
static void shared_namespace(const Project &p) {
  LEANAT_CHECK(p.schema_major == 5 && p.programs.size() == 2 && p.instances.size() == 1 &&
               p.system_metadata && p.instances.front().handlers.size() == 2);
  const auto &bindings = p.instances.front().handlers;
  LEANAT_CHECK(bindings[0].local_id == 0 && bindings[1].local_id == 0 &&
               bindings[0].context != bindings[1].context);
  auto checked = valid(p);
  Interpreter vm(checked);
  for (const auto &h : bindings) {
    const auto program = std::find_if(p.programs.begin(), p.programs.end(),
        [&](const Program &pr) { return pr.id == h.program_id; });
    LEANAT_CHECK(program != p.programs.end());
    bool process = h.context == ContextKind::Process;
    LEANAT_CHECK(process || h.context == ContextKind::Timed);
    LEANAT_CHECK(program->instruction_fuel == (process ? 2 : 0) &&
                 program->owner_policy == (process ? "caller" : "") &&
                 program->result_lifetime_policy == (process ? "until-release" : ""));
    ExecutionContext ctx;
    ctx.kind = h.context;
    ctx.domain = DomainId{p.system_metadata->runtime_domain};
    ctx.instance = InstanceId{p.instances.front().id};
    EventTxn txn({}, ctx);
    FuelCounter fuel{100};
    auto result = vm.execute_segment(h.program_id, ctx, {}, txn, fuel);
    LEANAT_CHECK(result && result.value().kind == SegmentResult::Kind::Returned &&
        result.value().values == std::vector<Value>{Value{std::uint64_t(process ? 42 : 7)}} &&
        result.value().fuel_used == 2 && fuel.remaining == 98 && txn.commit());
  }
  auto duplicate = p;
  auto copy = p.programs.front();
  copy.id = 100;
  duplicate.programs.push_back(copy);
  auto binding = bindings.front();
  binding.program_id = copy.id;
  duplicate.instances.front().handlers.push_back(binding);
  LEANAT_CHECK(!validate(duplicate));
}
static void native_shared_namespace() {
  auto p = writing(5, 2);
  p.state_types.clear();
  p.initial_state.clear();
  auto &process = p.programs.front();
  process.effect_mask = 0;
  process.blocks.front().instructions.resize(1);
  auto timed = process;
  timed.id = 1;
  timed.context = ContextKind::Timed;
  timed.instruction_fuel = 0;
  timed.owner_policy.clear();
  timed.result_lifetime_policy.clear();
  timed.blocks.front().instructions.front().value = Value{std::uint64_t{7}};
  p.programs.push_back(timed);
  metadata(p);
  p.instances.front().handlers[1].local_id = 0;
  shared_namespace(p);
}
static void lean_shared_fixture(const char *path) {
  std::ifstream file(path, std::ios::binary);
  LEANAT_CHECK(file);
  Bytes bytes;
  char ch;
  while (file.get(ch)) {
    LEANAT_CHECK(bytes.size() < LoadPolicy{}.max_file_bytes);
    bytes.push_back(static_cast<unsigned char>(ch));
  }
  LEANAT_CHECK(file.eof());
  auto loaded = load_descriptor(bytes);
  LEANAT_CHECK(loaded);
  auto encoded = serialize(loaded.value());
  LEANAT_CHECK(encoded && encoded.value() == bytes);
  shared_namespace(loaded.value().get());
  std::cout << "Lean shared local ID cross-codec: timed7/process42, separate policies passed\n";
}
int main(int argc, char **argv) {
  LEANAT_CHECK(argc >= 1 && argc <= 3);
  schema();
  budgets();
  flat_metadata();
  resume_policy();
  fin_and_indices();
  failed_trace_prefix();
  native_shared_namespace();
  if (argc >= 2)
    lean_fixture(argv[1]);
  if (argc == 3)
    lean_shared_fixture(argv[2]);
}
