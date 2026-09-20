#pragma once
struct ConformanceLifecycleHost : RuntimeHost {
  Runtime *runtime{};
  unsigned requests{}, acknowledgements{}, writes{};
  std::vector<TraceEvent> traces;
  Expected<WireReturn> transport(const SendIntent &i) override {
    take(runtime->start_outbound(
        i, {i.call_id, i.connection, i.transport, i.flow, i.phase, runtime->now(), {}, i.payload}));
    if (i.phase == begin_req) {
      ++requests;
      if (i.payload.command == Command::Write)
        ++writes;
    }
    if (i.phase == end_resp)
      ++acknowledgements;
    return WireReturn{Sync::Accepted, {}, {}, {}};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &event) override { traces.push_back(event); }
};
inline void conformance_pump(Runtime &runtime, Tick time) {
  for (unsigned n = 0; n < 128; ++n) {
    take(runtime.pump_batch(time, 64));
    auto wake = runtime.next_wakeup();
    if (!wake || time < wake->time)
      return;
  }
  require(false, "runtime did not quiesce within128 batches");
}
inline std::string conformance_late_response(bool write) {
  ConformanceLifecycleHost host;
  RuntimeConfig config;
  config.descriptor_identity = "conformance-late";
  config.connections = {ConnectionId{1}};
  Runtime runtime(config, host);
  host.runtime = &runtime;
  take(runtime.start({{}, config.descriptor_identity, config.connections, true, true}));
  AdmissionStore admission({}, {}, {8, 8, 4, 8, 8});
  TransactOperation operation(runtime, admission);
  ExecutionContext context;
  context.kind = ContextKind::Process;
  PayloadSnapshot request;
  request.command = write ? Command::Write : Command::Read;
  request.data = {4, 5};
  request.streaming_width = 2;
  take(operation.start(context, ConnectionId{1}, TransportId{1}, request));
  conformance_pump(runtime, Tick{});
  take(operation.advance(context));
  conformance_pump(runtime, Tick{});
  take(operation.advance(context));
  require(host.requests == 1, "request not published exactly once");
  conformance_pump(runtime, Tick{2});
  context.ready = {Tick{2}, 0};
  auto cancelled = take(operation.cancel(context, CancelReason::Timeout));
  require(cancelled.receipt.has_value(), "published timeout has no drain receipt");
  auto local = take(operation.result());
  require(local.local_cancel == CancelReason::Timeout, "timeout local result missing");
  request.status = ResponseStatus::Ok;
  request.data = {9, 8};
  auto id = take(runtime.allocate_call_id(CallOrigin::ExternalIngress));
  take(runtime.ingress(
      {id, ConnectionId{1}, TransportId{1}, Flow::Backward, begin_resp, Tick{7}, {}, request}));
  conformance_pump(runtime, Tick{7});
  context.ready = {Tick{7}, 0};
  take(operation.reap(context));
  auto final = take(operation.result());
  require(final.local_cancel == CancelReason::Timeout && final.data == local.data,
          "late response overwrote timeout");
  require(host.acknowledgements == 1 && host.requests == 1 && host.writes == (write ? 1u : 0u),
          "late response did not drain ACK or repeated write");
  require(runtime.drains().outstanding() == 0, "drain responsibility leaked");
  return write ? "one WRITE published; local Timeout unchanged after late response7; END_RESP "
                 "once; drain0"
               : "READ local Timeout2 unchanged by response7; END_RESP once; drain0";
}
inline std::string conformance_zero_time(bool empty_all) {
  ConformanceLifecycleHost host;
  RuntimeConfig config;
  config.descriptor_identity = "conformance-zero";
  config.max_events_per_tick = 4;
  config.max_events = 32;
  Runtime runtime(config, host);
  host.runtime = &runtime;
  take(runtime.start({{}, config.descriptor_identity, {}, true, true}));
  auto process = take(runtime.processes().create(ProgramId{1}, 1));
  take(runtime.processes().begin(process));
  ResultStore results;
  WaitGroupStore waits({}, 1, 1, 1, 512, results);
  std::vector<std::uint64_t> turns;
  runtime.set_handler([&](const QueuedEvent &event, Runtime &rt) -> Expected<void> {
    turns.push_back(event.event.key.turn);
    if (empty_all) {
      WaitCreate c;
      c.mode = WaitMode::All;
      c.branch_count = 0;
      c.owner_process = process;
      c.owner_scope = {HandleKind::Scope, {}, 1, 0, 1, 0};
      auto h = take(waits.create(c));
      take(waits.commit(h));
      require(take(waits.resolve_closed_batch(BatchId{turns.size()}, true)).size() == 1,
              "empty All not immediately complete");
      take(waits.release(h, take(waits.result_handle(h, c.owner_scope)), c.owner_scope));
    } else {
      SingleWaitSpec wait;
      wait.until = Tick{};
      auto token = take(
          rt.processes().suspend(process, wait, {BlockId{1}, {}}, {Tick{}, event.event.key.turn}));
      take(rt.processes().take_resume(token));
    }
    EventDraft next;
    next.key.time = Tick{};
    next.key.turn = event.event.key.turn + 1;
    take(rt.schedule(next));
    return {};
  });
  take(runtime.schedule(EventDraft{}));
  for (unsigned n = 0; n < 16 && !runtime.stopped(); ++n)
    take(runtime.pump_batch(Tick{}, 1));
  require(runtime.stopped() && turns.size() == 4 && !runtime.next_wakeup(),
          "zero-time execution did not stop at configured budget4");
  require(turns == std::vector<std::uint64_t>({0, 1, 2, 3}), "cause chain lost turn ordering");
  require(results.occupied() == 0, "empty All result leak");
  return "four causal turns0,1,2,3 at tick0; per-tick budget4 stopped dispatch before fifth "
         "iteration; no pending wake or result leak";
}
