#include "leanat/drain.hpp"
#include "leanat/storage_identity.hpp"
namespace leanat {
DrainStore::DrainStore(DomainId d, std::size_t c, std::size_t h, std::size_t i)
    : domain_(d), capacity_(c), hop_limit_(h), instance_limit_(i),
      physical_store_(storage_detail::allocate_store_incarnation()) {}
Expected<std::unique_ptr<DrainUpdate>> DrainStore::prepare() {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  return std::unique_ptr<DrainUpdate>(new DrainUpdate(*this));
}
Expected<std::unique_ptr<DrainUpdate>> DrainStore::prepare(EventTxn &txn) {
  if (preparation_pending_ && pending_txn_ != &txn)
    return fail(ErrorCode::NotReady, "another drain transaction pending");
  return std::unique_ptr<DrainUpdate>(new DrainUpdate(*this, &txn));
}
DrainUpdate::DrainUpdate(DrainStore &t, EventTxn *txn)
    : target_(&t),
      shadow_(std::unique_ptr<DrainStore>(new DrainStore(t.pending_view_ ? *t.pending_view_ : t))),
      previous_view_(t.pending_view_), previous_txn_(t.pending_txn_) {
  shadow_->preparation_pending_ = false;
  shadow_->pending_view_ = nullptr;
  shadow_->pending_txn_ = nullptr;
  // An earlier pending view predates identities burned by a rolled-back successor.
  shadow_->next_receipt_ = std::max(shadow_->next_receipt_, t.next_receipt_);
  shadow_->next_reset_ = std::max(shadow_->next_reset_, t.next_reset_);
  t.preparation_pending_ = true;
  t.pending_view_ = shadow_.get();
  t.pending_txn_ = txn;
}
DrainUpdate::~DrainUpdate() {
  discard();
}
std::size_t DrainUpdate::reserved_bytes() const noexcept {
  std::size_t bytes = sizeof(*this) + sizeof(DrainStore);
  auto add = [&](std::size_t count, std::size_t element) {
    if (count > (SIZE_MAX - bytes) / element)
      bytes = SIZE_MAX;
    else
      bytes += count * element;
  };
  // Conservative tree-node envelope for the supported standard libraries: parent,
  // children, colour/padding and allocation alignment, in addition to the value.
  constexpr auto node_overhead = 4 * sizeof(void *) + alignof(std::max_align_t);
  add(shadow_->instances_.size(),
      sizeof(decltype(shadow_->instances_)::value_type) + node_overhead);
  for (const auto &i : shadow_->instances_)
    add(i.second.cohort.size(), sizeof(Handle) + node_overhead);
  add(shadow_->records_.size(), sizeof(decltype(shadow_->records_)::value_type) + node_overhead);
  for (const auto &r : shadow_->records_)
    add(r.second.responsibility.hops.capacity(), sizeof(DrainHop));
  add(shadow_->receipts_.size(), sizeof(decltype(shadow_->receipts_)::value_type) + node_overhead);
  for (const auto &r : shadow_->receipts_)
    add(r.second.hops.capacity(), sizeof(DrainHop));
  return bytes;
}
Expected<void> DrainUpdate::validate() const {
  if (!active_ || !target_->preparation_pending_)
    return fail(ErrorCode::InvalidState, "drain preparation lost");
  return {};
}
void DrainUpdate::apply() noexcept {
  target_->records_.swap(shadow_->records_);
  target_->instances_.swap(shadow_->instances_);
  target_->receipts_.swap(shadow_->receipts_);
  target_->next_receipt_ = std::max(target_->next_receipt_, shadow_->next_receipt_);
  target_->next_reset_ = std::max(target_->next_reset_, shadow_->next_reset_);
  if (target_->pending_view_ == shadow_.get()) {
    target_->preparation_pending_ = false;
    target_->pending_view_ = nullptr;
    target_->pending_txn_ = nullptr;
  }
  active_ = false;
}
void DrainUpdate::discard() noexcept {
  if (active_) {
    target_->next_receipt_ = std::max(target_->next_receipt_, shadow_->next_receipt_);
    target_->next_reset_ = std::max(target_->next_reset_, shadow_->next_reset_);
    target_->preparation_pending_ = previous_view_ != nullptr;
    target_->pending_view_ = previous_view_;
    target_->pending_txn_ = previous_txn_;
    active_ = false;
  }
}
Expected<void> DrainStore::add_instance(InstanceId i) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  if (instances_.count(i))
    return {};
  if (instances_.size() >= instance_limit_)
    return fail(ErrorCode::Capacity, "reset instances full");
  instances_.emplace(i, Instance{});
  return {};
}
bool DrainStore::complete(const Record &r) const {
  if (r.deferred || !r.responsibility.local_finished)
    return false;
  for (auto &h : r.responsibility.hops)
    if (!h.wire_terminal || !h.timing_consumed || !h.cleanup_returned || h.call_pin)
      return false;
  return true;
}
void DrainStore::refresh(Record &r) {
  if (r.receipt && !r.receipt_released) {
    auto &i = receipts_.at(*r.receipt);
    i.hops = r.responsibility.hops;
    i.state = complete(r) ? DrainState::Complete : DrainState::Pending;
  }
}
void DrainStore::collect() {
  for (auto i = records_.begin(); i != records_.end();) {
    bool cohort = false;
    for (auto &v : instances_)
      if (v.second.cohort.count(i->first))
        cohort = true;
    if (complete(i->second) && (!i->second.receipt || i->second.receipt_released) && !cohort &&
        !(i->second.responsibility.cancelled && !i->second.receipt &&
          !i->second.retirement_requested))
      i = records_.erase(i);
    else
      ++i;
  }
}
Expected<void> DrainStore::register_responsibility(Responsibility r, std::optional<Handle> parent) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  if (r.transaction.domain != domain_)
    return fail(ErrorCode::WrongDomain, "responsibility domain");
  auto v = instances_.find(r.instance);
  if (v == instances_.end())
    return fail(ErrorCode::InvalidArgument, "unknown instance");
  if (records_.count(r.transaction))
    return fail(ErrorCode::Duplicate, "responsibility exists");
  if (records_.size() >= capacity_ || r.hops.size() > hop_limit_)
    return fail(ErrorCode::Capacity, "guaranteed drain capacity full");
  if (r.epoch != v->second.epoch)
    return fail(ErrorCode::StaleHandle, "responsibility epoch");
  bool inherited = false;
  if (parent) {
    auto p = records_.find(*parent);
    if (p == records_.end() || p->second.responsibility.cancelled ||
        p->second.responsibility.local_finished ||
        p->second.responsibility.instance != r.instance || p->first.owner != r.transaction.owner)
      return fail(ErrorCode::WrongOwner, "unverified cohort provenance");
    r.parent = parent;
    inherited = v->second.cohort.count(*parent) != 0;
  } else if (r.parent)
    return fail(ErrorCode::WrongOwner, "self asserted parent");
  if (v->second.reset && v->second.reset->state == ResetState::Draining && !inherited)
    return fail(ErrorCode::NotReady, "reset root barrier");
  auto h = r.transaction;
  records_.emplace(h, Record{std::move(r), {}, CancelReason::User, false});
  if (inherited)
    v->second.cohort.insert(h);
  return {};
}
Expected<void> DrainStore::defer_root(Responsibility r) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto instance = instances_.find(r.instance);
  if (instance == instances_.end() || !instance->second.reset ||
      instance->second.reset->state != ResetState::Draining)
    return fail(ErrorCode::InvalidState, "no reset barrier");
  if (r.transaction.domain != domain_ || r.parent)
    return fail(ErrorCode::WrongOwner, "deferred root identity");
  if (records_.count(r.transaction))
    return fail(ErrorCode::Duplicate, "responsibility exists");
  if (records_.size() >= capacity_ || r.hops.size() > hop_limit_)
    return fail(ErrorCode::Capacity, "deferred drain capacity");
  r.epoch = instance->second.epoch;
  auto h = r.transaction;
  records_.emplace(h, Record{std::move(r), {}, CancelReason::User, false, true});
  return {};
}
Expected<void> DrainStore::add_hop(Handle transaction, DrainHop hop) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto r = records_.find(transaction);
  if (r == records_.end())
    return fail(ErrorCode::StaleHandle, "responsibility");
  for (auto &h : r->second.responsibility.hops)
    if (h.hop == hop.hop)
      return {};
  if (r->second.responsibility.hops.size() >= hop_limit_)
    return fail(ErrorCode::Capacity, "hop drain guarantee full");
  if (r->second.responsibility.cancelled)
    return fail(ErrorCode::Cancelled, "cannot start cancelled transaction");
  r->second.responsibility.hops.push_back(hop);
  return {};
}
Expected<void> DrainStore::drop_unstarted_hop(Handle transaction, Handle hop,
                                              const ProtocolEngine &protocol,
                                              const EventTxn *prepared) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto proof = prepared ? protocol.prepared_inspect(*prepared, hop) : protocol.inspect(hop);
  if (!proof)
    return proof.error();
  if (proof.value().state != WireState::Idle || proof.value().pending || proof.value().call_ordinal)
    return fail(ErrorCode::InvalidState, "hop already entered wire");
  auto r = records_.find(transaction);
  if (r == records_.end())
    return fail(ErrorCode::StaleHandle, "responsibility");
  auto &hops = r->second.responsibility.hops;
  auto h = std::find_if(hops.begin(), hops.end(), [&](const DrainHop &x) { return x.hop == hop; });
  if (h == hops.end())
    return fail(ErrorCode::StaleHandle, "hop responsibility");
  hops.erase(h);
  return {};
}
bool DrainStore::is_deferred(Handle h) const {
  auto i = records_.find(h);
  return i != records_.end() && i->second.deferred;
}
Expected<void> DrainStore::set_hop(Handle t, DrainHop h) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto i = records_.find(t);
  if (i == records_.end())
    return fail(ErrorCode::StaleHandle, "responsibility");
  auto &hs = i->second.responsibility.hops;
  auto p = std::find_if(hs.begin(), hs.end(), [&](const DrainHop &x) { return x.hop == h.hop; });
  if (p == hs.end())
    return fail(ErrorCode::WrongOwner, "unknown cleanup hop");
  if ((p->wire_terminal && !h.wire_terminal) || (p->timing_consumed && !h.timing_consumed) ||
      (p->cleanup_returned && !h.cleanup_returned) || (!p->call_pin && h.call_pin))
    return fail(ErrorCode::InvalidState, "cleanup cannot regress");
  *p = h;
  refresh(i->second);
  collect();
  return {};
}
Expected<void> DrainStore::finish_local(Handle h) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto i = records_.find(h);
  if (i == records_.end())
    return fail(ErrorCode::StaleHandle, "responsibility");
  i->second.responsibility.local_finished = true;
  refresh(i->second);
  collect();
  return {};
}
Expected<void> DrainStore::retire_responsibility(Handle h) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto i = records_.find(h);
  if (i == records_.end())
    return fail(ErrorCode::StaleHandle, "responsibility");
  if (!complete(i->second) || (i->second.receipt && !i->second.receipt_released))
    return fail(ErrorCode::NotReady, "responsibility still owned");
  for (auto &v : instances_)
    if (v.second.cohort.count(h)) {
      // The caller released its ownership; the reset still retains this record
      // until its complete cohort advances. collect() then reclaims it.
      i->second.retirement_requested = true;
      return {};
    }
  records_.erase(i);
  return {};
}
Expected<void> DrainStore::observe_wire(Handle hop, bool terminal, bool pin) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  for (auto &r : records_)
    for (auto &h : r.second.responsibility.hops)
      if (h.hop == hop) {
        h.wire_terminal = h.wire_terminal || terminal;
        h.call_pin = pin;
        if (terminal && !pin)
          h.cleanup_returned = true;
        refresh(r.second);
      }
  collect();
  return {};
}
Expected<void> DrainStore::consume_terminal(Handle hop) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  for (auto &r : records_)
    for (auto &h : r.second.responsibility.hops)
      if (h.hop == hop) {
        if (!h.wire_terminal)
          return fail(ErrorCode::InvalidState, "terminal timing before wire terminal");
        h.timing_consumed = true;
        refresh(r.second);
      }
  collect();
  return {};
}
bool DrainStore::is_cancelled(Handle h) const {
  auto i = records_.find(h);
  return i != records_.end() && i->second.responsibility.cancelled;
}
Expected<CancelDisposition> DrainStore::cancel_local(Handle h, CancelReason reason) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto i = records_.find(h);
  if (i == records_.end())
    return fail(ErrorCode::StaleHandle, "responsibility");
  auto &r = i->second;
  if (r.receipt)
    return CancelDisposition{false, r.receipt_released ? std::nullopt : r.receipt};
  if (r.responsibility.hops.empty()) {
    r.responsibility.cancelled = true;
    r.responsibility.local_finished = true;
    collect();
    return CancelDisposition{true, {}};
  }
  if (next_receipt_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "drain receipts exhausted");
  Handle receipt{HandleKind::Drain, domain_,
                 physical_store_,   static_cast<std::uint32_t>(next_receipt_ % capacity_),
                 next_receipt_++,   h.owner};
  r.responsibility.cancelled = true;
  r.responsibility.local_finished = true;
  r.receipt = receipt;
  r.reason = reason;
  receipts_.emplace(receipt, DrainStatus{DrainState::Pending, r.responsibility.hops, reason});
  refresh(r);
  return CancelDisposition{false, receipt};
}
Expected<DrainStatus> DrainStore::inspect(Handle h) const {
  auto i = receipts_.find(h);
  if (i == receipts_.end())
    return fail(ErrorCode::StaleHandle, "drain receipt");
  return i->second;
}
Expected<void> DrainStore::release_receipt(Handle h) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto i = receipts_.find(h);
  if (i == receipts_.end())
    return fail(ErrorCode::StaleHandle, "drain receipt");
  for (auto &r : records_)
    if (r.second.receipt == h)
      r.second.receipt_released = true;
  receipts_.erase(i);
  collect();
  return {};
}
Expected<ResetDisposition> DrainStore::reset(InstanceId id, ResetPolicy policy) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto i = instances_.find(id);
  if (i == instances_.end())
    return fail(ErrorCode::InvalidArgument, "unknown instance");
  auto &v = i->second;
  if (v.reset && v.reset->state == ResetState::Draining && policy == ResetPolicy::DrainThenReset)
    return *v.reset;
  if (v.epoch == UINT64_MAX || next_reset_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "reset identity exhausted");
  if (policy == ResetPolicy::AbortLocalAndDrain) {
    std::size_t needed = 0;
    for (auto &r : records_)
      if (r.second.responsibility.instance == id && !r.second.receipt &&
          !r.second.responsibility.hops.empty())
        ++needed;
    if (needed > UINT64_MAX - next_receipt_)
      return fail(ErrorCode::Overflow, "drain receipt capacity exhausted");
  }
  ResetDisposition out{next_reset_++, policy, v.epoch, v.epoch + 1, ResetState::Draining};
  v.cohort.clear();
  for (auto &r : records_)
    if (r.second.responsibility.instance == id && r.second.responsibility.epoch == v.epoch &&
        !r.second.deferred)
      v.cohort.insert(r.first);
  v.reset = out;
  if (policy == ResetPolicy::AbortLocalAndDrain) {
    auto cohort = v.cohort;
    for (auto h : cohort) {
      auto c = cancel_local(h, CancelReason::Reset);
      if (!c)
        return c.error();
    }
    ++v.epoch;
    v.reset->state = ResetState::Applied;
    v.cohort.clear();
    for (auto &r : records_)
      if (r.second.responsibility.instance == id && r.second.deferred) {
        r.second.deferred = false;
        r.second.responsibility.epoch = v.epoch;
      }
    collect();
    return *v.reset;
  }
  return advance_reset(id);
}
Expected<ResetDisposition> DrainStore::advance_reset(InstanceId id) {
  if (preparation_pending_)
    return fail(ErrorCode::NotReady, "drain preparation pending");
  auto i = instances_.find(id);
  if (i == instances_.end() || !i->second.reset)
    return fail(ErrorCode::InvalidState, "no reset");
  auto &v = i->second;
  if (v.reset->state != ResetState::Draining)
    return *v.reset;
  for (auto h : v.cohort) {
    auto r = records_.find(h);
    if (r != records_.end() && !complete(r->second))
      return *v.reset;
  }
  v.epoch = v.reset->next_epoch;
  v.reset->state = ResetState::Applied;
  v.cohort.clear();
  for (auto &r : records_)
    if (r.second.responsibility.instance == id && r.second.deferred) {
      r.second.deferred = false;
      r.second.responsibility.epoch = v.epoch;
    }
  collect();
  return *v.reset;
}
Expected<std::uint64_t> DrainStore::epoch(InstanceId id) const {
  auto i = instances_.find(id);
  if (i == instances_.end())
    return fail(ErrorCode::InvalidArgument, "unknown instance");
  return i->second.epoch;
}
bool DrainStore::admits_root(InstanceId id) const {
  auto i = instances_.find(id);
  return i != instances_.end() &&
         (!i->second.reset || i->second.reset->state != ResetState::Draining);
}
std::size_t DrainStore::outstanding() const {
  std::size_t n = 0;
  for (auto &r : records_)
    if (!complete(r.second))
      ++n;
  return n;
}
std::vector<Responsibility> DrainStore::stop_report() const {
  std::vector<Responsibility> out;
  for (auto &r : records_)
    if (!complete(r.second))
      out.push_back(r.second.responsibility);
  return out;
}
} // namespace leanat
