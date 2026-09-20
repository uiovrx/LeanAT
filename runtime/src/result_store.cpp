#include "leanat/result_store.hpp"
#include "leanat/storage_identity.hpp"

namespace leanat {
std::size_t owned_value_bytes(const Value &v) {
  return bounded_value_bytes(v);
}
struct ResultStore::Impl {
  struct Slot {
    bool used{}, producer{}, publishing{};
    std::uint64_t generation{};
    ResultCreate create;
    std::shared_ptr<const Value> value;
    ReadyKey ready{};
    std::size_t consumers{}, pins{};
  };
  struct Consumer {
    bool active{};
    std::uint64_t generation{}, owner{};
    Handle result;
  };
  std::shared_ptr<std::uint64_t> next_consumer_generation{std::make_shared<std::uint64_t>(0)};
  std::shared_ptr<std::uint64_t> next_result_generation{std::make_shared<std::uint64_t>(0)};
  std::uint64_t version{};
  std::vector<Slot> slots;
  std::vector<Consumer> consumers;
  std::size_t pin_limit, pin_count{};
  DomainId domain;
  std::uint32_t store;
  Impl(std::size_t n, std::size_t c, std::size_t p, DomainId d, std::uint32_t s)
      : slots(n), consumers(c), pin_limit(p), domain(d),
        store(storage_detail::allocate_store_incarnation()) {
    (void)s;
  }
  Expected<std::size_t> result(Handle h) const {
    if (h.domain != domain)
      return fail(ErrorCode::WrongDomain, "result domain");
    if (h.kind != HandleKind::Result || h.store != store || h.slot >= slots.size())
      return fail(ErrorCode::StaleHandle, "result identity");
    auto &s = slots[h.slot];
    if (!s.used || s.generation != h.generation)
      return fail(ErrorCode::StaleHandle, "result generation");
    if (h.owner != s.create.producer_owner)
      return fail(ErrorCode::WrongOwner, "result producer");
    return h.slot;
  }
  Expected<std::size_t> consumer(ResultHandle h) const {
    auto r = result(h.result);
    if (!r)
      return r.error();
    auto c = h.consumer;
    if (c.domain != domain || c.kind != HandleKind::Consumer || c.store != store ||
        c.slot >= consumers.size())
      return fail(ErrorCode::StaleHandle, "consumer identity");
    auto &x = consumers[c.slot];
    if (x.generation != c.generation)
      return fail(ErrorCode::StaleHandle, "consumer generation");
    if (x.owner != c.owner)
      return fail(ErrorCode::WrongOwner, "consumer owner");
    if (!x.active)
      return fail(ErrorCode::AlreadyReleased, "consumer consumed");
    if (x.result != h.result)
      return fail(ErrorCode::StaleHandle, "consumer result mismatch");
    return c.slot;
  }
  void collect(std::size_t i) {
    auto &s = slots[i];
    if (s.used && !s.producer && !s.consumers && !s.pins && !s.publishing) {
      s.used = false;
      s.value.reset();
    }
  }
  Expected<ResultHandle> add(Handle r, std::uint64_t owner) {
    for (std::size_t i = 0; i < consumers.size(); ++i) {
      auto &c = consumers[i];
      if (!c.active && *next_consumer_generation != UINT64_MAX) {
        c.active = true;
        c.generation = ++*next_consumer_generation;
        c.owner = owner;
        c.result = r;
        ++slots[r.slot].consumers;
        ++version;
        return ResultHandle{r, Handle{HandleKind::Consumer, domain, store,
                                      static_cast<std::uint32_t>(i), c.generation, owner}};
      }
    }
    return fail(ErrorCode::Capacity, "consumer capacity");
  }
};
ResultStore::ResultStore(std::size_t n, std::size_t c, std::size_t p, DomainId d, std::uint32_t s)
    : impl_(std::make_shared<Impl>(n, c, p, d, s)) {
  if (!n || !c || !p || n > UINT32_MAX || c > UINT32_MAX)
    throw std::invalid_argument("result capacity");
}
Expected<ReservedResult> ResultStore::reserve(ResultCreate c) {
  for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
    auto &s = impl_->slots[i];
    if (s.used || *impl_->next_result_generation == UINT64_MAX)
      continue;
    s.used = s.producer = true;
    s.generation = ++*impl_->next_result_generation;
    s.create = std::move(c);
    s.consumers = s.pins = 0;
    s.value.reset();
    Handle r{HandleKind::Result, impl_->domain,
             impl_->store,       static_cast<std::uint32_t>(i),
             s.generation,       s.create.producer_owner};
    auto h = impl_->add(r, s.create.consumer_owner);
    if (!h) {
      s.used = s.producer = false;
      return h.error();
    }
    return ReservedResult{{r}, h.value()};
  }
  return fail(ErrorCode::Capacity, "result capacity");
}
Expected<PublicationReceipt> ResultStore::publish(ResultOwnerHandle h, Value value,
                                                  PublicationContext c) {
  auto r = impl_->result(h.result);
  if (!r)
    return r.error();
  auto &s = impl_->slots[r.value()];
  if (!s.producer)
    return fail(ErrorCode::InvalidState, "producer released");
  if (s.value || s.publishing)
    return fail(ErrorCode::Duplicate, "result published");
  if (!c.validated || c.source != s.create.source)
    return fail(ErrorCode::InvalidArgument, "publication context");
  if (owned_value_bytes(value) == SIZE_MAX || owned_value_bytes(value) > s.create.max_bytes)
    return fail(ErrorCode::Capacity, "result bytes");
  auto owned = std::make_shared<const Value>(std::move(value));
  s.value = std::move(owned);
  s.ready = c.ready;
  ++impl_->version;
  return PublicationReceipt{h.result, c.ready};
}
Expected<ResultHandle> ResultStore::retain(ResultHandle h, std::uint64_t owner) {
  auto c = impl_->consumer(h);
  if (!c)
    return c.error();
  return impl_->add(h.result, owner);
}
Expected<ResultSubscription> ResultStore::subscribe(ResultOwnerHandle h, std::uint64_t owner) {
  auto r = impl_->result(h.result);
  if (!r)
    return r.error();
  auto c = impl_->add(h.result, owner);
  if (!c)
    return c.error();
  auto &s = impl_->slots[r.value()];
  return ResultSubscription{c.value(), s.value ? std::optional<ReadyKey>{s.ready} : std::nullopt};
}
Expected<Value> ResultStore::read(ResultHandle h, std::size_t capacity) const {
  auto c = impl_->consumer(h);
  if (!c)
    return c.error();
  auto &s = impl_->slots[h.result.slot];
  if (!s.value)
    return fail(ErrorCode::NotReady, "result reserved");
  if (owned_value_bytes(*s.value) > capacity)
    return fail(ErrorCode::Capacity, "destination capacity");
  return *s.value;
}
Expected<Value> ResultStore::take(ResultHandle h, std::size_t capacity) {
  auto v = read(h, capacity);
  if (!v)
    return v.error();
  auto r = release(h);
  if (!r)
    return r.error();
  return std::move(v.value());
}
Expected<void> ResultStore::release(ResultHandle h, ReleasePolicy) {
  auto c = impl_->consumer(h);
  if (!c)
    return c.error();
  ++impl_->version;
  impl_->consumers[c.value()].active = false;
  --impl_->slots[h.result.slot].consumers;
  impl_->collect(h.result.slot);
  return {};
}
Expected<void> ResultStore::release_owner(ResultOwnerHandle h) {
  auto r = impl_->result(h.result);
  if (!r)
    return r.error();
  auto &s = impl_->slots[r.value()];
  if (!s.producer)
    return fail(ErrorCode::AlreadyReleased, "producer released");
  ++impl_->version;
  s.producer = false;
  impl_->collect(r.value());
  return {};
}
Expected<PinnedResultView> ResultStore::pin(ResultHandle h) {
  auto c = impl_->consumer(h);
  if (!c)
    return c.error();
  auto &s = impl_->slots[h.result.slot];
  if (!s.value)
    return fail(ErrorCode::NotReady, "result reserved");
  if (impl_->pin_count == impl_->pin_limit)
    return fail(ErrorCode::Capacity, "pin capacity");
  auto state = impl_;
  auto i = h.result.slot;
  auto guard = std::shared_ptr<void>(new int(0), [state, i](void *p) {
    delete static_cast<int *>(p);
    ++state->version;
    --state->pin_count;
    --state->slots[i].pins;
    state->collect(i);
  });
  ++impl_->version;
  ++s.pins;
  ++impl_->pin_count;
  return PinnedResultView{s.value, std::move(guard)};
}
Expected<ReadyKey> ResultStore::published_ready(ResultHandle h) const {
  auto c = impl_->consumer(h);
  if (!c)
    return c.error();
  auto &s = impl_->slots[h.result.slot];
  if (!s.value)
    return fail(ErrorCode::NotReady, "result reserved");
  return s.ready;
}
std::size_t ResultStore::occupied() const {
  std::size_t n = 0;
  for (auto &s : impl_->slots)
    if (s.used)
      ++n;
  return n;
}
} // namespace leanat
namespace leanat {
ResultStoreSnapshot ResultStore::snapshot() const {
  ResultStoreSnapshot snapshot;
  snapshot.result_slots.reserve(impl_->slots.size());
  snapshot.consumer_slots.reserve(impl_->consumers.size());
  for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
    const auto &slot = impl_->slots[index];
    ResultSlotSnapshot entry;
    entry.identity = {HandleKind::Result, impl_->domain,
                      impl_->store,       static_cast<std::uint32_t>(index),
                      slot.generation,    slot.create.producer_owner};
    entry.alive = slot.used;
    entry.producer_alive = slot.producer;
    entry.publishing = slot.publishing;
    entry.create = slot.create;
    if (slot.value) {
      entry.value = *slot.value;
      entry.ready = slot.ready;
    }
    entry.consumer_count = slot.consumers;
    entry.pin_count = slot.pins;
    snapshot.result_slots.push_back(std::move(entry));
  }
  for (std::size_t index = 0; index < impl_->consumers.size(); ++index) {
    const auto &consumer = impl_->consumers[index];
    snapshot.consumer_slots.push_back(
        {Handle{HandleKind::Consumer, impl_->domain, impl_->store,
                static_cast<std::uint32_t>(index), consumer.generation, consumer.owner},
         consumer.result, consumer.active});
  }
  snapshot.next_result_generation = *impl_->next_result_generation;
  snapshot.next_consumer_generation = *impl_->next_consumer_generation;
  snapshot.pin_count = impl_->pin_count;
  snapshot.pin_limit = impl_->pin_limit;
  return snapshot;
}
Expected<ResultOwnership> ResultStore::inspect_ownership(ResultOwnerHandle h) const {
  auto index = impl_->result(h.result);
  if (!index)
    return index.error();
  const auto &slot = impl_->slots[index.value()];
  return ResultOwnership{slot.consumers, slot.pins, bool(slot.value), !slot.producer,
                         slot.publishing};
}
bool ResultStore::alive(ResultOwnerHandle h) const {
  return bool(impl_->result(h.result));
}
} // namespace leanat
namespace leanat {
Expected<std::unique_ptr<PreparedParticipant>>
ResultStore::prepare_publish(ResultOwnerHandle h, Value v, PublicationContext c) {
  auto r = impl_->result(h.result);
  if (!r)
    return r.error();
  auto &s = impl_->slots[r.value()];
  if (!s.producer || s.value || s.publishing)
    return fail(ErrorCode::InvalidState, "result producer state");
  if (!c.validated || c.source != s.create.source)
    return fail(ErrorCode::InvalidArgument, "publication context");
  if (owned_value_bytes(v) == SIZE_MAX || owned_value_bytes(v) > s.create.max_bytes)
    return fail(ErrorCode::Capacity, "result bytes");
  struct Publish final : PreparedParticipant {
    std::size_t reserved_bytes() const noexcept override {
      auto bytes = value ? bounded_value_bytes(*value) : 0;
      auto fixed = sizeof(*this) + 4 * sizeof(void *);
      return bytes > SIZE_MAX - fixed ? SIZE_MAX : fixed + bytes;
    }
    std::shared_ptr<Impl> state;
    Handle handle;
    PublicationContext context;
    std::shared_ptr<const Value> value;
    bool done{};
    Publish(std::shared_ptr<Impl> s, Handle h, PublicationContext c, std::shared_ptr<const Value> v)
        : state(std::move(s)), handle(h), context(c), value(std::move(v)) {}
    Expected<void> validate() const override {
      if (done)
        return fail(ErrorCode::InvalidState, "publication closed");
      auto r = state->result(handle);
      if (!r)
        return r.error();
      auto &s = state->slots[r.value()];
      if (!s.producer || s.value)
        return fail(ErrorCode::InvalidState, "publication conflict");
      return {};
    }
    void apply() noexcept override {
      auto &s = state->slots[handle.slot];
      s.value = std::move(value);
      s.ready = context.ready;
      s.publishing = false;
      ++state->version;
      done = true;
    }
    void discard() noexcept override {
      if (!done) {
        auto r = state->result(handle);
        if (r) {
          state->slots[r.value()].publishing = false;
          ++state->version;
          state->collect(r.value());
        }
      }
      value.reset();
      done = true;
    }
    ~Publish() {
      if (!done)
        discard();
    }
  };
  auto prepared = std::unique_ptr<PreparedParticipant>(
      new Publish(impl_, h.result, c, std::make_shared<const Value>(std::move(v))));
  s.publishing = true;
  ++impl_->version;
  return prepared;
}
} // namespace leanat

namespace leanat {
struct ResultStore::Staged final : PreparedParticipant {
  std::shared_ptr<Impl> original, snapshot;
  std::uint64_t version;
  bool done{};
  std::size_t extra_bytes{};
  static std::size_t required_bytes(const Impl &s) noexcept {
    std::size_t total = sizeof(Staged) + sizeof(Impl) + 4 * sizeof(void *);
    if (s.slots.size() > (SIZE_MAX - total) / sizeof(Impl::Slot))
      return SIZE_MAX;
    total += s.slots.size() * sizeof(Impl::Slot);
    if (s.consumers.size() > (SIZE_MAX - total) / sizeof(Impl::Consumer))
      return SIZE_MAX;
    return total + s.consumers.size() * sizeof(Impl::Consumer);
  }
  std::size_t reserved_bytes() const noexcept override {
    auto base = required_bytes(*snapshot);
    return extra_bytes > SIZE_MAX - base ? SIZE_MAX : base + extra_bytes;
  }
  explicit Staged(std::shared_ptr<Impl> s, const Impl &base)
      : original(s), snapshot(std::make_shared<Impl>(base)), version(s->version) {}
  const void *identity() const noexcept override {
    return original.get();
  }
  Expected<void> validate() const override {
    if (done || original->version != version)
      return fail(ErrorCode::InvalidState, "result store version conflict");
    if (original->version == UINT64_MAX)
      return fail(ErrorCode::Overflow, "result version exhausted");
    return {};
  }
  void apply() noexcept override {
    original->slots.swap(snapshot->slots);
    original->consumers.swap(snapshot->consumers);
    ++original->version;
    done = true;
  }
  void discard() noexcept override {
    done = true;
  }
};
Expected<ResultStore::Staged *> ResultStore::stage(EventTxn &t, std::size_t extra) {
  for (const auto &slot : impl_->slots)
    if (slot.publishing)
      return fail(ErrorCode::InvalidState, "publication and registry transaction conflict");
  auto previous = static_cast<Staged *>(t.participant(impl_.get()));
  auto &base = previous ? *previous->snapshot : *impl_;
  auto bytes = Staged::required_bytes(base);
  if (bytes == SIZE_MAX || extra > SIZE_MAX - bytes || bytes + extra > t.remaining_bytes())
    return fail(ErrorCode::Capacity, "result registry snapshot bytes");
  auto p = std::make_unique<Staged>(impl_, base);
  p->extra_bytes = extra;
  auto raw = p.get();
  auto r = t.stage_participant(std::move(p));
  if (!r)
    return r.error();
  return raw;
}
Expected<ResultHandle> ResultStore::prepare_retain(EventTxn &t, ResultHandle h,
                                                   std::uint64_t owner) {
  auto checkpoint = t.checkpoint();
  if (!checkpoint)
    return checkpoint.error();
  auto p = stage(t);
  if (!p)
    return p.error();
  auto &state = *p.value()->snapshot;
  auto c = state.consumer(h);
  if (!c) {
    t.rollback(std::move(checkpoint.value()));
    return c.error();
  }
  auto added = state.add(h.result, owner);
  if (!added)
    t.rollback(std::move(checkpoint.value()));
  return added;
}
Expected<void> ResultStore::prepare_release(EventTxn &t, ResultHandle h) {
  auto checkpoint = t.checkpoint();
  if (!checkpoint)
    return checkpoint.error();
  auto p = stage(t);
  if (!p)
    return p.error();
  auto &state = *p.value()->snapshot;
  auto c = state.consumer(h);
  if (!c) {
    t.rollback(std::move(checkpoint.value()));
    return c.error();
  }
  state.consumers[c.value()].active = false;
  --state.slots[h.result.slot].consumers;
  state.collect(h.result.slot);
  return {};
}
Expected<ResultHandle> ResultStore::prepare_transfer(EventTxn &t, ResultHandle h,
                                                     std::uint64_t owner) {
  auto checkpoint = t.checkpoint();
  if (!checkpoint)
    return checkpoint.error();
  auto p = stage(t);
  if (!p)
    return p.error();
  auto &state = *p.value()->snapshot;
  auto c = state.consumer(h);
  if (!c) {
    t.rollback(std::move(checkpoint.value()));
    return c.error();
  }
  auto n = state.add(h.result, owner);
  if (!n) {
    t.rollback(std::move(checkpoint.value()));
    return n.error();
  }
  state.consumers[c.value()].active = false;
  --state.slots[h.result.slot].consumers;
  return n.value();
}
} // namespace leanat
namespace leanat {
Expected<PayloadShadow::View *> PayloadShadow::resolve(const PayloadViewKey &k, const EventTxn &t) {
  if (!t.is_open())
    return fail(ErrorCode::InvalidState, "closed payload segment");
  auto it = views_.find(k);
  if (it == views_.end())
    return fail(ErrorCode::StaleHandle, "payload view");
  auto &v = it->second;
  auto &c = t.context();
  if (c.domain != v.context.domain || c.instance != k.instance ||
      c.connection != v.context.connection || c.owner != v.context.owner)
    return fail(ErrorCode::WrongOwner, "payload local view");
  if (c.epoch != v.context.epoch)
    return fail(ErrorCode::InvalidState, "payload epoch");
  return &v;
}
Expected<void> PayloadShadow::create(PayloadViewKey k, PayloadSnapshot p, ExecutionContext c,
                                     PayloadRole role, bool writable) {
  std::size_t bytes = p.data.size();
  auto charge = [&](std::size_t count) {
    if (bytes > max_bytes_ || count > max_bytes_ - bytes)
      return false;
    bytes += count;
    return true;
  };
  if (!charge(p.byte_enable.size()))
    return fail(ErrorCode::Capacity, "payload bytes");
  for (const auto &extension : p.extensions)
    if (!charge(extension.first.size()) || !charge(extension.second.size()))
      return fail(ErrorCode::Capacity, "payload extension bytes");
  if (k.txn.kind != HandleKind::Transaction || k.hop.kind != HandleKind::Hop ||
      k.txn.domain != c.domain || k.hop.domain != c.domain || k.instance != c.instance)
    return fail(ErrorCode::InvalidArgument, "payload identity");
  if (views_.count(k))
    return fail(ErrorCode::Duplicate, "payload view");
  if (views_.size() >= capacity_ || p.data.size() > max_bytes_ ||
      p.byte_enable.size() > max_bytes_ || !p.streaming_width)
    return fail(ErrorCode::Capacity, "payload bounds");
  auto data = Value(p.data);
  auto status = Value(static_cast<std::uint64_t>(p.status));
  auto extensions = check_extensions(p.extensions, false);
  if (!extensions)
    return extensions.error();
  View view{p,
            c,
            role,
            VersionedCell{std::move(data), 0, c.epoch},
            VersionedCell{std::move(status), 0, c.epoch},
            VersionedCell{Value(p.dmi_hint), 0, c.epoch},
            VersionedCell{Value(false), 0, c.epoch},
            {},
            writable};
  for (const auto &rule : extension_rules_) {
    auto it = p.extensions.find(rule.first);
    view.extensions.emplace(
        rule.first,
        VersionedCell{it == p.extensions.end() ? Value{} : Value(it->second), 0, c.epoch});
  }
  views_.emplace(std::move(k), std::move(view));
  return {};
}
Expected<Value> PayloadShadow::read_data(const PayloadViewKey &k, const EventTxn &t) {
  auto v = resolve(k, t);
  if (!v)
    return v.error();
  return t.read(v.value()->data);
}
Expected<void> PayloadShadow::buffer_data(const PayloadViewKey &k, Value value, EventTxn &t) {
  auto r = resolve(k, t);
  if (!r)
    return r.error();
  auto &v = *r.value();
  auto b = std::get_if<Bytes>(&value.data);
  if (!v.writable || v.role != PayloadRole::Target || v.baseline.command != Command::Read)
    return fail(ErrorCode::InvalidState, "illegal payload data write");
  if (!b || b->size() != v.baseline.data.size())
    return fail(ErrorCode::TypeMismatch, "payload data shape");
  for (std::size_t i = 0; i < b->size(); ++i)
    if (!v.baseline.byte_enable.empty() &&
        !v.baseline.byte_enable[i % v.baseline.byte_enable.size()])
      (*b)[i] = v.baseline.data[i];
  return t.buffer(v.data, std::move(value));
}
Expected<void> PayloadShadow::deliver_response(const PayloadViewKey &k, ResponseSnapshot response,
                                               EventTxn &t) {
  PayloadAccessContext context;
  context.hop = k.hop;
  context.local_side = k.local_side;
  context.flow = Flow::Backward;
  context.call_phase = begin_resp;
  context.validated = true;
  return deliver_response(k, std::move(response), context, t);
}
Expected<void> PayloadShadow::erase(const PayloadViewKey &k) {
  auto it = views_.find(k);
  if (it == views_.end())
    return fail(ErrorCode::StaleHandle, "payload view");
  views_.erase(it);
  return {};
}
} // namespace leanat
namespace leanat {
Expected<void> MilestoneStore::mark(Handle hop, StorageMilestoneKind kind, MilestoneValue value,
                                    bool validated) {
  auto bytes = bounded_value_bytes(value.value);
  if (bytes == SIZE_MAX || bytes > max_bytes_)
    return fail(ErrorCode::Capacity, "milestone value bytes");
  if (!validated || hop.kind != HandleKind::Hop || !hop.generation)
    return fail(ErrorCode::InvalidArgument, "milestone exchange");
  auto key = std::make_pair(hop, kind);
  if (values_.count(key))
    return fail(ErrorCode::Duplicate, "milestone already latched");
  if (values_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "milestone capacity");
  values_.emplace(key, std::move(value));
  return {};
}
Expected<MilestoneValue> MilestoneStore::read(Handle hop, StorageMilestoneKind kind) const {
  auto it = values_.find(std::make_pair(hop, kind));
  if (it == values_.end())
    return fail(ErrorCode::NotReady, "milestone not latched");
  return it->second;
}
Expected<void> MilestoneStore::release(Handle hop) {
  bool found = false;
  for (auto it = values_.begin(); it != values_.end();)
    if (it->first.first == hop) {
      it = values_.erase(it);
      found = true;
    } else
      ++it;
  if (!found)
    return fail(ErrorCode::StaleHandle, "milestone hop");
  return {};
}
} // namespace leanat

namespace leanat {
Expected<PayloadSnapshot> PayloadShadow::snapshot(const PayloadViewKey &k, const EventTxn &t) {
  auto r = resolve(k, t);
  if (!r)
    return r.error();
  auto data = t.read(r.value()->data);
  if (!data)
    return data.error();
  auto status = t.read(r.value()->status);
  if (!status)
    return status.error();
  auto result = r.value()->baseline;
  result.data = std::get<Bytes>(data.value().data);
  result.status = static_cast<ResponseStatus>(std::get<std::uint64_t>(status.value().data));
  auto dmi = t.read(r.value()->dmi);
  if (!dmi)
    return dmi.error();
  result.dmi_hint = std::get<bool>(dmi.value().data);
  result.extensions.clear();
  for (auto &extension : r.value()->extensions) {
    auto value = t.read(extension.second);
    if (!value)
      return value.error();
    if (auto bytes = std::get_if<Bytes>(&value.value().data))
      result.extensions.emplace(extension.first, *bytes);
  }
  return result;
}
Expected<void> PayloadShadow::validate_target_access(const PayloadViewKey &k, const EventTxn &t,
                                                     bool writing) {
  auto r = resolve(k, t);
  if (!r)
    return r.error();
  if (r.value()->role != PayloadRole::Target)
    return fail(ErrorCode::WrongOwner, "target view required");
  if (writing && (!r.value()->writable || r.value()->baseline.command != Command::Read))
    return fail(ErrorCode::InvalidState, "response write forbidden");
  return {};
}
} // namespace leanat
namespace leanat {
Expected<ReservedResult> ResultStore::prepare_reserve(EventTxn &t, ResultCreate create) {
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto staged = stage(t);
  if (!staged)
    return staged.error();
  ResultStore view(staged.value()->snapshot);
  auto result = view.reserve(std::move(create));
  if (!result)
    t.rollback(std::move(cp.value()));
  return result;
}
Expected<PublicationReceipt> ResultStore::prepare_publish_in(EventTxn &t, ResultOwnerHandle owner,
                                                             Value value,
                                                             PublicationContext context) {
  if (context.ready != t.context().ready)
    return fail(ErrorCode::InvalidArgument, "publication differs from segment ReadyKey");
  auto bytes = bounded_value_bytes(value);
  if (bytes == SIZE_MAX || bytes > SIZE_MAX - 4 * sizeof(void *))
    return fail(ErrorCode::Capacity, "publication value bounds");
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto staged = stage(t, bytes + 4 * sizeof(void *));
  if (!staged)
    return staged.error();
  ResultStore view(staged.value()->snapshot);
  auto result = view.publish(owner, std::move(value), context);
  if (!result)
    t.rollback(std::move(cp.value()));
  return result;
}
Expected<void> ResultStore::prepare_release_owner(EventTxn &t, ResultOwnerHandle owner) {
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto staged = stage(t);
  if (!staged)
    return staged.error();
  ResultStore view(staged.value()->snapshot);
  auto result = view.release_owner(owner);
  if (!result)
    t.rollback(std::move(cp.value()));
  return result;
}
Expected<Value> ResultStore::prepare_read(const EventTxn &t, ResultHandle h,
                                          std::size_t bytes) const {
  if (!t.is_open())
    return fail(ErrorCode::InvalidState, "closed result segment");
  auto p = static_cast<Staged *>(t.participant(impl_.get()));
  ResultStore view(p ? p->snapshot : impl_);
  return view.read(h, bytes);
}
Expected<ReadyKey> ResultStore::prepare_ready(const EventTxn &t, ResultHandle h) const {
  if (!t.is_open())
    return fail(ErrorCode::InvalidState, "closed result segment");
  auto p = static_cast<Staged *>(t.participant(impl_.get()));
  ResultStore view(p ? p->snapshot : impl_);
  return view.published_ready(h);
}
} // namespace leanat
namespace leanat {
Expected<bool> ResultStore::prepare_alive(const EventTxn &t, ResultOwnerHandle h) const {
  if (!t.is_open())
    return fail(ErrorCode::InvalidState, "closed result segment");
  auto p = static_cast<Staged *>(t.participant(impl_.get()));
  ResultStore view(p ? p->snapshot : impl_);
  return view.alive(h);
}
namespace {
bool snapshot_fits(const PayloadSnapshot &snapshot, std::size_t max) {
  std::size_t bytes = 0;
  auto add = [&](std::size_t n) {
    if (n > max - bytes)
      return false;
    bytes += n;
    return true;
  };
  if (!add(snapshot.data.size()) || !add(snapshot.byte_enable.size()))
    return false;
  for (const auto &extension : snapshot.extensions)
    if (!add(extension.first.size()) || !add(extension.second.size()))
      return false;
  return true;
}
Expected<bool> supplies_response(const PayloadAccessContext &c) {
  if (!c.validated)
    return fail(ErrorCode::InvalidArgument, "unvalidated payload access");
  if (c.call_phase != begin_req && c.call_phase != end_req && c.call_phase != begin_resp &&
      c.call_phase != end_resp)
    return fail(ErrorCode::Unsupported, "unknown payload phase");
  bool forward = c.call_phase == begin_req || c.call_phase == end_resp;
  if ((c.flow == Flow::Forward) != forward)
    return fail(ErrorCode::ProtocolViolation, "payload flow/phase");
  if (c.stage == PayloadAccessStage::Call)
    return c.call_phase == begin_resp;
  if (c.sync == Sync::Accepted)
    return false;
  if (c.call_phase != begin_req)
    return false;
  if (c.sync == Sync::Completed)
    return true;
  if (c.sync == Sync::Updated) {
    if (!c.returned_phase)
      return fail(ErrorCode::InvalidArgument, "missing updated phase");
    if (*c.returned_phase != end_req && *c.returned_phase != begin_resp)
      return fail(ErrorCode::ProtocolViolation, "invalid request update");
    return *c.returned_phase == begin_resp;
  }
  return false;
}
} // namespace
Expected<PayloadShadow::View *> PayloadShadow::resolve(const PayloadViewKey &k,
                                                       const PayloadAccessContext &access,
                                                       const EventTxn &t) {
  if (access.hop != k.hop || access.local_side != k.local_side)
    return fail(ErrorCode::WrongOwner, "payload access local side");
  auto valid = supplies_response(access);
  if (!valid)
    return valid.error();
  return resolve(k, t);
}
Expected<void> PayloadShadow::register_extension(std::string key, PayloadExtensionRule rule) {
  if (!views_.empty())
    return fail(ErrorCode::InvalidState, "extension schema already in use");
  if (key.empty() || key.size() > max_bytes_ || rule.max_bytes > max_bytes_)
    return fail(ErrorCode::Capacity, "extension schema bounds");
  if (extension_rules_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "extension schema capacity");
  if (extension_rules_.count(key))
    return fail(ErrorCode::Duplicate, "extension schema");
  if ((rule.response_writable && !rule.response_allowed) ||
      (rule.request_required && !rule.request_allowed) ||
      (rule.response_required && !rule.response_allowed))
    return fail(ErrorCode::InvalidArgument, "extension phase rule");
  extension_rules_.emplace(std::move(key), rule);
  return {};
}
Expected<void> PayloadShadow::check_extensions(const std::map<std::string, Bytes> &extensions,
                                               bool response) const {
  std::size_t bytes = 0;
  for (const auto &extension : extensions) {
    auto rule = extension_rules_.find(extension.first);
    if (rule == extension_rules_.end())
      return fail(ErrorCode::Unsupported, "unregistered semantic extension");
    if (!(response ? rule->second.response_allowed : rule->second.request_allowed))
      return fail(ErrorCode::InvalidState, "extension invalid in phase");
    if (extension.second.size() > rule->second.max_bytes ||
        extension.first.size() > max_bytes_ - bytes)
      return fail(ErrorCode::Capacity, "extension byte bounds");
    bytes += extension.first.size();
    if (extension.second.size() > max_bytes_ - bytes)
      return fail(ErrorCode::Capacity, "extension byte bounds");
    bytes += extension.second.size();
  }
  for (const auto &rule : extension_rules_)
    if ((response ? rule.second.response_required : rule.second.request_required) &&
        !extensions.count(rule.first))
      return fail(ErrorCode::NotReady, "required phase extension absent");
  return {};
}
Expected<Value> PayloadShadow::read_field(const PayloadViewKey &k, PayloadField field,
                                          const EventTxn &t) {
  auto r = resolve(k, t);
  if (!r)
    return r.error();
  auto &v = *r.value();
  switch (field) {
  case PayloadField::Command:
    return Value(static_cast<std::uint64_t>(v.baseline.command));
  case PayloadField::Address:
    return Value(v.baseline.address);
  case PayloadField::Data:
    return t.read(v.data);
  case PayloadField::StreamingWidth:
    return Value(v.baseline.streaming_width);
  case PayloadField::ByteEnable:
    return Value(v.baseline.byte_enable);
  case PayloadField::Status:
    return t.read(v.status);
  case PayloadField::DmiHint:
    return t.read(v.dmi);
  }
  return fail(ErrorCode::InvalidArgument, "payload field");
}
Expected<void> PayloadShadow::buffer_field(const PayloadViewKey &k, PayloadField field, Value value,
                                           const PayloadAccessContext &access, EventTxn &t) {
  auto r = resolve(k, access, t);
  if (!r)
    return r.error();
  auto &v = *r.value();
  if (v.role != PayloadRole::Target || !v.writable || !access.response_write_permit ||
      (access.call_phase != begin_req && access.call_phase != begin_resp))
    return fail(ErrorCode::InvalidState, "payload response write permission");
  if (field == PayloadField::Data)
    return buffer_data(k, std::move(value), t);
  if (field == PayloadField::Status) {
    auto status = std::get_if<std::uint64_t>(&value.data);
    if (!status || *status > static_cast<std::uint64_t>(ResponseStatus::ByteEnableError))
      return fail(ErrorCode::TypeMismatch, "response status");
    return t.buffer(v.status, std::move(value));
  }
  if (field == PayloadField::DmiHint) {
    if (!std::holds_alternative<bool>(value.data))
      return fail(ErrorCode::TypeMismatch, "dmi hint");
    return t.buffer(v.dmi, std::move(value));
  }
  return fail(ErrorCode::InvalidState, "frozen request field");
}
Expected<std::optional<Bytes>>
PayloadShadow::read_extension(const PayloadViewKey &k, const std::string &name, const EventTxn &t) {
  auto r = resolve(k, t);
  if (!r)
    return r.error();
  auto extension = r.value()->extensions.find(name);
  if (extension == r.value()->extensions.end())
    return fail(ErrorCode::Unsupported, "extension not registered");
  auto value = t.read(extension->second);
  if (!value)
    return value.error();
  if (auto bytes = std::get_if<Bytes>(&value.value().data))
    return std::optional<Bytes>{*bytes};
  return std::optional<Bytes>{};
}
Expected<void> PayloadShadow::buffer_extension(const PayloadViewKey &k, const std::string &name,
                                               Bytes bytes, const PayloadAccessContext &access,
                                               EventTxn &t) {
  auto r = resolve(k, access, t);
  if (!r)
    return r.error();
  auto rule = extension_rules_.find(name);
  if (rule == extension_rules_.end())
    return fail(ErrorCode::Unsupported, "extension not registered");
  if (r.value()->role != PayloadRole::Target || !r.value()->writable ||
      !access.response_write_permit || !rule->second.response_writable ||
      (access.call_phase != begin_req && access.call_phase != begin_resp))
    return fail(ErrorCode::InvalidState, "extension response write permission");
  if (bytes.size() > rule->second.max_bytes)
    return fail(ErrorCode::Capacity, "extension value bounds");
  auto projected = snapshot(k, t);
  if (!projected)
    return projected.error();
  projected.value().extensions[name] = bytes;
  if (!snapshot_fits(projected.value(), max_bytes_))
    return fail(ErrorCode::Capacity, "payload aggregate bytes");
  return t.buffer(r.value()->extensions.at(name), Value(std::move(bytes)));
}
Expected<ResponseSnapshot> PayloadShadow::project_response(const PayloadViewKey &k,
                                                           const PayloadAccessContext &access,
                                                           const EventTxn &t) {
  auto r = resolve(k, access, t);
  if (!r)
    return r.error();
  auto supplies = supplies_response(access);
  if (!supplies)
    return supplies.error();
  if (!supplies.value())
    return fail(ErrorCode::NotReady, "exchange supplies no new response");
  auto current = snapshot(k, t);
  if (!current)
    return current.error();
  ResponseSnapshot result;
  result.status = current.value().status;
  result.dmi_hint = current.value().dmi_hint;
  if (current.value().command == Command::Read)
    result.data = current.value().data;
  for (const auto &extension : current.value().extensions)
    if (extension_rules_.at(extension.first).response_allowed)
      result.extensions.emplace(extension);
  auto valid = check_extensions(result.extensions, true);
  if (!valid)
    return valid.error();
  return result;
}
Expected<PayloadSnapshot> PayloadShadow::project_call(const PayloadViewKey &k,
                                                      const PayloadAccessContext &access,
                                                      const EventTxn &t) {
  auto r = resolve(k, access, t);
  if (!r)
    return r.error();
  auto supplies = supplies_response(access);
  if (!supplies)
    return supplies.error();
  if (supplies.value()) {
    auto response = project_response(k, access, t);
    if (!response)
      return response.error();
    PayloadSnapshot result;
    result.status = response.value().status;
    result.data = std::move(response.value().data);
    result.dmi_hint = response.value().dmi_hint;
    result.extensions = std::move(response.value().extensions);
    return result;
  }
  if (access.stage != PayloadAccessStage::Call || access.call_phase != begin_req)
    return PayloadSnapshot{};
  Expected<PayloadSnapshot> current = r.value()->baseline;
  current.value().status = ResponseStatus::Incomplete;
  current.value().dmi_hint = false;
  for (auto it = current.value().extensions.begin(); it != current.value().extensions.end();)
    if (!extension_rules_.at(it->first).request_allowed)
      it = current.value().extensions.erase(it);
    else
      ++it;
  auto valid = check_extensions(current.value().extensions, false);
  if (!valid)
    return valid.error();
  return std::move(current.value());
}
Expected<void> PayloadShadow::deliver_response(const PayloadViewKey &k, ResponseSnapshot response,
                                               const PayloadAccessContext &access, EventTxn &t) {
  auto r = resolve(k, access, t);
  if (!r)
    return r.error();
  auto supplied = supplies_response(access);
  if (!supplied)
    return supplied.error();
  if (!supplied.value())
    return fail(ErrorCode::NotReady, "exchange supplies no response");
  auto &v = *r.value();
  if (v.role != PayloadRole::Initiator)
    return fail(ErrorCode::WrongOwner, "response projection target");
  auto latched = t.read(v.response_latched);
  if (!latched)
    return latched.error();
  if (std::get<bool>(latched.value().data))
    return fail(ErrorCode::Duplicate, "response already captured");
  if (static_cast<unsigned>(response.status) >
      static_cast<unsigned>(ResponseStatus::ByteEnableError))
    return fail(ErrorCode::TypeMismatch, "response status");
  auto extensions = check_extensions(response.extensions, true);
  if (!extensions)
    return extensions.error();
  if (v.baseline.command == Command::Read && response.data.size() != v.baseline.data.size())
    return fail(ErrorCode::TypeMismatch, "response data shape");
  auto projected = snapshot(k, t);
  if (!projected)
    return projected.error();
  for (const auto &rule : extension_rules_)
    if (rule.second.response_allowed) {
      auto found = response.extensions.find(rule.first);
      if (found == response.extensions.end())
        projected.value().extensions.erase(rule.first);
      else
        projected.value().extensions[rule.first] = found->second;
    }
  if (!snapshot_fits(projected.value(), max_bytes_))
    return fail(ErrorCode::Capacity, "response aggregate bytes");
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto write = [&](VersionedCell &cell, Value value) -> Expected<void> {
    auto result = t.buffer(cell, std::move(value));
    if (!result)
      t.rollback(std::move(cp.value()));
    return result;
  };
  if (v.baseline.command == Command::Read) {
    for (std::size_t i = 0; i < response.data.size(); ++i)
      if (!v.baseline.byte_enable.empty() &&
          !v.baseline.byte_enable[i % v.baseline.byte_enable.size()])
        response.data[i] = v.baseline.data[i];
    auto data = write(v.data, Value(std::move(response.data)));
    if (!data)
      return data.error();
  }
  auto status = write(v.status, Value(static_cast<std::uint64_t>(response.status)));
  if (!status)
    return status.error();
  auto dmi = write(v.dmi, Value(response.dmi_hint));
  if (!dmi)
    return dmi.error();
  for (auto &extension : v.extensions)
    if (extension_rules_.at(extension.first).response_allowed) {
      auto found = response.extensions.find(extension.first);
      auto result = write(extension.second,
                          found == response.extensions.end() ? Value{} : Value(found->second));
      if (!result)
        return result.error();
    }
  return write(v.response_latched, Value(true));
}
} // namespace leanat
namespace leanat {
Expected<ResultCreate> ResultStore::describe(ResultHandle h) const {
  auto valid = impl_->consumer(h);
  if (!valid)
    return valid.error();
  return impl_->slots[h.result.slot].create;
}
} // namespace leanat
namespace leanat {
Expected<ResultCreate> ResultStore::prepare_describe(const EventTxn &t, ResultHandle h) const {
  if (!t.is_open())
    return fail(ErrorCode::InvalidState, "closed result segment");
  auto p = static_cast<Staged *>(t.participant(impl_.get()));
  ResultStore view(p ? p->snapshot : impl_);
  return view.describe(h);
}
Expected<PayloadExtensionRule> PayloadShadow::describe_extension(const std::string &key) const {
  auto rule = extension_rules_.find(key);
  if (rule == extension_rules_.end())
    return fail(ErrorCode::Unsupported, "extension not registered");
  return rule->second;
}
} // namespace leanat
namespace leanat {
Expected<void> PayloadShadow::validate_access(const PayloadViewKey &key,
                                              const PayloadAccessContext &access,
                                              const EventTxn &txn) {
  auto view = resolve(key, access, txn);
  if (!view)
    return view.error();
  return {};
}
} // namespace leanat
