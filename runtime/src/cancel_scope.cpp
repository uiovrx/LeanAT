#include "leanat/cancel_scope.hpp"
#include "leanat/storage_identity.hpp"
namespace leanat {
Expected<CancelScopeStore::Snapshot> CancelScopeStore::snapshot() const {
  if(transaction_pending_)return fail(ErrorCode::NotReady,"scope snapshot transaction pending");
  Snapshot out;out.sequence=sequence_;
  for(std::size_t i=0;i<records_.size();++i){const auto&r=records_[i];if(r.used)out.scopes.push_back({{HandleKind::Scope,domain_,store_,static_cast<std::uint32_t>(i),r.generation,0},r.parent,r.state,r.entries,r.plan});}
  for(const auto&o:observations_)out.observers.push_back({o.id,o.source,o.process,o.wait});
  return out;
}
CancelScopeStore::CancelScopeStore(DomainId d, std::uint32_t s, std::size_t n, std::size_t e,
                                   std::size_t a, std::size_t o)
    : domain_(d), store_(storage_detail::allocate_store_incarnation()), entries_(e), actions_(a),
      observers_(o), records_(n) {
  (void)s;
  if (!n)
    throw std::invalid_argument("scope capacity");
  records_[0].used = true;
}
Handle CancelScopeStore::root() const {
  return {HandleKind::Scope, domain_, store_, 0, records_[0].generation, 0};
}
Expected<CancelScopeStore::Record *> CancelScopeStore::get(Handle h) {
  if (transaction_pending_ && !transaction_)
    return fail(ErrorCode::InvalidState, "scope transaction pending");
  if (h.domain != domain_)
    return fail(ErrorCode::WrongDomain, "scope domain");
  if (h.kind != HandleKind::Scope || h.store != store_ || h.slot >= records_.size() ||
      !records_[h.slot].used || records_[h.slot].generation != h.generation)
    return fail(ErrorCode::StaleHandle, "scope identity");
  if (h.owner != 0)
    return fail(ErrorCode::WrongOwner, "scope handle owner");
  return &records_[h.slot];
}
Expected<const CancelScopeStore::Record *> CancelScopeStore::get(Handle h) const {
  auto r = const_cast<CancelScopeStore *>(this)->get(h);
  if (!r)
    return r.error();
  return static_cast<const Record *>(r.value());
}
std::optional<Handle> CancelScopeStore::owner_of(Handle object) const {
  if (object.kind == HandleKind::Scope) {
    if (get(object))
      return object;
    return {};
  }
  for (std::size_t i = 0; i < records_.size(); ++i)
    if (records_[i].used)
      for (auto &e : records_[i].entries)
        if (e.handle == object)
          return Handle{HandleKind::Scope,      domain_, store_, static_cast<std::uint32_t>(i),
                        records_[i].generation, 0};
  return {};
}
bool CancelScopeStore::inside(Handle h, Handle ancestor) const {
  for (std::size_t i = 0; i < records_.size(); ++i) {
    if (h == ancestor)
      return true;
    auto r = get(h);
    if (!r || h.slot == 0)
      return false;
    h = r.value()->parent;
  }
  return false;
}
bool CancelScopeStore::frozen(Handle h) const {
  for (std::size_t i = 0; i < records_.size(); ++i) {
    auto r = get(h);
    if (!r || r.value()->state != ScopeState::Open)
      return true;
    if (h.slot == 0)
      return false;
    h = r.value()->parent;
  }
  return true;
}
Expected<Handle> CancelScopeStore::create(Handle parent) {
  auto p = get(parent);
  if (!p)
    return p.error();
  if (frozen(parent))
    return fail(ErrorCode::InvalidState, "scope frozen");
  if (p.value()->entries.size() >= entries_)
    return fail(ErrorCode::Capacity, "scope owner entries");
  for (std::size_t i = 1; i < records_.size(); ++i)
    if (!records_[i].used && records_[i].generation != UINT64_MAX) {
      auto &r = records_[i];
      if (*allocation_generation_ == UINT64_MAX)
        return fail(ErrorCode::Overflow, "scope generation");
      r.generation = (*allocation_generation_)++;
      r.used = true;
      r.parent = parent;
      r.state = ScopeState::Open;
      r.plan.reset();
      r.entries.clear();
      Handle h{HandleKind::Scope, domain_, store_, static_cast<std::uint32_t>(i), r.generation, 0};
      p.value()->entries.push_back({h, OwnedKind::ChildScope, false});
      return h;
    }
  return fail(ErrorCode::Capacity, "scope slots");
}
Expected<void> CancelScopeStore::attach(ScopeOwned x, Handle h) {
  auto r = get(h);
  if (!r)
    return r.error();
  if (x.handle.domain != domain_)
    return fail(ErrorCode::WrongDomain, "owned domain");
  if (frozen(h))
    return fail(ErrorCode::InvalidState, "scope frozen");
  if (x.kind == OwnedKind::ChildScope)
    return fail(ErrorCode::InvalidArgument, "child scopes attach through create");
  for (auto &s : records_)
    for (auto &e : s.entries)
      if (e.handle == x.handle)
        return fail(ErrorCode::Duplicate, "owned handle already attached");
  if (r.value()->entries.size() >= entries_)
    return fail(ErrorCode::Capacity, "owner entries");
  r.value()->entries.push_back(x);
  return {};
}
Expected<void> CancelScopeStore::transfer(Handle object, Handle from, Handle to) {
  auto a = get(from);
  auto b = get(to);
  if (!a)
    return a.error();
  if (!b)
    return b.error();
  auto it = std::find_if(a.value()->entries.begin(), a.value()->entries.end(),
                         [&](const auto &e) { return e.handle == object; });
  if (it == a.value()->entries.end())
    return fail(ErrorCode::WrongOwner, "transfer owner");
  if (frozen(from) || frozen(to))
    return fail(ErrorCode::InvalidState, "frozen transfer");
  if (from == to)
    return {};
  if (b.value()->entries.size() >= entries_)
    return fail(ErrorCode::Capacity, "target owner entries");
  if (it->kind == OwnedKind::ChildScope) {
    if (inside(to, object))
      return fail(ErrorCode::InvalidArgument, "scope cycle");
    auto child = get(object);
    if (!child)
      return child.error();
    auto previous = child.value()->parent;
    child.value()->parent = to;
    for (auto &o : observations_)
      if ((owner_of(o.process) && inside(*owner_of(o.process), o.source)) ||
          (owner_of(o.wait) && inside(*owner_of(o.wait), o.source))) {
        child.value()->parent = previous;
        return fail(ErrorCode::InvalidArgument, "observer enters cancellation closure");
      }
    child.value()->parent = previous;
  }
  for (auto &o : observations_) {
    auto po = o.process == object ? std::optional<Handle>{to} : owner_of(o.process);
    auto wo = o.wait == object ? std::optional<Handle>{to} : owner_of(o.wait);
    if ((po && inside(*po, o.source)) || (wo && inside(*wo, o.source)))
      return fail(ErrorCode::InvalidArgument, "observer enters cancellation closure");
  }
  b.value()->entries.push_back(*it);
  if (it->kind == OwnedKind::ChildScope)
    records_[object.slot].parent = to;
  a.value()->entries.erase(it);
  return {};
}
Expected<void> CancelScopeStore::replace_owned(Handle old, ScopeOwned replacement, Handle scope) {
  auto r = get(scope);
  if (!r)
    return r.error();
  if (frozen(scope))
    return fail(ErrorCode::InvalidState, "scope frozen");
  if (replacement.handle.domain != domain_ || replacement.kind == OwnedKind::ChildScope)
    return fail(ErrorCode::InvalidArgument, "replacement owner");
  auto &es = r.value()->entries;
  auto it = std::find_if(es.begin(), es.end(), [&](auto &e) { return e.handle == old; });
  if (it == es.end())
    return fail(ErrorCode::WrongOwner, "replacement source");
  for (auto &q : records_)
    for (auto &e : q.entries)
      if (e.handle == replacement.handle)
        return fail(ErrorCode::Duplicate, "replacement already owned");
  *it = replacement;
  return {};
}
Expected<void> CancelScopeStore::detach_completed(Handle object, Handle scope) {
  auto r = get(scope);
  if (!r)
    return r.error();
  if (r.value()->state == ScopeState::Cancelling)
    return fail(ErrorCode::InvalidState, "cancel plan owns cleanup");
  auto &es = r.value()->entries;
  auto it = std::find_if(es.begin(), es.end(), [&](auto &e) { return e.handle == object; });
  if (it == es.end())
    return fail(ErrorCode::WrongOwner, "completed owner");
  if (it->kind == OwnedKind::ChildScope)
    return fail(ErrorCode::InvalidState, "child must close");
  es.erase(it);
  return {};
}
Expected<ScopeCancelPlan> CancelScopeStore::cancel(Handle h, std::string reason) {
  if (reason.size() > 256)
    return fail(ErrorCode::Capacity, "cancel reason bytes");
  auto r = get(h);
  if (!r)
    return r.error();
  if (r.value()->plan)
    return *r.value()->plan;
  if (r.value()->state != ScopeState::Open)
    return fail(ErrorCode::InvalidState, "scope closed");
  std::vector<Handle> closure;
  std::size_t count = 0;
  for (std::size_t i = 0; i < records_.size(); ++i)
    if (records_[i].used) {
      Handle s{HandleKind::Scope,      domain_, store_, static_cast<std::uint32_t>(i),
               records_[i].generation, 0};
      if (inside(s, h) && records_[i].state == ScopeState::Open) {
        closure.push_back(s);
        if (records_[i].entries.size() > actions_ - std::min(actions_, count))
          return fail(ErrorCode::Capacity, "cancel action budget");
        count += records_[i].entries.size();
      }
    }
  if (count > UINT64_MAX - sequence_)
    return fail(ErrorCode::Overflow, "cancel sequence");
  for (auto s : closure) {
    auto &q = records_[s.slot];
    ScopeCancelPlan p{s, reason, {}};
    for (auto e : q.entries) {
      auto k = e.kind == OwnedKind::ResultConsumer ? CancelActionKind::ReleaseConsumer
               : e.kind == OwnedKind::ChildScope || e.kind == OwnedKind::Task
                   ? CancelActionKind::CancelChild
               : e.published ? CancelActionKind::TransferToDrain
                             : CancelActionKind::DropLocal;
      p.actions.push_back({++sequence_, s, e, k, false});
    }
    q.plan = std::move(p);
    q.state = q.entries.empty() ? ScopeState::Cancelled : ScopeState::Cancelling;
  }
  return *r.value()->plan;
}
Expected<void>
CancelScopeStore::apply(Handle h, std::uint64_t id,
                        const std::function<Expected<void>(const ScopeCancelAction &)> &fn) {
  auto r = get(h);
  if (!r)
    return r.error();
  if (!r.value()->plan)
    return fail(ErrorCode::InvalidState, "no cancellation plan");
  auto &p = *r.value()->plan;
  auto it = std::find_if(p.actions.begin(), p.actions.end(), [&](auto &a) { return a.id == id; });
  if (it == p.actions.end())
    return fail(ErrorCode::InvalidArgument, "cancel action identity");
  if (it->applied)
    return {};
  if (!fn)
    return fail(ErrorCode::InvalidArgument, "missing cleanup executor");
  auto done = fn(*it);
  if (!done)
    return done.error();
  it->applied = true;
  auto &es = r.value()->entries;
  es.erase(
      std::remove_if(es.begin(), es.end(), [&](auto &e) { return e.handle == it->owned.handle; }),
      es.end());
  if (es.empty())
    r.value()->state = ScopeState::Cancelled;
  return {};
}
Expected<void> CancelScopeStore::close(Handle h) {
  auto r = get(h);
  if (!r)
    return r.error();
  if (h.slot == 0)
    return fail(ErrorCode::InvalidState, "runtime root cannot close");
  if (r.value()->state == ScopeState::Cancelling || !r.value()->entries.empty())
    return fail(ErrorCode::InvalidState, "scope not empty");
  for (const auto &child : records_)
    if (child.used && child.parent == h)
      return fail(ErrorCode::InvalidState, "scope has live child scopes");
  for (auto &o : observations_)
    if (o.source == h || o.process == h || o.wait == h)
      return fail(ErrorCode::InvalidState, "scope observation retained");
  auto p = get(r.value()->parent);
  if (p) {
    auto &es = p.value()->entries;
    es.erase(std::remove_if(es.begin(), es.end(), [&](auto &e) { return e.handle == h; }),
             es.end());
  }
  r.value()->state = ScopeState::Closed;
  r.value()->used = false;
  if (r.value()->generation != UINT64_MAX)
    ++r.value()->generation;
  return {};
}
Expected<ScopeState> CancelScopeStore::state(Handle h) const {
  auto r = get(h);
  if (!r)
    return r.error();
  return r.value()->state;
}
Expected<void> CancelScopeStore::validate_owner(Handle h, bool require_open) const {
  auto r = get(h);
  if (!r)
    return r.error();
  if (require_open && frozen(h))
    return fail(ErrorCode::Cancelled, "scope cancellation closure frozen");
  return {};
}
Expected<std::uint64_t> CancelScopeStore::observe_cancel(Handle source, Handle process,
                                                         Handle wait) {
  auto sr = get(source);
  if (!sr)
    return sr.error();
  auto po = owner_of(process), wo = owner_of(wait);
  if (!po || !wo)
    return fail(ErrorCode::StaleHandle, "observer ownership");
  if (inside(*po, source) || inside(*wo, source))
    return fail(ErrorCode::InvalidArgument, "observer cancelled with source");
  if (observations_.size() >= observers_)
    return fail(ErrorCode::Capacity, "cancel observers");
  if (sequence_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "observer sequence");
  observations_.push_back({++sequence_, source, process, wait});
  return sequence_;
}
Expected<void> CancelScopeStore::release_observer(std::uint64_t id) {
  auto it =
      std::find_if(observations_.begin(), observations_.end(), [&](auto &o) { return o.id == id; });
  if (it == observations_.end())
    return fail(ErrorCode::StaleHandle, "observer");
  observations_.erase(it);
  return {};
}
Expected<std::optional<std::string>> CancelScopeStore::cancel_reason(Handle h) const {
  auto r = get(h);
  if (!r)
    return r.error();
  if (!r.value()->plan)
    return std::optional<std::string>{};
  return std::optional<std::string>{r.value()->plan->reason};
}
} // namespace leanat
