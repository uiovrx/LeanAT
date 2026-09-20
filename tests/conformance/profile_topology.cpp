#include "harness.hpp"
#include "profile_context.hpp"
#include <iostream>
#include <leanat/crossbar.hpp>
namespace conformance::profile {
#define ROUTE_REQUIRE(x) conformance::require(bool(x), "C-T29: " #x)
struct ConformanceRouterHost : RuntimeHost {
  Runtime *runtime{};
  ConnectionId downstream;
  Recorder records;
  std::vector<WireCall> calls;
  std::vector<std::uint64_t> responses;
  Expected<WireReturn> transport(const SendIntent &i) override {
    WireCall call{i.call_id, i.connection,   i.transport, i.flow,
                  i.phase,   runtime->now(), {},          i.payload};
    records.add("sendIntent", i);
    records.add("wireCall", call);
    auto started = runtime->start_outbound(i, call);
    if (!started)
      return started.error();
    auto hop = take(runtime->protocol().find_ledger(i.connection, i.transport));
    records.add("afterCallLedger", take(runtime->protocol().inspect(hop)));
    calls.push_back(call);
    WireReturn returned;
    if (i.connection == downstream && i.phase == begin_req)
      returned = {Sync::Updated, end_req, Duration{}, {}};
    else if (i.phase == begin_resp) {
      responses.push_back(i.transport.value);
      returned = {Sync::Completed,
                  {},
                  Duration{},
                  ResponseSnapshot{i.payload.status, i.payload.data, false, {}}};
    } else
      returned = {Sync::Accepted, {}, Duration{}, {}};
    records.add("wireReturn", returned);
    return returned;
  }
  void arm(std::optional<WakePoint> value) override {
    if (value)
      records.add("wake", Json::object({{"time", observe(value->time)},
                                        {"turn", observe(value->turn)},
                                        {"generation", observe(value->generation)}}));
  }
  void publish_output(PortId p, const Value &v) override {
    records.add("output", Json::object({{"port", observe(p)}, {"value", observe(v)}}));
  }
  void emit_trace(const TraceEvent &t) override {
    records.add("trace", t);
  }
  void observe_milestone(const RuntimeMilestoneObservation &m) override {
    records.add("effectiveMilestone", m);
  }
};
struct RoutingRun {
  Json input, observations;
};
RoutingRun run_routes(ProfileContext &context, bool reverse) {
  ConformanceRouterHost host;
  Recorder inputs;
  std::uint64_t handler_invocations = 0, total_fuel = 0;
  auto cfg = context.config();
  const auto &system = *context.project().system_metadata;
  require(system.address_maps.size() == 1, "routing descriptor must select one map");
  const auto &region = system.address_maps.front();
  require(region.decoder_instance_id == cfg.instance.value, "primary instance must own decoder");
  std::vector<ConnectionId> upstreams;
  ConnectionId downstream;
  bool found = false;
  for (const auto &b : system.bindings) {
    if (b.sink_endpoint.instance_id == region.decoder_instance_id)
      upstreams.push_back(ConnectionId{b.id});
    if (b.source_endpoint.instance_id == region.decoder_instance_id &&
        b.source_endpoint.endpoint == region.output_endpoint &&
        b.source_endpoint.binding_index == region.binding_index) {
      downstream = ConnectionId{b.id};
      found = true;
    }
  }
  require(found && upstreams.size() == 2 && region.size >= 256, "descriptor routing shape");
  const auto base = region.source_start, target = region.target_start;
  CrossbarConfig map{{AddressRegion{base, region.size, target, downstream}},
                     upstreams,
                     {downstream},
                     8,
                     8,
                     {},
                     {}};
  host.downstream = downstream;
  Runtime runtime(cfg, host);
  host.runtime = &runtime;
  take(runtime.start(context.manifest(cfg)));
  AdmissionStore admission(cfg.domain, cfg.instance, AdmissionLimits{8, 16, 8, 8, 8});
  inputs.add("runtimeConfig", cfg);
  inputs.add("hostLimits", Json::object({{"handlerInstructionFuel", observe(std::uint64_t{4096})},
                                         {"routes", observe(std::uint64_t{8})},
                                         {"grants", observe(std::uint64_t{8})},
                                         {"admissionTransactions", observe(std::uint64_t{8})},
                                         {"admissionHops", observe(std::uint64_t{16})}}));
  inputs.add("selectedMap", Json::object({{"id", observe(region.id)},
                                          {"decoder", observe(region.decoder_instance_id)},
                                          {"outputEndpoint", observe(region.output_endpoint)},
                                          {"bindingIndex", observe(region.binding_index)},
                                          {"source", observe(base)},
                                          {"size", observe(region.size)},
                                          {"target", observe(target)},
                                          {"downstream", observe(downstream)}}));
  Crossbar crossbar(map, cfg.domain, 4);
  CrossbarSession first(crossbar, runtime, admission), second(crossbar, runtime, admission);
  ExecutionContext c;
  c.kind = ContextKind::Timed;
  c.domain = cfg.domain;
  c.instance = cfg.instance;
  auto enter = [&](ConnectionId connection, TransportId transport, std::uint64_t address) {
    PayloadSnapshot payload;
    payload.command = Command::Read;
    payload.address = address;
    payload.data = Bytes(4);
    payload.streaming_width = 4;
    payload.byte_enable = {255, 0};
    AdmissionRequest request{connection, transport, 1, 0, runtime.now(), {}, payload};
    auto admitted = admission.admit(request, true);
    ROUTE_REQUIRE(admitted && admitted.value().service);
    ROUTE_REQUIRE(runtime.protocol().bind_ledger(admitted.value().hop, connection, transport));
    ROUTE_REQUIRE(runtime.drains().register_responsibility(
        Responsibility{admitted.value().txn,
                       cfg.instance,
                       0,
                       {},
                       {{admitted.value().hop, false, false, false, false}},
                       false,
                       false}));
    auto id = runtime.allocate_call_id(CallOrigin::ExternalIngress);
    ROUTE_REQUIRE(id);
    WireCall call{id.value(), connection,    transport,  Flow::Forward,
                  begin_req,  runtime.now(), Duration{}, payload};
    inputs.add("externalCall", call);
    host.records.add("externalCall", call);
    host.records.add("externalReturn", take(runtime.ingress(call)));
    host.records.add("admittedTransaction", admitted.value().txn);
    host.records.add("admittedHop", take(runtime.protocol().inspect(admitted.value().hop)));
    const auto program = context.handler(cfg.instance.value, 0).program_id;
    auto execution = context.execution(program, {runtime.now(), 0}, connection);
    host.records.add("selectedProgram", program);
    host.records.add("handlerExecution", execution);
    EventTxn tx({}, execution);
    exec::FuelCounter fuel{4096};
    exec::Interpreter vm(context.validated(), context.state_refs());
    auto ran = take(vm.execute_segment(program, execution, {}, tx, fuel));
    require(ran.kind == exec::SegmentResult::Kind::Returned, "descriptor route handler failed");
    take(runtime.commit_segment(tx));
    host.records.add("modelHandler", ran);
    host.records.add("modelState", context.state());
    total_fuel += ran.fuel_used;
    require(ran.values == std::vector<Value>{Value{++handler_invocations}},
            "route handler counter did not advance");
    return *admitted.value().service;
  };
  auto pump = [&](Tick time) {
    auto tick = runtime.pump_batch(time, 64);
    if (!tick)
      std::cerr << "pump error @" << time.value << ": " << tick.error().message << '\n';
    ROUTE_REQUIRE(tick);
    for (unsigned n = 0; n < 64 && runtime.next_wakeup() && !(time < runtime.next_wakeup()->time);
         ++n) {
      tick = runtime.pump_batch(time, 64);
      ROUTE_REQUIRE(tick);
    }
    ROUTE_REQUIRE(!runtime.next_wakeup() || time < runtime.next_wakeup()->time);
    ROUTE_REQUIRE(!runtime.stopped());
  };
  ServicePermit a, b;
  if (reverse) {
    b = enter(upstreams[1], TransportId{2}, base + 64);
    a = enter(upstreams[0], TransportId{1}, base + 128);
  } else {
    a = enter(upstreams[0], TransportId{1}, base + 128);
    b = enter(upstreams[1], TransportId{2}, base + 64);
  }
  inputs.add("callbackOrder", reverse ? "reverse" : "forward");
  pump(Tick{});
  ROUTE_REQUIRE(first.start(c, a));
  ROUTE_REQUIRE(second.start(c, b));
  ROUTE_REQUIRE(!first.advance(c).value());
  ROUTE_REQUIRE(!second.advance(c).value());
  pump(Tick{});
  ROUTE_REQUIRE(!first.advance(c).value());
  ROUTE_REQUIRE(!second.advance(c).value());
  pump(Tick{});
  for (auto child : {first.child(), second.child()}) {
    auto route = take(crossbar.inspect(*child));
    host.records.add("retainedRoute", Json::object({{"hop", observe(*child)},
                                                    {"transaction", observe(route.txn)},
                                                    {"upstream", observe(route.upstream)},
                                                    {"downstream", observe(route.downstream)},
                                                    {"ingressAddress", observe(route.ingress)},
                                                    {"egressAddress", observe(route.egress)},
                                                    {"payload", observe(route.payload)}}));
  }
  ROUTE_REQUIRE(host.calls.size() == 2 && host.calls[0].request.address == target + 128 &&
                host.calls[1].request.address == target + 64);
  auto response = [&](TransportId transport, Tick time, std::uint64_t address) {
    PayloadSnapshot payload;
    payload.command = Command::Read;
    payload.address = address;
    payload.data = Bytes(4, 7);
    payload.streaming_width = 4;
    payload.byte_enable = {255, 0};
    payload.status = ResponseStatus::Ok;
    auto id = runtime.allocate_call_id(CallOrigin::ExternalIngress);
    ROUTE_REQUIRE(id);
    WireCall call{id.value(), downstream, transport,  Flow::Backward,
                  begin_resp, time,       Duration{}, payload};
    inputs.add("externalCall", call);
    host.records.add("externalCall", call);
    auto r = runtime.ingress(call);
    if (r) {
      host.records.add("externalReturn", r.value());
      host.records.add("afterExternalResponse",
                       take(runtime.protocol().inspect(
                           take(runtime.protocol().find_ledger(downstream, transport)))));
    }
    if (!r)
      std::cerr << r.error().message << '\n';
    ROUTE_REQUIRE(r);
  };
  response(TransportId{2}, Tick{1}, target + 64);
  pump(Tick{1});
  c.ready = {Tick{1}, 0};
  ROUTE_REQUIRE(!second.advance(c).value());
  pump(Tick{1});
  ROUTE_REQUIRE(second.advance(c).value());
  ROUTE_REQUIRE(crossbar.outstanding() == 1 && host.responses == std::vector<std::uint64_t>{2});
  response(TransportId{1}, Tick{7}, target + 128);
  pump(Tick{7});
  c.ready = {Tick{7}, 0};
  ROUTE_REQUIRE(!first.advance(c).value());
  pump(Tick{7});
  ROUTE_REQUIRE(first.advance(c).value());
  ROUTE_REQUIRE(crossbar.outstanding() == 0 &&
                host.responses == std::vector<std::uint64_t>({2, 1}));
  for (auto &call : host.calls)
    if (call.phase == begin_resp) {
      ROUTE_REQUIRE(call.request.address ==
                    (call.transport == TransportId{1} ? base + 128 : base + 64));
      ROUTE_REQUIRE(call.request.data == Bytes({7, 0, 7, 0}));
    }
  ROUTE_REQUIRE(runtime.drains().outstanding() == 0);
  CrossbarSession unmapped(crossbar, runtime, admission);
  auto missing = enter(upstreams[0], TransportId{3}, base + region.size + 4096);
  pump(Tick{7});
  ROUTE_REQUIRE(unmapped.start(c, missing));
  ROUTE_REQUIRE(!unmapped.child());
  ROUTE_REQUIRE(!unmapped.advance(c).value());
  pump(Tick{7});
  ROUTE_REQUIRE(unmapped.advance(c).value());
  ROUTE_REQUIRE(host.calls.back().request.address == base + region.size + 4096 &&
                host.calls.back().request.status == ResponseStatus::AddressError);
  CrossbarSession cancelled(crossbar, runtime, admission);
  auto cancel_service = enter(upstreams[0], TransportId{4}, base + 32);
  pump(Tick{7});
  ROUTE_REQUIRE(cancelled.start(c, cancel_service));
  ROUTE_REQUIRE(!cancelled.advance(c).value());
  auto receipt = cancelled.cancel(c);
  ROUTE_REQUIRE(receipt && receipt.value().receipt);
  pump(Tick{7});
  ROUTE_REQUIRE(cancelled.advance(c).value());
  ROUTE_REQUIRE(crossbar.outstanding() == 0);
  ROUTE_REQUIRE(runtime.drains().inspect(*receipt.value().receipt).value().state ==
                DrainState::Complete);
  host.records.add("cancelReceipt", *receipt.value().receipt);
  const auto drain = take(runtime.drains().inspect(*receipt.value().receipt));
  std::vector<Json> drain_hops;
  for (const auto &h : drain.hops)
    drain_hops.push_back(Json::object({{"hop", observe(h.hop)},
                                       {"wireTerminal", h.wire_terminal},
                                       {"timingConsumed", h.timing_consumed},
                                       {"cleanupReturned", h.cleanup_returned},
                                       {"callPin", h.call_pin}}));
  host.records.add("cancelDrain", Json::object({{"state", observe(drain.state)},
                                                {"reason", observe(drain.reason)},
                                                {"hops", Json::array(drain_hops)}}));
  ROUTE_REQUIRE(runtime.drains().release_receipt(*receipt.value().receipt));
  for (auto &call : host.calls)
    ROUTE_REQUIRE(!(call.connection == downstream && call.transport == TransportId{4}));
  host.records.add("finalModelState", context.state());
  host.records.add("finalRoutes", crossbar.outstanding());
  host.records.add("finalDrainBacklog", runtime.drains().outstanding());
  return {inputs.json(), Json::object({{"records", host.records.json()},
                                       {"stop", "ScenarioCompleted"},
                                       {"fuel", observe(total_fuel)}})};
}
#undef ROUTE_REQUIRE
} // namespace conformance::profile

int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  try {
    auto context =
        conformance::take(conformance::profile::ProfileContext::load(argv[2], argv[3], argv[4]));
    std::ofstream output(argv[1]);
    conformance::require(bool(output), "routing output unavailable");
    std::string scenario = argv[5];
    using namespace conformance::profile;
    if (scenario == "topology") {
      auto run = run_routes(*context, false);
      output << context->record("C-T29", run.input, run.observations, {"ooo-routes"}).dump()
             << '\n';
    } else if (scenario == "callback-orders") {
      auto forward = run_routes(*context, false);
      auto second = conformance::take(ProfileContext::load(argv[2], argv[3], argv[4]));
      auto reverse = run_routes(*second, true);
      output << context
                    ->record("C-T24",
                             Json::object({{"forward", forward.input}, {"reverse", reverse.input}}),
                             Json::object({{"forward", forward.observations},
                                           {"reverse", reverse.observations}}),
                             {"callback-order-forward", "callback-order-reverse"})
                    .dump()
             << '\n';
    } else
      throw std::runtime_error("unknown routing scenario");
    conformance::require(bool(output), "routing output failed");
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
