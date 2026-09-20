#include "leanat/protocol.hpp"
#include "test_support.hpp"
using namespace leanat;
static WireCall call(std::uint64_t id, PhaseId p, Tick time = Tick{10},
                     TransportId transport = TransportId{1}) {
  WireCall c;
  c.id = CallId{id};
  c.connection = ConnectionId{1};
  c.transport = transport;
  c.phase = p;
  c.flow = (p == end_req || p == begin_resp) ? Flow::Backward : Flow::Forward;
  c.call_time = time;
  c.request.status = ResponseStatus::Ok;
  c.request.data = {4, 5};
  return c;
}
static WireReturn ret(Sync s, std::optional<PhaseId> p = {}) {
  WireReturn r;
  r.sync = s;
  r.phase = p;
  r.response = ResponseSnapshot{ResponseStatus::Ok, {9}, false, {}};
  return r;
}
static ProtocolPackage probe() {
  ProtocolPackage p;
  p.name = "ProbeV1";
  p.version = "test-mirror";
  p.phases = {{"begin", Flow::Forward},
              {"end", Flow::Backward},
              {"response", Flow::Backward},
              {"ack", Flow::Forward}};
  p.states = {"idle", "request", "released", "response", "terminal"};
  p.terminal = {4};
  p.lanes = {1, 1};
  auto row = [&](unsigned pre, unsigned ph, Sync sync, std::optional<PhaseId> rp, unsigned post,
                 std::vector<ProtocolAction> ca, std::vector<ProtocolAction> ra,
                 std::vector<unsigned> lanes = {}) {
    FiniteProtocolRule r;
    r.pre = pre;
    r.phase = PhaseId{ph};
    r.flow = p.phases[ph].flow;
    r.sync = sync;
    r.returned = rp;
    r.post = post;
    r.call_actions = std::move(ca);
    r.return_actions = std::move(ra);
    r.entry_lanes = std::move(lanes);
    p.rules.push_back(std::move(r));
  };
  using A = ProtocolActionKind;
  std::vector<ProtocolAction> open = {{A::OpenHop, 0, {}}, {A::AcquireLane, 0, {}}};
  std::vector<ProtocolAction> release = {{A::ReleaseLane, 0, {}}, {A::RequestReleased, 0, {}}};
  std::vector<ProtocolAction> response = {{A::AcquireLane, 1, {}}, {A::ResponseReady, 0, {}}};
  std::vector<ProtocolAction> close = {{A::ReleaseLane, 1, {}}, {A::CloseHop, 0, {}}};
  row(0, 0, Sync::Accepted, {}, 1, open, {}, {0});
  row(0, 0, Sync::Updated, PhaseId{1}, 2, open, release, {0});
  row(1, 1, Sync::Accepted, {}, 2, release, {});
  row(2, 2, Sync::Accepted, {}, 3, response, {}, {1});
  row(2, 2, Sync::Updated, PhaseId{3}, 4, response, close, {1});
  row(2, 2, Sync::Completed, {}, 4, response, close, {1});
  row(3, 3, Sync::Accepted, {}, 4, {{A::ReleaseLane, 1, {}}}, {{A::CloseHop, 0, {}}});
  row(3, 3, Sync::Completed, {}, 4, {{A::ReleaseLane, 1, {}}}, {{A::CloseHop, 0, {}}});
  return p;
}
int main() {
  // Independent matrix oracle: every base call x sync x returned phase.
  for (auto phase : {begin_req, end_req, begin_resp, end_resp})
    for (auto sync : {Sync::Accepted, Sync::Updated, Sync::Completed})
      for (auto rp : {PhaseId{0}, begin_req, end_req, begin_resp, end_resp, PhaseId{99}}) {
        ProtocolEngine e(DomainId{1}, InstanceId{1}, 4, 4, 20);
        auto h = e.create_ledger(ConnectionId{1}, TransportId{1});
        LEANAT_CHECK(h);
        if (phase != begin_req) {
          auto t = e.begin_call(h.value(), call(1, begin_req));
          LEANAT_CHECK(t);
          LEANAT_CHECK(e.end_call(t.value(), ret(Sync::Accepted)));
        }
        if (phase == end_resp) {
          auto t = e.begin_call(h.value(), call(2, begin_resp));
          LEANAT_CHECK(t);
          LEANAT_CHECK(e.end_call(t.value(), ret(Sync::Accepted)));
        }
        auto ticket = e.begin_call(h.value(), call(3, phase));
        LEANAT_CHECK(ticket);
        auto result = e.end_call(ticket.value(), ret(sync, rp));
        bool valid = phase == begin_req
                         ? (sync != Sync::Updated || rp == end_req || rp == begin_resp)
                     : phase == end_req    ? sync == Sync::Accepted
                     : phase == begin_resp ? (sync != Sync::Updated || rp == end_resp)
                                           : sync != Sync::Updated;
        LEANAT_CHECK(bool(result) == valid);
        if (valid) {
          LEANAT_CHECK(!e.end_call(ticket.value(), ret(sync, rp)));
          bool terminal = (phase == end_resp) || (phase == begin_resp && sync != Sync::Accepted) ||
                          (phase == begin_req && sync == Sync::Completed);
          LEANAT_CHECK(result.value().wire_terminal == terminal);
        } else
          LEANAT_CHECK(e.inspect(h.value()).value().faulted);
      }
  {
    ProtocolEngine e(DomainId{1}, InstanceId{1}, 3, 3, 20);
    auto a = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto b = e.create_ledger(ConnectionId{1}, TransportId{2}).value();
    auto t = e.begin_call(a, call(1, begin_req)).value();
    auto malicious = ret(Sync::Accepted, PhaseId{999});
    malicious.outgoing_delay = Duration{UINT64_MAX};
    LEANAT_CHECK(e.end_call(t, malicious));
    auto overlap = e.begin_call(b, call(2, begin_req, Tick{10}, TransportId{2}));
    LEANAT_CHECK(!overlap);
    auto c = call(3, end_req);
    c.incoming_delay = Duration{100};
    t = e.begin_call(a, c).value();
    LEANAT_CHECK(e.request_lane_free(ConnectionId{1}));
    LEANAT_CHECK(e.end_call(t, malicious));
    t = e.begin_call(b, call(4, begin_req, Tick{10}, TransportId{2})).value();
    LEANAT_CHECK(e.end_call(t, malicious));
    t = e.begin_call(a, call(5, begin_resp, Tick{110})).value();
    auto r = ret(Sync::Completed);
    r.response.reset();
    r.outgoing_delay = Duration{15};
    auto x = e.end_call(t, r);
    LEANAT_CHECK(x);
    LEANAT_CHECK(x.value().milestones[0].kind == MilestoneKind::ResponseReady &&
                 x.value().milestones[0].time == Tick{110});
    LEANAT_CHECK(x.value().milestones[1].time == Tick{125});
    LEANAT_CHECK(!e.request_lane_free(ConnectionId{1}));
  }
  {
    ProtocolEngine e(DomainId{1}, InstanceId{1}, 1, 1, 10);
    auto h = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto c = call(1, begin_req, Tick{100});
    c.incoming_delay = Duration{10};
    auto t = e.begin_call(h, c).value();
    auto r = ret(Sync::Updated, begin_resp);
    r.outgoing_delay = Duration{15};
    auto x = e.end_call(t, r);
    LEANAT_CHECK(x && x.value().milestones[0].time == Tick{115});
    t = e.begin_call(h, call(2, end_resp, Tick{120})).value();
    LEANAT_CHECK(e.inspect(h).value().state == WireState::Terminal);
    r = ret(Sync::Completed, PhaseId{0});
    r.outgoing_delay = Duration{5};
    r.response.reset();
    x = e.end_call(t, r);
    LEANAT_CHECK(x && x.value().milestones.size() == 1 &&
                 x.value().milestones[0].time == Tick{125});
    LEANAT_CHECK(e.retire_ledger(h));
    LEANAT_CHECK(!e.inspect(h));
  }
  {
    ProtocolEngine e(DomainId{1}, InstanceId{1}, 1, 1, 1);
    auto h = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
    LEANAT_CHECK(e.allow_ignorable(PhaseId{90}));
    auto t = e.begin_call(h, call(1, PhaseId{90})).value();
    auto x = e.end_call(t, ret(Sync::Accepted));
    LEANAT_CHECK(x && x.value().ignored && x.value().milestones.empty() &&
                 e.request_lane_free(ConnectionId{1}));
    LEANAT_CHECK(!e.begin_call(h, call(2, begin_req)));
  }
  {
    ProtocolEngine e(DomainId{1}, InstanceId{1}, 1, 1, 10);
    LEANAT_CHECK(e.install_rules(
        {{WireState::Idle, Flow::Forward, PhaseId{8}, Sync::Accepted, {}, WireState::Idle, false},
         {WireState::Idle,
          Flow::Forward,
          PhaseId{8},
          Sync::Completed,
          {},
          WireState::Terminal,
          true}}));
    auto h = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto t = e.begin_call(h, call(1, PhaseId{8})).value();
    LEANAT_CHECK(e.end_call(t, ret(Sync::Completed)).value().wire_terminal);
  }
  {
    ProtocolEngine e(DomainId{1}, InstanceId{1}, 2, 1, 10);
    auto h = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto t = e.begin_call(h, call(1, begin_req)).value();
    LEANAT_CHECK(!e.begin_call(h, call(2, end_req)));
    auto bad = ret(Sync::Updated, begin_resp);
    bad.response.reset();
    LEANAT_CHECK(!e.validate_end_call(t, bad));
    LEANAT_CHECK(!e.inspect(h).value().faulted);
    LEANAT_CHECK(!e.end_call(t, bad));
    auto failure = e.inspect_failure(t);
    LEANAT_CHECK(failure && failure.value().call.id == CallId{1} && failure.value().returned);
    LEANAT_CHECK(!e.end_call(t, ret(Sync::Completed)));
    LEANAT_CHECK(!e.retire_ledger(h));
    LEANAT_CHECK(!e.request_lane_free(ConnectionId{1}));
    LEANAT_CHECK(e.request_local_cancel(h).value().wait_response);
  }
  {
    ProtocolEngine e(DomainId{1}, InstanceId{1}, 1, 1, 10);
    auto h = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto c = call(1, begin_req, Tick{100});
    c.incoming_delay = Duration{10};
    auto t = e.begin_call(h, c).value();
    auto r = ret(Sync::Completed);
    r.outgoing_delay = Duration{9};
    LEANAT_CHECK(!e.end_call(t, r));
    LEANAT_CHECK(e.inspect_failure(t).value().error.code == ErrorCode::TimeRegression);
  }
  for (unsigned phase = 0; phase < 4; ++phase)
    for (auto sync : {Sync::Accepted, Sync::Updated, Sync::Completed})
      for (unsigned returned = 0; returned < 5; ++returned) {
        ProtocolEngine e(DomainId{1}, InstanceId{1}, 2, 2, 20);
        LEANAT_CHECK(e.install_package(probe()));
        auto h = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
        auto finite_call = [&](unsigned id, unsigned ph) {
          auto c = call(id, PhaseId{ph});
          c.flow = ph == 0 || ph == 3 ? Flow::Forward : Flow::Backward;
          return e.begin_call(h, c);
        };
        if (phase != 0) {
          auto t = finite_call(1, 0).value();
          LEANAT_CHECK(e.end_call(t, ret(Sync::Accepted)));
        }
        if (phase >= 2) {
          auto t = finite_call(2, 1).value();
          LEANAT_CHECK(e.end_call(t, ret(Sync::Accepted)));
        }
        if (phase == 3) {
          auto t = finite_call(3, 2).value();
          LEANAT_CHECK(e.end_call(t, ret(Sync::Accepted)));
        }
        auto t = finite_call(4, phase);
        LEANAT_CHECK(t);
        auto result = e.end_call(t.value(), ret(sync, PhaseId{returned}));
        bool valid = phase == 0
                         ? (sync == Sync::Accepted || (sync == Sync::Updated && returned == 1))
                     : phase == 1 ? sync == Sync::Accepted
                     : phase == 2 ? (sync != Sync::Updated || returned == 3)
                                  : sync != Sync::Updated;
        LEANAT_CHECK(bool(result) == valid);
      }
  {
    auto p = probe();
    p.rules[0].call_guard = {ProtocolGuardKind::FieldEq, 4, 0, {}};
    LEANAT_CHECK(!validate_protocol_package(p));
    p = probe();
    p.rules.push_back(p.rules[0]);
    LEANAT_CHECK(!validate_protocol_package(p));
    p.rules[0].priority = 10;
    p.rules.back().priority = 1;
    LEANAT_CHECK(validate_protocol_package(p));
    p.rules.back().entry_lanes.clear();
    LEANAT_CHECK(!validate_protocol_package(p));
  }
  {
    ProtocolEngine e(DomainId{1}, InstanceId{1}, 2, 2, 20);
    LEANAT_CHECK(e.install_package(probe()));
    auto a = e.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto b = e.create_ledger(ConnectionId{1}, TransportId{2}).value();
    auto c = call(1, PhaseId{0});
    auto t = e.begin_call(a, c).value();
    LEANAT_CHECK(e.end_call(t, ret(Sync::Accepted)));
    c = call(2, PhaseId{0}, Tick{10}, TransportId{2});
    LEANAT_CHECK(!e.begin_call(b, c));
    LEANAT_CHECK(e.inspect(b).value().call_ordinal == 0);
  }
  {
    auto p = probe();
    p.rules[0].guard = {ProtocolGuardKind::Not, 0, 0, {{ProtocolGuardKind::FieldEq, 6, 0, {}}}};
    ProtocolEngine engine(DomainId{1}, InstanceId{1}, 2, 2, 10);
    LEANAT_CHECK(engine.install_package(p));
    auto hop = engine.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto ticket = engine.begin_call(hop, call(1, PhaseId{0})).value();
    LEANAT_CHECK(!engine.end_call(ticket, ret(Sync::Accepted)));
  }
  {
    auto p = probe();
    p.rules[0].priority = 10;
    p.rules[0].return_actions = {{ProtocolActionKind::TraceTag, 0, "low"}};
    auto preferred = p.rules[0];
    preferred.priority = 1;
    preferred.return_actions = {{ProtocolActionKind::TraceTag, 0, "preferred"}};
    p.rules.push_back(preferred);
    ProtocolEngine engine(DomainId{1}, InstanceId{1}, 2, 2, 10);
    LEANAT_CHECK(engine.install_package(p));
    auto hop = engine.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto ticket = engine.begin_call(hop, call(1, PhaseId{0})).value();
    auto result = engine.end_call(ticket, ret(Sync::Accepted));
    LEANAT_CHECK(result && result.value().trace_tags == std::vector<std::string>{"preferred"});
  }
  {
    ProtocolEngine engine(DomainId{1}, InstanceId{1}, 1, 1, 10);
    auto hop = engine.create_ledger(ConnectionId{1}, TransportId{1}).value();
    auto c = call(1, begin_req);
    c.request.command = Command::Read;
    c.request.data = {1, 2};
    auto ticket = engine.begin_call(hop, c).value();
    c.request.data.clear();
    auto response = ret(Sync::Completed);
    LEANAT_CHECK(!engine.validate_end_call(ticket, response));
    response.response->data = {8, 9};
    LEANAT_CHECK(engine.end_call(ticket, response));
  }
  return 0;
}
