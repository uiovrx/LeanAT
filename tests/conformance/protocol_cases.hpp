#pragma once
static ProtocolPackage conformance_probe() {
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
