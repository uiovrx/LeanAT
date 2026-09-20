#include "leanat/protocol.hpp"
namespace leanat {
namespace {
bool guard_equal(const ProtocolGuard &a, const ProtocolGuard &b) {
  if (a.kind != b.kind || a.field != b.field || a.value != b.value ||
      a.operands.size() != b.operands.size())
    return false;
  for (std::size_t i = 0; i < a.operands.size(); ++i)
    if (!guard_equal(a.operands[i], b.operands[i]))
      return false;
  return true;
}
bool disjoint(const ProtocolGuard &a, const ProtocolGuard &b) {
  if (a.kind == ProtocolGuardKind::FieldEq && b.kind == ProtocolGuardKind::FieldEq)
    return a.field == b.field && a.value != b.value;
  if (a.kind == ProtocolGuardKind::Not)
    return guard_equal(a.operands[0], b);
  if (b.kind == ProtocolGuardKind::Not)
    return guard_equal(a, b.operands[0]);
  return false;
}
bool actions_equal(const std::vector<ProtocolAction> &a, const std::vector<ProtocolAction> &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].kind != b[i].kind || a[i].lane != b[i].lane || a[i].tag != b[i].tag)
      return false;
  return true;
}
bool same_call(const FiniteProtocolRule &a, const FiniteProtocolRule &b) {
  return a.pre == b.pre && a.flow == b.flow && a.phase == b.phase;
}
Expected<void> check_guard(const ProtocolGuard &g, std::uint32_t fields, std::uint32_t depth,
                           std::size_t &work) {
  if (depth > 32 || ++work > 4096)
    return fail(ErrorCode::Capacity, "protocol guard depth/work");
  std::size_t arity = 0;
  switch (g.kind) {
  case ProtocolGuardKind::Always:
    break;
  case ProtocolGuardKind::FieldEq:
    if (g.field >= fields)
      return fail(ErrorCode::ProtocolViolation, "guard reads unavailable/illegal fact");
    break;
  case ProtocolGuardKind::And:
    arity = 2;
    break;
  case ProtocolGuardKind::Not:
    arity = 1;
    break;
  default:
    return fail(ErrorCode::InvalidArgument, "unknown guard opcode");
  }
  if (g.operands.size() != arity)
    return fail(ErrorCode::InvalidArgument, "guard arity");
  for (auto &x : g.operands) {
    auto c = check_guard(x, fields, depth + 1, work);
    if (!c)
      return c;
  }
  return {};
}
} // namespace
Expected<void> validate_protocol_package(const ProtocolPackage &p) {
  if (p.name.empty() || p.version.empty() || p.states.empty() || p.phases.empty() ||
      p.rules.empty() || p.terminal.empty())
    return fail(ErrorCode::InvalidArgument, "protocol identity/finite table missing");
  if (p.states.size() > 256 || p.phases.size() > 256 || p.rules.size() > 4096 ||
      p.lanes.size() > 32)
    return fail(ErrorCode::Capacity, "protocol table limit");
  if (p.initial >= p.states.size())
    return fail(ErrorCode::InvalidArgument, "initial state");
  std::set<std::string> names;
  for (auto &s : p.states)
    if (s.empty() || !names.insert(s).second)
      return fail(ErrorCode::Duplicate, "state key");
  names.clear();
  for (auto &s : p.phases)
    if (s.name.empty() || !names.insert(s.name).second)
      return fail(ErrorCode::Duplicate, "phase key");
  std::set<std::uint32_t> terminals;
  for (auto t : p.terminal)
    if (t >= p.states.size() || !terminals.insert(t).second)
      return fail(ErrorCode::InvalidArgument, "terminal state");
  for (auto c : p.lanes)
    if (!c)
      return fail(ErrorCode::InvalidArgument, "zero lane capacity");
  std::size_t validation_work = 0;
  std::vector<std::size_t> rule_work;
  for (std::size_t i = 0; i < p.rules.size(); ++i) {
    auto &r = p.rules[i];
    if (r.pre >= p.states.size() || r.post >= p.states.size() || terminals.count(r.pre) ||
        r.phase.value >= p.phases.size() || p.phases[r.phase.value].flow != r.flow)
      return fail(ErrorCode::ProtocolViolation, "rule state/phase/flow");
    if (r.sync == Sync::Updated ? (!r.returned || r.returned->value >= p.phases.size())
                                : bool(r.returned))
      return fail(ErrorCode::ProtocolViolation, "meaningful returned phase shape");
    std::size_t work = 0;
    auto g = check_guard(r.guard, 8, 1, work);
    if (!g)
      return g;
    g = check_guard(r.call_guard, 4, 1, work);
    if (!g)
      return g;
    work += r.call_actions.size() + r.return_actions.size() + 1;
    if (work > 1000000 - validation_work)
      return fail(ErrorCode::Capacity, "protocol validation work limit");
    validation_work += work;
    rule_work.push_back(work);
    std::set<std::uint32_t> entries;
    for (auto lane : r.entry_lanes)
      if (lane >= p.lanes.size() || !entries.insert(lane).second)
        return fail(ErrorCode::InvalidArgument, "entry lane");
    bool close = false;
    std::set<std::pair<ProtocolActionKind, std::uint32_t>> actions;
    for (auto *list : {&r.call_actions, &r.return_actions}) {
      if (list->size() > 128)
        return fail(ErrorCode::Capacity, "protocol action limit");
      for (auto &a : *list) {
        if (a.kind > ProtocolActionKind::TraceTag)
          return fail(ErrorCode::InvalidArgument, "action opcode");
        if ((a.kind == ProtocolActionKind::AcquireLane ||
             a.kind == ProtocolActionKind::ReleaseLane) &&
            a.lane >= p.lanes.size())
          return fail(ErrorCode::InvalidArgument, "action lane");
        if (a.kind != ProtocolActionKind::TraceTag &&
            !actions
                 .insert({a.kind, (a.kind == ProtocolActionKind::AcquireLane ||
                                   a.kind == ProtocolActionKind::ReleaseLane)
                                      ? a.lane
                                      : 0})
                 .second)
          return fail(ErrorCode::ProtocolViolation, "action duplicated across call/return");
        if (a.kind == ProtocolActionKind::CloseHop) {
          if (list == &r.call_actions)
            return fail(ErrorCode::Unsupported, "finite call CloseHop unsupported");
          close = true;
        }
      }
    }
    if (close != bool(terminals.count(r.post)))
      return fail(ErrorCode::ProtocolViolation, "terminal requires exactly one CloseHop");
    for (std::size_t j = 0; j < i; ++j) {
      auto &b = p.rules[j];
      auto comparison_work = std::size_t{1};
      if (same_call(r, b))
        comparison_work += work + rule_work[j];
      if (comparison_work > 1000000 - validation_work)
        return fail(ErrorCode::Capacity, "protocol validation work limit");
      validation_work += comparison_work;
      if (same_call(r, b)) {
        if (r.entry_lanes != b.entry_lanes || !actions_equal(r.call_actions, b.call_actions) ||
            !guard_equal(r.call_guard, b.call_guard))
          return fail(ErrorCode::ProtocolViolation, "ambiguous shared call obligations");
        if (r.sync == b.sync && r.returned == b.returned && !disjoint(r.guard, b.guard) &&
            (!r.priority || !b.priority || r.priority == b.priority))
          return fail(ErrorCode::ProtocolViolation, "ambiguous exchange guard/priority");
      }
    }
  }
  return {};
}
Expected<void> ProtocolEngine::install_package(ProtocolPackage p) {
  if (!ledgers_.empty() || !prepared_bindings_.empty())
    return fail(ErrorCode::InvalidState, "protocol frozen after ledger creation");
  auto check = validate_protocol_package(p);
  if (!check)
    return check;
  package_ = std::move(p);
  rules_.clear();
  ignorable_.clear();
  return {};
}
Expected<void> ProtocolEngine::finite_actions(const std::vector<ProtocolAction> &actions,
                                              Handle hop, const WireCall &call,
                                              const WireReturn *returned, Tick time,
                                              WireState &state, LaneRegistry &lanes,
                                              std::vector<ProtocolMilestone> &milestones) const {
  bool close = false;
  for (auto &a : actions) {
    auto key = std::make_pair(call.connection, a.lane);
    switch (a.kind) {
    case ProtocolActionKind::OpenHop:
      state = WireState::Request;
      break;
    case ProtocolActionKind::AcquireLane: {
      auto &owners = lanes[key];
      if (owners.count(hop) || owners.size() >= package_->lanes[a.lane])
        return fail(ErrorCode::ProtocolViolation, "finite lane unavailable/duplicate acquire");
      owners.insert(hop);
      break;
    }
    case ProtocolActionKind::ReleaseLane: {
      auto it = lanes.find(key);
      if (it == lanes.end() || !it->second.erase(hop))
        return fail(ErrorCode::ProtocolViolation, "finite lane owner mismatch");
      if (it->second.empty())
        lanes.erase(it);
      break;
    }
    case ProtocolActionKind::RequestReleased:
      milestones.push_back({MilestoneKind::RequestReleased, time, returned != nullptr, {}});
      state = WireState::RequestReleased;
      break;
    case ProtocolActionKind::ResponseReady: {
      std::optional<ResponseSnapshot> response;
      if (returned) {
        if (returned->sync == Sync::Accepted)
          return fail(ErrorCode::ProtocolViolation, "ACCEPTED return fields unavailable");
        response = returned->response;
      } else
        response = ResponseSnapshot{call.request.status, call.request.data, call.request.dmi_hint,
                                    call.request.extensions};
      if (!response || response->status == ResponseStatus::Incomplete)
        return fail(ErrorCode::ProtocolViolation, "finite response snapshot unavailable");
      milestones.push_back(
          {MilestoneKind::ResponseReady, time, returned != nullptr, std::move(response)});
      state = WireState::Response;
      break;
    }
    case ProtocolActionKind::CloseHop:
      close = true;
      milestones.push_back({MilestoneKind::Terminal, time, false, {}});
      state = WireState::Terminal;
      break;
    case ProtocolActionKind::TraceTag:
      break;
    }
  }
  if (close)
    if (state != WireState::Terminal)
      return fail(ErrorCode::ProtocolViolation, "lifecycle action after CloseHop");
  if (close)
    for (auto &p : lanes)
      if (p.first.first == call.connection && p.second.count(hop))
        return fail(ErrorCode::ProtocolViolation, "CloseHop leaks finite lane");
  return {};
}
} // namespace leanat
