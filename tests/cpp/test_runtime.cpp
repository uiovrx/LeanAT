#include "leanat/runtime.hpp"
#include "test_support.hpp"
using namespace leanat;
struct Host : RuntimeHost {
  Runtime *runtime{};
  std::optional<WakePoint> armed;
  unsigned calls{}, arms{};
  bool omit_start{}, reenter{};
  WireReturn reply;
  std::function<void()> during;
  Expected<WireReturn> transport(const SendIntent &i) override {
    ++calls;
    if (!omit_start) {
      WireCall c{i.call_id, i.connection,   i.transport, i.flow,
                 i.phase,   runtime->now(), {},          i.payload};
      auto start = runtime->start_outbound(i, c);
      if (!start)
        return start.error();
    }
    if (reenter)
      LEANAT_CHECK(!runtime->pump_batch(runtime->now(), 1));
    if (during)
      during();
    return reply;
  }
  void arm(std::optional<WakePoint> w) override {
    armed = w;
    ++arms;
  }
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
RuntimeConfig config() {
  RuntimeConfig c;
  c.domain = DomainId{1};
  c.instance = InstanceId{1};
  c.descriptor_identity = "verified:1";
  c.connections = {ConnectionId{1}};
  return c;
}
HostBindingManifest manifest() {
  return {DomainId{1}, "verified:1", {ConnectionId{1}}, true, true, {}, {}};
}
void drain_batches(Runtime &r, Tick now) {
  for (int n = 0; n < 20 && r.next_wakeup() && !(now < r.next_wakeup()->time); ++n)
    LEANAT_CHECK(r.pump_batch(now, 64));
}
struct Service : PreparedParticipant {
  int &count;
  explicit Service(int &c) : count(c) {}
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this);
  }
  Expected<void> validate() const override {
    return {};
  }
  void apply() noexcept override {
    ++count;
  }
  void discard() noexcept override {}
};
int main() {
  Host host;
  Runtime rt(config(), host);
  host.runtime = &rt;
  LEANAT_CHECK(!rt.pump_batch(Tick{}, 1));
  auto bad = manifest();
  bad.descriptor_identity = "wrong";
  LEANAT_CHECK(!rt.start(bad));
  LEANAT_CHECK(host.arms == 0);
  LEANAT_CHECK(rt.start(manifest()));
  LEANAT_CHECK(!rt.start(manifest()));
  unsigned events = 0;
  rt.set_handler([&](const QueuedEvent &, Runtime &r) -> Expected<void> {
    ++events;
    LEANAT_CHECK(!r.pump_batch(r.now(), 1));
    return {};
  });
  for (int n = 0; n < 3; ++n) {
    EventDraft e;
    e.key.time = Tick{1};
    LEANAT_CHECK(rt.schedule(e));
  }
  auto slice = rt.pump_batch(Tick{1}, 1);
  LEANAT_CHECK(slice && slice.value().executed_events == 1 &&
               slice.value().progress == ProgressDisposition::ContinueDispatch);
  auto id = slice.value().closed_batch_id;
  auto next = rt.pump_batch(Tick{1}, 1);
  LEANAT_CHECK(next && next.value().closed_batch_id == id);
  drain_batches(rt, Tick{1});
  LEANAT_CHECK(events == 3);
  LEANAT_CHECK(!host.armed);
  LEANAT_CHECK(!rt.pump_batch(Tick{}, 1));
  auto ledger = rt.protocol().create_ledger(ConnectionId{1}, TransportId{1});
  LEANAT_CHECK(ledger);
  auto call = rt.allocate_call_id(CallOrigin::Outgoing);
  LEANAT_CHECK(call);
  SendIntent intent;
  intent.connection = ConnectionId{1};
  intent.transport = TransportId{1};
  intent.call_id = call.value();
  intent.not_before = Tick{10};
  intent.payload.data = {1};
  LEANAT_CHECK(rt.publish(intent));
  LEANAT_CHECK(rt.protocol().inspect(ledger.value()).value().state == WireState::Idle);
  LEANAT_CHECK(rt.pump_batch(Tick{5}, 8));
  LEANAT_CHECK(host.calls == 0);
  host.reply.sync = Sync::Completed;
  host.reply.outgoing_delay = Duration{5};
  host.reply.response = ResponseSnapshot{ResponseStatus::Ok, {9}, false, {}};
  host.reenter = true;
  LEANAT_CHECK(rt.pump_batch(Tick{10}, 8));
  LEANAT_CHECK(host.calls == 1);
  LEANAT_CHECK(rt.protocol().inspect(ledger.value()).value().state == WireState::Terminal);
  LEANAT_CHECK(!rt.milestone(ledger.value(), MilestoneKind::Terminal));
  drain_batches(rt, Tick{15});
  LEANAT_CHECK(rt.milestone(ledger.value(), MilestoneKind::Terminal));
  LEANAT_CHECK(!rt.record_return(call.value(), host.reply));
  Host ingress_host;
  Runtime receiver(config(), ingress_host);
  ingress_host.runtime = &receiver;
  LEANAT_CHECK(receiver.start(manifest()));
  LEANAT_CHECK(receiver.protocol().create_ledger(ConnectionId{1}, TransportId{2}));
  auto inputid = receiver.allocate_call_id(CallOrigin::ExternalIngress);
  LEANAT_CHECK(inputid);
  WireCall input{inputid.value(), ConnectionId{1}, TransportId{2}, Flow::Forward,
                 begin_req,       Tick{3},         Duration{7},    {}};
  input.request.address = 42;
  input.request.data = {4, 5};
  unsigned inputs = 0;
  receiver.set_input_handler([&](const WireCall &c, ReadyKey key, Runtime &) -> Expected<void> {
    ++inputs;
    LEANAT_CHECK(c.id == input.id && c.request.address == 42 && c.phase == begin_req &&
                 key.time == Tick{10});
    return {};
  });
  auto accepted = receiver.ingress(input);
  LEANAT_CHECK(accepted && accepted.value().outgoing_delay == Duration{7});
  LEANAT_CHECK(inputs == 0);
  drain_batches(receiver, Tick{3});
  LEANAT_CHECK(inputs == 0);
  drain_batches(receiver, Tick{10});
  LEANAT_CHECK(inputs == 1);
  Host ch;
  Runtime complete(config(), ch);
  ch.runtime = &complete;
  LEANAT_CHECK(complete.start(manifest()));
  LEANAT_CHECK(complete.protocol().create_ledger(ConnectionId{1}, TransportId{3}));
  int services = 0;
  complete.set_ingress_policy([&](const WireCall &) -> Expected<IngressPlan> {
    IngressPlan p;
    p.complete_now = true;
    p.reply.sync = Sync::Completed;
    p.reply.response = ResponseSnapshot{ResponseStatus::Ok, {7}, false, {}};
    p.prepared_service = std::make_unique<Service>(services);
    return p;
  });
  unsigned extra = 0;
  complete.set_input_handler([&](const WireCall &, ReadyKey, Runtime &) -> Expected<void> {
    ++extra;
    return {};
  });
  input.id = complete.allocate_call_id(CallOrigin::ExternalIngress).value();
  input.transport = TransportId{3};
  input.incoming_delay = Duration{};
  LEANAT_CHECK(complete.ingress(input));
  drain_batches(complete, Tick{3});
  LEANAT_CHECK(services == 1 && extra == 0);
  Host cancellation;
  Runtime cancel_runtime(config(), cancellation);
  cancellation.runtime = &cancel_runtime;
  LEANAT_CHECK(cancel_runtime.start(manifest()));
  auto first = cancel_runtime.schedule(EventDraft{});
  auto second = cancel_runtime.schedule(EventDraft{});
  LEANAT_CHECK(first && second);
  unsigned executed = 0;
  cancel_runtime.set_handler([&](const QueuedEvent &e, Runtime &r) -> Expected<void> {
    ++executed;
    if (e.token == first.value())
      LEANAT_CHECK(r.queue().cancel(second.value()));
    return {};
  });
  drain_batches(cancel_runtime, Tick{});
  LEANAT_CHECK(executed == 1 && !cancel_runtime.stopped());
  Host resetting;
  Runtime reset_runtime(config(), resetting);
  resetting.runtime = &reset_runtime;
  LEANAT_CHECK(reset_runtime.start(manifest()));
  unsigned stale_writes = 0;
  reset_runtime.set_handler([&](const QueuedEvent &, Runtime &) -> Expected<void> {
    ++stale_writes;
    return {};
  });
  LEANAT_CHECK(reset_runtime.schedule(EventDraft{}));
  LEANAT_CHECK(reset_runtime.reset(InstanceId{1}, ResetPolicy::AbortLocalAndDrain));
  drain_batches(reset_runtime, Tick{});
  LEANAT_CHECK(stale_writes == 0);
  EventDraft current;
  current.epoch = 1;
  current.key.turn = 1;
  LEANAT_CHECK(reset_runtime.schedule(current));
  drain_batches(reset_runtime, Tick{});
  LEANAT_CHECK(stale_writes == 1);
  Host limited;
  auto lc = config();
  lc.max_events = 1;
  Runtime limit(lc, limited);
  limited.runtime = &limit;
  LEANAT_CHECK(limit.start(manifest()));
  for (int n = 0; n < 2; ++n)
    LEANAT_CHECK(limit.schedule(EventDraft{}));
  auto stop = limit.pump_batch(Tick{}, 4);
  LEANAT_CHECK(stop && stop.value().progress == ProgressDisposition::Stopped);
  LEANAT_CHECK(!limit.next_wakeup() && !limited.armed);
  LEANAT_CHECK(!limit.pump_batch(Tick{}, 4));
}
