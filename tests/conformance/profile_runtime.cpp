#include "../support/event_queue_test_access.hpp"
#include "leanat/admission.hpp"
#include "leanat/memory.hpp"
#include "leanat/object_services.hpp"
#include "profile_runtime_vm.hpp"
#include <iostream>
using namespace leanat;
using namespace conformance;
using namespace conformance::profile;
namespace {
struct Host : RuntimeHost {
  Runtime *runtime{};
  Recorder &rec;
  std::function<WireReturn(const SendIntent &)> peer;
  std::vector<TraceEvent> traces;
  unsigned calls{};
  Bytes external;
  explicit Host(Recorder &r) : rec(r) {}
  Expected<WireReturn> transport(const SendIntent &i) override {
    WireCall call{i.call_id, i.connection,   i.transport, i.flow,
                  i.phase,   runtime->now(), {},          i.payload};
    rec.add("wire.call", call);
    auto start = runtime->start_outbound(i, call);
    if (!start) {
      rec.add("wire.start.error", start.error());
      return start.error();
    }
    ++calls;
    auto result = peer(i);
    rec.add("wire.return", result);
    return result;
  }
  void arm(std::optional<WakePoint> w) override {
    rec.add("host.arm",
            w ? Json::object(
                    {{"time", observe(w->time)}, {"turn", w->turn}, {"generation", w->generation}})
              : Json{});
  }
  void publish_output(PortId, const Value &) override {
    require(false, "output must be scoped");
  }
  void publish_output(InstanceId i, PortId p, const Value &v) override {
    rec.add("output",
            Json::object({{"instance", observe(i)}, {"port", observe(p)}, {"value", observe(v)}}));
  }
  void emit_trace(const TraceEvent &t) override {
    traces.push_back(t);
    rec.add("host.trace", t);
  }
  void observe_milestone(const RuntimeMilestoneObservation &m) override {
    rec.add("milestone", m);
  }
};
std::uint32_t program(ProfileContext &c, ContextKind kind) {
  for (const auto &p : c.project().programs)
    if (p.context == kind)
      return p.id;
  throw std::runtime_error("catalog program role missing");
}
void pump(Runtime &r, Recorder &rec, Tick time, unsigned budget = 64) {
  for (unsigned n = 0; n < 512 && !r.stopped(); ++n) {
    auto wake = r.next_wakeup();
    if (!wake || time < wake->time)
      return;
    auto result = r.pump_batch(time, budget);
    if (!result) {
      rec.add("pump.error", result.error());
      return;
    }
    auto &v = result.value();
    rec.add("pump", Json::object({{"executed", v.executed_events},
                                  {"stop", observe(v.stop_reason)},
                                  {"progress", observe(v.progress)},
                                  {"batch", observe(v.closed_batch_id)},
                                  {"complete", v.batch_complete},
                                  {"limit", v.limit_detail}}));
  }
  require(r.stopped() || !r.next_wakeup() || time < r.next_wakeup()->time,
          "fixture pump did not finish");
}
Json hop_record(const HopRecord &h) {
  return Json::object({{"txn", observe(h.txn)},
                       {"hop", observe(h.hop)},
                       {"connection", observe(h.connection)},
                       {"transport", observe(h.transport)},
                       {"generation", h.transport_generation},
                       {"inTime", observe(h.in_time)},
                       {"wireTerminal", h.wire_terminal},
                       {"semanticTerminal", h.semantic_terminal},
                       {"pending", h.pending},
                       {"servicing", h.servicing},
                       {"sequence", h.sequence},
                       {"request", observe(h.owned_request)}});
}
std::uint64_t pending(ProfileContext &c, Runtime &r, Recorder &rec) {
  auto id = program(c, ContextKind::Timed);
  auto ctx = c.execution(id, {}, {}, 7);
  std::vector<ConnectionId> connections;
  for (const auto &b : c.config().connection_bindings)
    if (b.target == ctx.instance)
      connections.push_back(b.connection);
  require(connections.size() >= 2, "pending fixture needs two loaded target connections");
  AdmissionStore admission(ctx.domain, ctx.instance, {4, 4, 1, 4, 4});
  RuntimeVmSession vm(c, r, rec);
  vm.bind_core();
  vm.freeze();
  std::vector<AdmissionDisposition> owned;
  for (unsigned n = 0; n < 2; ++n) {
    AdmissionRequest q;
    q.connection = connections[n];
    q.transport = TransportId{n + 1};
    q.owner = 7;
    q.request.command = Command::Read;
    q.request.data = {std::uint8_t(8 + n)};
    rec.add("admission.request", q.request);
    auto accepted = take(admission.admit(q, true));
    owned.push_back(accepted);
    rec.add("admission", hop_record(take(admission.inspect(accepted.hop))));
    rec.add("service.active", admission.active_services());
  }
  require(owned[0].service && owned[1].retained_pending && !owned[1].service,
          "pending admission not retained");
  require(admission.active_services() == 1 &&
              take(admission.inspect(owned[1].hop)).owned_request.data == Bytes{9},
          "pending owning request lost");
  for (unsigned n = 0; n < 2; ++n) {
    auto service = n ? take(admission.promote_pending(connections[n], Tick{1})) : owned[0].service;
    require(bool(service), "pending not promoted");
    ctx.ready = {Tick{n}, 0};
    ctx.connection = connections[n];
    auto result = take(vm.execute(id, {}, ctx));
    require(result.kind == exec::SegmentResult::Kind::Returned, "service VM did not return");
    auto record = take(admission.inspect(owned[n].hop));
    take(admission.queue_response(*service, ctx.ready,
                                  {ResponseStatus::Ok, record.owned_request.data, false, {}}));
    auto engine = take(r.protocol(ctx.instance));
    take(engine->bind_ledger(record.hop, record.connection, record.transport, 1));
    auto cid = take(r.allocate_call_id(CallOrigin::ExternalIngress));
    WireCall call{cid, record.connection,   record.transport, Flow::Forward, begin_req, Tick{n},
                  {},  record.owned_request};
    rec.add("retire.call", call);
    auto ticket = take(engine->begin_call(call));
    WireReturn reply{Sync::Completed,
                     {},
                     Duration{},
                     ResponseSnapshot{ResponseStatus::Ok, record.owned_request.data, false, {}}};
    rec.add("retire.return", reply);
    auto exchange = take(engine->end_call(ticket, reply));
    rec.add("retire.ledger", take(engine->inspect(record.hop)));
    take(r.release_call_id(cid));
    take(admission.mark_wire_terminal(record.hop, exchange));
    take(admission.mark_semantic_terminal(record.hop, Tick{n}));
    take(admission.retire(record.hop));
    rec.add("service.active", admission.active_services());
  }
  require(admission.active_services() == 0, "pending service leak");
  return vm.fuel_used();
}
std::uint64_t response_wait(ProfileContext &c, Runtime &r, Host &host, Recorder &rec) {
  auto id = program(c, ContextKind::Process);
  auto ctx = c.execution(id, {}, {}, 7);
  ConnectionId connection;
  for (const auto &b : c.config().connection_bindings)
    if (b.initiator == ctx.instance) {
      connection = b.connection;
      break;
    }
  require(connection.value != 0, "response waiter has no loaded outgoing connection");
  auto engine = take(r.protocol(ctx.instance));
  auto hop = take(engine->create_ledger(connection, TransportId{1}, 1, 7));
  host.peer = [](const SendIntent &) {
    return WireReturn{
        Sync::Completed, {}, Duration{}, ResponseSnapshot{ResponseStatus::Ok, {4, 9}, false, {}}};
  };
  SendIntent request;
  request.connection = connection;
  request.transport = TransportId{1};
  request.call_id = take(r.allocate_call_id(CallOrigin::Outgoing));
  request.not_before = Tick{2};
  request.payload.command = Command::Read;
  request.payload.data = {0, 0};
  request.payload.streaming_width = 2;
  take(r.publish(request));
  pump(r, rec, Tick{2});
  auto latch = take(r.response_wait_outcome(hop));
  rec.add("response.durable",
          Json::object({{"value", observe(latch.value)}, {"ready", observe(latch.source)}}));
  require(latch.value == Value{Value::Array{Value{std::uint64_t(ResponseStatus::Ok)},
                                            Value{Bytes{4, 9}}, Value{false}}},
          "wrong durable response");
  RuntimeVmSession vm(c, r, rec);
  vm.bind_core();
  vm.freeze();
  vm.schedule_process(id, {Value{hop}}, ReadyKey{Tick{3}, 0});
  pump(r, rec, Tick{3});
  require(!r.stopped() && vm.resumes() == 1 && r.processes().wait_count() == 0,
          "response wait lost or repeated wake");
  require(vm.last_result() && vm.last_result()->kind == exec::SegmentResult::Kind::Returned &&
              vm.last_result()->values == std::vector<Value>{latch.value, Value{std::uint64_t{41}}},
          "response outcome or saved continuation value corrupt");
  require(c.state().size() == 1 && c.state()[0].value == Value{std::uint64_t{41}},
          "response VM state did not commit");
  require(take(r.response_wait_outcome(hop)).value == latch.value,
          "response latch consumed by wait");
  rec.add("response.afterWait", take(r.response_wait_outcome(hop)).value);
  return vm.fuel_used();
}
std::uint64_t zero_time(ProfileContext &c, Runtime &r, Recorder &rec) {
  RuntimeVmSession vm(c, r, rec);
  vm.bind_core();
  vm.freeze();
  auto id = program(c, ContextKind::Process);
  vm.schedule_process(id, {}, {});
  pump(r, rec, Tick{}, 1);
  require(r.stopped() && !r.next_wakeup() && vm.resumes() == 15,
          "compiled ready loop did not stop at configured event limit");
  rec.add("loop.resumes", vm.resumes());
  rec.add("loop.stop", r.stop_detail());
  // A separate real VM segment exhausts instruction fuel before any await commits.
  Recorder fuelRecord;
  Host h(fuelRecord);
  auto cfg = c.config();
  Runtime limited(cfg, h);
  h.runtime = &limited;
  take(limited.start(c.manifest(cfg)));
  RuntimeVmSession shortFuel(c, limited, fuelRecord, 1);
  shortFuel.bind_core();
  shortFuel.freeze();
  auto before = c.state();
  shortFuel.schedule_process(id, {}, {});
  pump(limited, fuelRecord, Tick{}, 1);
  require(limited.stopped() && shortFuel.last_error() &&
              shortFuel.last_error()->code == ErrorCode::FuelExhausted,
          "actual VM fuel did not stop");
  for (std::size_t i = 0; i < before.size(); ++i)
    require(before[i].value == c.state()[i].value, "fuel failure committed state");
  rec.add("instructionFuelBranch", fuelRecord.json());
  return vm.fuel_used() + shortFuel.fuel_used();
}
std::uint64_t memory_rollback(ProfileContext &c, Runtime &r, Recorder &rec) {
  auto id = program(c, ContextKind::Timed);
  auto ctx = c.execution(id, {}, {}, 7);
  auto memory = std::make_shared<Memory>(take(Memory::make(2, {1, 2})));
  ObjectServiceConfig cfg;
  cfg.max_bytes = 64;
  cfg.max_entries = 8;
  auto objects = take(ObjectServices::bind_existing(
      c.project(), {{{ObjectId{1}, 35, ctx.domain, ctx.instance}, memory}}, cfg));
  RuntimeVmSession vm(c, r, rec);
  vm.bind_core();
  take(objects->register_into(vm.backend()));
  vm.freeze();
  auto old = take(r.schedule(EventDraft{}));
  rec.add("queue.preexisting", old);
  rec.add("memory.before", Bytes(memory->data(), memory->data() + memory->size()));
  auto result = vm.execute(id, {}, ctx);
  require(!result && result.error().code == ErrorCode::Capacity,
          "full queue VM schedule did not fail with Capacity");
  require(Bytes(memory->data(), memory->data() + memory->size()) == Bytes({1, 2}) &&
              r.queue().occupied() == 1,
          "ObjectCall leaked before schedule failure");
  rec.add("memory.after", Bytes(memory->data(), memory->data() + memory->size()));
  rec.add("queue.occupied", r.queue().occupied());
  return vm.fuel_used();
}
std::uint64_t external_prefix(ProfileContext &c, Runtime &r, Host &host, Recorder &rec) {
  auto id = program(c, ContextKind::Timed);
  auto ctx = c.execution(id, {}, {}, 7);
  RuntimeVmSession vm(c, r, rec);
  vm.bind_core();
  vm.freeze();
  take(vm.execute(id, {}, ctx));
  auto committed = c.state();
  auto binding = c.config().connection_bindings.front();
  auto engine = take(r.protocol(binding.initiator));
  auto hop = take(engine->create_ledger(binding.connection, TransportId{1}, 1, 7));
  host.peer = [&](const SendIntent &i) {
    host.external = i.payload.data;
    rec.add("peer.visibleWrite", host.external);
    return WireReturn{Sync::Updated, end_resp, {}, {}};
  };
  SendIntent request;
  request.connection = binding.connection;
  request.transport = TransportId{1};
  request.call_id = take(r.allocate_call_id(CallOrigin::Outgoing));
  request.payload.command = Command::Write;
  request.payload.data = {77};
  request.payload.streaming_width = 1;
  take(r.publish(request));
  pump(r, rec, Tick{});
  auto final = take(engine->inspect(hop));
  rec.add("faulted.ledger", final);
  rec.add("peer.final", host.external);
  rec.add("state.final", c.state());
  require(r.stopped() && final.faulted && host.external == Bytes{77} && host.calls == 1 &&
              !r.next_wakeup(),
          "external visible prefix lost");
  require(std::any_of(host.traces.begin(), host.traces.end(),
                      [](auto &t) { return t.kind == "runtime.stopped"; }),
          "fatal trace absent");
  for (std::size_t i = 0; i < committed.size(); ++i)
    require(committed[i].value == c.state()[i].value, "prior committed VM state rolled back");
  return vm.fuel_used();
}
std::uint64_t counters(ProfileContext &c, Runtime &r, Recorder &rec) {
  using Access = leanat::testing::EventQueueTestAccess;
  auto &q = r.queue();
  take(Access::seed_free_generation(q, 0, UINT64_MAX - 1));
  auto last = take(q.enqueue(EventDraft{}));
  rec.add("generation.final", last);
  auto close = [&](EventQueue &queue, Tick time) {
    auto batch = take(queue.pop_batch(time, 1));
    require(bool(batch), "boundary batch absent");
    rec.add("batch", Json::object({{"id", observe(batch->id)},
                                   {"ready", observe(batch->ready)},
                                   {"cursor", batch->cursor},
                                   {"members", observe(batch->members)},
                                   {"complete", batch->members_complete}}));
    take(queue.ack_executed(batch->id, batch->members[0].token, ExecutionDisposition::Committed));
    take(queue.resolver_step(batch->id, 1, 0));
    take(queue.finish_batch(batch->id));
  };
  close(q, {});
  EventDraft next;
  next.key.time = Tick{1};
  auto retired = q.enqueue(next);
  require(!retired && Access::retired(q, 0) && !q.cancel(last), "generation ABA");
  rec.add("generation.error", retired.error());
  for (bool sequence : {true, false}) {
    Host h(rec);
    auto cfg = c.config();
    cfg.event_capacity = 2;
    Runtime extra(cfg, h);
    h.runtime = &extra;
    take(extra.start(c.manifest(cfg)));
    auto &queue = extra.queue();
    if (sequence)
      take(Access::seed_next_sequence(queue, UINT64_MAX - 1));
    else
      take(Access::seed_next_batch(queue, UINT64_MAX - 1));
    take(queue.enqueue(EventDraft{}));
    if (sequence) {
      auto fail = queue.enqueue(next);
      require(!fail && fail.error().code == ErrorCode::Overflow &&
                  Access::next_sequence(queue) == UINT64_MAX,
              "sequence wrapped");
      rec.add("sequence.error", fail.error());
    } else {
      close(queue, {});
      take(queue.enqueue(next));
      auto fail = queue.pop_batch(Tick{1}, 1);
      require(!fail && fail.error().code == ErrorCode::Overflow &&
                  Access::next_batch(queue) == UINT64_MAX,
              "batch wrapped");
      rec.add("batch.error", fail.error());
    }
  }
  auto time = add_time(Tick{UINT64_MAX}, Duration{1});
  auto add = checked_add(UINT64_MAX, 1);
  auto mul = checked_mul(UINT64_MAX, 2);
  require(!time && !add && !mul, "native arithmetic wrapped");
  rec.add("time.error", time.error());
  rec.add("add.error", add.error());
  rec.add("mul.error", mul.error());
  auto id = program(c, ContextKind::Timed);
  RuntimeVmSession vm(c, r, rec);
  vm.bind_core();
  vm.freeze();
  auto before = c.state();
  auto result = vm.execute(id, {}, c.execution(id, {}, {}, 7));
  require(!result && result.error().code == ErrorCode::Overflow,
          "compiled checked arithmetic did not report Overflow");
  for (std::size_t i = 0; i < before.size(); ++i)
    require(before[i].value == c.state()[i].value, "compiled overflow committed state");
  return vm.fuel_used();
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  try {
    auto context = take(ProfileContext::load(argv[2], argv[3], argv[4]));
    std::string id = argv[5];
    Recorder rec;
    Host host(rec);
    RuntimeConfig limits;
    if (id == "C-T17") {
      limits.max_events_per_tick = 16;
      limits.max_events = 64;
    }
    if (id == "C-T18" || id == "C-T27")
      limits.event_capacity = 1;
    auto cfg = context->config(limits);
    Runtime runtime(cfg, host);
    host.runtime = &runtime;
    take(runtime.start(context->manifest(cfg)));
    rec.add("runtime.config", cfg);
    std::vector<std::string> branches;
    std::uint64_t fuelUsed = 0;
    if (id == "C-T10") {
      fuelUsed = pending(*context, runtime, rec);
      branches = {"bounded-pending-owned-service-vm-retirement"};
    } else if (id == "C-T16") {
      fuelUsed = response_wait(*context, runtime, host, rec);
      branches = {"completed-before-registration-compiled-response-wait"};
    } else if (id == "C-T17") {
      fuelUsed = zero_time(*context, runtime, rec);
      branches = {"compiled-ready-wait-native-limits"};
    } else if (id == "C-T18") {
      fuelUsed = memory_rollback(*context, runtime, rec);
      branches = {"compiled-object-write-schedule-atomic-rollback"};
    } else if (id == "C-T19") {
      fuelUsed = external_prefix(*context, runtime, host, rec);
      branches = {"compiled-local-prefix-real-peer-violation"};
    } else if (id == "C-T27") {
      fuelUsed = counters(*context, runtime, rec);
      branches = {"runtime-counter-boundaries-compiled-value-overflow"};
    } else
      throw std::runtime_error("unknown runtime profile scenario");
    auto observations =
        Json::object({{"stop", runtime.stopped() ? runtime.stop_detail() : "ScenarioCompleted"},
                      {"fuel", fuelUsed},
                      {"events", rec.json()}});
    std::ofstream out(argv[1]);
    require(bool(out), "output report open");
    out << context->record(id, observe(cfg), observations, branches).dump() << '\n';
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 4;
  }
}
