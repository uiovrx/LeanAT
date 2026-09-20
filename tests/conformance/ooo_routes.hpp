#pragma once
#include <iostream>
#define ROUTE_REQUIRE(x) conformance::require(bool(x), "C-T29: " #x)
struct ConformanceRouterHost : RuntimeHost {
  Runtime *runtime{};
  std::vector<WireCall> calls;
  std::vector<std::uint64_t> responses;
  Expected<WireReturn> transport(const SendIntent &i) override {
    WireCall call{i.call_id, i.connection,   i.transport, i.flow,
                  i.phase,   runtime->now(), {},          i.payload};
    auto started = runtime->start_outbound(i, call);
    if (!started)
      return started.error();
    calls.push_back(call);
    if (i.connection == ConnectionId{2} && i.phase == begin_req)
      return WireReturn{Sync::Updated, end_req, Duration{}, {}};
    if (i.phase == begin_resp) {
      responses.push_back(i.transport.value);
      return WireReturn{Sync::Completed,
                        {},
                        Duration{},
                        ResponseSnapshot{i.payload.status, i.payload.data, false, {}}};
    }
    return WireReturn{Sync::Accepted, {}, Duration{}, {}};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
void conformance_ooo_routes() {
  ConformanceRouterHost host;
  RuntimeConfig cfg;
  cfg.descriptor_identity = "router";
  cfg.connections = {ConnectionId{1}, ConnectionId{2}, ConnectionId{3}};
  Runtime runtime(cfg, host);
  host.runtime = &runtime;
  ROUTE_REQUIRE(
      runtime.start(HostBindingManifest{DomainId{}, "router", cfg.connections, true, true}));
  AdmissionStore admission(DomainId{}, InstanceId{}, AdmissionLimits{8, 16, 8, 8, 8});
  CrossbarConfig map{{AddressRegion{0x1000, 256, 0, ConnectionId{2}}},
                     {ConnectionId{1}, ConnectionId{3}},
                     {ConnectionId{2}},
                     8,
                     8,
                     {},
                     {}};
  Crossbar crossbar(map, DomainId{}, 4);
  CrossbarSession first(crossbar, runtime, admission), second(crossbar, runtime, admission);
  ExecutionContext c;
  c.kind = ContextKind::Timed;
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
                       InstanceId{},
                       0,
                       {},
                       {{admitted.value().hop, false, false, false, false}},
                       false,
                       false}));
    auto id = runtime.allocate_call_id(CallOrigin::ExternalIngress);
    ROUTE_REQUIRE(id);
    ROUTE_REQUIRE(runtime.ingress(WireCall{id.value(), connection, transport, Flow::Forward,
                                           begin_req, runtime.now(), Duration{}, payload}));
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
  auto a = enter(ConnectionId{1}, TransportId{1}, 0x1080);
  auto b = enter(ConnectionId{3}, TransportId{2}, 0x1040);
  pump(Tick{});
  ROUTE_REQUIRE(first.start(c, a));
  ROUTE_REQUIRE(second.start(c, b));
  ROUTE_REQUIRE(!first.advance(c).value());
  ROUTE_REQUIRE(!second.advance(c).value());
  pump(Tick{});
  ROUTE_REQUIRE(!first.advance(c).value());
  ROUTE_REQUIRE(!second.advance(c).value());
  pump(Tick{});
  ROUTE_REQUIRE(host.calls.size() == 2 && host.calls[0].request.address == 128 &&
                host.calls[1].request.address == 64);
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
    auto r = runtime.ingress(WireCall{id.value(), ConnectionId{2}, transport, Flow::Backward,
                                      begin_resp, time, Duration{}, payload});
    if (!r)
      std::cerr << r.error().message << '\n';
    ROUTE_REQUIRE(r);
  };
  response(TransportId{2}, Tick{1}, 64);
  pump(Tick{1});
  c.ready = {Tick{1}, 0};
  ROUTE_REQUIRE(!second.advance(c).value());
  pump(Tick{1});
  ROUTE_REQUIRE(second.advance(c).value());
  ROUTE_REQUIRE(crossbar.outstanding() == 1 && host.responses == std::vector<std::uint64_t>{2});
  response(TransportId{1}, Tick{7}, 128);
  pump(Tick{7});
  c.ready = {Tick{7}, 0};
  ROUTE_REQUIRE(!first.advance(c).value());
  pump(Tick{7});
  ROUTE_REQUIRE(first.advance(c).value());
  ROUTE_REQUIRE(crossbar.outstanding() == 0 &&
                host.responses == std::vector<std::uint64_t>({2, 1}));
  for (auto &call : host.calls)
    if (call.phase == begin_resp) {
      ROUTE_REQUIRE(call.request.address == (call.transport == TransportId{1} ? 0x1080 : 0x1040));
      ROUTE_REQUIRE(call.request.data == Bytes({7, 0, 7, 0}));
    }
  ROUTE_REQUIRE(runtime.drains().outstanding() == 0);
  CrossbarSession unmapped(crossbar, runtime, admission);
  auto missing = enter(ConnectionId{1}, TransportId{3}, 0x3000);
  pump(Tick{7});
  ROUTE_REQUIRE(unmapped.start(c, missing));
  ROUTE_REQUIRE(!unmapped.child());
  ROUTE_REQUIRE(!unmapped.advance(c).value());
  pump(Tick{7});
  ROUTE_REQUIRE(unmapped.advance(c).value());
  ROUTE_REQUIRE(host.calls.back().request.address == 0x3000 &&
                host.calls.back().request.status == ResponseStatus::AddressError);
  CrossbarSession cancelled(crossbar, runtime, admission);
  auto cancel_service = enter(ConnectionId{1}, TransportId{4}, 0x1020);
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
  ROUTE_REQUIRE(runtime.drains().release_receipt(*receipt.value().receipt));
  for (auto &call : host.calls)
    ROUTE_REQUIRE(!(call.connection == ConnectionId{2} && call.transport == TransportId{4}));
}

#undef ROUTE_REQUIRE
