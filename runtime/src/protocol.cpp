#include "leanat/protocol.hpp"
#include "leanat/storage_identity.hpp"
namespace leanat {
namespace {
bool guard_available(const ProtocolGuard &g, const WireReturn *r) {
  if (g.kind == ProtocolGuardKind::FieldEq) {
    if (g.field < 4)
      return true;
    if (g.field == 4)
      return r != nullptr;
    if (g.field == 5)
      return r && r->sync == Sync::Updated && r->phase;
    return r && r->sync != Sync::Accepted && r->response;
  }
  for (auto &operand : g.operands)
    if (!guard_available(operand, r))
      return false;
  return true;
}
bool eval_guard(const ProtocolGuard &g, const WireCall &c, const WireReturn *r) {
  if (!guard_available(g, r))
    return false;
  switch (g.kind) {
  case ProtocolGuardKind::Always:
    return true;
  case ProtocolGuardKind::And:
    return eval_guard(g.operands[0], c, r) && eval_guard(g.operands[1], c, r);
  case ProtocolGuardKind::Not:
    return !eval_guard(g.operands[0], c, r);
  case ProtocolGuardKind::FieldEq: {
    std::optional<std::uint64_t> value;
    switch (g.field) {
    case 0:
      value = static_cast<std::uint64_t>(c.request.command);
      break;
    case 1:
      value = c.request.address;
      break;
    case 2:
      value = c.incoming_delay.value;
      break;
    case 3:
      value = static_cast<std::uint64_t>(c.request.status);
      break;
    case 4:
      if (r)
        value = static_cast<std::uint64_t>(r->sync);
      break;
    case 5:
      if (r && r->sync == Sync::Updated && r->phase)
        value = r->phase->value;
      break;
    case 6:
      if (r && r->sync != Sync::Accepted && r->response)
        value = static_cast<std::uint64_t>(r->response->status);
      break;
    case 7:
      if (r && r->sync != Sync::Accepted && r->response)
        value = r->response->data.size();
      break;
    }
    return value && *value == g.value;
  }
  }
  return false;
}
} // namespace
ProtocolEngine::ProtocolEngine(DomainId d, InstanceId s, std::size_t l, std::size_t t,
                               std::size_t h)
    : domain_(d), side_(s), store_id_(storage_detail::allocate_store_incarnation()),
      ledger_limit_(l), ticket_limit_(t), history_limit_(h) {}
Expected<ProtocolEngine::Ledger *> ProtocolEngine::ledger(Handle h) {
  auto it = ledgers_.find(h);
  if (it == ledgers_.end())
    return fail(ErrorCode::StaleHandle, "unknown local ledger");
  return &it->second;
}
void ProtocolEngine::release(std::map<ConnectionId, Handle> &lanes, ConnectionId c, Handle h) {
  auto i = lanes.find(c);
  if (i != lanes.end() && i->second == h)
    lanes.erase(i);
}
bool ProtocolEngine::request_lane_free(ConnectionId c) const {
  return !requests_.count(c);
}
bool ProtocolEngine::response_lane_free(ConnectionId c) const {
  return !responses_.count(c);
}
Expected<Handle> ProtocolEngine::create_ledger(ConnectionId c, TransportId t, std::uint64_t g,
                                               std::uint64_t owner) {
  if (!g)
    return fail(ErrorCode::InvalidArgument, "zero transport generation");
  for (auto &p : prepared_bindings_)
    if (p.second.connection == c && p.second.transport == t)
      return fail(ErrorCode::Duplicate, "transport reserved by pending ledger binding");
  for (auto &p : ledgers_)
    if (p.second.snapshot.identity.connection == c && p.second.snapshot.identity.transport == t)
      return fail(ErrorCode::Duplicate, "active transport already bound on local connection");
  if (ledgers_.size() + prepared_bindings_.size() >= ledger_limit_)
    return fail(ErrorCode::Capacity, "ledger capacity");
  if (generation_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "ledger generation");
  Handle h{HandleKind::Hop, domain_, store_id_, 0, generation_++, owner};
  WireSnapshot s;
  s.identity = {domain_, side_, c, t, g};
  s.protocol_state = package_ ? package_->initial : 0;
  ledgers_.emplace(h, Ledger{s, h, {}, {}, Command::Ignore, 0});
  return h;
}
Expected<void> ProtocolEngine::retire_ledger(Handle h) {
  auto l = ledger(h);
  if (!l)
    return l.error();
  if (l.value()->snapshot.pending || l.value()->snapshot.faulted ||
      l.value()->snapshot.state != WireState::Terminal)
    return fail(ErrorCode::InvalidState, "ledger retains external responsibility");
  ledgers_.erase(h);
  return {};
}
Expected<void> ProtocolEngine::discard_unstarted_ledger(Handle h) {
  auto l = ledger(h);
  if (!l)
    return l.error();
  auto &s = l.value()->snapshot;
  if (s.pending || s.faulted || s.call_ordinal || s.state != WireState::Idle)
    return fail(ErrorCode::InvalidState, "ledger has real call responsibility");
  ledgers_.erase(h);
  return {};
}
Expected<void> ProtocolEngine::bind_ledger(Handle h, ConnectionId c, TransportId t,
                                           std::uint64_t g) {
  if (h.kind != HandleKind::Hop || h.domain != domain_ || !h.generation || !g)
    return fail(ErrorCode::WrongOwner, "hop identity cannot bind local ledger");
  if (h.generation == UINT64_MAX)
    return fail(ErrorCode::Overflow, "bound ledger generation exhausted");
  if (ledgers_.count(h) || prepared_bindings_.count(h))
    return fail(ErrorCode::Duplicate, "hop ledger already bound");
  for (auto &p : ledgers_)
    if (p.second.snapshot.identity.connection == c && p.second.snapshot.identity.transport == t)
      return fail(ErrorCode::Duplicate, "active local transport");
  for (auto &p : prepared_bindings_)
    if (p.second.connection == c && p.second.transport == t)
      return fail(ErrorCode::Duplicate, "transport reserved by pending ledger binding");
  if (ledgers_.size() + prepared_bindings_.size() >= ledger_limit_)
    return fail(ErrorCode::Capacity, "ledger capacity");
  WireSnapshot s;
  s.identity = {domain_, side_, c, t, g};
  s.protocol_state = package_ ? package_->initial : 0;
  ledgers_.emplace(h, Ledger{s, h, {}, {}, Command::Ignore, 0});
  generation_ = std::max(generation_, h.generation + 1);
  return {};
}
Expected<void> ProtocolEngine::allow_ignorable(PhaseId p) {
  if (package_)
    return fail(ErrorCode::Unsupported, "standalone phases cannot use base ignorable policy");
  if (p.value <= 4)
    return fail(ErrorCode::InvalidArgument, "base/uninitialized cannot be ignorable");
  ignorable_.insert(p.value);
  return {};
}
Expected<void> ProtocolEngine::install_rules(std::vector<ProtocolRule> rs) {
  if (!ledgers_.empty() || !prepared_bindings_.empty())
    return fail(ErrorCode::InvalidState, "rules frozen while ledgers exist");
  for (std::size_t i = 0; i < rs.size(); ++i) {
    auto &a = rs[i];
    if (a.predecessor != WireState::Idle ||
        (a.next_state != WireState::Idle && a.next_state != WireState::Terminal))
      return fail(ErrorCode::Unsupported, "stateful custom protocols require install_package");
    if (a.response_required && a.sync == Sync::Accepted)
      return fail(ErrorCode::ProtocolViolation, "ACCEPTED cannot require returned response");
    if (a.phase.value <= 4)
      return fail(ErrorCode::ProtocolViolation, "cannot override base phase");
    if (a.sync == Sync::Updated && !a.returned_phase)
      return fail(ErrorCode::InvalidArgument, "UPDATED phase required");
    for (std::size_t j = 0; j < i; ++j) {
      auto &b = rs[j];
      if (a.predecessor == b.predecessor && a.flow == b.flow && a.phase == b.phase &&
          a.sync == b.sync && (a.sync != Sync::Updated || a.returned_phase == b.returned_phase))
        return fail(ErrorCode::Duplicate, "overlapping finite rules");
    }
  }
  rules_ = std::move(rs);
  package_.reset();
  finite_lanes_.clear();
  return {};
}
Expected<CallTicket> ProtocolEngine::begin_call(const WireCall &c) {
  std::optional<Handle> found;
  for (auto &p : ledgers_)
    if (p.second.snapshot.identity.connection == c.connection &&
        p.second.snapshot.identity.transport == c.transport) {
      if (found)
        return fail(ErrorCode::InvalidState, "ambiguous local transport");
      found = p.first;
    }
  if (!found)
    return fail(ErrorCode::StaleHandle, "unregistered transport");
  return begin_call(*found, c);
}
Expected<Handle> ProtocolEngine::find_ledger(ConnectionId connection, TransportId transport) const {
  std::optional<Handle> found;
  for (auto &p : ledgers_)
    if (p.second.snapshot.identity.connection == connection &&
        p.second.snapshot.identity.transport == transport) {
      if (found)
        return fail(ErrorCode::InvalidState, "ambiguous local transport ledger");
      found = p.first;
    }
  if (!found)
    return fail(ErrorCode::StaleHandle, "unregistered local transport");
  return *found;
}
Expected<CallTicket> ProtocolEngine::begin_call(Handle h, const WireCall &c) {
  auto lp = ledger(h);
  if (!lp)
    return lp.error();
  auto &l = *lp.value();
  auto &s = l.snapshot;
  if (s.identity.connection != c.connection || s.identity.transport != c.transport)
    return fail(ErrorCode::WrongOwner, "call identity mismatch");
  if (s.faulted || s.pending)
    return fail(ErrorCode::InvalidState, "faulted or call in progress");
  for (auto &active : pending_)
    if (active.second.call.connection == c.connection && active.second.call.flow != c.flow)
      return fail(ErrorCode::ProtocolViolation, "reverse reentrancy on local connection");
  if (l.calls.count(c.id.value))
    return fail(ErrorCode::Duplicate, "duplicate call id");
  if (pending_.size() >= ticket_limit_ || l.calls.size() >= history_limit_)
    return fail(ErrorCode::Capacity, "call record capacity");
  auto input = add_time(c.call_time, c.incoming_delay);
  if (!input)
    return input.error();
  if (input.value() < s.last_timing)
    return fail(ErrorCode::TimeRegression, "input timing regression");
  bool ignored = !package_ && ignorable_.count(c.phase.value);
  auto pre = s.state;
  auto next = pre;
  std::optional<std::size_t> finite_rule;
  std::vector<ProtocolMilestone> draft;
  auto finite_lanes = finite_lanes_;
  if (package_) {
    for (std::size_t i = 0; i < package_->rules.size(); ++i) {
      auto &r = package_->rules[i];
      if (r.pre == s.protocol_state && r.flow == c.flow && r.phase == c.phase &&
          eval_guard(r.call_guard, c, nullptr)) {
        finite_rule = i;
        break;
      }
    }
    if (!finite_rule)
      return fail(ErrorCode::ProtocolViolation, "no finite call rule");
    auto &r = package_->rules[*finite_rule];
    for (auto lane : r.entry_lanes) {
      auto i = finite_lanes.find({c.connection, lane});
      if (i != finite_lanes.end() &&
          (i->second.size() >= package_->lanes[lane] || i->second.count(h)))
        return fail(ErrorCode::ProtocolViolation, "finite entry lane unavailable");
    }
    auto applied =
        finite_actions(r.call_actions, h, c, nullptr, input.value(), next, finite_lanes, draft);
    if (!applied)
      return applied.error();
  } else if (!ignored) {
    if (c.phase == begin_req) {
      if (c.flow != Flow::Forward || pre != WireState::Idle || !request_lane_free(c.connection))
        return fail(ErrorCode::ProtocolViolation, "BEGIN_REQ predecessor/direction/request lane");
      next = WireState::Request;
    } else if (c.phase == end_req) {
      if (c.flow != Flow::Backward || pre != WireState::Request)
        return fail(ErrorCode::ProtocolViolation, "END_REQ predecessor/direction");
      next = WireState::RequestReleased;
    } else if (c.phase == begin_resp) {
      if (c.flow != Flow::Backward ||
          (pre != WireState::Request && pre != WireState::RequestReleased) ||
          !response_lane_free(c.connection) || c.request.status == ResponseStatus::Incomplete)
        return fail(ErrorCode::ProtocolViolation, "BEGIN_RESP predecessor/lane/response");
      if (l.request_command == Command::Read && c.request.status == ResponseStatus::Ok &&
          c.request.data.size() != l.request_length)
        return fail(ErrorCode::ProtocolViolation, "BEGIN_RESP incomplete READ data");
      next = WireState::Response;
    } else if (c.phase == end_resp) {
      if (c.flow != Flow::Forward || pre != WireState::Response)
        return fail(ErrorCode::ProtocolViolation, "END_RESP predecessor/direction");
      next = WireState::Terminal;
    } else {
      bool match = false;
      for (auto &r : rules_)
        if (r.predecessor == pre && r.flow == c.flow && r.phase == c.phase)
          match = true;
      if (!match)
        return fail(ErrorCode::ProtocolViolation,
                    "mandatory/uninitialized phase or missing call rule");
    }
  }
  if (generation_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "ticket generation");
  Handle th{HandleKind::GateTicket, domain_, store_id_, 0, generation_++, h.owner};
  pending_.emplace(
      th, Pending{th, h, c, pre, input.value(), ignored, {}, {}, finite_rule, std::move(draft)});
  l.calls.insert(c.id.value);
  s.pending = true;
  ++s.call_ordinal;
  if (package_) {
    finite_lanes_ = std::move(finite_lanes);
    s.state = next;
    s.last_timing = input.value();
  } else if (!ignored) {
    s.state = next;
    s.last_timing = input.value();
    if (c.phase == begin_req) {
      l.request_command = c.request.command;
      l.request_length = c.request.data.size();
      requests_[c.connection] = h;
    }
    if (c.phase == end_req || c.phase == begin_resp)
      release(requests_, c.connection, h);
    if (c.phase == begin_resp)
      responses_[c.connection] = h;
    if (c.phase == end_resp)
      release(responses_, c.connection, h);
  }
  return CallTicket{th};
}
Expected<void> ProtocolEngine::fail_call(CallTicket t, Error error) {
  auto i = pending_.find(t.handle);
  if (i == pending_.end())
    return fail(ErrorCode::StaleHandle, "call ticket consumed");
  if (i->second.failure)
    return fail(ErrorCode::InvalidState, "call already failed");
  i->second.failure = std::move(error);
  ledgers_.at(i->second.hop).snapshot.faulted = true;
  return {};
}
Expected<CallFailureRecord> ProtocolEngine::inspect_failure(CallTicket t) const {
  auto i = pending_.find(t.handle);
  if (i == pending_.end())
    return fail(ErrorCode::StaleHandle, "call ticket");
  if (!i->second.failure)
    return fail(ErrorCode::NotReady, "call has not failed");
  return CallFailureRecord{i->second.call, i->second.failed_return, *i->second.failure};
}
Expected<ExchangeResult> ProtocolEngine::end_call(CallTicket t, const WireReturn &r) {
  auto it = pending_.find(t.handle);
  if (it == pending_.end())
    return fail(ErrorCode::StaleHandle, "call ticket consumed");
  auto &p = it->second;
  auto &s = ledgers_.at(p.hop).snapshot;
  if (s.faulted)
    return fail(ErrorCode::InvalidState, "failed call cannot retry");
  auto bad = [&](ErrorCode ec, const char *msg) -> Expected<ExchangeResult> {
    s.faulted = true;
    p.failed_return = r;
    p.failure = fail(ec, msg);
    return *p.failure;
  };
  ExchangeResult out;
  out.call_id = p.call.id;
  out.hop = p.hop;
  out.ignored = p.ignored;
  if (r.sync != Sync::Accepted && r.sync != Sync::Updated && r.sync != Sync::Completed)
    return bad(ErrorCode::ProtocolViolation, "unknown sync result");
  if (p.ignored) {
    if (r.sync != Sync::Accepted)
      return bad(ErrorCode::ProtocolViolation, "ignorable phase requires ACCEPTED");
    out.next_state = s.state;
    pending_.erase(it);
    s.pending = false;
    return out;
  }
  Tick time = p.input;
  if (r.sync != Sync::Accepted) {
    auto sum = add_time(p.call.call_time, r.outgoing_delay);
    if (!sum)
      return bad(sum.error().code, "return timing overflow");
    time = sum.value();
    if (time < s.last_timing)
      return bad(ErrorCode::TimeRegression, "return timing regression");
  }
  if (package_) {
    auto &original = package_->rules[*p.finite_rule];
    const FiniteProtocolRule *winner = nullptr;
    for (auto &rule : package_->rules) {
      if (rule.pre != original.pre || rule.flow != p.call.flow || rule.phase != p.call.phase ||
          rule.sync != r.sync || (r.sync == Sync::Updated && rule.returned != r.phase) ||
          !eval_guard(rule.guard, p.call, &r))
        continue;
      if (!winner || rule.priority.value_or(0) < winner->priority.value_or(0))
        winner = &rule;
    }
    if (!winner)
      return bad(ErrorCode::ProtocolViolation, "unmatched finite return rule");
    auto lanes = finite_lanes_;
    auto state = s.state;
    out.milestones = p.draft;
    auto applied = finite_actions(winner->return_actions, p.hop, p.call, &r, time, state, lanes,
                                  out.milestones);
    if (!applied)
      return bad(applied.error().code, applied.error().message.c_str());
    for (const auto *actions :
         {static_cast<const std::vector<ProtocolAction> *>(&original.call_actions),
          &winner->return_actions})
      for (auto &action : *actions)
        if (action.kind == ProtocolActionKind::TraceTag)
          out.trace_tags.push_back(action.tag);
    finite_lanes_ = std::move(lanes);
    s.state = state;
    s.protocol_state = winner->post;
    if (r.sync != Sync::Accepted)
      s.last_timing = time;
    out.next_state = state;
    out.wire_terminal = state == WireState::Terminal;
    out.needs_ack = state == WireState::Response;
    out.authentic_ = true;
    out.proof_terminal_ = out.wire_terminal;
    out.proof_hop_ = p.hop;
    out.proof_time_ = time;
    s.pending = false;
    pending_.erase(it);
    return out;
  }
  auto phase = p.call.phase;
  bool terminal = false, shortcut = false, req_release = false;
  if (phase == begin_req) {
    if (r.sync == Sync::Updated) {
      if (!r.phase || (*r.phase != end_req && *r.phase != begin_resp))
        return bad(ErrorCode::ProtocolViolation, "invalid BEGIN_REQ updated phase");
      req_release = true;
      shortcut = *r.phase == begin_resp;
    } else if (r.sync == Sync::Completed) {
      req_release = true;
      shortcut = true;
      terminal = true;
    }
  } else if (phase == end_req) {
    if (r.sync != Sync::Accepted)
      return bad(ErrorCode::ProtocolViolation, "END_REQ must ACCEPT");
  } else if (phase == begin_resp) {
    if (r.sync == Sync::Updated && (!r.phase || *r.phase != end_resp))
      return bad(ErrorCode::ProtocolViolation, "BEGIN_RESP update requires END_RESP");
    terminal = r.sync != Sync::Accepted;
  } else if (phase == end_resp) {
    if (r.sync == Sync::Updated)
      return bad(ErrorCode::ProtocolViolation, "END_RESP cannot UPDATE");
    terminal = true;
  } else {
    const ProtocolRule *rule = nullptr;
    for (auto &x : rules_)
      if (x.predecessor == p.pre && x.flow == p.call.flow && x.phase == phase && x.sync == r.sync &&
          (r.sync != Sync::Updated || x.returned_phase == r.phase))
        rule = &x;
    if (!rule)
      return bad(ErrorCode::ProtocolViolation, "no complete extension exchange rule");
    if (rule->response_required && (r.sync == Sync::Accepted || !r.response ||
                                    r.response->status == ResponseStatus::Incomplete))
      return bad(ErrorCode::ProtocolViolation, "missing extension response");
    s.state = rule->next_state;
    terminal = s.state == WireState::Terminal;
  }
  if (shortcut && (!r.response || r.response->status == ResponseStatus::Incomplete))
    return bad(ErrorCode::ProtocolViolation, "shortcut response unavailable");
  auto &source = ledgers_.at(p.hop);
  if (shortcut && source.request_command == Command::Read &&
      r.response->status == ResponseStatus::Ok && r.response->data.size() != source.request_length)
    return bad(ErrorCode::ProtocolViolation, "shortcut incomplete READ data");
  if (shortcut && !response_lane_free(p.call.connection))
    return bad(ErrorCode::ProtocolViolation, "shortcut response lane occupied");
  auto milestone = [&](MilestoneKind k, Tick at, bool implicit = false,
                       std::optional<ResponseSnapshot> response = std::nullopt) {
    out.milestones.push_back({k, at, implicit, std::move(response)});
  };
  if (phase == end_req)
    milestone(MilestoneKind::RequestReleased, p.input);
  if (phase == begin_resp) {
    if (p.pre == WireState::Request)
      milestone(MilestoneKind::RequestReleased, p.input, true);
    auto &q = p.call.request;
    milestone(MilestoneKind::ResponseReady, p.input, false,
              ResponseSnapshot{q.status, q.data, q.dmi_hint, q.extensions});
  }
  if (req_release) {
    release(requests_, p.call.connection, p.hop);
    s.state = WireState::RequestReleased;
    milestone(MilestoneKind::RequestReleased, time, shortcut);
  }
  if (shortcut) {
    responses_[p.call.connection] = p.hop;
    s.state = WireState::Response;
    milestone(MilestoneKind::ResponseReady, time, true, r.response);
  }
  if (terminal) {
    release(requests_, p.call.connection, p.hop);
    release(responses_, p.call.connection, p.hop);
    s.state = WireState::Terminal;
    milestone(MilestoneKind::Terminal, time);
  }
  if (r.sync != Sync::Accepted)
    s.last_timing = time;
  out.next_state = s.state;
  out.wire_terminal = s.state == WireState::Terminal;
  out.needs_ack = s.state == WireState::Response;
  out.authentic_ = true;
  out.proof_terminal_ = out.wire_terminal;
  out.proof_hop_ = p.hop;
  out.proof_time_ = time;
  s.pending = false;
  pending_.erase(it);
  return out;
}
Expected<ExchangeResult> ProtocolEngine::validate_end_call(CallTicket t,
                                                           const WireReturn &r) const {
  auto copy = *this;
  auto result = copy.end_call(t, r);
  if (result)
    result.value().authentic_ = false;
  return result;
}
Expected<WireSnapshot> ProtocolEngine::inspect(Handle h) const {
  auto i = ledgers_.find(h);
  if (i == ledgers_.end())
    return fail(ErrorCode::StaleHandle, "unknown local ledger");
  return i->second.snapshot;
}
Expected<VersionedCell *> ProtocolEngine::ack_cell(Handle h, std::uint64_t epoch) {
  auto l = ledger(h);
  if (!l)
    return l.error();
  if (l.value()->snapshot.state != WireState::Response || l.value()->snapshot.faulted)
    return fail(ErrorCode::InvalidState, "hop has no response awaiting acknowledgement");
  auto &cell = l.value()->ack;
  if (!cell)
    cell = VersionedCell{Value{false}, 0, epoch, false};
  if (cell->epoch != epoch)
    return fail(ErrorCode::WrongOwner, "acknowledgement epoch mismatch");
  return &*cell;
}
Expected<ProtocolDrainPlan> ProtocolEngine::request_local_cancel(Handle h) {
  auto l = ledger(h);
  if (!l)
    return l.error();
  auto s = l.value()->snapshot.state;
  return ProtocolDrainPlan{h, s == WireState::Request,
                           s == WireState::Request || s == WireState::RequestReleased,
                           s == WireState::Response, s == WireState::Terminal};
}
} // namespace leanat
