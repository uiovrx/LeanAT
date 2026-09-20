#include "leanat/queue.hpp"
namespace leanat {
namespace {
using A = Value::Array;
A &a(Value &v) {
  return std::get<A>(v.data);
}
const A &a(const Value &v) {
  return std::get<A>(v.data);
}
std::uint64_t n(const Value &v) {
  return std::get<std::uint64_t>(v.data);
}
Value consumer_value(ResultHandle h) {
  return Value(A{Value(h.result), Value(h.consumer)});
}
QueueEntry entry(const A &r) {
  QueueEntry e{n(r[0]),
               r[1],
               {},
               std::get<bool>(r[3].data) ? QueuePublication::PublishedProtocol
                                         : QueuePublication::LocalUnpublished,
               {}};
  if (auto h = std::get_if<Handle>(&r[2].data))
    e.owner = *h;
  if (r.size() > 4 && std::holds_alternative<A>(r[4].data)) {
    auto &v = a(r[4]);
    e.consumer = ResultHandle{std::get<Handle>(v[0].data), std::get<Handle>(v[1].data)};
  }
  return e;
}
} // namespace
Expected<std::vector<QueueEntry>> BoundedQueue::inspect_entries(EventTxn &t) const {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "queue context domain");
  auto context = object_context(t);
  if (!context)
    return context.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  const auto &rows = a(a(v.value())[1]);
  if (rows.size() > scan_budget_)
    return fail(ErrorCode::FuelExhausted, "queue inspection scan budget");
  std::vector<QueueEntry> out;
  out.reserve(rows.size());
  for (const auto &r : rows)
    out.push_back(entry(a(r)));
  return out;
}
BoundedQueue::BoundedQueue(std::size_t c, std::size_t b, std::size_t scan, DomainId d)
    : capacity_(c), max_value_bytes_(b), scan_budget_(scan), domain_(d) {
  if (!c || !b || !scan)
    throw std::invalid_argument("queue capacities must be positive");
  cell_.value = Value(A{Value(std::uint64_t(1)), Value(A{})});
  cell_.epoch_independent = true;
}
Expected<bool> BoundedQueue::try_push(EventTxn &t, Value v, std::optional<Handle> owner,
                                      QueueOwnership policy) {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "queue context domain");
  auto c = object_context(t);
  if (!c)
    return c.error();
  if (policy != QueueOwnership::ValueOnly)
    return fail(ErrorCode::InvalidArgument, "use try_push_owned for consumer transfer");
  if (owned_value_bytes(v) > max_value_bytes_)
    return fail(ErrorCode::Capacity, "queue element bytes");
  if (owner) {
    if (owner->kind != HandleKind::Scope || owner->domain != domain_ || !owner->generation)
      return fail(ErrorCode::StaleHandle, "queue scope identity");
    if (scopes_) {
      auto valid = scopes_->validate_owner(*owner, true);
      if (!valid)
        return valid.error();
    } else if (!owner_validator_)
      return fail(ErrorCode::Unsupported, "scope validator required for cancel aware queue");
    if (owner_validator_) {
      auto valid = owner_validator_(*owner);
      if (!valid)
        return valid.error();
    }
  }
  auto read = t.read(cell_);
  if (!read)
    return read.error();
  auto state = a(read.value());
  auto &rows = a(state[1]);
  if (rows.size() == capacity_)
    return false;
  auto id = n(state[0]);
  if (id == UINT64_MAX)
    return fail(ErrorCode::Overflow, "queue entry sequence exhausted");
  rows.emplace_back(
      A{Value(id), std::move(v), owner ? Value(*owner) : Value{}, Value(false), Value{}});
  state[0] = Value(id + 1);
  auto w = t.buffer(cell_, Value(std::move(state)));
  if (!w)
    return w.error();
  return true;
}
Expected<bool> BoundedQueue::try_push_owned(EventTxn &t, ResultHandle source,
                                            std::optional<Handle> scope, QueueOwnership policy) {
  if (source.consumer.owner != t.context().owner)
    return fail(ErrorCode::WrongOwner, "queue source consumer owner");
  if (!results_ || policy == QueueOwnership::ValueOnly)
    return fail(ErrorCode::InvalidArgument, "owned queue needs store and ownership policy");
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto pushed = try_push(t, Value{}, scope);
  if (!pushed || !pushed.value())
    return pushed;
  auto owned = policy == QueueOwnership::MoveOwned
                   ? results_->prepare_transfer(t, source, consumer_owner_)
                   : results_->prepare_retain(t, source, consumer_owner_);
  if (!owned) {
    t.rollback(std::move(cp.value()));
    return owned.error();
  }
  auto v = t.read(cell_);
  if (!v) {
    t.rollback(std::move(cp.value()));
    return v.error();
  }
  auto state = a(v.value());
  a(a(state[1]).back())[4] = consumer_value(owned.value());
  auto w = t.buffer(cell_, Value(std::move(state)));
  if (!w) {
    t.rollback(std::move(cp.value()));
    return w.error();
  }
  return true;
}
Expected<std::optional<QueueEntry>> BoundedQueue::try_pop(EventTxn &t) {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "queue context domain");
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto state = a(v.value());
  auto &rows = a(state[1]);
  if (rows.empty())
    return std::optional<QueueEntry>{};
  auto e = entry(a(rows.front()));
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  if (e.consumer) {
    if (!results_)
      return fail(ErrorCode::InvalidState, "queue consumer store unavailable");
    auto moved = results_->prepare_transfer(t, *e.consumer, t.context().owner);
    if (!moved)
      return moved.error();
    e.consumer = moved.value();
  }
  rows.erase(rows.begin());
  auto w = t.buffer(cell_, Value(std::move(state)));
  if (!w) {
    t.rollback(std::move(cp.value()));
    return w.error();
  }
  return std::optional<QueueEntry>{std::move(e)};
}
Expected<std::size_t> BoundedQueue::size(EventTxn &t) const {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "queue context domain");
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  return a(a(v.value())[1]).size();
}
Expected<void> BoundedQueue::mark_published(EventTxn &t, std::uint64_t id) {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "queue context domain");
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto state = a(v.value());
  auto &rows = a(state[1]);
  if (rows.size() > scan_budget_)
    return fail(ErrorCode::FuelExhausted, "queue scan budget");
  for (auto &rv : rows) {
    auto &r = a(rv);
    if (n(r[0]) == id) {
      r[3] = Value(true);
      return t.buffer(cell_, Value(std::move(state)));
    }
  }
  return fail(ErrorCode::StaleHandle, "queue entry retired");
}
Expected<QueueRemoval> BoundedQueue::remove_unpublished_owned(EventTxn &t, Handle owner) {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "queue context domain");
  auto c = object_context(t);
  if (!c)
    return c.error();
  if (!owner_validator_ && !scopes_)
    return fail(ErrorCode::Unsupported, "scope validator required");
  auto valid = scopes_ ? scopes_->validate_owner(owner, false) : owner_validator_(owner);
  if (!valid)
    return valid.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto state = a(v.value());
  auto &rows = a(state[1]);
  if (rows.size() > scan_budget_)
    return fail(ErrorCode::FuelExhausted, "queue scan budget");
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  QueueRemoval report;
  A keep;
  for (auto &rv : rows) {
    auto e = entry(a(rv));
    if (e.owner && *e.owner == owner && e.publication == QueuePublication::LocalUnpublished) {
      if (e.consumer) {
        if (!results_) {
          t.rollback(std::move(cp.value()));
          return fail(ErrorCode::InvalidState, "consumer store missing");
        }
        auto release = results_->prepare_release(t, *e.consumer);
        if (!release) {
          t.rollback(std::move(cp.value()));
          return release.error();
        }
      }
      report.entry_ids.push_back(e.id);
    } else
      keep.push_back(rv);
  }
  rows = std::move(keep);
  if (!report.entry_ids.empty()) {
    auto w = t.buffer(cell_, Value(std::move(state)));
    if (!w) {
      t.rollback(std::move(cp.value()));
      return w.error();
    }
  }
  return report;
}
Expected<QueueRemoval> BoundedQueue::transfer_drain_owned(EventTxn &t, Handle owner, Handle drain,
                                                          std::size_t cap) {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "queue context domain");
  auto c = object_context(t);
  if (!c)
    return c.error();
  if (!owner_validator_ && !scopes_)
    return fail(ErrorCode::Unsupported, "scope validator required");
  auto valid = scopes_ ? scopes_->validate_owner(owner, false) : owner_validator_(owner);
  if (!valid)
    return valid.error();
  if (drain.kind != HandleKind::Drain || drain.domain != domain_ || !drain.generation)
    return fail(ErrorCode::StaleHandle, "invalid drain owner");
  if (!owner_validator_)
    return fail(ErrorCode::Unsupported, "drain validator required");
  valid = owner_validator_(drain);
  if (!valid)
    return valid.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto state = a(v.value());
  auto &rows = a(state[1]);
  if (rows.size() > scan_budget_)
    return fail(ErrorCode::FuelExhausted, "queue scan budget");
  QueueRemoval report;
  for (auto &rv : rows) {
    auto e = entry(a(rv));
    if (e.owner && *e.owner == owner && e.publication == QueuePublication::PublishedProtocol) {
      if (report.entry_ids.size() == cap)
        return fail(ErrorCode::Capacity, "drain transfer capacity");
      report.entry_ids.push_back(e.id);
    }
  }
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  for (auto &rv : rows) {
    auto e = entry(a(rv));
    if (e.owner && *e.owner == owner && e.publication == QueuePublication::PublishedProtocol) {
      a(rv)[2] = Value(drain);
      if (e.consumer) {
        if (!results_) {
          t.rollback(std::move(cp.value()));
          return fail(ErrorCode::InvalidState, "consumer store missing");
        }
        auto moved = results_->prepare_transfer(t, *e.consumer, drain.owner);
        if (!moved) {
          t.rollback(std::move(cp.value()));
          return moved.error();
        }
        a(rv)[4] = consumer_value(moved.value());
      }
    }
  }
  if (!report.entry_ids.empty()) {
    auto w = t.buffer(cell_, Value(std::move(state)));
    if (!w) {
      t.rollback(std::move(cp.value()));
      return w.error();
    }
  }
  return report;
}
} // namespace leanat
