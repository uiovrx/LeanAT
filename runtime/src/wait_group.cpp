#include "leanat/wait_group.hpp"
#include "leanat/storage_identity.hpp"
namespace leanat {
Expected<std::vector<WaitGroupStore::GroupSnapshot>> WaitGroupStore::snapshot() const {
  if(transaction_pending_)return fail(ErrorCode::NotReady,"wait snapshot transaction pending");
  std::vector<GroupSnapshot> out;
  for(std::size_t i=0;i<groups_.size();++i){const auto&g=groups_[i];if(!g.used)continue;
    GroupSnapshot s{{HandleKind::Wait,domain_,store_,static_cast<std::uint32_t>(i),g.generation,g.result.consumer.result.owner},g.desc.owner_process,g.desc.owner_scope,g.desc.mode,g.desc.failure_policy,g.desc.branch_count,g.desc.result_bytes,g.desc.result_type,g.result,g.committed,g.resolved,g.resume_pending,{}};
    for(const auto&b:g.branches)s.branches.push_back({b.desc.ordinal,b.desc.priority,b.desc.source,b.desc.result_type,b.desc.field_name,b.consumer,b.candidate?std::optional<ReadyKey>{b.candidate->ready}:std::nullopt,b.candidate?std::optional<Value>{b.candidate->outcome}:std::nullopt,b.candidate?b.candidate->failed:false,bool(b.pin),b.transferred});
    out.push_back(std::move(s));
  }return out;
}
WaitGroupStore::WaitGroupStore(DomainId d, std::uint32_t s, std::size_t n, std::size_t b,
                               std::size_t cb, ResultStore &r)
    : domain_(d), store_(storage_detail::allocate_store_incarnation()), branch_limit_(b),
      candidate_bytes_(cb), results_(r), groups_(n) {
  (void)s;
}
Expected<WaitGroupStore::Group *> WaitGroupStore::get(Handle h) {
  if (transaction_pending_ && !transaction_)
    return fail(ErrorCode::InvalidState, "wait transaction pending");
  if (h.domain != domain_)
    return fail(ErrorCode::WrongDomain, "wait domain");
  if (h.kind != HandleKind::Wait || h.store != store_ || h.slot >= groups_.size() ||
      !groups_[h.slot].used || groups_[h.slot].generation != h.generation)
    return fail(ErrorCode::StaleHandle, "wait identity");
  if (h.owner != groups_[h.slot].result.consumer.result.owner)
    return fail(ErrorCode::WrongOwner, "wait handle owner");
  return &groups_[h.slot];
}
Expected<void> WaitGroupStore::cleanup(Group &g) {
  for (auto &b : g.branches) {
    if (b.desc.unregister && !b.transferred) {
      try { b.desc.unregister(); }
      catch (...) { return fail(ErrorCode::ExternalFailure,"native wait unregister failed"); }
      b.desc.unregister = {};
    }
    b.pin.reset();
    if (b.consumer) {
      auto released = release_cap(*b.consumer);
      if (!released) return released.error();
      b.consumer.reset();
    }
  }
  return {};
}
Expected<void> WaitGroupStore::abort(Handle h, Error e) {
  auto g = get(h);
  if (g) {
    if (transaction_)
      for (auto &b : g.value()->branches)
        if (b.desc.unregister && !b.transferred)
          return fail(ErrorCode::Unsupported,
                      "transaction cannot execute native unregister callback");
    auto cleaned = cleanup(*g.value());
    if (!cleaned) return cleaned.error();
    auto released = release_cap(g.value()->result.consumer, ReleasePolicy::DropOnTerminal);
    if (!released) return released.error();
    auto producer = release_producer(g.value()->result.reservation);
    if (!producer) return producer.error();
    g.value()->used = false;
    if (g.value()->generation != UINT64_MAX)
      ++g.value()->generation;
  }
  return e;
}
Expected<Handle> WaitGroupStore::create(WaitCreate d) {
  if (transaction_pending_ && !transaction_)
    return fail(ErrorCode::InvalidState, "wait transaction pending");
  if (d.owner_process.domain != domain_ || d.owner_scope.domain != domain_)
    return fail(ErrorCode::WrongDomain, "wait owners");
  if (d.owner_process.kind != HandleKind::Process || d.owner_scope.kind != HandleKind::Scope)
    return fail(ErrorCode::InvalidArgument, "wait owner kinds");
  if (d.branch_count > branch_limit_ || (d.mode == WaitMode::Any && !d.branch_count))
    return fail(ErrorCode::InvalidArgument, "wait branch count");
  for (auto &g : groups_)
    if (g.used && (!g.resolved || g.resume_pending) && g.desc.owner_process == d.owner_process)
      return fail(ErrorCode::InvalidState, "process already suspended");
  for (std::size_t i = 0; i < groups_.size(); ++i)
    if (!groups_[i].used && groups_[i].generation != UINT64_MAX) {
      auto &g = groups_[i];
      if (*allocation_generation_ == UINT64_MAX)
        return fail(ErrorCode::Overflow, "wait generation");
      g.generation = (*allocation_generation_)++;
      Handle h{HandleKind::Wait, domain_,
               store_,           static_cast<std::uint32_t>(i),
               g.generation,     d.owner_scope.slot + 1ULL};
      auto r = reserve_result({h, d.result_type, d.result_bytes, h.owner, h.owner});
      if (!r)
        return r.error();
      g.used = true;
      g.resolved = false;
      g.resume_pending = false;
      g.committed = false;
      g.desc = std::move(d);
      g.result = r.value();
      g.branches.clear();
      g.branches.reserve(g.desc.branch_count);
      return h;
    }
  return fail(ErrorCode::Capacity, "wait groups");
}
Expected<void> WaitGroupStore::arm(Handle h, WaitBranch b, BatchId batch, ReadyKey now) {
  auto gr = get(h);
  if (!gr)
    return gr.error();
  auto &g = *gr.value();
  if (transaction_)
    for (auto &existing : g.branches)
      if (existing.desc.unregister && !existing.transferred)
        return fail(ErrorCode::Unsupported, "transaction cannot mutate native wait registration");
  if (b.field_name.size() > 256)
    return abort(h, fail(ErrorCode::Capacity, "wait branch field name bytes"));
  if (transaction_ && (b.unregister || b.transfer_winner))
    return fail(ErrorCode::Unsupported, "transactional wait arm requires staged resource hooks");
  if (g.committed || g.resolved)
    return fail(ErrorCode::InvalidState, "wait already committed");
  if (g.branches.size() >= g.desc.branch_count || b.ordinal >= g.desc.branch_count)
    return abort(h, fail(ErrorCode::InvalidArgument, "branch ordinal"));
  if (b.source.domain != domain_)
    return abort(h, fail(ErrorCode::WrongDomain, "branch source"));
  for (auto &x : g.branches)
    if (x.desc.ordinal == b.ordinal || (!b.field_name.empty() && x.desc.field_name == b.field_name))
      return abort(h, fail(ErrorCode::Duplicate, "branch declaration"));
  Branch branch{std::move(b), {}, {}, {}, false};
  if (branch.desc.source_result) {
    auto description = transaction_
                           ? results_.prepare_describe(*transaction_, *branch.desc.source_result)
                           : results_.describe(*branch.desc.source_result);
    if (!description)
      return abort(h, description.error());
    if (description.value().source != branch.desc.source)
      return abort(h, fail(ErrorCode::WrongOwner, "wait result belongs to another source"));
    branch.desc.result_type = description.value().type;
    auto retained = retain_cap(*branch.desc.source_result, h.owner);
    if (!retained)
      return abort(h, retained.error());
    branch.consumer = retained.value();
    auto ready = transaction_ ? results_.prepare_ready(*transaction_, *branch.consumer)
                              : results_.published_ready(*branch.consumer);
    if (ready && !(now < ready.value())) {
      Expected<Value> owning = transaction_ ? results_.prepare_read(*transaction_, *branch.consumer)
                                            : results_.read(*branch.consumer);
      if (!owning) {
        release_cap(*branch.consumer);
        return abort(h, owning.error());
      }
      if (!transaction_) {
        auto pin = results_.pin(*branch.consumer);
        if (!pin) {
          release_cap(*branch.consumer);
          return abort(h, pin.error());
        }
        branch.pin = pin.value();
      }
      auto bytes = owned_value_bytes(owning.value());
      if (bytes > candidate_bytes_) {
        release_cap(*branch.consumer);
        return abort(h, fail(ErrorCode::Capacity, "candidate bytes"));
      }
      bool failed = false;
      if (branch.desc.source.kind == HandleKind::Task) {
        if (auto a = std::get_if<Value::Array>(&owning.value().data); a && !a->empty())
          if (auto tag = std::get_if<std::uint64_t>(&(*a)[0].data))
            failed = *tag != 0;
      }
      branch.candidate = Candidate{ready.value(), batch, std::move(owning.value()), failed};
    } else if (!ready && ready.error().code != ErrorCode::NotReady) {
      release_cap(*branch.consumer);
      return abort(h, ready.error());
    }
  }
  g.branches.push_back(std::move(branch));
  return {};
}
Expected<void> WaitGroupStore::commit(Handle h) {
  auto r = get(h);
  if (!r)
    return r.error();
  if (r.value()->committed)
    return fail(ErrorCode::Duplicate, "wait commit");
  if (r.value()->branches.size() != r.value()->desc.branch_count)
    return abort(h, fail(ErrorCode::InvalidState, "incomplete wait prepare"));
  std::sort(r.value()->branches.begin(), r.value()->branches.end(),
            [](auto &a, auto &b) { return a.desc.ordinal < b.desc.ordinal; });
  r.value()->committed = true;
  return {};
}
Expected<void> WaitGroupStore::record(WaitNotification n) {
  auto r = get(n.group);
  if (!r)
    return r.error();
  auto &g = *r.value();
  if (g.resolved)
    return fail(ErrorCode::InvalidState, "wait already resolved");
  auto it = std::find_if(g.branches.begin(), g.branches.end(),
                         [&](auto &b) { return b.desc.ordinal == n.ordinal; });
  if (it == g.branches.end() || it->desc.source != n.source)
    return fail(ErrorCode::StaleHandle, "wait notification source");
  if (it->candidate)
    return {};
  if (owned_value_bytes(n.outcome) > candidate_bytes_)
    return fail(ErrorCode::Capacity, "wait candidate bytes");
  it->candidate = Candidate{n.ready, n.delivery_batch, std::move(n.outcome), n.failed};
  return {};
}
Expected<std::vector<WaitResolution>> WaitGroupStore::resolve_closed_batch(BatchId batch,
                                                                           bool closed) {
  if (transaction_pending_ || transaction_)
    return fail(ErrorCode::InvalidState, "resolve requires committed closed-batch state");
  if (!closed)
    return fail(ErrorCode::InvalidState, "candidate batch not closed");
  std::vector<WaitResolution> out;
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    auto &g = groups_[i];
    if (!g.used || !g.committed)
      continue;
    if (g.resolved) {
      if (g.resume_pending) {
        auto stored = results_.read(g.result.consumer);
        if (!stored)
          return stored.error();
        auto key = results_.published_ready(g.result.consumer);
        if (!key)
          return key.error();
        auto delivered = g.desc.resume(stored.value(), key.value());
        if (!delivered)
          return delivered.error();
        g.resume_pending = false;
        Handle h{HandleKind::Wait, domain_,
                 store_,           static_cast<std::uint32_t>(i),
                 g.generation,     g.desc.owner_scope.slot + 1ULL};
        out.push_back({h, key.value(), std::move(stored.value())});
      }
      continue;
    }
    auto eligible = [&](const Branch &b) { return b.candidate && !(batch < b.candidate->batch); };
    auto less = [](const Branch *a, const Branch *b) {
      return std::tie(a->candidate->ready, a->desc.priority, a->desc.ordinal) <
             std::tie(b->candidate->ready, b->desc.priority, b->desc.ordinal);
    };
    Branch *winner = nullptr;
    std::size_t count = 0;
    ReadyKey key{};
    for (auto &b : g.branches)
      if (eligible(b)) {
        ++count;
        if (key < b.candidate->ready)
          key = b.candidate->ready;
        if ((g.desc.mode == WaitMode::Any || b.candidate->failed) && (!winner || less(&b, winner)))
          winner = &b;
      }
    Value result;
    if (g.desc.mode == WaitMode::Any) {
      if (!winner)
        continue;
      key = winner->candidate->ready;
      result = Value{Value::Array{Value{static_cast<std::uint64_t>(winner->desc.ordinal)},
                                  winner->candidate->outcome}};
    } else if (count == g.branches.size() &&
               (g.desc.failure_policy == WaitFailurePolicy::CollectAll || !winner)) {
      Value::Array values;
      for (auto &b : g.branches)
        values.push_back(b.candidate->outcome);
      result = Value{Value::Array{Value{std::uint64_t{0}}, Value{std::move(values)}}};
    } else if (g.desc.failure_policy == WaitFailurePolicy::FailFast && winner) {
      Value::Array observed;
      for (auto &b : g.branches)
        observed.push_back(eligible(b) ? Value{Value::Array{Value{true}, b.candidate->outcome}}
                                       : Value{Value::Array{Value{false}}});
      key = winner->candidate->ready;
      result = Value{Value::Array{Value{std::uint64_t{1}},
                                  Value{static_cast<std::uint64_t>(winner->desc.ordinal)},
                                  winner->candidate->outcome, Value{std::move(observed)}}};
    } else
      continue;
    Handle h{HandleKind::Wait, domain_,
             store_,           static_cast<std::uint32_t>(i),
             g.generation,     g.desc.owner_scope.slot + 1ULL};
    if (owned_value_bytes(result) > g.desc.result_bytes)
      return fail(ErrorCode::Capacity, "wait aggregate result bytes");
    if (g.desc.mode == WaitMode::Any && winner->desc.transfer_winner && !winner->transferred) {
      auto t = winner->desc.transfer_winner();
      if (!t)
        return t.error();
      winner->transferred = true;
    }
    auto p = publish_result(g.result.reservation, result, {h, key, true});
    if (!p)
      return p.error();
    g.resolved = true;
    auto cleaned = cleanup(g);
    if (!cleaned) return fail(ErrorCode::Integrity,"published wait cleanup failed: " + cleaned.error().message);
    auto producer_released = release_producer(g.result.reservation);
    if (!producer_released)
      return fail(ErrorCode::Integrity,
                  "wait publication producer release failed: " + producer_released.error().message);
    out.push_back({h, key, result});
    if (g.desc.resume) {
      g.resume_pending = true;
      auto resumed = g.desc.resume(result, key);
      if (!resumed)
        return resumed.error();
      g.resume_pending = false;
    }
  }
  return out;
}
Expected<ResultHandle> WaitGroupStore::result_handle(Handle h, Handle owner) {
  auto g = get(h);
  if (!g)
    return g.error();
  if (g.value()->desc.owner_scope != owner)
    return fail(ErrorCode::WrongOwner, "wait owner");
  return g.value()->result.consumer;
}
Expected<PinnedResultView> WaitGroupStore::result(Handle h, ResultHandle r) {
  auto g = get(h);
  if (!g)
    return g.error();
  if (g.value()->result.consumer.result != r.result)
    return fail(ErrorCode::WrongOwner, "wait result");
  return results_.pin(r);
}
Expected<ResultHandle> WaitGroupStore::retain_wait_result(Handle h, ResultHandle r,
                                                          std::uint64_t owner) {
  auto g = get(h);
  if (!g)
    return g.error();
  if (g.value()->result.consumer.result != r.result)
    return fail(ErrorCode::WrongOwner, "wait retain");
  return retain_cap(r, owner);
}
Expected<void> WaitGroupStore::release(Handle h, ResultHandle r, Handle owner) {
  auto gr = get(h);
  if (!gr)
    return gr.error();
  auto &g = *gr.value();
  if (transaction_)
    for (auto &b : g.branches)
      if (b.desc.unregister && !b.transferred)
        return fail(ErrorCode::Unsupported,
                    "transaction cannot execute native unregister callback");
  if (g.desc.owner_scope != owner || r.consumer != g.result.consumer.consumer ||
      r.result != g.result.consumer.result)
    return fail(ErrorCode::WrongOwner, "wait control release owner");
  auto cleaned = cleanup(g);
  if (!cleaned) return cleaned.error();
  auto rel =
      release_cap(r, g.resolved ? ReleasePolicy::CurrentConsumer : ReleasePolicy::DropOnTerminal);
  if (!rel)
    return rel.error();
  if (!g.resolved) {
    auto producer = release_producer(g.result.reservation);
    if (!producer) return producer.error();
  }
  g.used = false;
  if (g.generation != UINT64_MAX)
    ++g.generation;
  return {};
}
} // namespace leanat
