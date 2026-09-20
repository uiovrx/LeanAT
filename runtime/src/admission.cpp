#include "leanat/admission.hpp"
#include "leanat/storage_identity.hpp"
namespace leanat {
AdmissionStore::AdmissionStore(DomainId d, InstanceId s, AdmissionLimits l, ResponseOrder o)
    : domain_(d), side_(s), store_id_(storage_detail::allocate_store_incarnation()), limits_(l),
      order_(o) {}
Expected<void> AdmissionStore::touch() {
  if (prepared_)
    return fail(ErrorCode::InvalidState, "admission store reserved by prepared update");
  if (revision_ > UINT64_MAX - 32)
    return fail(ErrorCode::Overflow, "admission revision exhausted");
  ++revision_;
  return {};
}
AdmissionUpdate::AdmissionUpdate(AdmissionStore &s,AdmissionUpdate* previous)
    : target_(&s), staged_(previous?previous->staged_:s), revision_(s.revision_),previous_(previous) {
  staged_.prepared_ = nullptr;
  staged_.identity_source_ = &s;
  s.prepared_ = this;
}
AdmissionUpdate::~AdmissionUpdate() {
  discard();
}
Expected<AdmissionUpdate::Checkpoint> AdmissionUpdate::checkpoint() const {
  if (finished_)
    return fail(ErrorCode::InvalidState, "closed admission update");
  Checkpoint point;
  point.state = std::unique_ptr<AdmissionStore>{new AdmissionStore(staged_)};
  return point;
}
Expected<void> AdmissionUpdate::rollback(Checkpoint &&point) {
  if (finished_ || !point.state)
    return fail(ErrorCode::InvalidState, "invalid admission checkpoint");
  staged_ = std::move(*point.state);
  point.state.reset();
  return {};
}
Expected<void> AdmissionUpdate::validate() const {
  if (finished_ || !target_ || !target_->prepared_ || target_->revision_ != revision_)
    return fail(ErrorCode::InvalidState, "admission preparation invalidated");
  if (staged_.prepared_)
    return fail(ErrorCode::InvalidState, "nested admission preparation remains open");
  return {};
}
std::size_t AdmissionUpdate::reserved_bytes() const noexcept {
  std::size_t total = sizeof(*this);
  auto add = [&](std::size_t n) { total = n > SIZE_MAX - total ? SIZE_MAX : total + n; };
  auto extensions = [&](const std::map<std::string, Bytes> &values) {
    for (auto &p : values) {
      add(sizeof(p) + 8 * sizeof(void *));
      add(p.first.capacity());
      add(p.second.capacity());
    }
  };
  for (auto &p : staged_.hops_) {
    add(sizeof(p) + 8 * sizeof(void *));
    add(p.second.owned_request.data.capacity());
    add(p.second.owned_request.byte_enable.capacity());
    extensions(p.second.owned_request.extensions);
  }
  for (auto &p : staged_.responses_) {
    add(sizeof(p) + 8 * sizeof(void *));
    add(p.second.snapshot.data.capacity());
    extensions(p.second.snapshot.extensions);
  }
  for (auto &p : staged_.gates_)
    add(sizeof(p) + 8 * sizeof(void *));
  for (auto &p : staged_.wire_busy_)
    add(sizeof(p) + 8 * sizeof(void *));
  return total;
}
void AdmissionUpdate::apply() noexcept {
  if (finished_)
    return;
  staged_.prepared_ = target_->prepared_==this?nullptr:target_->prepared_;
  staged_.identity_source_ = nullptr;
  staged_.generation_ = target_->generation_;
  *target_ = std::move(staged_);
  finished_ = true;
}
void AdmissionUpdate::discard() noexcept {
  if (!finished_ && target_ && target_->prepared_ == this)
    target_->prepared_ = previous_;
  finished_ = true;
}
Expected<std::unique_ptr<AdmissionUpdate>> AdmissionStore::prepare() {
  if (prepared_ || identity_source_)
    return fail(ErrorCode::InvalidState,
                "one prepared update per admission store; nesting prohibited");
  if (revision_ > UINT64_MAX - 32)
    return fail(ErrorCode::Overflow, "admission revision exhausted");
  return std::unique_ptr<AdmissionUpdate>{new AdmissionUpdate(*this)};
}
Expected<std::unique_ptr<AdmissionUpdate>> AdmissionStore::prepare(EventTxn&tx){auto prior=dynamic_cast<AdmissionUpdate*>(tx.participant(this));if(identity_source_||(prepared_&&prepared_!=prior))return fail(ErrorCode::InvalidState,"another admission preparation owns store");if(revision_>UINT64_MAX-32)return fail(ErrorCode::Overflow,"admission revision exhausted");return std::unique_ptr<AdmissionUpdate>{new AdmissionUpdate(*this,prior)};}
Expected<Handle> AdmissionStore::fresh(HandleKind k, std::uint64_t owner) {
  auto &source = identity_source_ ? *identity_source_ : *this;
  if (source.generation_ == UINT64_MAX || sequence_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "admission identity exhausted");
  auto generation = source.generation_++;
  generation_ = source.generation_;
  return Handle{k, domain_, store_id_, 0, generation, owner};
}
bool AdmissionStore::valid_txn(Handle t) const {
  for (auto &p : hops_)
    if (p.second.txn == t)
      return true;
  return false;
}
Expected<AdmissionDisposition> AdmissionStore::create_initiator(const AdmissionRequest &r) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  if (!r.transport_generation)
    return fail(ErrorCode::InvalidArgument, "zero transport generation");
  std::set<Handle> txns;
  for (auto &p : hops_) {
    txns.insert(p.second.txn);
    if (p.second.connection == r.connection && p.second.transport == r.transport)
      return fail(ErrorCode::Duplicate, "active local transport");
  }
  if (txns.size() >= limits_.transactions || hops_.size() >= limits_.hops)
    return fail(ErrorCode::Capacity, "initiator transaction/hop capacity");
  auto txn = fresh(HandleKind::Transaction, r.owner);
  if (!txn)
    return txn.error();
  auto hop = fresh(HandleKind::Hop, r.owner);
  if (!hop)
    return hop.error();
  hops_.emplace(hop.value(), HopRecord{txn.value(), hop.value(), r.connection, r.transport,
                                       r.transport_generation, r.route, r.in_time, false, false,
                                       false, false, sequence_++, r.request, false});
  return AdmissionDisposition{txn.value(), hop.value(), false, {}};
}
Expected<void> AdmissionStore::retire_unstarted(Handle h, ProtocolEngine &engine) {
  if (identity_source_)
    return fail(ErrorCode::Unsupported,
                "unstarted cross-store retirement requires direct scheduler cleanup");
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = hops_.find(h);
  if (i == hops_.end())
    return fail(ErrorCode::StaleHandle, "unstarted hop");
  if (i->second.servicing || i->second.pending || responses_.count(h))
    return fail(ErrorCode::InvalidState, "admission holds service/response responsibility");
  for (auto &p : gates_)
    if (p.second.txn == i->second.txn)
      return fail(ErrorCode::InvalidState, "unstarted transaction retains gate ticket");
  auto discarded = engine.discard_unstarted_ledger(h);
  if (!discarded)
    return discarded;
  hops_.erase(i);
  return {};
}
Expected<void> AdmissionStore::stage_retire_unstarted(EventTxn &tx, Handle h, ProtocolEngine &engine) {
  auto saved = tx.checkpoint();
  if (!saved) return saved.error();
  auto update = prepare(tx);
  if (!update) return update.error();
  auto &draft = update.value()->draft();
  auto found = draft.hops_.find(h);
  if (found == draft.hops_.end()) return fail(ErrorCode::StaleHandle, "unstarted hop");
  if (found->second.servicing || found->second.pending || draft.responses_.count(h))
    return fail(ErrorCode::InvalidState, "unstarted hop retains service responsibility");
  for (const auto &gate : draft.gates_)
    if (gate.second.txn == found->second.txn)
      return fail(ErrorCode::InvalidState, "unstarted hop retains a request gate");
  auto proof = engine.stage_retire_unstarted(tx, h);
  if (!proof) return proof.error();
  draft.hops_.erase(found);
  auto touched = draft.touch();
  if (!touched) {
    auto rollback = tx.rollback(std::move(saved.value()));
    return rollback ? touched : rollback;
  }
  auto staged = tx.stage_participant(std::move(update.value()));
  if (!staged) {
    auto rollback = tx.rollback(std::move(saved.value()));
    return rollback ? staged : rollback;
  }
  return {};
}
Expected<AdmissionDisposition> AdmissionStore::admit(const AdmissionRequest &r, bool lane_free) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  if (!lane_free)
    return fail(ErrorCode::ProtocolViolation, "overlapping wire request is not backpressure");
  if (!limits_.services || !r.transport_generation)
    return fail(ErrorCode::InvalidArgument, "zero service capacity/generation");
  std::set<Handle> txns;
  for (auto &p : hops_) {
    txns.insert(p.second.txn);
    if (p.second.connection == r.connection &&
        (p.second.pending || p.second.transport == r.transport))
      return fail(ErrorCode::ProtocolViolation, "pending occupied/duplicate active transport");
  }
  if (txns.size() >= limits_.transactions || hops_.size() >= limits_.hops)
    return fail(ErrorCode::Capacity, "transaction/hop reservation exhausted");
  bool service = !r.defer_service && active_services_ < limits_.services &&
                 reserved_responses_ < limits_.responses;
  auto t = fresh(HandleKind::Transaction, r.owner);
  if (!t)
    return t.error();
  auto h = fresh(HandleKind::Hop, r.owner);
  if (!h)
    return h.error();
  HopRecord rec{t.value(), h.value(),   r.connection, r.transport,    r.transport_generation,
                r.route,   r.in_time,   false,        false,          !service,
                service,   sequence_++, r.request,    r.defer_service};
  hops_.emplace(h.value(), rec);
  AdmissionDisposition out{t.value(), h.value(), !service, std::nullopt};
  if (service) {
    ++active_services_;
    ++reserved_responses_;
    out.service = ServicePermit{t.value(), h.value(), r.in_time};
  }
  return out;
}
Expected<std::optional<ServicePermit>> AdmissionStore::promote_pending(ConnectionId c, Tick now) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  if (active_services_ >= limits_.services || reserved_responses_ >= limits_.responses)
    return std::optional<ServicePermit>{};
  for (auto &p : hops_) {
    auto &r = p.second;
    if (r.connection == c && r.pending) {
      if (r.reset_deferred)
        return fail(ErrorCode::NotReady, "reset root requires verified drain promotion");
      r.pending = false;
      r.servicing = true;
      ++active_services_;
      ++reserved_responses_;
      return std::optional<ServicePermit>{
          ServicePermit{r.txn, r.hop, Tick{std::max(now.value, r.in_time.value)}}};
    }
  }
  return std::optional<ServicePermit>{};
}
Expected<Handle> AdmissionStore::create_hop(Handle t, ConnectionId c, TransportId transport,
                                            std::uint64_t generation, RouteSnapshot route) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  if (!valid_txn(t))
    return fail(ErrorCode::StaleHandle, "unknown transaction");
  if (!generation)
    return fail(ErrorCode::InvalidArgument, "zero transport generation");
  if (hops_.size() >= limits_.hops)
    return fail(ErrorCode::Capacity, "hop capacity");
  for (auto &p : hops_)
    if (p.second.connection == c && p.second.transport == transport)
      return fail(ErrorCode::Duplicate, "local transport already bound");
  auto h = fresh(HandleKind::Hop, t.owner);
  if (!h)
    return h.error();
  hops_.emplace(h.value(), HopRecord{t,
                                     h.value(),
                                     c,
                                     transport,
                                     generation,
                                     route,
                                     Tick{},
                                     false,
                                     false,
                                     false,
                                     false,
                                     sequence_++,
                                     {},
                                     false});
  return h;
}
Expected<HopRecord> AdmissionStore::inspect(Handle h) const {
  auto i = hops_.find(h);
  if (i == hops_.end())
    return fail(ErrorCode::StaleHandle, "unknown hop");
  return i->second;
}
Expected<HopRecord> AdmissionStore::lookup(Handle txn, ConnectionId connection) const {
  std::optional<HopRecord> result;
  for (auto &p : hops_)
    if (p.second.txn == txn && p.second.connection == connection) {
      if (result)
        return fail(ErrorCode::InvalidState, "ambiguous local transaction connection");
      result = p.second;
    }
  if (!result)
    return fail(ErrorCode::StaleHandle, "transaction connection has no local hop");
  return *result;
}
Expected<void> AdmissionStore::queue_response(ServicePermit p, ReadyKey ready,
                                              ResponseSnapshot response) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = hops_.find(p.hop);
  if (i == hops_.end() || i->second.txn != p.txn)
    return fail(ErrorCode::StaleHandle, "service identity");
  if (!i->second.servicing || responses_.count(p.hop))
    return fail(ErrorCode::InvalidState, "service already transferred");
  if (ready.time < i->second.in_time)
    return fail(ErrorCode::TimeRegression, "response before input");
  if (response.status == ResponseStatus::Incomplete)
    return fail(ErrorCode::InvalidArgument, "incomplete response");
  responses_.emplace(p.hop, Response{p.hop, ready, std::move(response), {}, false});
  i->second.servicing = false;
  --active_services_;
  return {};
}
Expected<std::optional<ResponsePermit>> AdmissionStore::select_response(ConnectionId c,
                                                                        bool lane_free) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  if (!lane_free)
    return std::optional<ResponsePermit>{};
  for (auto &p : responses_)
    if (hops_.at(p.first).connection == c && (p.second.permit || p.second.sent))
      return std::optional<ResponsePermit>{};
  HopRecord *first = nullptr;
  for (auto &p : hops_)
    if (p.second.connection == c && !p.second.wire_terminal &&
        (!first || p.second.sequence < first->sequence))
      first = &p.second;
  Response *choice = nullptr;
  for (auto &p : responses_) {
    auto &h = hops_.at(p.first);
    if (h.connection != c || p.second.sent)
      continue;
    if (order_ == ResponseOrder::InOrder) {
      if (first && first->hop == h.hop)
        choice = &p.second;
    } else if (!choice || p.second.ready < choice->ready ||
               (p.second.ready == choice->ready && h.sequence < hops_.at(choice->hop).sequence))
      choice = &p.second;
  }
  if (!choice)
    return std::optional<ResponsePermit>{};
  auto h = fresh(HandleKind::ResourceTicket, choice->hop.owner);
  if (!h)
    return h.error();
  choice->permit = h.value();
  return std::optional<ResponsePermit>{ResponsePermit{h.value(), choice->hop, choice->snapshot}};
}
Expected<void> AdmissionStore::cancel_response_permit(ResponsePermit p) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = responses_.find(p.hop);
  if (i == responses_.end() || i->second.permit != p.handle)
    return fail(ErrorCode::StaleHandle, "response permit");
  if (i->second.sent)
    return fail(ErrorCode::InvalidState, "response already sent");
  i->second.permit.reset();
  return {};
}
Expected<void> AdmissionStore::response_sent(ResponsePermit p) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = responses_.find(p.hop);
  if (i == responses_.end() || i->second.permit != p.handle)
    return fail(ErrorCode::StaleHandle, "response permit");
  if (i->second.sent)
    return fail(ErrorCode::Duplicate, "response already sent");
  i->second.sent = true;
  return {};
}
Expected<void> AdmissionStore::mark_wire_terminal(Handle h, const ExchangeResult &e) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = hops_.find(h);
  if (i == hops_.end())
    return fail(ErrorCode::StaleHandle, "hop");
  if (!e.proves_terminal(h))
    return fail(ErrorCode::WrongOwner, "terminal proof does not identify hop");
  if (i->second.wire_terminal)
    return fail(ErrorCode::Duplicate, "wire terminal");
  if (i->second.pending || i->second.servicing)
    return fail(ErrorCode::InvalidState, "service responsibility not handed off");
  i->second.wire_terminal = true;
  auto r = responses_.find(h);
  if (r != responses_.end()) {
    responses_.erase(r);
    --reserved_responses_;
  }
  return {};
}
Expected<void> AdmissionStore::mark_semantic_terminal(Handle h, Tick time) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = hops_.find(h);
  if (i == hops_.end())
    return fail(ErrorCode::StaleHandle, "hop");
  if (!i->second.wire_terminal || time < i->second.in_time)
    return fail(ErrorCode::InvalidState, "semantic terminal before wire/input");
  if (i->second.semantic_terminal)
    return fail(ErrorCode::Duplicate, "semantic terminal");
  i->second.semantic_terminal = true;
  return {};
}
Expected<void> AdmissionStore::sync_terminal(Handle h, const ProtocolEngine &engine, Tick time) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = hops_.find(h);
  if (i == hops_.end())
    return fail(ErrorCode::StaleHandle, "hop");
  auto observed = engine.inspect(h);
  if (!observed)
    return observed.error();
  auto &s = observed.value();
  auto &r = i->second;
  if (s.state != WireState::Terminal || s.pending || s.faulted || s.last_timing != time ||
      time < r.in_time || s.identity.connection != r.connection ||
      s.identity.transport != r.transport ||
      s.identity.transport_generation != r.transport_generation)
    return fail(ErrorCode::InvalidState, "terminal latch does not match completed local ledger");
  if (r.pending || r.servicing)
    return fail(ErrorCode::InvalidState, "service responsibility not handed off");
  if (r.semantic_terminal)
    return fail(ErrorCode::Duplicate, "semantic terminal");
  r.wire_terminal = true;
  r.semantic_terminal = true;
  auto response = responses_.find(h);
  if (response != responses_.end()) {
    responses_.erase(response);
    --reserved_responses_;
  }
  return {};
}
Expected<void> AdmissionStore::retire(Handle h) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = hops_.find(h);
  if (i == hops_.end())
    return fail(ErrorCode::StaleHandle, "hop");
  if (!i->second.wire_terminal || !i->second.semantic_terminal)
    return fail(ErrorCode::InvalidState, "unfinished hop");
  for (auto &p : gates_)
    if (p.second.txn == i->second.txn)
      return fail(ErrorCode::InvalidState, "transaction still owns gate records");
  hops_.erase(i);
  return {};
}
Expected<void> AdmissionStore::update_request_lane(ConnectionId c, bool free, ReadyKey ready) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  wire_busy_[c] = !free;
  if (!free)
    return {};
  Gate *next = nullptr;
  for (auto &p : gates_)
    if (p.second.connection == c) {
      if (p.second.state == RequestGateState::Granted ||
          p.second.state == RequestGateState::Consumed)
        return {};
      if (p.second.state == RequestGateState::Pending &&
          (!next || p.second.sequence < next->sequence))
        next = &p.second;
    }
  if (next) {
    next->state = RequestGateState::Granted;
    next->ready = ready;
  }
  return {};
}
Expected<RequestGateTicket> AdmissionStore::request_request_gate(Handle t, ConnectionId c,
                                                                 ReadyKey ready) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  if (!valid_txn(t))
    return fail(ErrorCode::StaleHandle, "gate transaction");
  std::size_t count = 0;
  for (auto &p : gates_)
    if (p.second.connection == c) {
      ++count;
      if (p.second.txn == t && p.second.state != RequestGateState::Cancelled &&
          p.second.state != RequestGateState::Sent)
        return fail(ErrorCode::Duplicate, "duplicate transaction gate request");
    }
  if (count >= limits_.request_gate_tickets)
    return fail(ErrorCode::Capacity, "gate ticket capacity");
  auto h = fresh(HandleKind::GateTicket, t.owner);
  if (!h)
    return h.error();
  gates_.emplace(h.value(), Gate{h.value(), t, c, RequestGateState::Pending, {}, sequence_++});
  update_request_lane(c, !wire_busy_[c], ready);
  return RequestGateTicket{h.value()};
}
Expected<std::optional<RequestPermit>> AdmissionStore::try_request_permit(Handle t, ConnectionId c,
                                                                          ReadyKey ready) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  if (!valid_txn(t))
    return fail(ErrorCode::StaleHandle, "gate transaction");
  if (wire_busy_[c])
    return std::optional<RequestPermit>{};
  for (auto &p : gates_)
    if (p.second.connection == c && (p.second.state == RequestGateState::Pending ||
                                     p.second.state == RequestGateState::Granted ||
                                     p.second.state == RequestGateState::Consumed))
      return std::optional<RequestPermit>{};
  auto ticket = request_request_gate(t, c, ready);
  if (!ticket)
    return ticket.error();
  auto permit = consume_request_gate(ticket.value());
  if (!permit)
    return permit.error();
  return std::optional<RequestPermit>{permit.value()};
}
Expected<RequestGateState> AdmissionStore::gate_state(RequestGateTicket t) const {
  auto i = gates_.find(t.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "gate ticket");
  return i->second.state;
}
Expected<ReadyKey> AdmissionStore::gate_ready(RequestGateTicket t) const {
  auto i = gates_.find(t.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "gate ticket");
  if (i->second.state != RequestGateState::Granted)
    return fail(ErrorCode::NotReady, "gate not granted");
  return i->second.ready;
}
Expected<RequestPermit> AdmissionStore::consume_request_gate(RequestGateTicket t) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = gates_.find(t.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "gate ticket");
  if (i->second.state != RequestGateState::Granted)
    return fail(ErrorCode::NotReady, "gate not granted");
  i->second.state = RequestGateState::Consumed;
  return RequestPermit{t.handle};
}
Expected<void> AdmissionStore::validate_request_permit(RequestPermit p, Handle txn,
                                                       ConnectionId connection) const {
  auto i = gates_.find(p.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "request permit");
  if (i->second.txn != txn || i->second.connection != connection)
    return fail(ErrorCode::WrongOwner, "request permit binding");
  if (i->second.state != RequestGateState::Consumed)
    return fail(ErrorCode::InvalidState, "request permit not consumed or already sent");
  return {};
}
Expected<void> AdmissionStore::cancel_request_gate(RequestGateTicket t, ReadyKey ready) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = gates_.find(t.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "gate ticket");
  if (i->second.state == RequestGateState::Consumed || i->second.state == RequestGateState::Sent)
    return fail(ErrorCode::InvalidState, "ticket transferred to permit");
  i->second.state = RequestGateState::Cancelled;
  return update_request_lane(i->second.connection, !wire_busy_[i->second.connection], ready);
}
Expected<void> AdmissionStore::cancel_request_permit(RequestPermit p, ReadyKey ready) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = gates_.find(p.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "request permit");
  if (i->second.state == RequestGateState::Cancelled)
    return {};
  if (i->second.state != RequestGateState::Consumed)
    return fail(ErrorCode::InvalidState, "permit unavailable/already sent");
  i->second.state = RequestGateState::Cancelled;
  return update_request_lane(i->second.connection, !wire_busy_[i->second.connection], ready);
}
Expected<void> AdmissionStore::request_sent(RequestPermit p) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = gates_.find(p.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "request permit");
  if (i->second.state != RequestGateState::Consumed)
    return fail(ErrorCode::InvalidState, "permit not consumed");
  i->second.state = RequestGateState::Sent;
  wire_busy_[i->second.connection] = true;
  return {};
}
Expected<void> AdmissionStore::retire_request_ticket(RequestGateTicket t) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  auto i = gates_.find(t.handle);
  if (i == gates_.end())
    return fail(ErrorCode::StaleHandle, "gate ticket");
  if (i->second.state != RequestGateState::Sent && i->second.state != RequestGateState::Cancelled)
    return fail(ErrorCode::InvalidState, "live gate ticket");
  gates_.erase(i);
  return {};
}
} // namespace leanat
