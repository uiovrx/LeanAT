#include "leanat/managed.hpp"
#include <atomic>
namespace leanat {
namespace {
std::uint32_t managed_store_id() {
  static std::atomic<std::uint32_t> next{0x25000000};
  auto id = next.fetch_add(1);
  if (id < 0x25000000 || id == UINT32_MAX) {
    throw std::overflow_error("managed store identity exhausted");
  }
  return id;
}
class ManagedWrite final : public PreparedParticipant {
  std::shared_ptr<Bytes> backing_;
  const Bytes &input_;
  std::size_t offset_, size_;
  std::shared_ptr<VersionedCell> cell_;
  std::uint64_t version_{};

public:
  ManagedWrite(std::shared_ptr<Bytes> backing, const Bytes &input, std::size_t offset,
               std::shared_ptr<VersionedCell> cell)
      : backing_(std::move(backing)), input_(input), offset_(offset), size_(backing_->size()),
        cell_(std::move(cell)), version_(cell_ ? cell_->version : 0) {}
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this);
  }
  Expected<void> validate() const override {
    if ((cell_ && (cell_->version != version_ || version_ == UINT64_MAX)) ||
        backing_->size() != size_ || offset_ > size_ || input_.size() > size_ - offset_) {
      return fail(ErrorCode::Integrity, "managed backing changed while pinned");
    }
    return {};
  }
  void apply() noexcept override {
    std::copy(input_.begin(), input_.end(), backing_->begin() + offset_);
    if (cell_) {
      ++cell_->version;
    }
  }
  void discard() noexcept override {}
};
} // namespace
Expected<Tick> ManagedServiceResource::reserve(Tick arrival, Duration service) {
  auto finish = add_time(Tick{std::max(arrival.value, free_.value)}, service);
  if (!finish) {
    return finish.error();
  }
  free_ = finish.value();
  return free_;
}
struct ManagedAccessManager::Impl {
  struct Lease {
    Handle handle;
    ManagedRequest request;
    std::uint64_t version{};
    bool valid{};
  };
  struct Op {
    Handle handle;
    OwnedAccessRequest request;
    ReservedResult result;
    RegionId region;
    Handle ticket;
    std::shared_ptr<Bytes> pin;
    std::shared_ptr<VersionedCell> cell;
    std::uint64_t version{}, offset{}, sequence{};
    Tick finish{};
    bool admitted{}, terminal{}, released{}, resource_settled{};
    ManagedCompletion completion;
    Value prepared;
  };
  struct Change {
    RegionId region;
    std::uint64_t start, end, sequence;
    Tick time;
  };
  ResultStore &results;
  DomainId domain;
  bool enabled;
  ManagedLimits limits;
  std::uint32_t store;
  Tick now{};
  std::uint64_t sequence{}, current_turn{};
  std::size_t input_bytes{};
  std::shared_ptr<ManagedServiceResource> resource;
  std::shared_ptr<ManagedResourceBinding> binding{std::make_shared<ManagedResourceBinding>()};
  std::shared_ptr<std::map<RegionId, ManagedRegionDesc>> regions{
      std::make_shared<std::map<RegionId, ManagedRegionDesc>>()};
  std::uint64_t state_version{};
  EventTxn *preparing_txn{};
  std::vector<std::optional<Lease>> leases;
  // Allocation identities survive rollback; only object occupancy is transactional.
  std::shared_ptr<std::vector<std::uint64_t>> lease_generations;
  std::vector<std::optional<Op>> ops;
  std::shared_ptr<std::vector<std::uint64_t>> op_generations;
  std::vector<Change> changes;
  Impl(ResultStore &r, DomainId d, bool e, ManagedLimits l, std::uint32_t s,
       std::shared_ptr<ManagedServiceResource> resource_)
      : results(r), domain(d), enabled(e), limits(l), store(s ? s : managed_store_id()),
        resource(resource_ ? std::move(resource_) : std::make_shared<ManagedServiceResource>()),
        leases(l.leases), lease_generations(std::make_shared<std::vector<std::uint64_t>>(l.leases)),
        ops(l.operations),
        op_generations(std::make_shared<std::vector<std::uint64_t>>(l.operations)) {}
  Expected<Lease *> lease(Handle h) {
    if (h.domain != domain) {
      return fail(ErrorCode::WrongDomain, "managed lease domain");
    }
    if (h.kind != HandleKind::Lease || h.store != store || h.slot >= leases.size() ||
        !leases[h.slot] || leases[h.slot]->handle != h) {
      return fail(ErrorCode::StaleHandle, "managed lease identity");
    }
    return &*leases[h.slot];
  }
  Expected<Op *> op(Handle h) {
    if (h.domain != domain) {
      return fail(ErrorCode::WrongDomain, "managed access domain");
    }
    if (h.kind != HandleKind::Access || h.store != store || h.slot >= ops.size() || !ops[h.slot] ||
        ops[h.slot]->handle != h) {
      return fail(ErrorCode::StaleHandle, "managed access identity");
    }
    return &*ops[h.slot];
  }
  Expected<void> terminal(Op &o, AccessFailure reason, CommitDisposition disposition) {
    if (o.terminal) {
      return {};
    }
    auto &a = std::get<Value::Array>(o.prepared.data);
    a[0] = Value{static_cast<std::uint64_t>(reason)};
    a[1] = Value{static_cast<std::uint64_t>(disposition)};
    if (reason != AccessFailure::None) {
      std::get<Bytes>(a[2].data).clear();
    }
    if (preparing_txn) {
      if (reason == AccessFailure::None) {
        return fail(ErrorCode::InvalidState, "finish cannot run during manager prepare");
      }
      auto published =
          results.prepare_publish_in(*preparing_txn, o.result.reservation, o.prepared,
                                     PublicationContext{o.handle, {now, current_turn}, true});
      if (!published) {
        return published.error();
      }
      auto released = results.prepare_release_owner(*preparing_txn, o.result.reservation);
      if (!released) {
        return released;
      }
      o.prepared = Value{};
      o.terminal = true;
      o.completion = {reason, disposition, {now, current_turn}};
      input_bytes -= o.request.input.size();
      o.request.input.clear();
      return {};
    }
    ExecutionContext context;
    context.domain = domain;
    context.owner = o.handle.owner;
    context.ready = {now, current_turn};
    SegmentBudget segment_budget;
    segment_budget.bytes = o.request.count + 65536;
    EventTxn txn(segment_budget, context);
    const bool completing_resource = reason == AccessFailure::None && bool(binding->complete);
    if (completing_resource) {
      auto done = binding->complete(txn, o.ticket, now);
      if (!done) {
        return done.error();
      }
      if (!done.value()) {
        reason = AccessFailure::Cancelled;
        disposition = CommitDisposition::NotCommitted;
        a[0] = Value{static_cast<std::uint64_t>(reason)};
        a[1] = Value{static_cast<std::uint64_t>(disposition)};
        std::get<Bytes>(a[2].data).clear();
      }
    }
    if (disposition == CommitDisposition::WriteCommitted) {
      auto staged = txn.stage_participant(
          std::make_unique<ManagedWrite>(o.pin, o.request.input, o.offset, o.cell));
      if (!staged) {
        return staged;
      }
    }
    auto prepared =
        results.prepare_publish(o.result.reservation, o.prepared,
                                PublicationContext{o.handle, ReadyKey{now, current_turn}, true});
    if (!prepared) {
      return prepared.error();
    }
    auto staged = txn.stage_participant(std::move(prepared.value()));
    if (!staged) {
      return staged;
    }
    auto committed = txn.commit();
    if (!committed) {
      return committed.error();
    }
    o.resource_settled = completing_resource;
    o.prepared = Value{};
    o.terminal = true;
    o.completion = {reason, disposition, {now, current_turn}};
    input_bytes -= o.request.input.size();
    if (reason != AccessFailure::None) {
      o.request.input.clear();
    }
    auto owner = results.release_owner(o.result.reservation);
    if (!owner) {
      return owner.error();
    }
    return {};
  }
  void collect() {
    for (auto &o : ops) {
      if (o && o->terminal && !o->pin && o->released) {
        o.reset();
      }
    }
  }
  Expected<void> change(const Change &c) {
    auto &r = regions->at(c.region);
    if (c.sequence) {
      current_turn = c.sequence;
    }
    for (auto &l : leases) {
      if (l && l->request.region == c.region && l->request.start <= c.end &&
          c.start <= l->request.end) {
        l->valid = false;
      }
    }
    for (auto &o : ops) {
      if (!o || o->terminal) {
        continue;
      }
      if (o->region != c.region || o->request.address > c.end ||
          c.start > o->request.address + o->request.count - 1) {
        continue;
      }
      if (!o->admitted || r.invalidation == InFlightPolicy::CancelBeforeCommit) {
        auto t = terminal(*o, AccessFailure::Invalidated, CommitDisposition::NotCommitted);
        if (!t) {
          return t;
        }
      }
    }
    collect();
    return {};
  }
};
ManagedAccessManager::ManagedAccessManager(ResultStore &r, DomainId d, bool e, ManagedLimits l,
                                           std::uint32_t s,
                                           std::shared_ptr<ManagedServiceResource> resource)
    : impl_(std::make_unique<Impl>(r, d, e, l, s, std::move(resource))) {}
ManagedAccessManager::~ManagedAccessManager() = default;
Expected<void> ManagedAccessManager::bind_resource(ManagedResourceBinding binding) {
  if (impl_->state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed version");
  }
  ++impl_->state_version;
  if (!binding.reserve || !binding.complete) {
    return fail(ErrorCode::InvalidArgument, "incomplete resource binding");
  }
  for (auto &o : impl_->ops) {
    if (o) {
      return fail(ErrorCode::InvalidState, "resource binding while operations exist");
    }
  }
  impl_->binding = std::make_shared<ManagedResourceBinding>(std::move(binding));
  return {};
}
Expected<void> ManagedAccessManager::add_region(ManagedRegionDesc r) {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  if (!m.enabled) {
    return fail(ErrorCode::Unsupported, "managed capability disabled");
  }
  if (r.allowed_connections.empty() || r.allowed_connections.size() > 128 ||
      static_cast<unsigned>(r.permission) < 1 || static_cast<unsigned>(r.permission) > 3 ||
      static_cast<unsigned>(r.invalidation) > 1) {
    return fail(ErrorCode::InvalidArgument, "invalid managed region capability/policy");
  }
  if (!r.static_path || (r.timing_strict && r.raw_alias)) {
    return fail(ErrorCode::Unsupported, "unsupported managed path or uncontrolled raw alias");
  }
  if (!r.backing || !r.service || r.end < r.start || r.end - r.start == UINT64_MAX ||
      r.end - r.start + 1 != r.backing->size() || !r.version) {
    return fail(ErrorCode::InvalidArgument, "invalid managed backing range");
  }
  if (m.regions->count(r.id)) {
    return fail(ErrorCode::Duplicate, "managed region");
  }
  if (m.regions->size() >= m.limits.regions) {
    return fail(ErrorCode::Capacity, "managed regions full");
  }
  m.regions->emplace(r.id, std::move(r));
  return {};
}
Expected<LeaseHandle> ManagedAccessManager::request(const ManagedRequest &r) {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  if (!m.enabled) {
    return fail(ErrorCode::Unsupported, "managed capability denied");
  }
  if (r.domain != m.domain) {
    return fail(ErrorCode::WrongDomain, "cross-domain managed lease");
  }
  auto it = m.regions->find(r.region);
  if (it == m.regions->end()) {
    return fail(ErrorCode::Unsupported, "managed region denied");
  }
  auto &region = it->second;
  if (std::find(region.allowed_connections.begin(), region.allowed_connections.end(),
                r.connection) == region.allowed_connections.end()) {
    return fail(ErrorCode::WrongOwner, "managed connection denied");
  }
  auto p = static_cast<unsigned>(r.permission);
  if (!p || p > 3 || (p & static_cast<unsigned>(region.permission)) != p) {
    return fail(ErrorCode::InvalidArgument, "managed permission denied");
  }
  if (r.end < r.start || r.start < region.start || r.end > region.end) {
    return fail(ErrorCode::InvalidArgument, "managed requested range");
  }
  for (std::size_t i = 0; i < m.leases.size(); ++i) {
    if (m.leases[i] || (*m.lease_generations)[i] == UINT64_MAX) {
      continue;
    }
    Handle h{HandleKind::Lease,           m.domain, m.store, static_cast<std::uint32_t>(i),
             ++(*m.lease_generations)[i], r.owner};
    m.leases[i] = Impl::Lease{h, r, region.version, true};
    return h;
  }
  return fail(ErrorCode::Capacity, "managed leases full");
}
Expected<AccessHandle> ManagedAccessManager::begin(OwnedAccessRequest r) {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  auto l = m.lease(r.lease);
  if (!l) {
    return l.error();
  }
  if (!l.value()->valid) {
    return fail(ErrorCode::StaleHandle, "invalidated managed lease");
  }
  if (r.byte_enable || r.streaming || r.atomic ||
      (r.command != Command::Read && r.command != Command::Write)) {
    return fail(ErrorCode::Unsupported, "managed operation unsupported");
  }
  auto permission = r.command == Command::Read ? 1u : 2u;
  if (!(static_cast<unsigned>(l.value()->request.permission) & permission)) {
    return fail(ErrorCode::InvalidArgument, "managed operation permission denied");
  }
  if (r.arrival.value < m.now.value) {
    return fail(ErrorCode::TimeRegression, "past managed arrival");
  }
  if (!r.count || r.address < l.value()->request.start || r.address > l.value()->request.end ||
      r.count - 1 > l.value()->request.end - r.address) {
    return fail(ErrorCode::InvalidArgument, "managed operation range");
  }
  if ((r.command == Command::Write && r.input.size() != r.count) ||
      (r.command == Command::Read && !r.input.empty())) {
    return fail(ErrorCode::InvalidArgument, "managed input length");
  }
  if (r.count > m.limits.input_bytes || r.input.size() > m.limits.input_bytes - m.input_bytes ||
      r.count > SIZE_MAX - 65536) {
    return fail(ErrorCode::Capacity, "managed input capacity");
  }
  if (m.sequence == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed event sequence");
  }
  for (std::size_t i = 0; i < m.ops.size(); ++i) {
    if (m.ops[i] || (*m.op_generations)[i] == UINT64_MAX) {
      continue;
    }
    Handle h{HandleKind::Access,         m.domain,     m.store, static_cast<std::uint32_t>(i),
             (*m.op_generations)[i] + 1, r.lease.owner};
    Value prepared{Value::Array{Value{std::uint64_t{0}}, Value{std::uint64_t{0}},
                                Value{Bytes(r.command == Command::Read ? r.count : 0)}}};
    auto create = ResultCreate{h, TypeId{25}, r.count + 256, h.owner, h.owner};
    auto result = m.preparing_txn ? m.results.prepare_reserve(*m.preparing_txn, create)
                                  : m.results.reserve(create);
    if (!result) {
      return result.error();
    }
    Impl::Op o;
    o.handle = h;
    o.result = result.value();
    o.region = l.value()->request.region;
    o.request = std::move(r);
    o.prepared = std::move(prepared);
    o.sequence = ++m.sequence;
    m.input_bytes += o.request.input.size();
    (*m.op_generations)[i] = h.generation;
    m.ops[i] = std::move(o);
    return h;
  }
  return fail(ErrorCode::Capacity, "managed operations full");
}
Expected<void> ManagedAccessManager::advance(Tick until) {
  return advance_impl(until, SIZE_MAX, std::nullopt);
}
Expected<void> ManagedAccessManager::advance_one(ReadyKey ready) {
  auto next = next_ready();
  if (!next || next->time != ready.time) {
    return fail(ErrorCode::NotReady, "managed dispatch time mismatch");
  }
  return advance_impl(ready.time, 1, ready.turn);
}
Expected<void> ManagedAccessManager::advance_impl(Tick until, std::size_t budget,
                                                  std::optional<std::uint64_t> dispatch_turn) {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  if (until.value < m.now.value) {
    return fail(ErrorCode::TimeRegression, "managed time regression");
  }
  for (std::size_t processed = 0; processed < budget; ++processed) {
    std::optional<std::tuple<std::uint64_t, std::uint64_t, unsigned, std::size_t>> next;
    for (std::size_t i = 0; i < m.ops.size(); ++i) {
      auto &o = m.ops[i];
      if (!o || (o->terminal && !o->pin)) {
        continue;
      }
      auto time = o->admitted ? o->finish : o->request.arrival;
      auto k = std::make_tuple(time.value, o->sequence, 0u, i);
      if (time.value <= until.value && (!next || k < *next)) {
        next = k;
      }
    }
    for (std::size_t i = 0; i < m.changes.size(); ++i) {
      auto &c = m.changes[i];
      auto k = std::make_tuple(c.time.value, c.sequence, 1u, i);
      if (c.time.value <= until.value && (!next || k < *next)) {
        next = k;
      }
    }
    if (!next) {
      break;
    }
    m.now = Tick{std::get<0>(*next)};
    m.current_turn = dispatch_turn.value_or(std::get<1>(*next));
    auto index = std::get<3>(*next);
    if (std::get<2>(*next) == 1) {
      auto c = m.changes[index];
      if (dispatch_turn) {
        c.sequence = *dispatch_turn;
      }
      m.changes.erase(m.changes.begin() + index);
      auto changed = m.change(c);
      if (!changed) {
        return changed;
      }
      continue;
    }
    auto &o = *m.ops[index];
    if (!o.admitted) {
      auto l = m.lease(o.request.lease);
      if (!l || !l.value()->valid) {
        auto t = m.terminal(o, AccessFailure::Invalidated, CommitDisposition::NotCommitted);
        if (!t) {
          return t;
        }
        continue;
      }
      auto &r = m.regions->at(l.value()->request.region);
      Expected<Duration> service =
          fail(ErrorCode::ExternalFailure, "managed service callback failed");
      try {
        service = r.service(o.request.command, o.request.count);
      } catch (...) {
      }
      if (!service) {
        auto t = m.terminal(o, AccessFailure::ServiceFailure, CommitDisposition::NotCommitted);
        if (!t) {
          return t;
        }
        continue;
      }
      ExecutionContext context;
      context.domain = m.domain;
      context.owner = o.handle.owner;
      context.ready = {m.now, o.sequence};
      SegmentBudget segment_budget;
      segment_budget.bytes = o.request.count + 65536;
      EventTxn txn(segment_budget, context);
      Expected<Tick> finish = fail(ErrorCode::InvalidState, "resource not reserved");
      if (m.binding->reserve) {
        auto grant = m.binding->reserve(txn, o.handle, m.now, service.value());
        if (grant) {
          auto commit = txn.commit();
          if (commit) {
            finish = grant.value().finish;
            o.ticket = grant.value().ticket;
          } else {
            finish = commit.error();
          }
        } else {
          finish = grant.error();
        }
      } else {
        finish = m.resource->reserve(m.now, service.value());
      }
      if (!finish) {
        auto t = m.terminal(o, AccessFailure::ServiceFailure, CommitDisposition::NotCommitted);
        if (!t) {
          return t;
        }
        continue;
      }
      o.pin = r.backing;
      o.cell = r.backing_cell;
      o.version = r.version;
      o.offset = o.request.address - r.start;
      o.finish = finish.value();
      o.admitted = true;
    } else {
      if (!o.terminal) {
        if (o.offset > o.pin->size() || o.request.count > o.pin->size() - o.offset) {
          return fail(ErrorCode::Integrity, "managed pinned backing resized");
        }
        bool write = o.request.command == Command::Write;
        if (!write) {
          auto &out = std::get<Bytes>(std::get<Value::Array>(o.prepared.data)[2].data);
          std::copy_n(o.pin->begin() + o.offset, o.request.count, out.begin());
        }
        auto t =
            m.terminal(o, AccessFailure::None,
                       write ? CommitDisposition::WriteCommitted : CommitDisposition::ReadObserved);
        if (!t) {
          return t;
        }
      }
      if (o.terminal && o.completion.failure != AccessFailure::None && m.binding->complete &&
          !o.resource_settled) {
        ExecutionContext context;
        context.domain = m.domain;
        context.owner = o.handle.owner;
        context.ready = {m.now, o.sequence};
        EventTxn cleanup(SegmentBudget{}, context);
        auto done = m.binding->complete(cleanup, o.ticket, m.now);
        if (!done) {
          return done.error();
        }
        auto committed = cleanup.commit();
        if (!committed) {
          return committed.error();
        }
      }
      o.resource_settled = true;
      o.pin.reset();
      o.cell.reset();
      o.request.input.clear();
    }
    m.collect();
  }
  m.now = until;
  m.collect();
  return {};
}
Expected<void> ManagedAccessManager::invalidate(RegionId r, std::uint64_t start, std::uint64_t end,
                                                Tick effective) {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  if (!m.regions->count(r) || end < start) {
    return fail(ErrorCode::InvalidArgument, "managed invalidation range");
  }
  if (effective.value < m.now.value) {
    return fail(ErrorCode::TimeRegression, "past invalidation");
  }
  if (m.sequence == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed sequence");
  }
  if (m.changes.size() >= m.limits.scheduled_changes) {
    return fail(ErrorCode::Capacity, "managed changes full");
  }
  m.changes.push_back({r, start, end, ++m.sequence, effective});
  return {};
}
Expected<void> ManagedAccessManager::replace_backing(RegionId id, std::shared_ptr<Bytes> b,
                                                     std::uint64_t version) {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  auto it = m.regions->find(id);
  if (it == m.regions->end() || !b || b->size() != it->second.backing->size() ||
      version <= it->second.version) {
    return fail(ErrorCode::InvalidArgument, "backing version/size");
  }
  auto changed = m.change({id, it->second.start, it->second.end, 0, m.now});
  if (!changed) {
    return changed;
  }
  it->second.backing_cell.reset();
  it->second.backing = std::move(b);
  it->second.version = version;
  return {};
}
Expected<void> ManagedAccessManager::release_lease(LeaseHandle h) {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  auto l = m.lease(h);
  if (!l) {
    return l.error();
  }
  for (auto &o : m.ops) {
    if (o && o->request.lease == h && !o->admitted && !o->terminal) {
      auto t = m.terminal(*o, AccessFailure::Invalidated, CommitDisposition::NotCommitted);
      if (!t) {
        return t;
      }
    }
  }
  m.leases[h.slot].reset();
  m.collect();
  return {};
}
Expected<void> ManagedAccessManager::cancel(AccessHandle h, AccessFailure reason) {
  if (impl_->state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed version");
  }
  ++impl_->state_version;
  if (reason != AccessFailure::Cancelled && reason != AccessFailure::Reset) {
    return fail(ErrorCode::InvalidArgument, "invalid cancellation reason");
  }
  auto o = impl_->op(h);
  if (!o) {
    return o.error();
  }
  auto t = impl_->terminal(*o.value(), reason, CommitDisposition::NotCommitted);
  impl_->collect();
  return t;
}
Expected<void> ManagedAccessManager::reset() {
  auto &m = *impl_;
  if (m.state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed state version");
  }
  ++m.state_version;
  for (auto &l : m.leases) {
    if (l) {
      l->valid = false;
    }
  }
  for (auto &o : m.ops) {
    if (o && !o->terminal) {
      auto t = m.terminal(*o, AccessFailure::Reset, CommitDisposition::NotCommitted);
      if (!t) {
        return t;
      }
    }
  }
  m.collect();
  return {};
}
Expected<ManagedCompletion> ManagedAccessManager::completion(AccessHandle h) const {
  auto o = impl_->op(h);
  if (!o) {
    return o.error();
  }
  if (!o.value()->terminal) {
    return fail(ErrorCode::NotReady, "managed result not ready");
  }
  return o.value()->completion;
}
Expected<ResultHandle> ManagedAccessManager::result_handle(AccessHandle h) const {
  auto o = impl_->op(h);
  if (!o) {
    return o.error();
  }
  return o.value()->result.consumer;
}
Expected<PinnedResultView> ManagedAccessManager::pin_result(AccessHandle h, ConsumerToken c) {
  auto o = impl_->op(h);
  if (!o) {
    return o.error();
  }
  return impl_->results.pin(ResultHandle{o.value()->result.consumer.result, c});
}
Expected<void> ManagedAccessManager::release_result(AccessHandle h, ConsumerToken c) {
  if (impl_->state_version == UINT64_MAX) {
    return fail(ErrorCode::Overflow, "managed version");
  }
  ++impl_->state_version;
  auto o = impl_->op(h);
  if (!o) {
    return o.error();
  }
  auto rh = ResultHandle{o.value()->result.consumer.result, c};
  auto result = impl_->preparing_txn ? impl_->results.prepare_release(*impl_->preparing_txn, rh)
                                     : impl_->results.release(rh);
  if (!result) {
    return result;
  }
  if (c == o.value()->result.consumer.consumer) {
    o.value()->released = true;
  }
  impl_->collect();
  return {};
}
ManagedStateSnapshot ManagedAccessManager::snapshot() const {
  ManagedStateSnapshot out;
  out.lease_generations = *impl_->lease_generations;
  out.access_generations = *impl_->op_generations;
  out.limits = impl_->limits;
  out.sequence = impl_->sequence;
  out.now = impl_->now.value;
  out.free_at = impl_->resource->available().value;
  out.pending_input_bytes = impl_->input_bytes;
  using A = Value::Array;
  for (const auto &entry : *impl_->regions) {
    const auto &r = entry.second;
    out.regions.emplace_back(A{Value(std::uint64_t(r.id.value)), Value(r.start), Value(r.end),
                               Value(std::uint64_t(r.permission)), Value(r.version),
                               Value(std::uint64_t(r.invalidation)), Value(*r.backing)});
  }
  for (const auto &entry : impl_->leases)
    if (entry) {
      const auto &l = *entry;
      out.leases.emplace_back(A{Value(l.handle), Value(std::uint64_t(l.request.region.value)),
                                Value(l.request.start), Value(l.request.end),
                                Value(std::uint64_t(l.request.permission)), Value(l.version),
                                Value(l.valid)});
    }
  for (const auto &entry : impl_->ops)
    if (entry) {
      const auto &o = *entry;
      out.operations.emplace_back(A{Value(o.handle),
                                    Value(o.request.lease),
                                    Value(std::uint64_t(o.region.value)),
                                    Value(o.request.address),
                                    Value(std::uint64_t(o.request.count)),
                                    Value(o.request.input),
                                    Value(o.result.consumer.result),
                                    Value(o.result.consumer.consumer),
                                    Value(o.admitted),
                                    Value(o.terminal),
                                    Value(o.released),
                                    Value(o.finish.value),
                                    Value(std::uint64_t(o.completion.failure)),
                                    Value(std::uint64_t(o.completion.disposition)),
                                    Value(o.completion.ready.time.value),
                                    Value(o.completion.ready.turn),
                                    Value(bool(o.pin)),
                                    Value(std::uint64_t(o.request.command)),
                                    Value(o.request.arrival.value),
                                    Value(o.sequence)});
    }
  for (const auto &entry : impl_->ops)
    if (entry && (!entry->terminal || entry->pin)) {
      const auto &o = *entry;
      std::string kind = o.admitted ? "managed.finish" : "managed.admit";
      out.events.emplace_back(A{Value(Bytes(kind.begin(), kind.end())),
                                Value(o.admitted ? o.finish.value : o.request.arrival.value),
                                Value(o.sequence), Value(o.handle)});
    }
  for (const auto &c : impl_->changes) {
    std::string kind = "managed.invalidate";
    out.events.emplace_back(A{Value(Bytes(kind.begin(), kind.end())), Value(c.time.value),
                              Value(c.sequence), Value(std::uint64_t(c.region.value)),
                              Value(c.start), Value(c.end)});
  }
  return out;
}
std::size_t ManagedAccessManager::backing_pins() const {
  std::size_t n = 0;
  for (auto &o : impl_->ops) {
    if (o && o->pin) {
      ++n;
    }
  }
  return n;
}
std::optional<ReadyKey> ManagedAccessManager::next_ready() const {
  std::optional<ReadyKey> next;
  for (const auto &o : impl_->ops) {
    if (!o || (o->terminal && !o->pin)) {
      continue;
    }
    ReadyKey k{o->admitted ? o->finish : o->request.arrival, o->sequence};
    if (!next || k < *next) {
      next = k;
    }
  }
  for (const auto &c : impl_->changes) {
    ReadyKey k{c.time, c.sequence};
    if (!next || k < *next) {
      next = k;
    }
  }
  return next;
}
bool ManagedAccessManager::uses_results(const ResultStore &results) const {
  return &impl_->results == &results;
}
Tick ManagedAccessManager::now() const {
  return impl_->now;
}
} // namespace leanat

namespace leanat {
namespace {
template <class F> auto managed_prepare(EventTxn &txn, F operation) -> decltype(operation()) {
  auto checkpoint = txn.checkpoint();
  if (!checkpoint)
    return checkpoint.error();
  try {
    auto result = operation();
    if (!result)
      txn.rollback(std::move(checkpoint.value()));
    return result;
  } catch (...) {
    txn.rollback(std::move(checkpoint.value()));
    throw;
  }
}
class ManagedAllocation final : public PreparedParticipant {
  std::size_t bytes_;

public:
  explicit ManagedAllocation(std::size_t bytes) : bytes_(bytes) {}
  std::size_t reserved_bytes() const noexcept override {
    return bytes_ + sizeof(*this);
  }
  Expected<void> validate() const override {
    return {};
  }
  void apply() noexcept override {}
  void discard() noexcept override {}
};
} // namespace
struct ManagedAccessManager::Staged final : PreparedParticipant {
  ManagedAccessManager &original;
  ManagedAccessManager view;
  std::uint64_t version;
  std::size_t bytes;
  bool done{};
  Staged(ManagedAccessManager &manager, const Impl &source, std::size_t footprint, EventTxn &txn)
      : original(manager), view(std::make_unique<Impl>(source)),
        version(manager.impl_->state_version), bytes(footprint) {
    view.impl_->preparing_txn = &txn;
    view.impl_->now = txn.context().ready.time;
    view.impl_->current_turn = txn.context().ready.turn;
  }
  const void *identity() const noexcept override {
    return &original;
  }
  std::size_t reserved_bytes() const noexcept override {
    return bytes;
  }
  Expected<void> validate() const override {
    if (done || original.impl_->state_version != version || version == UINT64_MAX) {
      return fail(ErrorCode::InvalidState, "managed prepared state conflict");
    }
    return {};
  }
  void apply() noexcept override {
    view.impl_->preparing_txn = nullptr;
    view.impl_->state_version = version + 1;
    original.impl_.swap(view.impl_);
    done = true;
  }
  void discard() noexcept override {
    done = true;
  }
};
ManagedAccessManager::ManagedAccessManager(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Expected<ManagedAccessManager::Staged *> ManagedAccessManager::stage(EventTxn &txn) {
  if (txn.context().domain != impl_->domain) {
    return fail(ErrorCode::WrongDomain, "managed service context");
  }
  if (txn.context().ready.time < impl_->now) {
    return fail(ErrorCode::TimeRegression, "managed service before frontier");
  }
  // Never mutate a participant that predates a caller savepoint.
  const Impl *source = impl_.get();
  if (auto existing = txn.participant(this)) {
    auto *staged = static_cast<Staged *>(existing);
    auto valid = staged->validate();
    if (!valid) {
      return valid.error();
    }
    source = staged->view.impl_.get();
  }
  auto next = next_ready();
  if (next && next->time < txn.context().ready.time) {
    return fail(ErrorCode::NotReady, "managed events precede service segment");
  }
  std::size_t bytes =
      sizeof(Staged) + sizeof(Impl) +
      source->leases.size() * (sizeof(std::optional<Impl::Lease>) + sizeof(std::uint64_t)) +
      source->ops.size() * (sizeof(std::optional<Impl::Op>) + sizeof(std::uint64_t)) +
      source->changes.size() * sizeof(Impl::Change);
  for (const auto &o : source->ops) {
    if (!o) {
      continue;
    }
    auto extra = checked_add(o->request.input.size(), owned_value_bytes(o->prepared));
    if (!extra) {
      return extra.error();
    }
    auto total = checked_add(bytes, extra.value());
    if (!total) {
      return total.error();
    }
    bytes = total.value();
  }
  if (bytes > txn.remaining_bytes()) {
    return fail(ErrorCode::Capacity, "managed snapshot segment bytes");
  }
  auto prepared = std::make_unique<Staged>(*this, *source, bytes, txn);
  auto *ptr = prepared.get();
  auto staged = txn.stage_participant(std::move(prepared));
  if (!staged) {
    return staged.error();
  }
  return ptr;
}
Expected<LeaseHandle> ManagedAccessManager::prepare_request(EventTxn &txn,
                                                            const ManagedRequest &request) {
  return managed_prepare(txn, [&]() -> Expected<LeaseHandle> {
    if (request.owner != txn.context().owner || request.connection != txn.context().connection) {
      return fail(ErrorCode::WrongOwner, "managed request context");
    }
    auto update = stage(txn);
    if (!update) {
      return update.error();
    }
    return update.value()->view.request(request);
  });
}
Expected<AccessHandle> ManagedAccessManager::prepare_begin(EventTxn &txn,
                                                           OwnedAccessRequest request) {
  return managed_prepare(txn, [&]() -> Expected<AccessHandle> {
    if (request.lease.owner != txn.context().owner) {
      return fail(ErrorCode::WrongOwner, "managed begin context");
    }
    auto update = stage(txn);
    if (!update) {
      return update.error();
    }
    auto size =
        checked_add(request.input.size(), request.command == Command::Read ? request.count : 0);
    if (!size || size.value() > SIZE_MAX - 2048) {
      return fail(ErrorCode::Overflow, "managed operation staged bytes");
    }
    auto reserved = txn.stage_participant(std::make_unique<ManagedAllocation>(size.value() + 1024));
    if (!reserved) {
      return reserved.error();
    }
    return update.value()->view.begin(std::move(request));
  });
}
Expected<void> ManagedAccessManager::prepare_release_lease(EventTxn &txn, LeaseHandle lease) {
  return managed_prepare(txn, [&]() -> Expected<void> {
    if (lease.owner != txn.context().owner) {
      return fail(ErrorCode::WrongOwner, "managed release context");
    }
    auto update = stage(txn);
    if (!update) {
      return update.error();
    }
    return update.value()->view.release_lease(lease);
  });
}
Expected<void> ManagedAccessManager::prepare_invalidate(EventTxn &txn, RegionId region,
                                                        std::uint64_t start, std::uint64_t end,
                                                        Tick effective) {
  return managed_prepare(txn, [&]() -> Expected<void> {
    auto update = stage(txn);
    if (!update) {
      return update.error();
    }
    auto reserved =
        txn.stage_participant(std::make_unique<ManagedAllocation>(sizeof(Impl::Change)));
    if (!reserved) {
      return reserved.error();
    }
    return update.value()->view.invalidate(region, start, end, effective);
  });
}
Expected<ResultHandle> ManagedAccessManager::prepare_result_handle(EventTxn &txn,
                                                                   AccessHandle access) {
  return managed_prepare(txn, [&]() -> Expected<ResultHandle> {
    if (access.owner != txn.context().owner) {
      return fail(ErrorCode::WrongOwner, "managed result context");
    }
    auto update = stage(txn);
    if (!update) {
      return update.error();
    }
    return update.value()->view.result_handle(access);
  });
}
Expected<void> ManagedAccessManager::prepare_release_result(EventTxn &txn, AccessHandle access) {
  return managed_prepare(txn, [&]() -> Expected<void> {
    auto result = prepare_result_handle(txn, access);
    if (!result) {
      return result.error();
    }
    auto update = stage(txn);
    if (!update) {
      return update.error();
    }
    return update.value()->view.release_result(access, result.value().consumer);
  });
}
} // namespace leanat
