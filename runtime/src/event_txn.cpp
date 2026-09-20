#include "leanat/event_txn.hpp"
#include "leanat/result_store.hpp"
namespace leanat {
EventTxn::EventTxn(SegmentBudget b, ExecutionContext c) : context_(std::move(c)), budget_(b) {
  writes_.reserve(b.writes);
  actions_.reserve(b.actions);
  events_.reserve(b.events);
  cancels_.reserve(b.events);
  participants_.reserve(b.writes);
  participant_bytes_.reserve(b.writes);
}
Expected<Value> EventTxn::read(const VersionedCell &c) const {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  if (!c.epoch_independent && c.epoch != context_.epoch)
    return fail(ErrorCode::InvalidState, "cell epoch");
  for (auto &w : writes_)
    if (w.cell == &c)
      return w.value;
  return c.value;
}
Expected<void> EventTxn::buffer(VersionedCell &c, Value v) {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  if (!c.epoch_independent && c.epoch != context_.epoch)
    return fail(ErrorCode::InvalidState, "cell epoch");
  auto n = owned_value_bytes(v);
  std::size_t prior = 0;
  for (auto &w : writes_)
    if (w.cell == &c)
      prior = owned_value_bytes(w.value);
  if (n == SIZE_MAX || n > budget_.bytes - (bytes_ - prior))
    return fail(ErrorCode::Capacity, "segment bytes");
  if (c.stable_bytes) {
    auto old = std::get_if<Bytes>(&c.value.data), next = std::get_if<Bytes>(&v.data);
    if (!old || !next || old->size() != next->size())
      return fail(ErrorCode::TypeMismatch, "stable byte cell shape");
  }
  for (auto &w : writes_)
    if (w.cell == &c) {
      bytes_ -= owned_value_bytes(w.value);
      w.value = std::move(v);
      bytes_ += n;
      return {};
    }
  if (writes_.size() == budget_.writes)
    return fail(ErrorCode::Capacity, "write capacity");
  writes_.push_back({&c, c.version, std::move(v)});
  bytes_ += n;
  return {};
}
Expected<void> EventTxn::stage_action(SendIntent a) {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  if (actions_.size() == budget_.actions)
    return fail(ErrorCode::Capacity, "action capacity");
  std::size_t cost = bounded_payload_bytes(a.payload);
  constexpr auto metadata = sizeof(SendIntent) - sizeof(PayloadSnapshot);
  if (cost == SIZE_MAX || metadata > SIZE_MAX - cost)
    return fail(ErrorCode::Capacity, "action byte overflow");
  cost += metadata;
  if (cost == SIZE_MAX || cost > budget_.bytes - bytes_)
    return fail(ErrorCode::Capacity, "action bytes");
  actions_.push_back(std::move(a));
  bytes_ += cost;
  return {};
}
Expected<EventToken> EventTxn::stage_event(EventQueue &q, EventDraft e) {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  if (events_.size() == budget_.events)
    return fail(ErrorCode::Capacity, "event capacity");
  auto cost = owned_value_bytes(e.value);
  if (cost == SIZE_MAX || cost > budget_.bytes - bytes_)
    return fail(ErrorCode::Capacity, "event bytes");
  auto r = q.prepare(std::move(e));
  if (!r)
    return r.error();
  auto h = r.value().token();
  events_.emplace_back(&q, std::move(r.value()));
  bytes_ += cost;
  return h;
}
Expected<void> EventTxn::stage_cancel(EventQueue &q, EventToken h) {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  if (cancels_.size() == budget_.events)
    return fail(ErrorCode::Capacity, "cancel capacity");
  bool own = false;
  for (auto &e : events_)
    if (e.first == &q && e.second.token() == h)
      own = true;
  if (!own) {
    auto v = q.validate_cancel(h);
    if (!v)
      return v.error();
  }
  cancels_.emplace_back(&q, h);
  return {};
}
Expected<CommittedActions> EventTxn::commit(std::uint64_t epoch) {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  if (epoch != context_.epoch)
    return fail(ErrorCode::InvalidState, "epoch changed");
  for (auto &w : writes_) {
    if (w.cell->version != w.version || (!w.cell->epoch_independent && w.cell->epoch != epoch))
      return fail(ErrorCode::InvalidState, "write version conflict");
    if (w.version == UINT64_MAX)
      return fail(ErrorCode::Overflow, "cell version exhausted");
    if (w.cell->stable_bytes) {
      auto a = std::get_if<Bytes>(&w.cell->value.data), b = std::get_if<Bytes>(&w.value.data);
      if (!a || !b || a->size() != b->size())
        return fail(ErrorCode::TypeMismatch, "stable byte cell changed");
    }
  }
  for (auto &e : events_) {
    auto v = e.first->preflight(e.second);
    if (!v)
      return v.error();
  }
  for (auto &c : cancels_) {
    bool own = false;
    for (auto &e : events_)
      if (e.first == c.first && e.second.token() == c.second)
        own = true;
    if (!own) {
      auto v = c.first->validate_cancel(c.second);
      if (!v)
        return v.error();
    }
  }
  for (std::size_t i = 0; i < participants_.size(); ++i) {
    auto &p = participants_[i];
    if (p->reserved_bytes() > participant_bytes_[i])
      return fail(ErrorCode::Capacity, "participant grew beyond reservation");
    auto r = p->validate();
    if (!r)
      return r.error();
  }
  for (auto &w : writes_) {
    if (w.cell->stable_bytes) {
      auto &a = std::get<Bytes>(w.cell->value.data);
      auto &b = std::get<Bytes>(w.value.data);
      std::copy(b.begin(), b.end(), a.begin());
    } else
      w.cell->value.data.swap(w.value.data);
    ++w.cell->version;
  }
  for (auto &e : events_) {
    auto v = e.first->enqueue(std::move(e.second));
    if (!v)
      std::terminate();
  }
  for (auto &c : cancels_) {
    auto v = c.first->cancel(c.second);
    if (!v)
      std::terminate();
  }
  for (auto &p : participants_)
    p->apply();
  closed_ = committed_ = true;
  return CommittedActions{std::move(actions_), 0, false};
}
Expected<void> EventTxn::discard() {
  if (committed_)
    return fail(ErrorCode::InvalidState, "already committed");
  if (closed_)
    return {};
  for (auto it = participants_.rbegin(); it != participants_.rend(); ++it)
    (*it)->discard();
  participants_.clear();
  participant_bytes_.clear();
  events_.clear();
  writes_.clear();
  actions_.clear();
  cancels_.clear();
  closed_ = true;
  return {};
}
Expected<void> publish_actions(CommittedActions &a,
                               const std::function<Expected<void>(const SendIntent &)> &host) {
  if (a.failed)
    return fail(ErrorCode::ExternalFailure, "publication stopped");
  while (a.cursor < a.actions.size()) {
    auto r = host(a.actions[a.cursor]);
    if (!r) {
      a.failed = true;
      return r.error();
    }
    ++a.cursor;
  }
  return {};
}
} // namespace leanat

namespace leanat {
EventTxn::~EventTxn() {
  if (!closed_) {
    for (auto it = participants_.rbegin(); it != participants_.rend(); ++it)
      (*it)->discard();
  }
}
Expected<void> EventTxn::stage_participant(std::unique_ptr<PreparedParticipant> p) {
  if (closed_ || !p) {
    if (p)
      p->discard();
    return fail(ErrorCode::InvalidState, "invalid participant");
  }
  if (participants_.size() == budget_.writes) {
    p->discard();
    return fail(ErrorCode::Capacity, "participant capacity");
  }
  const auto bytes = p->reserved_bytes();
  if (bytes == SIZE_MAX || bytes > remaining_bytes()) {
    p->discard();
    return fail(ErrorCode::Capacity, "participant bytes");
  }
  auto v = p->validate_prepare();
  if (!v) {
    p->discard();
    return v.error();
  }
  participants_.push_back(std::move(p));
  participant_bytes_.push_back(bytes);
  bytes_ += bytes;
  return {};
}
Expected<EventTxn::Savepoint> EventTxn::checkpoint() const {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  Savepoint s;
  s.owner = this;
  s.writes = writes_;
  s.participant_bytes = participant_bytes_;
  s.bytes = bytes_;
  s.actions = actions_.size();
  s.events = events_.size();
  s.cancels = cancels_.size();
  s.participants = participants_.size();
  return s;
}
Expected<void> EventTxn::rollback(Savepoint &&s) {
  if (closed_ || s.owner != this || s.actions > actions_.size() || s.events > events_.size() ||
      s.cancels > cancels_.size() || s.participants > participants_.size())
    return fail(ErrorCode::InvalidState, "invalid savepoint");
  for (auto i = participants_.size(); i > s.participants; --i)
    participants_[i - 1]->discard();
  participants_.resize(s.participants);
  participant_bytes_ = std::move(s.participant_bytes);
  writes_ = std::move(s.writes);
  bytes_ = s.bytes;
  actions_.resize(s.actions);
  events_.resize(s.events);
  cancels_.resize(s.cancels);
  s.owner = nullptr;
  return {};
}
} // namespace leanat

namespace leanat {
PreparedParticipant *EventTxn::participant(const void *key) const {
  for (auto i = participants_.rbegin(); i != participants_.rend(); ++i)
    if ((*i)->identity() == key)
      return i->get();
  return nullptr;
}
} // namespace leanat
namespace leanat {
Expected<void> EventTxn::validate_effects_since(const Savepoint &s, bool writes, bool events,
                                                bool actions) const {
  if (closed_ || s.owner != this)
    return fail(ErrorCode::InvalidState, "invalid savepoint");
  if (!events && (events_.size() != s.events || cancels_.size() != s.cancels))
    return fail(ErrorCode::InvalidState, "undeclared scheduling effect");
  if (!actions && actions_.size() != s.actions)
    return fail(ErrorCode::InvalidState, "undeclared action effect");
  if (!writes) {
    if (participants_.size() != s.participants || writes_.size() != s.writes.size())
      return fail(ErrorCode::InvalidState, "undeclared store effect");
    for (std::size_t i = 0; i < writes_.size(); ++i)
      if (writes_[i].cell != s.writes[i].cell || writes_[i].value != s.writes[i].value)
        return fail(ErrorCode::InvalidState, "undeclared write effect");
  }
  return {};
}
} // namespace leanat
namespace leanat {
Expected<bool> EventTxn::can_cancel(const EventQueue &q, EventToken token) const {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  for (const auto &cancel : cancels_)
    if (cancel.first == &q && cancel.second == token)
      return false;
  for (const auto &event : events_)
    if (event.first == &q && event.second.token() == token)
      return true;
  return q.can_cancel(token);
}
} // namespace leanat
namespace leanat {
Expected<void> EventTxn::reserve_bytes(std::size_t bytes) {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  if (bytes == SIZE_MAX || bytes > remaining_bytes())
    return fail(ErrorCode::Capacity, "segment byte reservation");
  bytes_ += bytes;
  return {};
}
} // namespace leanat
namespace leanat {
Expected<void> EventTxn::reserve_participant_growth(PreparedParticipant &participant,
                                                    std::size_t new_bytes) {
  if (closed_)
    return fail(ErrorCode::InvalidState, "closed segment");
  for (std::size_t i = 0; i < participants_.size(); ++i)
    if (participants_[i].get() == &participant) {
      auto old = participant_bytes_[i];
      if (new_bytes == SIZE_MAX)
        return fail(ErrorCode::Capacity, "participant growth overflow");
      if (new_bytes <= old)
        return {};
      auto delta = new_bytes - old;
      if (delta > remaining_bytes())
        return fail(ErrorCode::Capacity, "participant growth bytes");
      participant_bytes_[i] = new_bytes;
      bytes_ += delta;
      return {};
    }
  return fail(ErrorCode::InvalidArgument, "participant not staged in this segment");
}
} // namespace leanat
