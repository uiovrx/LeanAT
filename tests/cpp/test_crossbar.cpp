#include "leanat/crossbar.hpp"
#include "test_support.hpp"
using namespace leanat;
void routed_transport();
int main() {
  routed_transport();
  CrossbarConfig c{{{0x1000, 256, 0, ConnectionId{2}},
                    {0x2000, 256, 0, ConnectionId{2}},
                    {UINT64_MAX, 1, UINT64_MAX, ConnectionId{2}}},
                   {ConnectionId{1}, ConnectionId{3}},
                   {ConnectionId{2}},
                   4,
                   8,
                   Duration{2},
                   Duration{3}};
  LEANAT_CHECK(Crossbar::validate_map(c));
  auto bad = c;
  bad.regions[2].size = 2;
  LEANAT_CHECK(!Crossbar::validate_map(bad));
  bad = c;
  bad.regions[1].source_start = 0x1080;
  LEANAT_CHECK(!Crossbar::validate_map(bad));
  Crossbar x(c, DomainId{}, 1);
  LEANAT_CHECK(x.decode(UINT64_MAX));
  LEANAT_CHECK(!x.decode(0x1100));
  auto route = *x.decode(0x1080);
  LEANAT_CHECK(x.translate(route, 0x1080).value() == 128);
  PayloadSnapshot p;
  p.command = Command::Read;
  p.address = 0x10ff;
  p.streaming_width = 1;
  p.data = Bytes(8);
  LEANAT_CHECK(x.translate_payload(route, p));
  p.streaming_width = 2;
  LEANAT_CHECK(!x.translate_payload(route, p));
  p.command = Command::Ignore;
  p.streaming_width = 0;
  LEANAT_CHECK(x.translate_payload(route, p));
  LEANAT_CHECK(!x.translate(Route{2, 0}, 0x1000));
  LEANAT_CHECK(x.debug_prefix(route, 0x10ff, 100, 100).value() == 1);
  LEANAT_CHECK(!x.request_ready(Tick{UINT64_MAX}));
  AdmissionStore admission(DomainId{}, InstanceId{}, AdmissionLimits{4, 8, 4, 4, 4});
  AdmissionRequest req;
  req.connection = ConnectionId{1};
  req.transport = TransportId{1};
  req.request = p;
  auto admitted = admission.admit(req, true);
  LEANAT_CHECK(admitted);
  auto h = x.forward(admission, route, admitted.value().txn, ConnectionId{1}, TransportId{1}, p);
  LEANAT_CHECK(h);
  p.address = 999;
  LEANAT_CHECK(x.inspect(h.value()).value().ingress == 0x10ff);
  LEANAT_CHECK(x.route_response(h.value()).value() == ConnectionId{1});
  LEANAT_CHECK(x.retain(h.value()));
  LEANAT_CHECK(x.cancel(h.value()));
  LEANAT_CHECK(x.release(h.value()));
  LEANAT_CHECK(x.terminal(h.value()));
  LEANAT_CHECK(x.outstanding() == 1);
  LEANAT_CHECK(x.release(h.value()));
  LEANAT_CHECK(!x.inspect(h.value()));
  Handle forged{HandleKind::Transaction, DomainId{}, 99, 0, 1, 0};
  LEANAT_CHECK(!x.forward(admission, route, forged, ConnectionId{1}, TransportId{2}, req.request));
  Bytes backing(512);
  RouteDmiGrant grant{backing.data(), 0, 511, 3, Duration{1}, Duration{2}};
  RouteDmiQuery query{ConnectionId{1}, 0x1080, Command::Read, 3};
  auto d = x.translate_dmi(route, grant, query, Duration{2});
  LEANAT_CHECK(d && d.value() && d.value()->start == 0x1000 && d.value()->end == 0x10ff &&
               d.value()->read_latency == Duration{3});
  query.upstream = ConnectionId{3};
  query.address = 0x2080;
  LEANAT_CHECK(x.translate_dmi(*x.decode(query.address), grant, query));
  LEANAT_CHECK(!x.invalidate(ConnectionId{2}, 64, 127, 1));
  auto inv = x.invalidate(ConnectionId{2}, 64, 127, 2);
  LEANAT_CHECK(inv && inv.value().size() == 2 && inv.value()[0].start == 0x1040);
  auto rest = x.invalidate(ConnectionId{2}, 128, 255, 2);
  LEANAT_CHECK(rest && rest.value().size() == 2);
  grant.end = 31;
  query.address = 0x1080;
  LEANAT_CHECK(!x.translate_dmi(route, grant, query));
  c.regions[0].target_start = 64;
  Crossbar y(c, DomainId{}, 2);
  grant.end = 511;
  query.address = 0x1080;
  auto adjusted = y.translate_dmi(*y.decode(0x1080), grant, query);
  LEANAT_CHECK(adjusted && adjusted.value()->pointer == backing.data() + 64);
  c.regions[0].affine = false;
  Crossbar z(c, DomainId{}, 3);
  LEANAT_CHECK(!z.translate_dmi(*z.decode(0x1080), grant, query).value());
}

struct RouterHost : RuntimeHost {
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
void routed_transport() {
  RouterHost host;
  RuntimeConfig cfg;
  cfg.descriptor_identity = "router";
  cfg.connections = {ConnectionId{1}, ConnectionId{2}, ConnectionId{3}};
  Runtime runtime(cfg, host);
  host.runtime = &runtime;
  LEANAT_CHECK(
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
    LEANAT_CHECK(admitted && admitted.value().service);
    LEANAT_CHECK(runtime.protocol().bind_ledger(admitted.value().hop, connection, transport));
    LEANAT_CHECK(runtime.drains().register_responsibility(
        Responsibility{admitted.value().txn,
                       InstanceId{},
                       0,
                       {},
                       {{admitted.value().hop, false, false, false, false}},
                       false,
                       false}));
    auto id = runtime.allocate_call_id(CallOrigin::ExternalIngress);
    LEANAT_CHECK(id);
    LEANAT_CHECK(runtime.ingress(WireCall{id.value(), connection, transport, Flow::Forward,
                                          begin_req, runtime.now(), Duration{}, payload}));
    return *admitted.value().service;
  };
  auto pump = [&](Tick time) {
    auto tick = runtime.pump_batch(time, 64);
    if (!tick)
      std::cerr << "pump error @" << time.value << ": " << tick.error().message << '\n';
    LEANAT_CHECK(tick);
    for (unsigned n = 0; n < 64 && runtime.next_wakeup() && !(time < runtime.next_wakeup()->time);
         ++n)
      auto tick = runtime.pump_batch(time, 64);
    if (!tick)
      std::cerr << "pump error @" << time.value << ": " << tick.error().message << '\n';
    LEANAT_CHECK(tick);
    LEANAT_CHECK(!runtime.stopped());
  };
  auto a = enter(ConnectionId{1}, TransportId{1}, 0x1080);
  auto b = enter(ConnectionId{3}, TransportId{2}, 0x1040);
  pump(Tick{});
  LEANAT_CHECK(first.start(c, a));
  LEANAT_CHECK(second.start(c, b));
  LEANAT_CHECK(!first.advance(c).value());
  LEANAT_CHECK(!second.advance(c).value());
  pump(Tick{});
  LEANAT_CHECK(!first.advance(c).value());
  LEANAT_CHECK(!second.advance(c).value());
  pump(Tick{});
  LEANAT_CHECK(host.calls.size() == 2 && host.calls[0].request.address == 128 &&
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
    LEANAT_CHECK(id);
    auto r = runtime.ingress(WireCall{id.value(), ConnectionId{2}, transport, Flow::Backward,
                                      begin_resp, time, Duration{}, payload});
    if (!r)
      std::cerr << r.error().message << '\n';
    LEANAT_CHECK(r);
  };
  bool partial_retirement = false;
  runtime.set_milestone_handler(
      [&](const RuntimeMilestone &m, ReadyKey ready, Runtime &) -> Expected<void> {
        if (m.hop == b.hop && m.milestone.kind == MilestoneKind::Terminal) {
          auto current = c;
          current.ready = ready;
          auto done = second.advance(current);
          if (!done)
            return done.error();
          partial_retirement = !done.value();
        }
        return {};
      });
  response(TransportId{2}, Tick{1}, 64);
  pump(Tick{1});
  c.ready = {Tick{1}, 0};
  LEANAT_CHECK(!second.advance(c).value());
  pump(Tick{1});
  LEANAT_CHECK(partial_retirement);
  LEANAT_CHECK(second.advance(c).value());
  runtime.set_milestone_handler({});
  LEANAT_CHECK(crossbar.outstanding() == 1 && host.responses == std::vector<std::uint64_t>{2});
  response(TransportId{1}, Tick{7}, 128);
  pump(Tick{7});
  c.ready = {Tick{7}, 0};
  LEANAT_CHECK(!first.advance(c).value());
  pump(Tick{7});
  LEANAT_CHECK(first.advance(c).value());
  LEANAT_CHECK(crossbar.outstanding() == 0 && host.responses == std::vector<std::uint64_t>({2, 1}));
  for (auto &call : host.calls)
    if (call.phase == begin_resp) {
      LEANAT_CHECK(call.request.address == (call.transport == TransportId{1} ? 0x1080 : 0x1040));
      LEANAT_CHECK(call.request.data == Bytes({7, 0, 7, 0}));
    }
  LEANAT_CHECK(runtime.drains().outstanding() == 0);
  CrossbarSession unmapped(crossbar, runtime, admission);
  auto missing = enter(ConnectionId{1}, TransportId{3}, 0x3000);
  pump(Tick{7});
  LEANAT_CHECK(unmapped.start(c, missing));
  LEANAT_CHECK(!unmapped.child());
  LEANAT_CHECK(!unmapped.advance(c).value());
  pump(Tick{7});
  LEANAT_CHECK(unmapped.advance(c).value());
  LEANAT_CHECK(host.calls.back().request.address == 0x3000 &&
               host.calls.back().request.status == ResponseStatus::AddressError);
  CrossbarSession cancelled(crossbar, runtime, admission);
  auto cancel_service = enter(ConnectionId{1}, TransportId{4}, 0x1020);
  pump(Tick{7});
  LEANAT_CHECK(cancelled.start(c, cancel_service));
  LEANAT_CHECK(!cancelled.advance(c).value());
  auto receipt = cancelled.cancel(c);
  LEANAT_CHECK(receipt && receipt.value().receipt);
  pump(Tick{7});
  LEANAT_CHECK(cancelled.advance(c).value());
  LEANAT_CHECK(crossbar.outstanding() == 0);
  LEANAT_CHECK(runtime.drains().inspect(*receipt.value().receipt).value().state ==
               DrainState::Complete);
  LEANAT_CHECK(runtime.drains().release_receipt(*receipt.value().receipt));
  for (auto &call : host.calls)
    LEANAT_CHECK(!(call.connection == ConnectionId{2} && call.transport == TransportId{4}));
}
