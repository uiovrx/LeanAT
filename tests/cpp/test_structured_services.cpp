#include "leanat/structured_services.hpp"
#include "test_support.hpp"
#include <type_traits>
using namespace leanat;
struct StructuredHost : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return fail(ErrorCode::ExternalFailure, "unexpected transport");
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
int main() {
  static_assert(!std::is_copy_constructible<TaskPool>::value);
  static_assert(!std::is_copy_constructible<CancelScopeStore>::value);
  static_assert(!std::is_copy_constructible<WaitGroupStore>::value);
  DomainId domain{1};
  StructuredHost host;
  RuntimeConfig cfg;
  cfg.domain = domain;
  cfg.instance = InstanceId{1};
  cfg.descriptor_identity = "structured-vm";
  cfg.result_capacity = 16;
  cfg.consumer_capacity = 32;
  cfg.pin_capacity = 16;
  cfg.frame_capacity = 4;
  cfg.wait_capacity = 8;
  Runtime runtime(cfg, host);
  LEANAT_CHECK(runtime.start({domain, "structured-vm", {}, true, true}));
  CancelScopeStore scopes(domain, 1, 4, 16, 32);
  TaskPoolDesc desc;
  desc.max_instances = 1;
  desc.task_capacity = 4;
  desc.result_capacity = 4;
  TaskPool tasks(domain, 1, desc, runtime.results(), &scopes);
  WaitGroupStore groups(domain, 1, 4, 4, 4096, runtime.results());
  StructuredServices services(runtime, scopes, tasks, groups);
  CoreRuntimeBackend backend(runtime);
  exec::Project project;
  project.profile = "AT-Ext-1.1-draft";
  project.schema_major = 2;
  auto ht = [](HandleKind k) {
    return exec::Type{exec::TypeKind::Handle, static_cast<std::uint64_t>(k), {}, {}};
  };
  project.types = {{exec::TypeKind::Unit, 0, {}, {}},
                   {exec::TypeKind::Bits, 64, {}, {}},
                   ht(HandleKind::Process),
                   ht(HandleKind::Scope),
                   ht(HandleKind::Task),
                   ht(HandleKind::Wait),
                   {exec::TypeKind::Record, 0, {}, {}},
                   {exec::TypeKind::Record, 0, {1, 1}, {}},
                   {exec::TypeKind::Record, 0, {1, 7}, {}},
                   {exec::TypeKind::Record, 0, {1, 8}, {}}};
  auto signature = [&](std::uint32_t id, exec::Op op, std::vector<std::uint32_t> inputs,
                       std::uint32_t output) {
    exec::ServiceSignature s;
    s.id = id;
    s.op = op;
    s.input_types = std::move(inputs);
    s.result_types = {output};
    s.context_mask = 2;
    s.effect_mask = exec::required_effect(op);
    s.provider_key = "LeanAT.Structured";
    s.provider_version = "1";
    s.abi_hash[0] = static_cast<std::uint8_t>(id);
    project.services.push_back(s);
    LEANAT_CHECK(services.bind(backend, s, ProgramId{1}));
    return s;
  };
  signature(100, exec::Op::SpawnProcess, {3, 6}, 4);
  signature(101, exec::Op::WaitGroupNew, {2, 3, 1, 1, 1, 1, 1}, 5);
  signature(102, exec::Op::WaitArm, {5, 1, 1, 4, 3}, 0);
  signature(103, exec::Op::TaskResultRelease, {4, 3}, 0);
  signature(104, exec::Op::WaitGroupRelease, {5, 3}, 0);
  LEANAT_CHECK(backend.freeze());
  exec::Program parent;
  parent.id = 0;
  parent.context = ContextKind::Process;
  parent.input_types = {2, 3};
  parent.result_types = {1};
  for (auto &s : project.services)
    parent.effect_mask |= s.effect_mask;
  parent.frame = {{3, 0, 8}, {4, 64, 8}, {5, 128, 8}};
  parent.frame_bytes = 192;
  exec::Block entry;
  entry.id = 0;
  entry.parameters = {{0, 2}, {1, 3}};
  auto constant = [&](std::uint32_t reg, std::uint32_t type, Value value) {
    exec::Instruction i;
    i.op = exec::Op::Const;
    i.dest = exec::Reg{reg, type};
    i.value = std::move(value);
    entry.instructions.push_back(i);
  };
  auto call = [&](exec::Block &b, std::uint32_t service, exec::Op op, std::vector<exec::Reg> args,
                  exec::Reg out) {
    exec::Instruction i;
    i.op = op;
    i.immediate = service;
    i.args = std::move(args);
    i.dest = out;
    b.instructions.push_back(i);
  };
  constant(2, 6, Value{Value::Array{}});
  call(entry, 100, exec::Op::SpawnProcess, {{1, 3}, {2, 6}}, {3, 4});
  constant(4, 1, Value{std::uint64_t{0}});
  constant(5, 1, Value{std::uint64_t{1}});
  constant(6, 1, Value{std::uint64_t{0}});
  constant(7, 1, Value{std::uint64_t{8}});
  constant(8, 1, Value{std::uint64_t{4096}});
  call(entry, 101, exec::Op::WaitGroupNew, {{0, 2}, {1, 3}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 1}},
       {9, 5});
  constant(10, 1, Value{std::uint64_t{0}});
  constant(11, 1, Value{std::uint64_t{0}});
  call(entry, 102, exec::Op::WaitArm, {{9, 5}, {10, 1}, {11, 1}, {3, 4}, {1, 3}}, {12, 0});
  entry.terminator.kind = exec::TermKind::Suspend;
  entry.terminator.value = {9, 5};
  entry.terminator.yes.target = 1;
  entry.terminator.values = {{1, 3}, {3, 4}, {9, 5}};
  exec::Block resume;
  resume.id = 1;
  resume.parameters = {{20, 8}, {21, 3}, {22, 4}, {23, 5}};
  auto field = [&](exec::Reg in, exec::Reg out, std::uint32_t n) {
    exec::Instruction i;
    i.op = exec::Op::GetField;
    i.args = {in};
    i.dest = out;
    i.immediate = n;
    resume.instructions.push_back(i);
  };
  field({20, 8}, {25, 7}, 1);
  field({25, 7}, {26, 1}, 1);
  call(resume, 103, exec::Op::TaskResultRelease, {{22, 4}, {21, 3}}, {27, 0});
  call(resume, 104, exec::Op::WaitGroupRelease, {{23, 5}, {21, 3}}, {28, 0});
  resume.terminator.values = {{26, 1}};
  parent.blocks = {entry, resume};
  project.programs.push_back(parent);
  exec::Program child;
  child.id = 1;
  child.context = ContextKind::Process;
  child.result_types = {1};
  exec::Block work;
  work.id = 0;
  exec::Instruction answer;
  answer.op = exec::Op::Const;
  answer.dest = exec::Reg{0, 1};
  answer.value = Value{std::uint64_t{42}};
  work.instructions.push_back(answer);
  work.terminator.values = {{0, 1}};
  child.blocks.push_back(work);
  project.programs.push_back(child);
  auto valid = exec::validate(project);
  if (!valid) {
    std::cerr << valid.error().message << '\n';
  }
  LEANAT_CHECK(valid);
  exec::Interpreter vm(valid.value(), {}, &backend);
  ExecutionContext ctx;
  ctx.kind = ContextKind::Process;
  ctx.domain = domain;
  ctx.instance = InstanceId{1};
  ctx.owner = 1;
  SegmentBudget budget{512, 64, 8 * 1024 * 1024, 64};
  auto controller = runtime.processes().create(ProgramId{0}, 1);
  LEANAT_CHECK(controller);
  {
    EventTxn bootstrap(budget, ctx);
    LEANAT_CHECK(runtime.processes().begin(controller.value(), bootstrap));
    LEANAT_CHECK(runtime.commit_segment(bootstrap));
    const auto before = runtime.processes().inspect(controller.value());
    LEANAT_CHECK(before && before.value().instance_bound && before.value().instance == ctx.instance);
    auto stale_scope = scopes.root();
    ++stale_scope.generation;
    EventTxn rejected(budget, ctx);
    auto result = backend.invoke(project.services[0], {Value{stale_scope}, Value{Value::Array{}}}, ctx, rejected);
    LEANAT_CHECK(!result && result.error().code == ErrorCode::StaleHandle);
    LEANAT_CHECK(rejected.discard());
    const auto after = runtime.processes().inspect(controller.value());
    LEANAT_CHECK(after && after.value().instance_bound && after.value().instance == before.value().instance &&
                 after.value().state == before.value().state && after.value().suspension_ordinal == before.value().suspension_ordinal);
    LEANAT_CHECK(tasks.active_frames() == 0 && runtime.results().occupied() == 0);
  }
  exec::FuelCounter fuel{10000};
  Handle discarded_wait;
  {
    EventTxn abandoned(budget, ctx);
    auto first = vm.execute_segment(0, ctx, {Value{controller.value()}, Value{scopes.root()}},
                                    abandoned, fuel);
    if (!first)
      std::cerr << first.error().message << '\n';
    LEANAT_CHECK(first && first.value().kind == exec::SegmentResult::Kind::Suspended);
    discarded_wait = *first.value().suspended_wait;
    LEANAT_CHECK(tasks.active_frames() == 0);
    LEANAT_CHECK(abandoned.discard());
    LEANAT_CHECK(runtime.results().occupied() == 0);
  }
  EventTxn start(budget, ctx);
  auto suspended =
      vm.execute_segment(0, ctx, {Value{controller.value()}, Value{scopes.root()}}, start, fuel);
  if (!suspended)
    std::cerr << suspended.error().message << '\n';
  LEANAT_CHECK(suspended && suspended.value().kind == exec::SegmentResult::Kind::Suspended);
  LEANAT_CHECK(*suspended.value().suspended_wait != discarded_wait);
  LEANAT_CHECK(runtime.commit_segment(start));
  LEANAT_CHECK(tasks.active_frames() == 1);
  LEANAT_CHECK(!runtime.processes().suspension_for_wait(discarded_wait));
  std::uint64_t final_value = 0;
  int resumes = 0;
  runtime.set_batch_epilogue(
      [&](BatchId b, ReadyKey k, Runtime &) { return services.resolve(b, k); });
  runtime.set_handler([&](const QueuedEvent &e, Runtime &r) -> Expected<void> {
    auto payload = std::get_if<Value::Array>(&e.event.value.data);
    if (!payload || payload->size() != 2)
      return fail(ErrorCode::TypeMismatch, "task start event");
    auto task = std::get<Handle>((*payload)[0].data),
         process = std::get<Handle>((*payload)[1].data);
    auto childctx = ctx;
    childctx.owner = process.owner;
    childctx.ready = {e.event.key.time, e.event.key.turn};
    EventTxn tx(budget, childctx);
    auto start = services.start_task(task, childctx, tx);
    if (!start)
      return start.error();
    LEANAT_CHECK(start.value().process == process);
    auto ran = vm.execute_segment(start.value().program.value, childctx, start.value().arguments,
                                  tx, fuel);
    if (!ran)
      return ran.error();
    auto completed = services.complete(task, {TaskOutcome::Kind::Success, ran.value().values[0]},
                                       childctx.ready, tx);
    if (!completed)
      return completed.error();
    auto committed = r.commit_segment(tx);
    if (!committed)
      return committed.error();
    return {};
  });
  runtime.set_resume_handler(
      [&](const SuspensionToken &token, ReadyKey key, Runtime &r) -> Expected<void> {
        ++resumes;
        auto resumectx = ctx;
        resumectx.ready = key;
        EventTxn tx(budget, resumectx);
        auto resumed = vm.resume_segment(token, resumectx, tx, fuel);
        if (!resumed)
          return resumed.error();
        final_value = std::get<std::uint64_t>(resumed.value().values[0].data);
        auto done = r.processes().complete(controller.value(), tx);
        if (!done)
          return done.error();
        auto committed = r.commit_segment(tx);
        if (!committed)
          return committed.error();
        return {};
      });
  for (unsigned i = 0; i < 16 && runtime.next_wakeup(); ++i) {
    auto pumped = runtime.pump_batch(Tick{0}, 8);
    if (!pumped)
      std::cerr << pumped.error().message << '\n';
    LEANAT_CHECK(pumped);
  }
  LEANAT_CHECK(final_value == 42 && resumes == 1 && !runtime.next_wakeup());
  LEANAT_CHECK(tasks.active_frames() == 0 && runtime.results().occupied() == 0 &&
               runtime.processes().frame_count() == 0);
  // Queue entries own results/arguments, but allocate no execution frame until promoted.
  StructuredHost queue_host;
  auto queue_cfg = cfg;
  queue_cfg.descriptor_identity = "structured-queue";
  queue_cfg.frame_capacity = 1;
  Runtime qr(queue_cfg, queue_host);
  LEANAT_CHECK(qr.start({domain, "structured-queue", {}, true, true}));
  CancelScopeStore qs(domain, 1, 4, 16, 32);
  auto qdesc = desc;
  qdesc.overflow = TaskOverflow::Queue;
  qdesc.queue_depth = 2;
  TaskPool qp(domain, 1, qdesc, qr.results(), &qs);
  WaitGroupStore qw(domain, 1, 2, 2, 4096, qr.results());
  StructuredServices qservices(qr, qs, qp, qw);
  CoreRuntimeBackend qb(qr);
  auto qsig = [&](std::uint32_t id, exec::Op op, std::vector<std::uint32_t> in, std::uint32_t out) {
    exec::ServiceSignature s;
    s.id = id;
    s.op = op;
    s.input_types = std::move(in);
    s.result_types = {out};
    s.context_mask = 2;
    s.effect_mask = exec::required_effect(op);
    s.provider_key = "LeanAT.QueueFixture";
    s.provider_version = "1";
    LEANAT_CHECK(qservices.bind(qb, s, ProgramId{1}));
    return s;
  };
  auto submit = qsig(200, exec::Op::SubmitTask, {3, 6}, 10),
       cancel = qsig(201, exec::Op::CancelTask, {4, 11}, 0),
       release = qsig(202, exec::Op::TaskResultRelease, {4, 3}, 0);
  LEANAT_CHECK(qb.freeze());
  auto unwrap = [](const std::vector<Value> &values) {
    auto v = std::get<Value::Array>(values[0].data);
    LEANAT_CHECK(std::get<std::uint64_t>(v[0].data) == 0);
    return std::get<Handle>(std::get<Value::Array>(v[1].data)[0].data);
  };
  EventTxn queueing(budget, ctx);
  std::vector<Handle> qt;
  for (unsigned i = 0; i < 3; ++i) {
    auto admitted = qb.invoke(submit, {Value{qs.root()}, Value{Value::Array{}}}, ctx, queueing);
    LEANAT_CHECK(admitted);
    qt.push_back(unwrap(admitted.value()));
  }
  LEANAT_CHECK(qr.processes().frame_count() == 0);
  LEANAT_CHECK(qr.commit_segment(queueing));
  LEANAT_CHECK(qr.processes().frame_count() == 1 && qp.active_frames() == 1);
  LEANAT_CHECK(qp.state(qt[1]).value() == TaskState::Queued);
  LEANAT_CHECK(!qservices.execution_process(qt[1]));
  EventTxn cancel_first(budget, ctx);
  LEANAT_CHECK(qb.invoke(cancel, {Value{qt[0]}, Value{Bytes{1}}}, ctx, cancel_first));
  LEANAT_CHECK(qr.commit_segment(cancel_first));
  LEANAT_CHECK(qr.processes().frame_count() == 1 && qservices.execution_process(qt[1]));
  EventTxn cancel_last(budget, ctx);
  LEANAT_CHECK(qb.invoke(cancel, {Value{qt[2]}, Value{Bytes{2}}}, ctx, cancel_last));
  LEANAT_CHECK(qr.commit_segment(cancel_last));
  LEANAT_CHECK(qr.processes().frame_count() == 1);
  unsigned started = 0;
  Handle actual_started;
  qr.set_handler([&](const QueuedEvent &e, Runtime &r) -> Expected<void> {
    auto a = std::get<Value::Array>(e.event.value.data);
    auto task = std::get<Handle>(a[0].data);
    auto current = ctx;
    current.ready = {e.event.key.time, e.event.key.turn};
    EventTxn tx(budget, current);
    auto start = qservices.start_task(task, current, tx);
    if (!start)
      return start.error();
    ++started;
    actual_started = task;
    auto done = qservices.complete(task, {TaskOutcome::Kind::Success, Value{std::uint64_t{7}}},
                                   current.ready, tx);
    if (!done)
      return done.error();
    auto committed = r.commit_segment(tx);
    if (!committed)
      return committed.error();
    return {};
  });
  for (unsigned i = 0; i < 16 && qr.next_wakeup(); ++i)
    LEANAT_CHECK(qr.pump_batch(Tick{0}, 8));
  LEANAT_CHECK(started == 1 && actual_started == qt[1] && qr.processes().frame_count() == 0);
  EventTxn release_all(budget, ctx);
  for (auto task : qt)
    LEANAT_CHECK(qb.invoke(release, {Value{task}, Value{qs.root()}}, ctx, release_all));
  LEANAT_CHECK(qr.commit_segment(release_all));
  LEANAT_CHECK(qr.results().occupied() == 0 && qp.active_frames() == 0);
  // Cancelling an ancestor executes every descendant plan, including queued
  // tasks and the producer/consumer capabilities independently owned there.
  auto ancestor = qs.create(qs.root());
  LEANAT_CHECK(ancestor);
  auto middle = qs.create(ancestor.value());
  LEANAT_CHECK(middle);
  auto leaf = qs.create(middle.value());
  LEANAT_CHECK(leaf);
  CoreRuntimeBackend nested_backend(qr);
  auto nested_submit = submit;
  nested_submit.id = 210;
  auto nested_cancel = cancel;
  nested_cancel.id = 211;
  nested_cancel.op = exec::Op::ScopeCancel;
  nested_cancel.input_types = {3, 11};
  nested_cancel.effect_mask = exec::required_effect(nested_cancel.op);
  LEANAT_CHECK(qservices.bind(nested_backend, nested_submit, ProgramId{1}));
  LEANAT_CHECK(qservices.bind(nested_backend, nested_cancel));
  LEANAT_CHECK(nested_backend.freeze());
  EventTxn nested_admission(budget, ctx);
  LEANAT_CHECK(nested_backend.invoke(nested_submit, {Value{middle.value()}, Value{Value::Array{}}},
                                     ctx, nested_admission));
  LEANAT_CHECK(nested_backend.invoke(nested_submit, {Value{leaf.value()}, Value{Value::Array{}}},
                                     ctx, nested_admission));
  LEANAT_CHECK(qr.commit_segment(nested_admission));
  LEANAT_CHECK(qp.active_frames() == 1 && qr.results().occupied() == 2);
  {
    EventTxn abandoned_cancel(budget, ctx);
    LEANAT_CHECK(nested_backend.invoke(nested_cancel, {Value{ancestor.value()}, Value{Bytes{5}}},
                                       ctx, abandoned_cancel));
    LEANAT_CHECK(abandoned_cancel.discard());
    LEANAT_CHECK(qs.state(leaf.value()).value() == ScopeState::Open && qp.active_frames() == 1);
  }
  EventTxn cancel_tree(budget, ctx);
  LEANAT_CHECK(nested_backend.invoke(nested_cancel, {Value{ancestor.value()}, Value{Bytes{5}}}, ctx,
                                     cancel_tree));
  LEANAT_CHECK(qr.commit_segment(cancel_tree));
  LEANAT_CHECK(qs.state(ancestor.value()).value() == ScopeState::Cancelled);
  LEANAT_CHECK(qs.state(middle.value()).value() == ScopeState::Cancelled);
  LEANAT_CHECK(qs.state(leaf.value()).value() == ScopeState::Cancelled);
  LEANAT_CHECK(!qs.close(ancestor.value()) && !qs.close(middle.value()));
  LEANAT_CHECK(qp.active_frames() == 0 && qr.processes().frame_count() == 0 &&
               qr.results().occupied() == 0);
  LEANAT_CHECK(qs.close(leaf.value()) && qs.close(middle.value()) && qs.close(ancestor.value()));
  LEANAT_CHECK(!qs.state(leaf.value()));
  // Explicit v5 caller ownership is the supplied scope capability, not the
  // caller's unrelated execution-owner integer. Results outlive execution.
  {
    auto policy_project = project;
    policy_project.schema_major = 5;
    policy_project.programs[1].instruction_fuel = 100;
    policy_project.programs[1].owner_policy = "caller";
    policy_project.programs[1].result_lifetime_policy = "until-release";
    CoreRuntimeBackend policy_backend(runtime);
    auto spawn = project.services[0];
    auto read = project.services[3];
    read.id = 301;
    read.op = exec::Op::TaskResultGet;
    read.effect_mask = exec::required_effect(read.op);
    read.result_types = {7};
    auto bad_policy = policy_project;
    bad_policy.programs[1].owner_policy = "detached";
    LEANAT_CHECK(!services.bind(policy_backend, spawn, bad_policy, ProgramId{1}));
    bad_policy = policy_project;
    bad_policy.programs[1].result_lifetime_policy = "frame";
    LEANAT_CHECK(!services.bind(policy_backend, spawn, bad_policy, ProgramId{1}));
    LEANAT_CHECK(services.bind(policy_backend, spawn, policy_project, ProgramId{1}));
    LEANAT_CHECK(services.bind(policy_backend, read));
    LEANAT_CHECK(services.bind(policy_backend, project.services[3]));
    LEANAT_CHECK(policy_backend.freeze());
    auto owner_scope = scopes.create(scopes.root());
    LEANAT_CHECK(owner_scope);
    auto caller = ctx;
    caller.owner = 99;
    auto invoke_spawn = [&](EventTxn &tx, Handle scope) {
      return policy_backend.invoke(spawn, {Value{scope}, Value{Value::Array{}}}, caller, tx);
    };
    {
      EventTxn abandoned(budget, caller);
      LEANAT_CHECK(invoke_spawn(abandoned, owner_scope.value()));
      LEANAT_CHECK(abandoned.discard());
      LEANAT_CHECK(runtime.results().occupied() == 0 && tasks.active_frames() == 0);
    }
    EventTxn admission(budget, caller);
    auto spawned = invoke_spawn(admission, owner_scope.value());
    LEANAT_CHECK(spawned);
    auto task = std::get<Handle>(spawned.value()[0].data);
    LEANAT_CHECK(task.owner == owner_scope.value().slot + 1ULL && task.owner != caller.owner);
    LEANAT_CHECK(runtime.commit_segment(admission));
    auto child_context = ctx;
    child_context.owner = task.owner;
    EventTxn completion(budget, child_context);
    LEANAT_CHECK(services.complete(task, {TaskOutcome::Kind::Success, Value{std::uint64_t{73}}},
                                   child_context.ready, completion));
    LEANAT_CHECK(runtime.commit_segment(completion));
    LEANAT_CHECK(runtime.processes().frame_count() == 0 && runtime.results().occupied() == 1);
    EventTxn reading(budget, caller);
    LEANAT_CHECK(
        !policy_backend.invoke(read, {Value{task}, Value{scopes.root()}}, caller, reading));
    for (unsigned i = 0; i < 2; ++i) {
      auto result =
          policy_backend.invoke(read, {Value{task}, Value{owner_scope.value()}}, caller, reading);
      LEANAT_CHECK(result);
      auto outcome = std::get<Value::Array>(result.value()[0].data);
      LEANAT_CHECK(std::get<std::uint64_t>(outcome[1].data) == 73);
    }
    LEANAT_CHECK(runtime.commit_segment(reading));
    LEANAT_CHECK(runtime.results().occupied() == 1);
    EventTxn releasing(budget, caller);
    LEANAT_CHECK(policy_backend.invoke(
        project.services[3], {Value{task}, Value{owner_scope.value()}}, caller, releasing));
    LEANAT_CHECK(runtime.commit_segment(releasing));
    LEANAT_CHECK(runtime.results().occupied() == 0 && scopes.close(owner_scope.value()));
    EventTxn stale_scope(budget, caller);
    LEANAT_CHECK(!invoke_spawn(stale_scope, owner_scope.value()));
    LEANAT_CHECK(stale_scope.discard());
  }
  // Slot admission validates the live caller capability before reserving resources,
  // including process frames created in this same uncommitted segment.
  {
    StructuredHost slot_host;
    Runtime sr(cfg, slot_host);
    LEANAT_CHECK(sr.start({domain, "structured-vm", {}, true, true}));
    CancelScopeStore ss(domain, 8, 4, 16, 32);
    auto slot_desc = desc;
    slot_desc.overflow = TaskOverflow::AwaitSlot;
    slot_desc.waiter_limit = 4;
    TaskPool st(domain, 8, slot_desc, sr.results(), &ss);
    WaitGroupStore sg(domain, 8, 4, 4, 4096, sr.results());
    StructuredServices adapter(sr, ss, st, sg);
    CoreRuntimeBackend sb(sr);
    exec::ServiceSignature slot;
    slot.id = 901;
    slot.op = exec::Op::RequestTaskSlot;
    slot.input_types = {2, 3};
    slot.result_types = {0};
    slot.context_mask = 2;
    slot.effect_mask = exec::required_effect(slot.op);
    slot.provider_key = "LeanAT.Structured";
    slot.provider_version = "1";
    slot.abi_hash[0] = 91;
    LEANAT_CHECK(adapter.bind(sb, slot, ProgramId{1}) && sb.freeze());
    auto live = sr.processes().create(ProgramId{0}, ctx.owner);
    LEANAT_CHECK(live && sr.processes().begin(live.value()));
    for (unsigned variant : {0u, 1u}) {
      auto forged = live.value();
      if (variant == 0) ++forged.generation;
      else ++forged.owner;
      EventTxn invalid(budget, ctx);
      auto result = sb.invoke(slot, {Value{forged}, Value{ss.root()}}, ctx, invalid);
      LEANAT_CHECK(!result && result.error().code == ErrorCode::StaleHandle);
      LEANAT_CHECK(invalid.discard());
      LEANAT_CHECK(st.snapshot().value().tickets.empty() && sr.results().occupied() == 0);
    }
    auto other = ctx;
    ++other.owner;
    EventTxn wrong_owner(budget, other);
    auto rejected = sb.invoke(slot, {Value{live.value()}, Value{ss.root()}}, other, wrong_owner);
    LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::WrongOwner);
    LEANAT_CHECK(wrong_owner.discard());
    EventTxn prepared(budget, ctx);
    auto fresh = sr.processes().create(ProgramId{0}, ctx.owner, prepared);
    LEANAT_CHECK(fresh);
    auto granted = sb.invoke(slot, {Value{fresh.value()}, Value{ss.root()}}, ctx, prepared);
    LEANAT_CHECK(granted);
    LEANAT_CHECK(prepared.discard());
    LEANAT_CHECK(!sr.processes().inspect(fresh.value()) && st.snapshot().value().tickets.empty());
    EventTxn valid_slot(budget, ctx);
    LEANAT_CHECK(sb.invoke(slot, {Value{live.value()}, Value{ss.root()}}, ctx, valid_slot));
    LEANAT_CHECK(sr.commit_segment(valid_slot));
    LEANAT_CHECK(st.snapshot().value().tickets.size() == 1 &&
                 st.snapshot().value().grants == 1);
  }
  // Scope ownership covers both the wait control and its primary consumer.
  // Cancellation before readiness and after a queued resume uses the same atomic cleanup.
  for (unsigned phase : {0u, 1u, 2u, 3u}) {
    StructuredHost wait_host;
    Runtime wr(cfg, wait_host);
    LEANAT_CHECK(wr.start({domain, "structured-vm", {}, true, true}));
    CancelScopeStore ws(domain, 7, 4, 16, 32);
    TaskPool wt(domain, 7, desc, wr.results(), &ws);
    WaitGroupStore wg(domain, 7, 4, 4, 4096, wr.results());
    StructuredServices adapter(wr, ws, wt, wg);
    CoreRuntimeBackend wb(wr);
    auto make = project.services[1];
    auto arm = project.services[2];
    auto cancel_scope = nested_cancel;
    LEANAT_CHECK(adapter.bind(wb, make));
    LEANAT_CHECK(adapter.bind(wb, arm));
    LEANAT_CHECK(adapter.bind(wb, project.services[4]));
    LEANAT_CHECK(adapter.bind(wb, cancel_scope));
    LEANAT_CHECK(wb.freeze());
    auto outer = ws.create(ws.root());
    LEANAT_CHECK(outer);
    auto inner = ws.create(outer.value());
    LEANAT_CHECK(inner);
    auto source_task = wt.try_spawn({}, inner.value(), ctx.kind);
    LEANAT_CHECK(source_task);
    auto process = wr.processes().create(ProgramId{0}, 1);
    LEANAT_CHECK(process && wr.processes().begin(process.value()));
    EventTxn admission(budget, ctx);
    auto created = wb.invoke(make,
                             {Value{process.value()}, Value{inner.value()}, Value{std::uint64_t{0}},
                              Value{std::uint64_t{1}}, Value{std::uint64_t{0}},
                              Value{std::uint64_t{8}}, Value{std::uint64_t{4096}}},
                             ctx, admission);
    LEANAT_CHECK(created);
    auto wait = std::get<Handle>(created.value()[0].data);
    LEANAT_CHECK(wb.invoke(arm,
                           {Value{wait}, Value{std::uint64_t{0}}, Value{std::uint64_t{0}},
                            Value{source_task.value()}, Value{inner.value()}},
                           ctx, admission));
    std::optional<SuspensionToken> token;
    if (phase) {
      auto suspended_wait = wr.processes().suspend_registered(wait, {BlockId{1}, {}}, admission);
      LEANAT_CHECK(suspended_wait);
      token = suspended_wait.value();
    }
    if (!phase) {
      auto cancelled =
          wb.invoke(cancel_scope, {Value{outer.value()}, Value{Bytes{9}}}, ctx, admission);
      LEANAT_CHECK(cancelled);
      LEANAT_CHECK(wr.commit_segment(admission));
      LEANAT_CHECK(wr.processes().wait_count() == 0 && wr.results().occupied() == 0);
      LEANAT_CHECK(wr.processes().inspect(process.value()).value().state ==
                   ProcessState::Executing);
      LEANAT_CHECK(wr.processes().complete(process.value()));
      LEANAT_CHECK(ws.close(inner.value()) && ws.close(outer.value()));
      continue;
    }
    LEANAT_CHECK(wr.commit_segment(admission));
    LEANAT_CHECK(!ws.close(inner.value()));
    if (phase >= 2) {
      LEANAT_CHECK(wt.complete(source_task.value(),
                               {TaskOutcome::Kind::Success, Value{std::uint64_t{42}}}, ctx.ready));
      LEANAT_CHECK(adapter.resolve(BatchId{1}, ctx.ready));
      LEANAT_CHECK(wr.processes().inspect(process.value()).value().state ==
                   ProcessState::ResumeQueued);
    }
    if (phase == 1) {
      EventTxn premature(budget, ctx);
      auto result =
          wb.invoke(project.services[4], {Value{wait}, Value{inner.value()}}, ctx, premature);
      LEANAT_CHECK(!result && result.error().code == ErrorCode::NotReady);
      LEANAT_CHECK(premature.discard());
    }
    if (phase == 3) {
      EventTxn release_ready(budget, ctx);
      LEANAT_CHECK(
          wb.invoke(project.services[4], {Value{wait}, Value{inner.value()}}, ctx, release_ready));
      LEANAT_CHECK(wr.commit_segment(release_ready));
      LEANAT_CHECK(wr.processes().inspect(process.value()).value().state ==
                   ProcessState::ResumeQueued);
      auto resumed = wr.processes().take_resume(*token);
      LEANAT_CHECK(resumed && wr.processes().complete(process.value()));
      EventTxn cleanup(budget, ctx);
      LEANAT_CHECK(wb.invoke(cancel_scope, {Value{outer.value()}, Value{Bytes{9}}}, ctx, cleanup));
      LEANAT_CHECK(wr.commit_segment(cleanup));
      LEANAT_CHECK(wr.results().occupied() == 0 && ws.close(inner.value()) &&
                   ws.close(outer.value()));
      continue;
    }
    auto cancel_wait = [&](EventTxn &tx) {
      return wb.invoke(cancel_scope, {Value{outer.value()}, Value{Bytes{9}}}, ctx, tx);
    };
    {
      EventTxn rollback(budget, ctx);
      auto cancelled = cancel_wait(rollback);
      if (!cancelled)
        std::cerr << cancelled.error().message << '\n';
      LEANAT_CHECK(cancelled && rollback.discard());
      LEANAT_CHECK(ws.state(inner.value()).value() == ScopeState::Open);
      LEANAT_CHECK(wr.processes().wait_count() == 1 && wr.results().occupied() == 2);
      LEANAT_CHECK(adapter.snapshot().value().waits.size() == 1);
    }
    EventTxn cancellation(budget, ctx);
    LEANAT_CHECK(cancel_wait(cancellation) && wr.commit_segment(cancellation));
    LEANAT_CHECK(wr.processes().wait_count() == 0 &&
                 wr.processes().frame_count() == (phase ? 0 : 1));
    LEANAT_CHECK(wr.results().occupied() == 0 && adapter.snapshot().value().waits.empty());
    if (token)
      LEANAT_CHECK(!wr.processes().take_resume(*token));
    else {
      LEANAT_CHECK(wr.processes().inspect(process.value()).value().state ==
                   ProcessState::Executing);
      LEANAT_CHECK(wr.processes().complete(process.value()));
    }
    LEANAT_CHECK(ws.close(inner.value()) && ws.close(outer.value()));
  }
}
