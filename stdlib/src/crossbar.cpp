#include "leanat/crossbar.hpp"
#include <atomic>
#include <set>
namespace leanat {
namespace {
std::atomic<std::uint64_t> crossbar_identity{1};
std::uint64_t new_identity() {
  auto value = crossbar_identity.fetch_add(1);
  if (value == 0 || value == UINT64_MAX)
    std::terminate();
  return value;
}
} // namespace
Expected<void> Crossbar::validate_map(const CrossbarConfig &c) {
  std::set<ConnectionId> up(c.upstreams.begin(), c.upstreams.end()),
      down(c.downstreams.begin(), c.downstreams.end());
  if (up.size() != c.upstreams.size() || down.size() != c.downstreams.size())
    return fail(ErrorCode::Duplicate, "duplicate binding");
  for (std::size_t i = 0; i < c.regions.size(); ++i) {
    const auto &r = c.regions[i];
    if (!r.size || !down.count(r.target) || r.raw_permissions > 3)
      return fail(ErrorCode::InvalidArgument, "invalid map region");
    if (!checked_add(r.source_start, r.size - 1) || !checked_add(r.target_start, r.size - 1))
      return fail(ErrorCode::Overflow, "map range overflow");
    for (std::size_t j = 0; j < i; ++j) {
      const auto &s = c.regions[j];
      if (r.source_start <= s.source_start + s.size - 1 &&
          s.source_start <= r.source_start + r.size - 1)
        return fail(ErrorCode::Duplicate, "overlapping source mappings");
    }
  }
  return {};
}
Crossbar::Crossbar(CrossbarConfig c, DomainId d, std::uint64_t v)
    : config_(std::move(c)), domain_(d), version_(v), identity_(new_identity()) {
  auto ok = validate_map(config_);
  if (!ok || !v)
    throw std::invalid_argument("invalid crossbar configuration");
}
Expected<const AddressRegion *> Crossbar::region(Route r) const {
  if (r.map_identity != identity_ || r.map_version != version_ || r.index >= config_.regions.size())
    return fail(ErrorCode::StaleHandle, "stale route");
  return &config_.regions[r.index];
}
std::optional<Route> Crossbar::decode(std::uint64_t a) const {
  for (std::size_t i = 0; i < config_.regions.size(); ++i) {
    auto &r = config_.regions[i];
    if (a >= r.source_start && a - r.source_start < r.size)
      return Route{version_, i, identity_};
  }
  return {};
}
Expected<std::uint64_t> Crossbar::translate(Route r, std::uint64_t a) const {
  auto p = region(r);
  if (!p)
    return p.error();
  auto &m = *p.value();
  if (a < m.source_start || a - m.source_start >= m.size)
    return fail(ErrorCode::InvalidArgument, "unmapped address");
  if (!m.affine)
    return fail(ErrorCode::Unsupported, "non-affine mapping");
  return checked_add(m.target_start, a - m.source_start);
}
Expected<PayloadSnapshot> Crossbar::translate_payload(Route r, const PayloadSnapshot &p) const {
  auto a = translate(r, p.address);
  if (!a)
    return a.error();
  auto m = region(r).value();
  if (p.command != Command::Ignore) {
    if (!p.streaming_width)
      return fail(ErrorCode::InvalidArgument, "zero streaming width");
    auto span = std::min<std::uint64_t>(p.data.size(), p.streaming_width);
    if (span && span - 1 >= m->size - (p.address - m->source_start))
      return fail(ErrorCode::InvalidArgument, "transfer crosses mapping");
  }
  auto out = p;
  out.address = a.value();
  return out;
}
Expected<Handle> Crossbar::forward(AdmissionStore &admission, Route r, Handle tx, ConnectionId up,
                                   TransportId transport, const PayloadSnapshot &p,
                                   std::uint64_t transport_generation) {
  if (tx.kind != HandleKind::Transaction || tx.domain != domain_ || !tx.generation)
    return fail(ErrorCode::InvalidArgument, "invalid transaction identity");
  if (std::find(config_.upstreams.begin(), config_.upstreams.end(), up) == config_.upstreams.end())
    return fail(ErrorCode::WrongOwner, "invalid upstream");
  auto out = translate_payload(r, p);
  if (!out)
    return out.error();
  if (routes_.size() >= config_.max_routes)
    return fail(ErrorCode::Capacity, "route capacity");
  if (next_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "route generation exhausted");
  auto child =
      admission.create_hop(tx, region(r).value()->target, transport, transport_generation,
                           RouteSnapshot{PortId{up.value}, PortId{region(r).value()->target.value},
                                         p.address, out.value().address});
  if (!child)
    return child.error();
  Handle h = child.value();
  ++next_;
  routes_.emplace(h, RoutedRequest{h, tx, up, region(r).value()->target, transport, p.address,
                                   out.value().address, std::move(out.value()), false, false, 1});
  return h;
}
Expected<RoutedRequest> Crossbar::inspect(Handle h) const {
  auto i = routes_.find(h);
  if (i == routes_.end())
    return fail(ErrorCode::StaleHandle, "route missing");
  return i->second;
}
Expected<ConnectionId> Crossbar::route_response(Handle h) const {
  auto r = inspect(h);
  if (!r)
    return r.error();
  return r.value().upstream;
}
Expected<void> Crossbar::retain(Handle h) {
  auto i = routes_.find(h);
  if (i == routes_.end())
    return fail(ErrorCode::StaleHandle, "route missing");
  if (i->second.references == SIZE_MAX)
    return fail(ErrorCode::Overflow, "route references");
  ++i->second.references;
  return {};
}
Expected<void> Crossbar::release(Handle h) {
  auto i = routes_.find(h);
  if (i == routes_.end())
    return fail(ErrorCode::StaleHandle, "route missing");
  if (!i->second.references)
    return fail(ErrorCode::AlreadyReleased, "route reference");
  if (--i->second.references == 0 && i->second.terminal)
    routes_.erase(i);
  return {};
}
Expected<void> Crossbar::terminal(Handle h) {
  auto i = routes_.find(h);
  if (i == routes_.end())
    return fail(ErrorCode::StaleHandle, "route missing");
  i->second.terminal = true;
  if (!i->second.references)
    routes_.erase(i);
  return {};
}
Expected<void> Crossbar::cancel(Handle h) {
  auto i = routes_.find(h);
  if (i == routes_.end())
    return fail(ErrorCode::StaleHandle, "route missing");
  i->second.draining = true;
  return {};
}
Expected<Tick> Crossbar::request_ready(Tick t) const {
  return add_time(t, config_.request_delay);
}
Expected<Tick> Crossbar::response_ready(Tick t) const {
  return add_time(t, config_.response_delay);
}
Expected<std::size_t> Crossbar::debug_prefix(Route r, std::uint64_t a, std::size_t n,
                                             std::size_t limit) const {
  auto t = translate(r, a);
  if (!t)
    return t.error();
  auto m = region(r).value();
  return static_cast<std::size_t>(
      std::min<std::uint64_t>(std::min(n, limit), m->size - (a - m->source_start)));
}
Expected<std::optional<RouteDmiGrant>> Crossbar::translate_dmi(Route r, const RouteDmiGrant &g,
                                                               const RouteDmiQuery &q, Duration rc,
                                                               Duration wc) {
  auto m = region(r);
  if (!m)
    return m.error();
  if (std::find(config_.upstreams.begin(), config_.upstreams.end(), q.upstream) ==
      config_.upstreams.end())
    return fail(ErrorCode::WrongOwner, "DMI upstream");
  if (q.permissions > 3 || q.address < m.value()->source_start ||
      q.address - m.value()->source_start >= m.value()->size)
    return fail(ErrorCode::InvalidArgument, "invalid DMI query");
  if (!m.value()->affine)
    return std::optional<RouteDmiGrant>{};
  auto target = translate(r, q.address);
  if (!target)
    return target.error();
  if (q.command == Command::Ignore)
    return std::optional<RouteDmiGrant>{};
  if (!g.pointer || g.start > g.end || g.permissions > 3 || target.value() < g.start ||
      target.value() > g.end)
    return fail(ErrorCode::InvalidArgument, "invalid downstream DMI grant");
  auto permission =
      static_cast<std::uint8_t>(g.permissions & m.value()->raw_permissions & q.permissions);
  if (!(permission & (q.command == Command::Read ? 1 : 2)))
    return std::optional<RouteDmiGrant>{};
  auto low = std::max(g.start, m.value()->target_start),
       high = std::min(g.end, m.value()->target_start + m.value()->size - 1);
  if (low > high)
    return std::optional<RouteDmiGrant>{};
  auto read = checked_add(g.read_latency.value, rc.value),
       write = checked_add(g.write_latency.value, wc.value);
  if (!read)
    return read.error();
  if (!write)
    return write.error();
  if (low - g.start > PTRDIFF_MAX)
    return fail(ErrorCode::Overflow, "DMI pointer offset");
  if (grants_.size() >= config_.max_grants)
    return fail(ErrorCode::Capacity, "DMI grant records");
  RouteDmiGrant out{g.pointer + static_cast<std::ptrdiff_t>(low - g.start),
                    m.value()->source_start + (low - m.value()->target_start),
                    m.value()->source_start + (high - m.value()->target_start),
                    permission,
                    Duration{read.value()},
                    Duration{write.value()}};
  grants_.push_back({r, q.upstream, out.start, out.end});
  return std::optional<RouteDmiGrant>{out};
}
Expected<std::vector<RouteInvalidation>> Crossbar::invalidate(ConnectionId down, std::uint64_t lo,
                                                              std::uint64_t hi, std::size_t cap) {
  if (lo > hi)
    return fail(ErrorCode::InvalidArgument, "inverted invalidation");
  std::vector<RouteInvalidation> out;
  std::vector<Grant> remaining;
  for (auto &g : grants_) {
    auto &m = *region(g.route).value();
    auto gl = m.target_start + (g.start - m.source_start),
         gh = m.target_start + (g.end - m.source_start);
    auto l = std::max(lo, gl), h = std::min(hi, gh);
    if (m.target != down || l > h) {
      remaining.push_back(g);
      continue;
    }
    if (out.size() == cap)
      return fail(ErrorCode::Capacity, "invalidation fanout capacity");
    auto source_low = m.source_start + (l - m.target_start),
         source_high = m.source_start + (h - m.target_start);
    out.push_back({g.upstream, source_low, source_high});
    if (source_low > g.start)
      remaining.push_back({g.route, g.upstream, g.start, source_low - 1});
    if (source_high < g.end)
      remaining.push_back({g.route, g.upstream, source_high + 1, g.end});
  }
  if (remaining.size() > config_.max_grants)
    return fail(ErrorCode::Capacity, "invalidation remainder capacity");
  grants_.swap(remaining);
  return out;
}
} // namespace leanat
namespace leanat {
Expected<void> CrossbarSession::check(const ExecutionContext &c) const {
  if (!started_)
    return fail(ErrorCode::InvalidState, "crossbar session not started");
  if (c.kind != ContextKind::Timed && c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "crossbar requires timed context");
  if (c.domain != context_.domain || c.owner != context_.owner || c.instance != context_.instance)
    return fail(ErrorCode::WrongOwner, "crossbar session owner");
  if (c.epoch != context_.epoch && !cancelled_)
    return fail(ErrorCode::StaleHandle, "crossbar session epoch");
  if (runtime_.now() < c.ready.time)
    return fail(ErrorCode::TimeRegression, "crossbar ahead of runtime");
  return {};
}
Expected<void> CrossbarSession::start(const ExecutionContext &c, ServicePermit upstream) {
  if (started_)
    return fail(ErrorCode::Duplicate, "crossbar session already started");
  if (c.kind != ContextKind::Timed && c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "crossbar context");
  auto ingress = admission_.inspect(upstream.hop);
  if (!ingress)
    return ingress.error();
  if (ingress.value().txn != upstream.txn || !ingress.value().servicing ||
      upstream.txn.owner != c.owner || upstream.txn.domain != c.domain)
    return fail(ErrorCode::WrongOwner, "crossbar service ownership");
  auto wire = runtime_.protocol().inspect(upstream.hop);
  if (!wire)
    return wire.error();
  if (wire.value().state != WireState::Request && wire.value().state != WireState::RequestReleased)
    return fail(ErrorCode::InvalidState, "crossbar ingress not accepted");
  if (c.ready.time < upstream.ready || c.ready.time < ingress.value().in_time)
    return fail(ErrorCode::NotReady, "crossbar service not effective");
  auto request = ingress.value().owned_request;
  auto route = crossbar_.decode(request.address);
  std::optional<PayloadSnapshot> translated;
  ResponseStatus error = ResponseStatus::AddressError;
  if (route) {
    auto p = crossbar_.translate_payload(*route, request);
    if (p)
      translated = std::move(p.value());
    else if (p.error().code == ErrorCode::Unsupported)
      error = ResponseStatus::BurstError;
  }
  auto adm = admission_.prepare();
  if (!adm)
    return adm.error();
  auto drains = runtime_.drains().prepare();
  if (!drains)
    return drains.error();
  std::optional<Handle> child;
  std::optional<RequestGateTicket> gate;
  Tick request_time = c.ready.time;
  auto rollback_child = [&]() {
    if (child) {
      runtime_.protocol().discard_unstarted_ledger(*child);
      crossbar_.terminal(*child);
      crossbar_.release(*child);
    }
  };
  if (translated) {
    auto deadline = crossbar_.request_ready(c.ready.time);
    if (!deadline)
      return deadline.error();
    request_time = deadline.value();
    auto forwarded =
        crossbar_.forward(adm.value()->draft(), *route, upstream.txn, ingress.value().connection,
                          ingress.value().transport, request, ingress.value().transport_generation);
    if (!forwarded)
      return forwarded.error();
    child = forwarded.value();
    auto native = crossbar_.inspect(*child).value();
    auto bound = runtime_.protocol().bind_ledger(*child, native.downstream, native.transport,
                                                 ingress.value().transport_generation);
    if (!bound) {
      rollback_child();
      return bound.error();
    }
    auto responsible =
        drains.value()->view().add_hop(upstream.txn, DrainHop{*child, false, false, false, false});
    if (!responsible) {
      rollback_child();
      return responsible.error();
    }
    auto lane = adm.value()->draft().update_request_lane(
        native.downstream, runtime_.protocol().request_lane_free(native.downstream), c.ready);
    if (!lane) {
      rollback_child();
      return lane.error();
    }
    auto ticket =
        adm.value()->draft().request_request_gate(upstream.txn, native.downstream, c.ready);
    if (!ticket) {
      rollback_child();
      return ticket.error();
    }
    gate = ticket.value();
  }
  std::optional<Tick> error_time;
  if (!child) {
    auto when = crossbar_.response_ready(c.ready.time);
    if (!when)
      return when.error();
    auto response =
        adm.value()->draft().queue_response(upstream, ReadyKey{when.value(), c.ready.turn},
                                            ResponseSnapshot{error, request.data, false, {}});
    if (!response)
      return response.error();
    error_time = when.value();
  }
  EventTxn tx({}, c);
  auto staged = runtime_.stage_track_cleanup(tx, upstream.txn, upstream.hop, false, request);
  if (staged && child)
    staged = runtime_.stage_track_cleanup(tx, upstream.txn, *child, true, *translated);
  if (staged)
    staged = tx.stage_participant(std::move(adm.value()));
  if (staged)
    staged = tx.stage_participant(std::move(drains.value()));
  if (!staged) {
    tx.discard();
    rollback_child();
    return staged.error();
  }
  auto committed = runtime_.commit_segment(tx);
  if (!committed) {
    tx.discard();
    rollback_child();
    return committed.error();
  }
  context_ = c;
  upstream_ = upstream;
  request_ = std::move(request);
  if (translated)
    translated_ = std::move(*translated);
  child_ = child;
  gate_ = gate;
  request_time_ = request_time;
  started_ = true;
  if (!child_) {
    response_ready_ = true;
    response_time_ = *error_time;
  }
  return {};
}
Expected<void> CrossbarSession::prepare_response(const ExecutionContext &c,
                                                 ResponseSnapshot snapshot, Tick ready) {
  if (response_ready_)
    return {};
  if (request_.command == Command::Write || request_.command == Command::Ignore)
    snapshot.data = request_.data;
  else {
    if (snapshot.data.size() != request_.data.size())
      return fail(ErrorCode::ProtocolViolation, "crossbar response length");
    for (std::size_t i = 0; i < snapshot.data.size(); ++i)
      if (!request_.byte_enable.empty() && !request_.byte_enable[i % request_.byte_enable.size()])
        snapshot.data[i] = request_.data[i];
  }
  auto when = crossbar_.response_ready(ready);
  if (!when)
    return when.error();
  auto update = admission_.prepare();
  if (!update)
    return update.error();
  auto queued = update.value()->draft().queue_response(
      upstream_, ReadyKey{when.value(), c.ready.turn}, std::move(snapshot));
  if (!queued)
    return queued.error();
  EventTxn tx({}, c);
  auto staged = tx.stage_participant(std::move(update.value()));
  if (!staged)
    return staged.error();
  auto committed = runtime_.commit_segment(tx);
  if (!committed)
    return committed.error();
  response_time_ = when.value();
  response_ready_ = true;
  return {};
}
Expected<bool> CrossbarSession::advance(const ExecutionContext &c) {
  auto valid = check(c);
  if (!valid)
    return valid.error();
  if (finished_)
    return true;
  if (local_finished_)
    return finish_retirement();
  auto ingress = runtime_.protocol().inspect(upstream_.hop);
  if (!ingress)
    return ingress.error();
  if (child_) {
    auto child = runtime_.protocol().inspect(*child_);
    if (!child)
      return child.error();
    auto route = crossbar_.inspect(*child_);
    if (!route)
      return route.error();
    if (!queued_ && !cancelled_) {
      auto updated = admission_.update_request_lane(
          route.value().downstream, runtime_.protocol().request_lane_free(route.value().downstream),
          c.ready);
      if (!updated)
        return updated.error();
      auto gate = admission_.gate_state(*gate_);
      if (!gate)
        return gate.error();
      if (gate.value() == RequestGateState::Granted) {
        auto adm = admission_.prepare();
        if (!adm)
          return adm.error();
        auto permit = adm.value()->draft().consume_request_gate(*gate_);
        if (!permit)
          return permit.error();
        auto id = runtime_.allocate_call_id(CallOrigin::Outgoing);
        if (!id)
          return id.error();
        EventTxn tx({}, c);
        auto staged = runtime_.stage_publish(
            tx, SendIntent{route.value().downstream, upstream_.txn, Flow::Forward, begin_req,
                           std::max(request_time_, c.ready.time), id.value(),
                           route.value().transport, translated_});
        if (staged)
          staged = tx.stage_participant(std::move(adm.value()));
        if (staged)
          staged = runtime_.stage_request_permit(tx, id.value(), admission_, permit.value());
        if (!staged) {
          tx.discard();
          runtime_.release_call_id(id.value());
          return staged.error();
        }
        auto committed = runtime_.commit_segment(tx);
        if (!committed) {
          tx.discard();
          runtime_.release_call_id(id.value());
          return committed.error();
        }
        permit_ = permit.value();
        gate_retired_ = true;
        queued_ = true;
      }
    }
    if (queued_ && !child_started_ && child.value().call_ordinal) {
      child_started_ = true;
      auto lane = admission_.update_request_lane(
          route.value().downstream, runtime_.protocol().request_lane_free(route.value().downstream),
          c.ready);
      if (!lane)
        return lane.error();
    }
    if (!response_ready_ && !cancelled_) {
      auto response = runtime_.milestone(*child_, MilestoneKind::ResponseReady);
      if (response) {
        if (!response.value().milestone.response)
          return fail(ErrorCode::ProtocolViolation, "child response missing snapshot");
        auto prepared = prepare_response(c, *response.value().milestone.response,
                                         response.value().milestone.time);
        if (!prepared)
          return prepared.error();
      } else if (response.error().code != ErrorCode::NotReady)
        return response.error();
    }
    if (response_ready_ && !child_ack_ && !cancelled_ &&
        child.value().state == WireState::Response) {
      auto id = runtime_.allocate_call_id(CallOrigin::Outgoing);
      if (!id)
        return id.error();
      EventTxn tx({}, c);
      ResponseAcknowledger ack(runtime_.protocol(), *child_, context_.epoch);
      auto staged =
          ack.stage_ack(tx, c.ready.time, id.value(), translated_, &runtime_, upstream_.txn);
      if (!staged) {
        tx.discard();
        runtime_.release_call_id(id.value());
        return staged.error();
      }
      if (!staged.value())
        runtime_.release_call_id(id.value());
      auto committed = runtime_.commit_segment(tx);
      if (!committed) {
        tx.discard();
        runtime_.release_call_id(id.value());
        return committed.error();
      }
      child_ack_ = true;
    }
  }
  if (response_ready_ && !response_queued_ && !cancelled_) {
    auto adm = admission_.prepare();
    if (!adm)
      return adm.error();
    auto selected = adm.value()->draft().select_response(
        ingress.value().identity.connection,
        runtime_.protocol().response_lane_free(ingress.value().identity.connection));
    if (!selected)
      return selected.error();
    if (selected.value() && selected.value()->hop == upstream_.hop) {
      auto snapshot = request_;
      snapshot.status = selected.value()->response.status;
      snapshot.data = selected.value()->response.data;
      snapshot.extensions = selected.value()->response.extensions;
      snapshot.dmi_hint = selected.value()->response.dmi_hint;
      auto id = runtime_.allocate_call_id(CallOrigin::Outgoing);
      if (!id)
        return id.error();
      Expected<Tick> effective{response_time_};
      EventTxn tx({}, c);
      auto staged = runtime_.stage_publish(
          tx, SendIntent{ingress.value().identity.connection, upstream_.txn, Flow::Backward,
                         begin_resp, std::max(effective.value(), c.ready.time), id.value(),
                         ingress.value().identity.transport, std::move(snapshot)});
      if (staged)
        staged = tx.stage_participant(std::move(adm.value()));
      if (!staged) {
        tx.discard();
        runtime_.release_call_id(id.value());
        return staged.error();
      }
      auto committed = runtime_.commit_segment(tx);
      if (!committed) {
        tx.discard();
        runtime_.release_call_id(id.value());
        return committed.error();
      }
      response_permit_ = *selected.value();
      response_queued_ = true;
    }
  }
  if (response_permit_ && !response_sent_ &&
      (ingress.value().state == WireState::Response ||
       ingress.value().state == WireState::Terminal)) {
    auto sent = admission_.response_sent(*response_permit_);
    if (!sent)
      return sent.error();
    response_sent_ = true;
  }
  auto upterminal = runtime_.milestone(upstream_.hop, MilestoneKind::Terminal);
  if (!upterminal) {
    if (upterminal.error().code == ErrorCode::NotReady)
      return false;
    return upterminal.error();
  }
  std::optional<RuntimeMilestone> childterminal;
  if (child_) {
    auto terminal = runtime_.milestone(*child_, MilestoneKind::Terminal);
    if (!terminal) {
      if (terminal.error().code == ErrorCode::NotReady)
        return false;
      return terminal.error();
    }
    childterminal = terminal.value();
  }
  if (!admission_retired_) {
    if (child_) {
      auto sync =
          admission_.sync_terminal(*child_, runtime_.protocol(), childterminal->milestone.time);
      if (!sync)
        return sync.error();
      auto retired = admission_.retire(*child_);
      if (!retired)
        return retired.error();
    }
    auto sync = admission_.sync_terminal(upstream_.hop, runtime_.protocol(),
                                         upterminal.value().milestone.time);
    if (!sync)
      return sync.error();
    auto retired = admission_.retire(upstream_.hop);
    if (!retired)
      return retired.error();
    admission_retired_ = true;
  }
  if (!local_finished_) {
    auto done = runtime_.drains().finish_local(upstream_.txn);
    if (!done && done.error().code != ErrorCode::StaleHandle)
      return done.error();
    local_finished_ = true;
  }
  return finish_retirement();
}
Expected<bool> CrossbarSession::finish_retirement() {
  if (child_ && !child_retired_) {
    auto retired = runtime_.retire_hop(*child_);
    if (!retired) {
      if (retired.error().code == ErrorCode::NotReady)
        return false;
      return retired.error();
    }
    child_retired_ = true;
  }
  if (!upstream_retired_) {
    auto retired = runtime_.retire_hop(upstream_.hop);
    if (!retired) {
      if (retired.error().code == ErrorCode::NotReady)
        return false;
      return retired.error();
    }
    upstream_retired_ = true;
  }
  if (child_) {
    auto terminal = crossbar_.terminal(*child_);
    if (!terminal)
      return terminal.error();
    auto released = crossbar_.release(*child_);
    if (!released)
      return released.error();
  }
  finished_ = true;
  return true;
}
Expected<CancelDisposition> CrossbarSession::cancel(const ExecutionContext &c,
                                                    CancelReason reason) {
  auto valid = check(c);
  if (!valid)
    return valid.error();
  if (!response_ready_) {
    auto prepared = prepare_response(
        c, ResponseSnapshot{ResponseStatus::GenericError, request_.data, false, {}}, c.ready.time);
    if (!prepared)
      return prepared.error();
  }
  if (child_ && gate_ && !gate_retired_) {
    auto wire = runtime_.protocol().inspect(*child_);
    if (!wire)
      return wire.error();
    auto gate = admission_.gate_state(*gate_);
    if (!gate)
      return gate.error();
    Expected<void> action;
    if (gate.value() == RequestGateState::Consumed) {
      if (wire.value().call_ordinal)
        action = admission_.request_sent(*permit_);
      else
        action = admission_.cancel_request_permit(*permit_, c.ready);
    } else
      action = admission_.cancel_request_gate(*gate_, c.ready);
    if (!action)
      return action.error();
    action = admission_.retire_request_ticket(*gate_);
    if (!action)
      return action.error();
    gate_retired_ = true;
  }
  auto cancelled = runtime_.cancel_local(upstream_.txn, reason);
  if (!cancelled)
    return cancelled.error();
  if (child_) {
    auto wire = runtime_.protocol().inspect(*child_);
    if (wire && !wire.value().call_ordinal) {
      auto retired = admission_.retire_unstarted(*child_, runtime_.protocol());
      if (!retired)
        return retired.error();
      runtime_.untrack_cleanup(*child_);
      crossbar_.terminal(*child_);
      crossbar_.release(*child_);
      child_.reset();
    } else
      crossbar_.cancel(*child_);
  }
  cancelled_ = true;
  return cancelled.value();
}
} // namespace leanat
