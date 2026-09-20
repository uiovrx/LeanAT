#include "leanat/event_queue.hpp"
#include "leanat/storage_identity.hpp"

namespace leanat {
EventReservation::EventReservation(EventReservation &&o) noexcept
    : queue_(o.queue_), token_(o.token_) {
  o.queue_ = nullptr;
}
EventReservation &EventReservation::operator=(EventReservation &&o) noexcept {
  if (this != &o) {
    if (queue_)
      queue_->abandon(token_);
    queue_ = o.queue_;
    token_ = o.token_;
    o.queue_ = nullptr;
  }
  return *this;
}
EventReservation::~EventReservation() {
  if (queue_)
    queue_->abandon(token_);
}
EventQueue::EventQueue(std::size_t n, DomainId d, std::uint32_t s, std::size_t bytes)
    : slots_(n), domain_(d), store_(storage_detail::allocate_store_incarnation()),
      max_event_bytes_(bytes) {
  (void)s;
  if (!n || n > UINT32_MAX)
    throw std::invalid_argument("queue capacity");
  members_.reserve(n);
}
Expected<std::uint32_t> EventQueue::validate(Handle h) const {
  if (h.domain != domain_)
    return fail(ErrorCode::WrongDomain, "event domain");
  if (h.kind != HandleKind::Event || h.store != store_ || h.slot >= slots_.size())
    return fail(ErrorCode::StaleHandle, "event identity");
  auto &s = slots_[h.slot];
  if (!h.generation || s.generation != h.generation || s.state == State::Retired)
    return fail(ErrorCode::StaleHandle, "event generation");
  if (h.owner != s.event.owner)
    return fail(ErrorCode::WrongOwner, "event owner");
  return h.slot;
}
void EventQueue::abandon(Handle h) noexcept {
  if (h.slot < slots_.size()) {
    auto &s = slots_[h.slot];
    if (s.generation == h.generation && s.state == State::Reserved)
      s.state = State::Free;
  }
}
Expected<EventReservation> EventQueue::prepare(EventDraft e) {
  if (bounded_value_bytes(e.value) == SIZE_MAX || bounded_value_bytes(e.value) > max_event_bytes_)
    return fail(ErrorCode::Capacity, "event value bytes");
  if (next_sequence_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "event sequence exhausted");
  ReadyKey r{e.key.time, e.key.turn};
  if (frontier_ && !(frontier_.value() < r))
    return fail(ErrorCode::TimeRegression, "event enters frozen frontier");
  for (std::uint32_t i = 0; i < slots_.size(); ++i) {
    auto &s = slots_[i];
    if (s.state != State::Free)
      continue;
    if (s.generation == UINT64_MAX) {
      s.state = State::Retired;
      continue;
    }
    ++s.generation;
    e.key.sequence = next_sequence_++;
    s.event = std::move(e);
    s.cancelled = false;
    s.state = State::Reserved;
    return EventReservation(
        this, Handle{HandleKind::Event, domain_, store_, i, s.generation, s.event.owner});
  }
  return fail(ErrorCode::Capacity, "event capacity");
}
Expected<EventToken> EventQueue::enqueue(EventReservation &&r) {
  if (r.queue_ != this)
    return fail(ErrorCode::InvalidArgument, "foreign reservation");
  auto v = validate(r.token_);
  if (!v)
    return v.error();
  auto &s = slots_[v.value()];
  if (s.state != State::Reserved)
    return fail(ErrorCode::InvalidState, "reservation already used");
  ReadyKey key{s.event.key.time, s.event.key.turn};
  if (frontier_ && !(frontier_.value() < key))
    return fail(ErrorCode::TimeRegression, "reservation predates frontier");
  s.state = State::Queued;
  r.queue_ = nullptr;
  return r.token_;
}
Expected<EventToken> EventQueue::enqueue(EventDraft e) {
  auto r = prepare(std::move(e));
  if (!r)
    return r.error();
  return enqueue(std::move(r.value()));
}
Expected<bool> EventQueue::cancel(EventToken h) {
  auto v = validate(h);
  if (!v)
    return v.error();
  auto &s = slots_[v.value()];
  if (s.cancelled || s.state == State::Done || s.state == State::Free)
    return false;
  if (s.state == State::Reserved)
    return fail(ErrorCode::InvalidState, "uncommitted event");
  s.cancelled = true;
  return true;
}
Expected<std::optional<ClosedBatchSlice>> EventQueue::pop_batch(Tick now, std::uint32_t budget) {
  if (!budget)
    return fail(ErrorCode::InvalidArgument, "zero batch budget");
  if (!batch_) {
    std::optional<ReadyKey> key;
    for (auto &s : slots_)
      if (s.state == State::Queued) {
        ReadyKey k{s.event.key.time, s.event.key.turn};
        if (!(now < k.time) && (!key || k < *key))
          key = k;
      }
    if (!key)
      return std::optional<ClosedBatchSlice>{};
    if (next_batch_ == UINT64_MAX)
      return fail(ErrorCode::Overflow, "batch id exhausted");
    batch_ = BatchId{next_batch_++};
    batch_ready_ = *key;
    frontier_ = *key;
    members_.clear();
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
      auto &s = slots_[i];
      if (s.state == State::Queued && ReadyKey{s.event.key.time, s.event.key.turn} == *key) {
        s.state = State::Active;
        members_.push_back(i);
      }
    }
    std::stable_sort(members_.begin(), members_.end(),
                     [&](auto a, auto b) { return slots_[a].event.key < slots_[b].event.key; });
    cursor_ = resolver_cursor_ = 0;
    resolver_total_.reset();
  }
  ClosedBatchSlice out{*batch_, batch_ready_, cursor_, {}, cursor_ == members_.size()};
  auto end = std::min(members_.size(), cursor_ + budget);
  out.members.reserve(end - cursor_);
  for (auto n = cursor_; n < end; ++n) {
    auto i = members_[n];
    auto &s = slots_[i];
    out.members.push_back(
        {Handle{HandleKind::Event, domain_, store_, i, s.generation, s.event.owner}, s.event,
         s.cancelled});
  }
  return std::optional<ClosedBatchSlice>{std::move(out)};
}
Expected<void> EventQueue::ack_executed(BatchId b, EventToken h, ExecutionDisposition d) {
  if (!batch_ || b != *batch_ || cursor_ == members_.size())
    return fail(ErrorCode::InvalidState, "no pending batch member");
  auto v = validate(h);
  if (!v)
    return v.error();
  if (v.value() != members_[cursor_])
    return fail(ErrorCode::InvalidState, "out of order ack");
  auto &s = slots_[v.value()];
  if (s.cancelled && d == ExecutionDisposition::Committed)
    return fail(ErrorCode::Cancelled, "cancelled member");
  s.state = State::Done;
  ++cursor_;
  return {};
}
Expected<bool> EventQueue::resolver_step(BatchId b, std::uint32_t budget, std::size_t total) {
  if (!budget)
    return fail(ErrorCode::InvalidArgument, "zero resolver budget");
  if (!batch_ || b != *batch_ || cursor_ != members_.size())
    return fail(ErrorCode::InvalidState, "producers incomplete");
  if (resolver_total_ && *resolver_total_ != total)
    return fail(ErrorCode::InvalidArgument, "resolver work changed");
  resolver_total_ = total;
  resolver_cursor_ += std::min<std::size_t>(budget, total - resolver_cursor_);
  return resolver_cursor_ == total;
}
Expected<void> EventQueue::finish_batch(BatchId b) {
  if (!batch_ || b != *batch_ || cursor_ != members_.size() || !resolver_total_ ||
      resolver_cursor_ != *resolver_total_)
    return fail(ErrorCode::InvalidState, "batch incomplete");
  for (auto i : members_)
    slots_[i].state = State::Free;
  members_.clear();
  batch_.reset();
  return {};
}
std::optional<WakePoint> EventQueue::next_wakeup() const {
  if (batch_)
    return WakePoint{batch_ready_.time, batch_ready_.turn, batch_->value};
  std::optional<ReadyKey> k;
  for (auto &s : slots_)
    if (s.state == State::Queued) {
      ReadyKey r{s.event.key.time, s.event.key.turn};
      if (!k || r < *k)
        k = r;
    }
  if (k)
    return WakePoint{k->time, k->turn, 0};
  return {};
}
std::size_t EventQueue::collect_stale(std::uint32_t budget) {
  std::size_t freed = 0;
  for (std::size_t scanned = 0; scanned < std::min<std::size_t>(budget, slots_.size()); ++scanned) {
    auto &s = slots_[reclaim_cursor_];
    reclaim_cursor_ = (reclaim_cursor_ + 1) % slots_.size();
    if ((s.state == State::Done && !batch_) || (s.state == State::Queued && s.cancelled)) {
      s.state = State::Free;
      ++freed;
    }
  }
  return freed;
}
std::size_t EventQueue::occupied() const {
  std::size_t n = 0;
  for (auto &s : slots_)
    if (s.state != State::Free && s.state != State::Retired)
      ++n;
  return n;
}
Expected<ReadyKey> EventQueue::successor(Tick t) const {
  if (frontier_ && t < frontier_->time)
    return fail(ErrorCode::TimeRegression, "past tick");
  if (frontier_ && t == frontier_->time) {
    auto n = checked_add(frontier_->turn, 1);
    if (!n)
      return n.error();
    return ReadyKey{t, n.value()};
  }
  return ReadyKey{t, 0};
}
} // namespace leanat
namespace leanat {
Expected<void> EventQueue::preflight(const EventReservation &r) const {
  if (r.queue_ != this)
    return fail(ErrorCode::InvalidArgument, "foreign reservation");
  auto v = validate(r.token_);
  if (!v)
    return v.error();
  auto &s = slots_[v.value()];
  if (s.state != State::Reserved)
    return fail(ErrorCode::InvalidState, "reservation state");
  if (frontier_ && !(frontier_.value() < ReadyKey{s.event.key.time, s.event.key.turn}))
    return fail(ErrorCode::TimeRegression, "reservation frontier");
  return {};
}
Expected<void> EventQueue::validate_cancel(EventToken h) const {
  auto v = validate(h);
  if (!v)
    return v.error();
  if (slots_[v.value()].state == State::Reserved)
    return fail(ErrorCode::InvalidState, "reserved cancellation");
  return {};
}
} // namespace leanat

namespace leanat {
Expected<void> EventQueue::retarget(EventReservation &r, EventDraft e) {
  if (bounded_value_bytes(e.value) == SIZE_MAX || bounded_value_bytes(e.value) > max_event_bytes_)
    return fail(ErrorCode::Capacity, "event value bytes");
  auto v = preflight(r);
  if (!v)
    return v.error();
  if (e.owner != r.token_.owner)
    return fail(ErrorCode::WrongOwner, "reservation owner change");
  if (frontier_ && !(frontier_.value() < ReadyKey{e.key.time, e.key.turn}))
    return fail(ErrorCode::TimeRegression, "retarget frontier");
  e.key.sequence = slots_[r.token_.slot].event.key.sequence;
  slots_[r.token_.slot].event = std::move(e);
  return {};
}
Expected<bool> EventQueue::is_active(EventToken h) const {
  auto v = validate(h);
  if (!v)
    return v.error();
  auto &s = slots_[v.value()];
  return s.state == State::Active && !s.cancelled;
}
} // namespace leanat

namespace leanat {
Expected<EventDraft> EventQueue::active_event(EventToken h) const {
  auto v = validate(h);
  if (!v)
    return v.error();
  if (!batch_ || cursor_ >= members_.size() || members_[cursor_] != v.value())
    return fail(ErrorCode::NotReady, "event is not current batch cursor");
  auto &s = slots_[v.value()];
  if (s.cancelled || s.state != State::Active)
    return fail(ErrorCode::Cancelled, "event inactive");
  return s.event;
}
} // namespace leanat
namespace leanat {
Expected<bool> EventQueue::can_cancel(EventToken h) const {
  auto valid = validate(h);
  if (!valid)
    return valid.error();
  const auto &s = slots_[valid.value()];
  if (s.state == State::Reserved)
    return fail(ErrorCode::InvalidState, "uncommitted event");
  return (s.state == State::Queued || s.state == State::Active) && !s.cancelled;
}
} // namespace leanat
