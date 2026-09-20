#include "leanat/runtime_services.hpp"
#include "test_support.hpp"
using namespace leanat;
struct ResponseWaitHost : RuntimeHost {
  Runtime *runtime{};
  Expected<WireReturn> transport(const SendIntent &i) override {
    auto started = runtime->start_outbound(
        i, {i.call_id, i.connection, i.transport, i.flow, i.phase, runtime->now(), {}, i.payload});
    if (!started)
      return started.error();
    return WireReturn{
        Sync::Completed, {}, Duration{}, ResponseSnapshot{ResponseStatus::Ok, {4, 9}, false, {}}};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
void check_response_wait(bool before_registration) {
  ResponseWaitHost host;
  RuntimeConfig cfg;
  cfg.domain = DomainId{1};
  cfg.instance = InstanceId{1};
  cfg.connections = {ConnectionId{1}};
  cfg.descriptor_identity = "response-wait";
  Runtime runtime(cfg, host);
  host.runtime = &runtime;
  LEANAT_CHECK(
      runtime.start({cfg.domain, cfg.descriptor_identity, cfg.connections, true, true, {}, {}}));
  auto hop = runtime.protocol().create_ledger(ConnectionId{1}, TransportId{1}, 1, 7);
  LEANAT_CHECK(hop);
  SendIntent request;
  request.connection = ConnectionId{1};
  request.transport = TransportId{1};
  request.not_before = Tick{2};
  request.call_id = runtime.allocate_call_id(CallOrigin::Outgoing).value();
  request.payload.command = Command::Read;
  request.payload.data = {0, 0};
  request.payload.streaming_width = 2;
  LEANAT_CHECK(runtime.publish(request));
  auto pump = [&](Tick tick) {
    for (unsigned i = 0; i < 32 && runtime.next_wakeup() && !(tick < runtime.next_wakeup()->time);
         ++i) {
      auto r = runtime.pump_batch(tick, 64);
      LEANAT_CHECK(r && !runtime.stopped());
    }
  };
  if (before_registration)
    pump(Tick{2});
  exec::Project p;
  p.types = {{exec::TypeKind::Handle, static_cast<std::uint64_t>(HandleKind::Process), {}, {}},
             {exec::TypeKind::Handle, static_cast<std::uint64_t>(HandleKind::Hop), {}, {}},
             {exec::TypeKind::Handle, static_cast<std::uint64_t>(HandleKind::Wait), {}, {}}};
  exec::ServiceSignature s;
  s.id = 0;
  s.op = exec::Op::RegisterWait;
  s.input_types = {0, 1};
  s.result_types = {2};
  s.context_mask = 2;
  s.effect_mask = 16;
  s.extra_fuel = 1;
  s.provider_key = "leanat.core.wait.response";
  s.provider_version = "1";
  const std::string hex = "7f0dfc014f1a8c07de8d55c2e40a52b85fc7d534be8df44f48d31c2962db1a04";
  for (std::size_t n = 0; n < 32; ++n)
    s.abi_hash[n] = static_cast<std::uint8_t>(std::stoul(hex.substr(n * 2, 2), nullptr, 16));
  CoreRuntimeBackend backend(runtime, p);
  LEANAT_CHECK(backend.register_core(s));
  LEANAT_CHECK(backend.freeze());
  ExecutionContext ctx;
  ctx.domain = cfg.domain;
  ctx.instance = cfg.instance;
  ctx.owner = 7;
  ctx.kind = ContextKind::Process;
  ctx.ready = {before_registration ? Tick{3} : Tick{}, 0};
  Handle process;
  {
    EventTxn tx({}, ctx);
    auto made = runtime.processes().create(ProgramId{1}, 7, tx);
    LEANAT_CHECK(made);
    process = made.value();
    LEANAT_CHECK(tx.commit());
  }
  {
    EventTxn tx({}, ctx);
    LEANAT_CHECK(runtime.processes().begin(process, tx));
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(backend.bind_current_process(process));
  {
    EventTxn tx({}, ctx);
    auto other = runtime.processes().create(ProgramId{1}, 7, tx);
    LEANAT_CHECK(other && runtime.processes().begin(other.value(), tx));
    LEANAT_CHECK(tx.commit());
    EventTxn wait_tx({}, ctx);
    auto rejected = backend.invoke(s, {Value{other.value()}, Value{hop.value()}}, ctx, wait_tx);
    LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::WrongOwner);
    LEANAT_CHECK(wait_tx.discard());
    LEANAT_CHECK(runtime.processes().complete(other.value()));
  }
  {
    EventTxn tx({}, ctx);
    auto wrong = hop.value();
    ++wrong.owner;
    LEANAT_CHECK(!backend.invoke(s, {Value{process}, Value{wrong}}, ctx, tx));
    LEANAT_CHECK(tx.discard());
  }
  {
    EventTxn tx({}, ctx);
    auto registered = backend.invoke(s, {Value{process}, Value{hop.value()}}, ctx, tx);
    LEANAT_CHECK(registered);
    auto wait = std::get<Handle>(registered.value()[0].data);
    LEANAT_CHECK(
        backend.prepare_suspend(wait, BlockId{4}, {Value{Bytes{1, 2}}}, TypeId{}, ctx, tx));
    LEANAT_CHECK(tx.discard());
  }
  LEANAT_CHECK(runtime.processes().wait_count() == 0);
  {
    EventTxn tx({}, ctx);
    auto registered = backend.invoke(s, {Value{process}, Value{hop.value()}}, ctx, tx);
    LEANAT_CHECK(registered);
    auto wait = std::get<Handle>(registered.value()[0].data);
    LEANAT_CHECK(
        backend.prepare_suspend(wait, BlockId{4}, {Value{Bytes{1, 2}}}, TypeId{}, ctx, tx));
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(runtime.processes().inspect(process).value().state ==
               (before_registration ? ProcessState::ResumeQueued : ProcessState::Suspended));
  unsigned resumes = 0;
  runtime.set_resume_handler(
      [&](const SuspensionToken &t, ReadyKey ready, Runtime &) -> Expected<void> {
        ++resumes;
        auto resumed = runtime.processes().take_resume(t);
        LEANAT_CHECK(resumed);
        LEANAT_CHECK(resumed.value().outcome.source.time == Tick{2});
        LEANAT_CHECK(resumed.value().frame.live == std::vector<Value>{Value{Bytes{1, 2}}});
        LEANAT_CHECK(resumed.value().outcome.value ==
                     Value{Value::Array{Value{std::uint64_t(ResponseStatus::Ok)},
                                        Value{Bytes{4, 9}}, Value{false}}});
        LEANAT_CHECK(!(ready.time < Tick{2}));
        return {};
      });
  pump(before_registration ? Tick{3} : Tick{2});
  LEANAT_CHECK(resumes == 1 && runtime.processes().wait_count() == 0);
  LEANAT_CHECK(runtime.response_wait_outcome(hop.value()));
}
int main() {
  check_response_wait(true);
  check_response_wait(false);
}
