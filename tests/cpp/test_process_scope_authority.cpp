#include "leanat/structured_services.hpp"
#include "test_support.hpp"
#include <type_traits>
using namespace leanat;
namespace {
struct Host : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return fail(ErrorCode::ExternalFailure, "unexpected transport");
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
// A private controlled method must not become a callable public escape hatch.
template <class T, class = void> struct public_control : std::false_type {};
template <class T> struct public_control<T, std::void_t<decltype(
  std::declval<T &>().create_controlled(ProgramId{}, 2, std::declval<EventTxn &>()))>>
    : std::true_type {};
}
int main() {
  static_assert(!public_control<ProcessStore>::value, "scoped authority must remain private");
  Host host;
  RuntimeConfig cfg;
  cfg.domain = DomainId{1}; cfg.instance = InstanceId{3};
  cfg.descriptor_identity = "process-scope-authority";
  cfg.frame_capacity = 8; cfg.wait_capacity = 8;
  Runtime runtime(cfg, host);
  LEANAT_CHECK(runtime.start({cfg.domain, cfg.descriptor_identity, {}, true, true}));
  CancelScopeStore scopes(cfg.domain, 1, 8, 32, 64);
  TaskPoolDesc desc;
  desc.max_instances = 8; desc.task_capacity = 8; desc.result_capacity = 8;
  TaskPool tasks(cfg.domain, 1, desc, runtime.results(), &scopes);
  WaitGroupStore groups(cfg.domain, 1, 8, 8, 4096, runtime.results());
  StructuredServices services(runtime, scopes, tasks, groups);
  CoreRuntimeBackend backend(runtime);
  auto signature = [&](std::uint32_t id, exec::Op op) {
    exec::ServiceSignature s;
    s.id = id; s.op = op; s.input_types = {0, 1}; s.result_types = {2};
    s.context_mask = 2; s.effect_mask = exec::required_effect(op);
    s.provider_key = "LeanAT.ScopeAuthority.Test"; s.provider_version = "1";
    LEANAT_CHECK(services.bind(backend, s, ProgramId{9}));
    return s;
  };
  auto spawn = signature(1, exec::Op::SpawnProcess);
  auto cancel = signature(2, exec::Op::ScopeCancel);
  exec::ServiceSignature wait_new;
  wait_new.id = 3; wait_new.op = exec::Op::WaitGroupNew;
  wait_new.input_types = {0, 1, 2, 2, 2, 2, 2}; wait_new.result_types = {3};
  wait_new.context_mask = 2; wait_new.effect_mask = exec::required_effect(wait_new.op);
  wait_new.provider_key = "LeanAT.ScopeAuthority.Wait"; wait_new.provider_version = "1";
  LEANAT_CHECK(services.bind(backend, wait_new));
  LEANAT_CHECK(backend.freeze());
  auto child = scopes.create(scopes.root()), sibling = scopes.create(scopes.root());
  LEANAT_CHECK(child && sibling);
  ExecutionContext ctx;
  ctx.kind = ContextKind::Process; ctx.domain = cfg.domain; ctx.instance = cfg.instance;
  ctx.owner = 1;
  SegmentBudget budget{1024, 64, 16 * 1024 * 1024, 64};
  auto admit = [&](Handle scope, EventTxn &tx) {
    auto r = backend.invoke(spawn, {Value(scope), Value(Value::Array{})}, tx.context(), tx);
    if (!r) std::cerr << r.error().message << '\n';
    LEANAT_CHECK(r);
    return std::get<Handle>(r.value()[0].data);
  };
  // Public process spawn cannot impersonate the child scope owner.
  {
    EventTxn tx(budget, ctx);
    LEANAT_CHECK(!runtime.processes().create(ProgramId{9}, child.value().slot + 1, tx));
    LEANAT_CHECK(tx.discard());
  }
  Handle abandoned;
  {
    EventTxn tx(budget, ctx);
    abandoned = admit(child.value(), tx);
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(runtime.processes().frame_count() == 0);
    LEANAT_CHECK(!services.execution_process(abandoned));
  }
  Handle task, other;
  {
    EventTxn tx(budget, ctx);
    task = admit(child.value(), tx); other = admit(sibling.value(), tx);
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  auto process = services.execution_process(task), other_process = services.execution_process(other);
  LEANAT_CHECK(process && other_process && process.value().owner != ctx.owner);
  LEANAT_CHECK(runtime.processes().frame_count() == 2);
  {
    EventTxn tx(budget, ctx);
    LEANAT_CHECK(!runtime.processes().cancel(process.value(), tx));
    LEANAT_CHECK(tx.discard());
  }
  // Even the scope adapter cannot cancel a process from a different instance.
  {
    auto foreign = ctx; foreign.instance = InstanceId{4};
    EventTxn tx(budget, foreign);
    LEANAT_CHECK(!backend.invoke(cancel, {Value(child.value()), Value(Bytes{1})}, foreign, tx));
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(runtime.processes().inspect(process.value()));
    LEANAT_CHECK(scopes.state(child.value()).value() == ScopeState::Open);
  }
  {
    EventTxn tx(budget, ctx);
    LEANAT_CHECK(backend.invoke(cancel, {Value(child.value()), Value(Bytes{1})}, ctx, tx));
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(runtime.processes().inspect(process.value()));
    LEANAT_CHECK(runtime.processes().inspect(other_process.value()));
    LEANAT_CHECK(scopes.state(child.value()).value() == ScopeState::Open);
  }
  {
    EventTxn tx(budget, ctx);
    LEANAT_CHECK(backend.invoke(cancel, {Value(child.value()), Value(Bytes{1})}, ctx, tx));
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  LEANAT_CHECK(!runtime.processes().inspect(process.value()));
  LEANAT_CHECK(runtime.processes().inspect(other_process.value()));
  LEANAT_CHECK(runtime.processes().frame_count() == 1);
  LEANAT_CHECK(scopes.state(sibling.value()).value() == ScopeState::Open);
  // A scope-owned wait has independent process ownership. Cancellation must
  // retire its suspended frame and invalidate the exact single-resume claim.
  auto wait_scope = scopes.create(scopes.root());
  LEANAT_CHECK(wait_scope);
  auto waiting_ctx = ctx; waiting_ctx.owner = 88;
  auto waiting_process = runtime.processes().create(ProgramId{10}, waiting_ctx.owner);
  LEANAT_CHECK(waiting_process && runtime.processes().begin(waiting_process.value()));
  Handle wait_handle;
  SuspensionToken token;
  {
    EventTxn tx(budget, waiting_ctx);
    auto made = backend.invoke(wait_new, {Value(waiting_process.value()), Value(wait_scope.value()),
      Value(std::uint64_t{0}), Value(std::uint64_t{1}), Value(std::uint64_t{0}),
      Value(std::uint64_t{0}), Value(std::uint64_t{4096})}, waiting_ctx, tx);
    LEANAT_CHECK(made);
    wait_handle = std::get<Handle>(made.value()[0].data);
    auto suspended = runtime.processes().suspend_registered(wait_handle, {}, tx);
    LEANAT_CHECK(suspended); token = suspended.value();
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  {
    EventTxn tx(budget, ctx);
    LEANAT_CHECK(backend.invoke(cancel, {Value(wait_scope.value()), Value(Bytes{1})}, ctx, tx));
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(runtime.processes().suspension_for_wait(wait_handle));
  }
  {
    EventTxn tx(budget, ctx);
    LEANAT_CHECK(backend.invoke(cancel, {Value(wait_scope.value()), Value(Bytes{1})}, ctx, tx));
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  LEANAT_CHECK(!runtime.processes().inspect(waiting_process.value()));
  LEANAT_CHECK(!runtime.processes().suspension_for_wait(wait_handle));
  LEANAT_CHECK(!runtime.processes().notify(token, {}));
  LEANAT_CHECK(runtime.processes().inspect(other_process.value()));
  std::cout << "process scope authority: private delegation, child/sibling, owner/instance and rollback passed\n";
}
