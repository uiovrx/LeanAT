#include "leanat/payload_helpers.hpp"
#include "test_support.hpp"
using namespace leanat;
void executable_helpers();
int main() {
  executable_helpers();
  PayloadBuilder b({4, 2, 3});
  auto r = b.read(UINT64_MAX, 4);
  LEANAT_CHECK(r && r.value().data == Bytes(4, 0) && r.value().streaming_width == 4);
  LEANAT_CHECK(!b.read(0, 5));
  auto z = b.read(0, 0);
  LEANAT_CHECK(z && z.value().streaming_width == 1);
  Bytes input{1, 2};
  auto w = b.write(0, input);
  input[0] = 9;
  LEANAT_CHECK(w.value().data[0] == 1);
  auto ignore = b.ignore(UINT64_MAX);
  LEANAT_CHECK(ignore.command == Command::Ignore && ignore.streaming_width == 0 &&
               ignore.data.empty());
  LEANAT_CHECK(b.with_streaming_width(ignore, 0));
  LEANAT_CHECK(!b.with_streaming_width(r.value(), 0));
  LEANAT_CHECK(b.with_streaming_width(r.value(), 8));
  LEANAT_CHECK(!b.with_byte_enable(r.value(), Bytes(3)));
  OptionalExtensionSchema s{"x", "schema", 1, TypeId{1}, true};
  LEANAT_CHECK(!b.with_optional_extension(r.value(), s, Bytes{1}));
  LEANAT_CHECK(b.register_extension(s));
  auto e = b.with_optional_extension(r.value(), s, Bytes{1, 2});
  LEANAT_CHECK(e);
  LEANAT_CHECK(b.with_optional_extension(e.value(), s, Bytes{3}));
  s.version = 2;
  LEANAT_CHECK(!b.with_optional_extension(e.value(), s, Bytes{1}));
  ExecutionContext c;
  c.kind = ContextKind::Process;
  LEANAT_CHECK(plan_transact(c, ConnectionId{1}, r.value(), {}));
  c.kind = ContextKind::Transport;
  LEANAT_CHECK(!plan_transact(c, ConnectionId{1}, r.value(), {}));
  ProtocolEngine protocol(DomainId{}, InstanceId{}, 4, 4, 16);
  auto hop = protocol.create_ledger(ConnectionId{1}, TransportId{1}, 1, 0).value();
  WireCall call{CallId{1}, ConnectionId{1}, TransportId{1}, Flow::Forward,
                begin_req, Tick{},          Duration{},     r.value()};
  auto ticket = protocol.begin_call(hop, call);
  LEANAT_CHECK(ticket);
  WireReturn ret{Sync::Updated, begin_resp, Duration{2},
                 ResponseSnapshot{ResponseStatus::Ok, Bytes(4, 1), false, {}}};
  LEANAT_CHECK(protocol.end_call(ticket.value(), ret));
  ResponseAcknowledger ack(protocol, hop, 0);
  c.kind = ContextKind::Process;
  c.ready = {Tick{2}, 0};
  {
    EventTxn tx({}, c);
    LEANAT_CHECK(!ack.stage_ack(tx, Tick{1}, CallId{2}, r.value()));
    LEANAT_CHECK(tx.commit());
  }
  {
    EventTxn tx({128, 0, 65536, 128}, c);
    LEANAT_CHECK(!ack.stage_ack(tx, Tick{2}, CallId{2}, r.value()));
    LEANAT_CHECK(tx.commit());
  }
  {
    EventTxn tx({}, c);
    LEANAT_CHECK(ack.stage_ack(tx, Tick{2}, CallId{2}, r.value()).value());
    LEANAT_CHECK(!ack.stage_ack(tx, Tick{2}, CallId{3}, r.value()).value());
    auto committed = tx.commit();
    LEANAT_CHECK(committed && committed.value().actions.size() == 1);
    LEANAT_CHECK(protocol.inspect(hop).value().state == WireState::Response);
    ResponseAcknowledger alias(protocol, hop, 0);
    EventTxn other({}, c);
    LEANAT_CHECK(!alias.stage_ack(other, Tick{2}, CallId{3}, r.value()).value());
    auto outgoing = committed.value().actions[0];
    WireCall actual{outgoing.call_id, outgoing.connection, outgoing.transport,
                    outgoing.flow,    outgoing.phase,      Tick{2},
                    Duration{},       outgoing.payload};
    auto end = protocol.begin_call(hop, actual);
    LEANAT_CHECK(end);
    LEANAT_CHECK(protocol.end_call(end.value(), WireReturn{Sync::Accepted, {}, Duration{1}, {}}));
    LEANAT_CHECK(protocol.inspect(hop).value().state == WireState::Terminal);
  }
}

#include "leanat/structured.hpp"
#include "leanat/task_pool.hpp"
struct HelperHost : RuntimeHost {
  Runtime *runtime{};
  unsigned requests{}, acks{};
  bool completed{true};
  Duration response_delay{3};
  Expected<WireReturn> transport(const SendIntent &i) override {
    auto start = runtime->start_outbound(
        i,
        WireCall{
            i.call_id, i.connection, i.transport, i.flow, i.phase, runtime->now(), {}, i.payload});
    if (!start)
      return start.error();
    if (i.phase == begin_req) {
      ++requests;
      ResponseSnapshot response{ResponseStatus::Ok, Bytes(i.payload.data.size(), 9), false, {}};
      return WireReturn{completed ? Sync::Completed : Sync::Updated,
                        completed ? std::optional<PhaseId>{} : std::optional<PhaseId>{begin_resp},
                        response_delay, response};
    }
    ++acks;
    return WireReturn{Sync::Accepted, {}, Duration{2}, {}};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
struct HelperFixture {
  HelperHost host;
  Runtime runtime;
  AdmissionStore admission;
  HelperFixture()
      : runtime(
            [] {
              RuntimeConfig c;
              c.descriptor_identity = "helper";
              c.connections = {ConnectionId{1}};
              return c;
            }(),
            host),
        admission(DomainId{}, InstanceId{}, AdmissionLimits{8, 8, 1, 1, 8}) {
    host.runtime = &runtime;
    LEANAT_CHECK(
        runtime.start(HostBindingManifest{DomainId{}, "helper", {ConnectionId{1}}, true, true}));
  }
  ExecutionContext context(Tick t = {}) {
    ExecutionContext c;
    c.kind = ContextKind::Process;
    c.ready = {t, 0};
    return c;
  }
  void pump(Tick time) {
    LEANAT_CHECK(runtime.pump_batch(time, 64));
    for (unsigned n = 0; n < 32 && runtime.next_wakeup() && !(time < runtime.next_wakeup()->time);
         ++n)
      LEANAT_CHECK(runtime.pump_batch(time, 64));
    LEANAT_CHECK(!runtime.stopped());
  }
};
void executable_helpers() {
  PayloadBuilder builder;
  auto payload = builder.read(0, 4).value();
  payload.byte_enable = {255, 0};
  {
    HelperFixture f;
    auto c = f.context();
    TransactOperation unbound(f.runtime, f.admission);
    LEANAT_CHECK(!unbound.start(c, ConnectionId{99}, TransportId{1}, payload));
    PayloadLimits limits;
    limits.extension_key_bytes = SIZE_MAX;
    TransactOperation overflow(f.runtime, f.admission, limits);
    LEANAT_CHECK(!overflow.start(c, ConnectionId{1}, TransportId{1}, payload));
    LEANAT_CHECK(f.runtime.results().occupied() == 0 && f.host.requests == 0);
    TaskPoolDesc desc;
    TaskPool tasks(c.domain, 31, desc, f.runtime.results());
    Handle scope{HandleKind::Scope, c.domain, 1, 0, 1, c.owner};
    auto task = tasks.try_spawn({}, scope).value();
    auto consumer = tasks.result_handle(task, scope).value();
    c.owner = consumer.consumer.owner;
    {
      EventTxn aborted({}, c);
      LEANAT_CHECK(release_task_result_when_done(aborted, f.runtime, consumer));
      aborted.discard();
      LEANAT_CHECK(f.runtime.results().describe(consumer));
    }
    EventTxn tx({}, c);
    LEANAT_CHECK(release_task_result_when_done(tx, f.runtime, consumer));
    LEANAT_CHECK(f.runtime.commit_segment(tx));
    LEANAT_CHECK(!f.runtime.results().describe(consumer));
    LEANAT_CHECK(tasks.active_frames() == 1);
    LEANAT_CHECK(tasks.complete(task, {TaskOutcome::Kind::Success, Value{}}, c.ready));
    LEANAT_CHECK(tasks.active_frames() == 0 && f.runtime.results().occupied() == 0);
  }

  {
    HelperFixture f;
    TransactOperation op(f.runtime, f.admission);
    auto c = f.context();
    LEANAT_CHECK(op.start(c, ConnectionId{1}, TransportId{1}, payload));
    auto observer = op.subscribe_result(7);
    LEANAT_CHECK(observer);
    LEANAT_CHECK(!op.advance(c).value());
    LEANAT_CHECK(!op.published());
    f.pump(Tick{});
    LEANAT_CHECK(op.published() && f.host.requests == 1);
    LEANAT_CHECK(!op.advance(c).value());
    LEANAT_CHECK(!op.result());
    f.pump(Tick{3});
    c = f.context(Tick{3});
    auto done = op.advance(c);
    LEANAT_CHECK(done && done.value());
    auto result = op.result();
    LEANAT_CHECK(result && result.value().success() &&
                 result.value().terminal_effective_time == Tick{3});
    LEANAT_CHECK(result.value().data == Bytes({9, 0, 9, 0}));
    LEANAT_CHECK(f.host.acks == 0 && f.runtime.drains().outstanding() == 0);
    LEANAT_CHECK(f.runtime.results().read(observer.value()));
    LEANAT_CHECK(f.runtime.results().release(observer.value()));
    LEANAT_CHECK(f.runtime.results().occupied() == 0);
  }
  {
    HelperFixture f;
    f.host.completed = false;
    TransactOperation op(f.runtime, f.admission);
    auto c = f.context();
    LEANAT_CHECK(op.start(c, ConnectionId{1}, TransportId{2}, payload));
    LEANAT_CHECK(op.advance(c));
    f.pump(Tick{});
    LEANAT_CHECK(!op.advance(c).value());
    f.pump(Tick{3});
    c = f.context(Tick{3});
    LEANAT_CHECK(!op.advance(c).value());
    LEANAT_CHECK(f.host.acks == 0);
    f.pump(Tick{3});
    LEANAT_CHECK(f.host.acks == 1);
    LEANAT_CHECK(!op.result());
    f.pump(Tick{5});
    c = f.context(Tick{5});
    LEANAT_CHECK(op.advance(c).value());
    LEANAT_CHECK(op.result().value().terminal_effective_time == Tick{3});
  }
  {
    HelperFixture f;
    TransactOperation op(f.runtime, f.admission);
    auto c = f.context();
    LEANAT_CHECK(op.start(c, ConnectionId{1}, TransportId{3}, payload));
    LEANAT_CHECK(op.advance(c));
    auto cancelled = op.cancel(c);
    LEANAT_CHECK(cancelled && cancelled.value().local_only && !cancelled.value().receipt);
    f.pump(Tick{});
    LEANAT_CHECK(f.host.requests == 0 && f.runtime.drains().outstanding() == 0);
    LEANAT_CHECK(op.result().value().local_cancel && !op.result().value().terminal_effective_time);
  }
  {
    HelperFixture f;
    TransactOperation op(f.runtime, f.admission);
    auto c = f.context();
    LEANAT_CHECK(op.start(c, ConnectionId{1}, TransportId{4}, payload, true));
    TimeoutOperation timeout(f.admission, op.transaction(), ConnectionId{1}, Tick{2},
                             [&]() -> Expected<std::optional<Handle>> {
                               auto cancelled = op.cancel(c, CancelReason::Timeout);
                               if (!cancelled)
                                 return cancelled.error();
                               return cancelled.value().receipt;
                             });
    LEANAT_CHECK(timeout.start(c.ready));
    LEANAT_CHECK(timeout.advance_gate(
        c.ready, true, [&](RequestPermit p) { return op.publish_with_permit(c, p); }, true,
        [&] { return op.published(); }));
    f.pump(Tick{});
    f.pump(Tick{2});
    c = f.context(Tick{2});
    LEANAT_CHECK(timeout.record_timeout(c.ready));
    auto result = timeout.resolve(true);
    LEANAT_CHECK(result && result.value().state == TimeoutState::TimedOut &&
                 result.value().published && result.value().drain);
    f.pump(Tick{3});
    c = f.context(Tick{3});
    LEANAT_CHECK(op.reap(c).value());
    LEANAT_CHECK(f.runtime.drains().inspect(*result.value().drain).value().state ==
                 DrainState::Complete);
    LEANAT_CHECK(f.runtime.drains().release_receipt(*result.value().drain));
  }
  {
    HelperFixture f;
    TransactOperation small(f.runtime, f.admission, {}, 1);
    auto started = small.start(f.context(), ConnectionId{1}, TransportId{9}, payload);
    LEANAT_CHECK(!started && started.error().code == ErrorCode::Capacity);
    LEANAT_CHECK(f.host.requests == 0 && f.runtime.drains().outstanding() == 0 &&
                 f.runtime.results().occupied() == 0);
  }
  {
    HelperFixture f;
    f.host.completed = false;
    TransactOperation op(f.runtime, f.admission);
    auto c = f.context();
    LEANAT_CHECK(op.start(c, ConnectionId{1}, TransportId{10}, payload));
    LEANAT_CHECK(op.advance(c));
    f.pump(Tick{});
    f.pump(Tick{3});
    c = f.context(Tick{3});
    LEANAT_CHECK(!op.advance(c).value());
    auto cancelled = op.cancel(c);
    LEANAT_CHECK(cancelled && cancelled.value().receipt);
    f.pump(Tick{3});
    LEANAT_CHECK(f.host.acks == 1);
    LEANAT_CHECK(op.reap(c).value());
    LEANAT_CHECK(op.published());
    LEANAT_CHECK(f.runtime.drains().release_receipt(*cancelled.value().receipt));
  }
  {
    HelperFixture f;
    f.host.completed = false;
    TransactOperation op(f.runtime, f.admission);
    auto c = f.context();
    LEANAT_CHECK(op.start(c, ConnectionId{1}, TransportId{11}, payload));
    auto transferred = op.subscribe_result(0);
    auto independent = op.subscribe_result(7);
    LEANAT_CHECK(transferred && independent);
    LEANAT_CHECK(op.advance(c));
    f.pump(Tick{});
    f.pump(Tick{3});
    c = f.context(Tick{3});
    FinishResponseOperation small(f.runtime, op, 1);
    LEANAT_CHECK(!small.start(c, transferred.value()));
    LEANAT_CHECK(f.runtime.results().describe(transferred.value()));
    LEANAT_CHECK(f.host.acks == 0);
    FinishResponseOperation finish(f.runtime, op);
    LEANAT_CHECK(finish.start(c, transferred.value()));
    LEANAT_CHECK(!f.runtime.results().describe(transferred.value()));
    LEANAT_CHECK(!finish.advance(c).value());
    f.pump(Tick{3});
    LEANAT_CHECK(finish.advance(c).value());
    LEANAT_CHECK(finish.result().value().success());
    LEANAT_CHECK(f.runtime.results().read(independent.value()));
    LEANAT_CHECK(f.runtime.results().release(independent.value()));
    LEANAT_CHECK(f.runtime.results().occupied() == 0 && f.host.acks == 1);
  }
}
