#include "leanat/admission.hpp"
#include "leanat/runtime.hpp"
#include "test_support.hpp"
using namespace leanat;
struct BoundaryHost : RuntimeHost {
  Runtime *runtime{};
  std::vector<std::pair<InstanceId, Value>> outputs;
  std::vector<std::pair<PhaseId, Tick>> calls;
  std::vector<RuntimeMilestoneObservation> observed;
  std::function<WireReturn(const SendIntent &)> reply;
  Expected<WireReturn> transport(const SendIntent &i) override {
    WireCall call{i.call_id, i.connection,   i.transport, i.flow,
                  i.phase,   runtime->now(), {},          i.payload};
    auto started = runtime->start_outbound(i, call);
    if (!started)
      return started.error();
    calls.emplace_back(i.phase, runtime->now());
    return reply(i);
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {
    LEANAT_CHECK(false);
  }
  void publish_output(InstanceId i, PortId p, const Value &v) override {
    LEANAT_CHECK(p == PortId{1});
    outputs.emplace_back(i, v);
  }
  void emit_trace(const TraceEvent &) override {}
  void observe_milestone(const RuntimeMilestoneObservation &o) override {
    auto active = runtime->queue().active_event(o.event);
    LEANAT_CHECK(active && !(active.value().key < o.key) && !(o.key < active.value().key));
    observed.push_back(o);
  }
};
RuntimeConfig settings() {
  RuntimeConfig c;
  c.domain = DomainId{1};
  c.instance = InstanceId{1};
  c.instances = {InstanceId{1}, InstanceId{2}};
  c.connections = {ConnectionId{1}, ConnectionId{2}};
  c.connection_bindings = {{ConnectionId{1}, InstanceId{1}, InstanceId{2}},
                           {ConnectionId{2}, InstanceId{1}, InstanceId{2}}};
  c.descriptor_identity = "runtime-boundaries";
  return c;
}
void start(Runtime &r, BoundaryHost &h) {
  h.runtime = &r;
  auto c = settings();
  LEANAT_CHECK(r.start({c.domain, c.descriptor_identity, c.connections, true, true, c.instances,
                        c.connection_bindings}));
}
void settle(Runtime &r, Tick now) {
  for (unsigned n = 0; n < 64 && r.next_wakeup() && !(now < r.next_wakeup()->time); ++n) {
    auto p = r.pump_batch(now, 64);
    LEANAT_CHECK(p && !r.stopped());
  }
  LEANAT_CHECK(!r.next_wakeup() || now < r.next_wakeup()->time);
}
ExecutionContext context(InstanceId instance = InstanceId{1}) {
  ExecutionContext c;
  c.domain = DomainId{1};
  c.instance = instance;
  c.owner = 7;
  c.kind = ContextKind::Process;
  c.connection = ConnectionId{1};
  return c;
}
void scoped_output_and_process() {
  BoundaryHost h;
  Runtime r(settings(), h);
  start(r, h);
  for (auto instance : {InstanceId{1}, InstanceId{2}}) {
    EventTxn tx({}, context(instance));
    LEANAT_CHECK(r.stage_output(tx, PortId{1}, Value{std::uint64_t{instance.value}}));
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(h.outputs.empty());
  settle(r, Tick{});
  LEANAT_CHECK(h.outputs.size() == 2 && h.outputs[0].first == InstanceId{1} &&
               h.outputs[1].first == InstanceId{2});
  auto &p = r.processes();
  Handle frame;
  {
    EventTxn tx({}, context());
    auto made = p.create(ProgramId{1}, 7, tx);
    LEANAT_CHECK(made);
    frame = made.value();
    LEANAT_CHECK(tx.commit());
  }
  {
    EventTxn tx({}, context(InstanceId{2}));
    LEANAT_CHECK(!p.begin(frame, tx));
    LEANAT_CHECK(tx.discard());
  }
  LEANAT_CHECK(p.inspect(frame).value().instance == InstanceId{1});
  {
    EventTxn tx({}, context());
    LEANAT_CHECK(p.begin(frame, tx));
    LEANAT_CHECK(tx.commit());
  }
  SingleWaitSpec spec;
  spec.until = Tick{2};
  {
    EventTxn tx({}, context(InstanceId{2}));
    LEANAT_CHECK(!p.register_wait(frame, spec, tx));
    LEANAT_CHECK(!p.cancel(frame, tx));
    LEANAT_CHECK(tx.discard());
  }
  SuspensionToken token;
  {
    EventTxn tx({}, context());
    auto w = p.register_wait(frame, spec, tx);
    LEANAT_CHECK(w);
    auto s = p.suspend_registered(w.value(), {}, tx);
    LEANAT_CHECK(s);
    token = s.value();
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(p.notify(token, {SingleWaitStatus::Ready, {}, ReadyKey{Tick{2}, 0}}));
  {
    EventTxn tx({}, context(InstanceId{2}));
    LEANAT_CHECK(!p.take_resume(token, tx));
    LEANAT_CHECK(tx.discard());
  }
  LEANAT_CHECK(p.inspect(frame).value().state == ProcessState::ResumeQueued);
  {
    EventTxn tx({}, context());
    LEANAT_CHECK(p.take_resume(token, tx));
    LEANAT_CHECK(tx.commit());
  }
}
void blocking_reset() {
  BoundaryHost h;
  auto c = settings();
  c.blocking_capacity = 2;
  Runtime r(c, h);
  start(r, h);
  unsigned services = 0, completed = 0;
  std::vector<ResponseStatus> statuses;
  r.set_blocking_handler([&](const BlockingRequest &q, Runtime &runtime) -> Expected<void> {
    ++services;
    LEANAT_CHECK(q.instance == InstanceId{2});
    return runtime.complete_blocking(q.token, {ResponseStatus::Ok, {9}, false, {}});
  });
  PayloadSnapshot request;
  request.command = Command::Read;
  request.data = {3};
  auto sink = [&](Expected<ResponseSnapshot> response) {
    LEANAT_CHECK(response);
    ++completed;
    statuses.push_back(response.value().status);
  };
  auto old = r.submit_blocking(ConnectionId{1}, request, Tick{3}, sink);
  LEANAT_CHECK(old);
  LEANAT_CHECK(r.reset(InstanceId{2}, ResetPolicy::AbortLocalAndDrain));
  settle(r, Tick{3});
  LEANAT_CHECK(services == 0 && completed == 1 && statuses.back() == ResponseStatus::GenericError &&
               h.calls.empty());
  LEANAT_CHECK(r.drains().outstanding() == 0);
  auto fresh = r.submit_blocking(ConnectionId{1}, request, Tick{4}, sink);
  LEANAT_CHECK(fresh && fresh.value() != old.value());
  settle(r, Tick{4});
  LEANAT_CHECK(services == 1 && completed == 2 && statuses.back() == ResponseStatus::Ok);
  // DrainThenReset retains the old business request and promotes a newly submitted root.
  auto prior = r.submit_blocking(ConnectionId{1}, request, Tick{5}, sink);
  LEANAT_CHECK(prior);
  LEANAT_CHECK(r.reset(InstanceId{2}, ResetPolicy::DrainThenReset));
  auto deferred = r.submit_blocking(ConnectionId{1}, request, Tick{5}, sink);
  LEANAT_CHECK(deferred);
  settle(r, Tick{5});
  LEANAT_CHECK(services == 3 && completed == 4 && r.drains().outstanding() == 0);
  LEANAT_CHECK(r.drains().epoch(InstanceId{2}).value() == 2);
  LEANAT_CHECK(!r.complete_blocking(old.value(), {ResponseStatus::Ok, {9}, false, {}}));
  unsigned failed = 0;
  r.set_blocking_handler([](const BlockingRequest &, Runtime &) -> Expected<void> {
    return fail(ErrorCode::ExternalFailure, "business provider failed");
  });
  LEANAT_CHECK(r.submit_blocking(ConnectionId{1}, request, Tick{6},
                                 [&](Expected<ResponseSnapshot> response) {
                                   LEANAT_CHECK(!response);
                                   ++failed;
                                 }));
  auto stopped = r.pump_batch(Tick{6}, 64);
  LEANAT_CHECK(stopped && r.stopped() && failed == 1);
  LEANAT_CHECK(r.drains().outstanding() == 1);
}
void permits_and_lane() {
  BoundaryHost h;
  Runtime r(settings(), h);
  start(r, h);
  AdmissionStore admission(DomainId{1}, InstanceId{1}, {8, 8, 8, 8, 8});
  AdmissionRequest req;
  req.owner = 7;
  req.connection = ConnectionId{1};
  req.transport = TransportId{1};
  req.request.data = {1};
  auto a = admission.create_initiator(req);
  LEANAT_CHECK(a);
  req.transport = TransportId{2};
  auto b = admission.create_initiator(req);
  LEANAT_CHECK(b);
  LEANAT_CHECK(r.protocol().bind_ledger(a.value().hop, ConnectionId{1}, TransportId{1}, 1));
  auto permit = admission.try_request_permit(a.value().txn, ConnectionId{1}, {});
  LEANAT_CHECK(permit && permit.value());
  auto waiting = admission.request_request_gate(b.value().txn, ConnectionId{1}, {});
  LEANAT_CHECK(waiting);
  LEANAT_CHECK(admission.gate_state(waiting.value()).value() == RequestGateState::Pending);
  for (unsigned failure = 0; failure < 3; ++failure) {
    EventTxn tx({}, context());
    auto id = r.stage_allocate_call_id(tx, CallOrigin::Outgoing);
    LEANAT_CHECK(id);
    SendIntent i;
    i.call_id = id.value();
    i.txn = failure == 0 ? b.value().txn : a.value().txn;
    i.connection = failure == 1 ? ConnectionId{2} : ConnectionId{1};
    i.transport = TransportId{1};
    i.payload = req.request;
    LEANAT_CHECK(r.stage_publish(tx, i));
    RequestPermit wrong = *permit.value();
    if (failure == 2)
      ++wrong.handle.owner;
    LEANAT_CHECK(!r.stage_request_permit(tx, id.value(), admission, wrong));
    LEANAT_CHECK(tx.discard());
  }
  {
    EventTxn tx({}, context());
    auto id = r.stage_allocate_call_id(tx, CallOrigin::Outgoing);
    LEANAT_CHECK(id);
    SendIntent i;
    i.call_id = id.value();
    i.txn = a.value().txn;
    i.connection = ConnectionId{1};
    i.transport = TransportId{1};
    i.payload = req.request;
    LEANAT_CHECK(r.stage_publish(tx, i));
    LEANAT_CHECK(r.stage_request_permit(tx, id.value(), admission, *permit.value()));
    LEANAT_CHECK(tx.commit());
  }
  h.reply = [](const SendIntent &) {
    WireReturn ret;
    ret.sync = Sync::Updated;
    ret.phase = end_req;
    ret.outgoing_delay = Duration{5};
    return ret;
  };
  settle(r, Tick{});
  LEANAT_CHECK(h.calls.size() == 1);
  // Wire request exclusion releases at the validated return; semantic timing remains at t=5.
  LEANAT_CHECK(admission.gate_state(waiting.value()).value() == RequestGateState::Granted);
  LEANAT_CHECK(!r.milestone(a.value().hop, MilestoneKind::RequestReleased));
  settle(r, Tick{5});
  LEANAT_CHECK(r.milestone(a.value().hop, MilestoneKind::RequestReleased));
}
void delayed_cancel() {
  BoundaryHost h;
  Runtime r(settings(), h);
  start(r, h);
  auto hop = r.protocol().create_ledger(ConnectionId{1}, TransportId{1});
  LEANAT_CHECK(hop);
  Handle transaction{HandleKind::Transaction, DomainId{1}, 99, 0, 1, 7};
  LEANAT_CHECK(r.drains().register_responsibility({transaction,
                                                   InstanceId{1},
                                                   0,
                                                   {},
                                                   {{hop.value(), false, false, false, false}},
                                                   false,
                                                   false}));
  PayloadSnapshot request;
  request.data = {1};
  LEANAT_CHECK(r.track_cleanup(transaction, hop.value(), true, request));
  h.reply = [](const SendIntent &i) {
    WireReturn ret;
    if (i.phase == begin_req) {
      ret.sync = Sync::Updated;
      ret.phase = begin_resp;
      ret.outgoing_delay = Duration{3};
      ret.response = ResponseSnapshot{ResponseStatus::Ok, {9}, false, {}};
    } else {
      LEANAT_CHECK(i.phase == end_resp);
      ret.sync = Sync::Completed;
      ret.outgoing_delay = Duration{2};
    }
    return ret;
  };
  SendIntent i;
  i.call_id = r.allocate_call_id(CallOrigin::Outgoing).value();
  i.txn = transaction;
  i.connection = ConnectionId{1};
  i.transport = TransportId{1};
  i.payload = request;
  LEANAT_CHECK(r.publish(i));
  settle(r, Tick{});
  auto cancel = r.cancel_local(transaction, CancelReason::User);
  LEANAT_CHECK(cancel && cancel.value().receipt);
  settle(r, Tick{2});
  LEANAT_CHECK(h.calls.size() == 1);
  settle(r, Tick{3});
  LEANAT_CHECK(h.calls.size() == 2 && h.calls.back().first == end_resp &&
               h.calls.back().second == Tick{3});
  LEANAT_CHECK(r.drains().inspect(*cancel.value().receipt).value().state == DrainState::Pending);
  settle(r, Tick{5});
  LEANAT_CHECK(r.drains().inspect(*cancel.value().receipt).value().state == DrainState::Complete);
  LEANAT_CHECK(h.observed.size() == 3);
  for (std::size_t n = 0; n < h.observed.size(); ++n) {
    const auto &o = h.observed[n];
    LEANAT_CHECK(o.causal.hop == hop.value());
    LEANAT_CHECK(o.identity.domain == DomainId{1} && o.identity.local_side == InstanceId{1} &&
                 o.identity.connection == ConnectionId{1} &&
                 o.identity.transport == TransportId{1} && o.identity.transport_generation == 1);
    LEANAT_CHECK(o.key.stage == EventStage::Internal && o.key.instance == InstanceId{1});
    LEANAT_CHECK(o.key.time == o.causal.milestone.time && o.key.turn == 0);
    if (n)
      LEANAT_CHECK(h.observed[n - 1].key < o.key && h.observed[n - 1].event != o.event);
  }
  LEANAT_CHECK(h.observed[0].causal.milestone.kind == MilestoneKind::RequestReleased &&
               h.observed[0].causal.milestone.implicit && h.observed[0].key.time == Tick{3});
  LEANAT_CHECK(h.observed[1].causal.milestone.kind == MilestoneKind::ResponseReady &&
               h.observed[1].causal.call_id == i.call_id &&
               h.observed[1].causal.milestone.response->data == Bytes{9});
  LEANAT_CHECK(h.observed[2].causal.milestone.kind == MilestoneKind::Terminal &&
               h.observed[2].causal.call_id != i.call_id && h.observed[2].key.time == Tick{5});
}
void ingress_reset_epoch() {
  BoundaryHost host;
  Runtime runtime(settings(), host);
  start(runtime, host);
  unsigned writes = 0;
  runtime.set_input_handler([&](const WireCall &, ReadyKey, Runtime &) -> Expected<void> {
    ++writes;
    return {};
  });
  auto enqueue = [&](ConnectionId connection, TransportId transport, Tick time) {
    auto engine = runtime.protocol(InstanceId{2});
    LEANAT_CHECK(engine && engine.value()->create_ledger(connection, transport));
    WireCall call;
    call.id = runtime.allocate_call_id(CallOrigin::ExternalIngress).value();
    call.connection = connection;
    call.transport = transport;
    call.phase = begin_req;
    call.call_time = runtime.now();
    call.incoming_delay = Duration{time.value - runtime.now().value};
    call.request.data = {1};
    LEANAT_CHECK(runtime.ingress(call));
  };
  enqueue(ConnectionId{1}, TransportId{1}, Tick{3});
  LEANAT_CHECK(runtime.reset(InstanceId{2}, ResetPolicy::AbortLocalAndDrain));
  settle(runtime, Tick{3});
  LEANAT_CHECK(writes == 0);
  enqueue(ConnectionId{2}, TransportId{2}, Tick{4});
  settle(runtime, Tick{4});
  LEANAT_CHECK(writes == 1);
}
int main() {
  ingress_reset_epoch();
  scoped_output_and_process();
  blocking_reset();
  permits_and_lane();
  delayed_cancel();
}
