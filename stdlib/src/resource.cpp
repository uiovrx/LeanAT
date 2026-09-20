#include "leanat/resource.hpp"
#include <atomic>
#include <mutex>
#include <set>
namespace leanat {
namespace {
using A = Value::Array;
std::uint64_t u(const Value &v) {
  return std::get<std::uint64_t>(v.data);
}
A &arr(Value &v) {
  return std::get<A>(v.data);
}
const A &arr(const Value &v) {
  return std::get<A>(v.data);
}
Grant grant(const A &r) {
  return {Tick{u(r[2])}, Tick{u(r[3])}, Duration{u(r[4])}, std::get<Handle>(r[1].data),
          static_cast<std::size_t>(u(r[5]))};
}
} // namespace
Expected<Resource> Resource::make(ResourceSpec s, DomainId d, std::uint32_t store) {
  static std::atomic<std::uint64_t> next_store{0x80000000ULL};
  if (!store) {
    auto unique = next_store.fetch_add(1);
    if (unique > UINT32_MAX)
      return fail(ErrorCode::Overflow, "resource identity exhausted");
    store = static_cast<std::uint32_t>(unique);
  }
  static std::mutex identity_mutex;
  static std::set<std::pair<std::uint32_t, std::uint32_t>> identities;
  if (!s.capacity || !s.reservation_capacity ||
      s.reservation_capacity > std::numeric_limits<std::uint32_t>::max() ||
      (s.kind == ResourceKind::Serial && s.capacity != 1))
    return fail(ErrorCode::InvalidArgument, "invalid resource capacity");
  {
    std::lock_guard<std::mutex> lock(identity_mutex);
    if (!identities.emplace(d.value, store).second)
      return fail(ErrorCode::Duplicate, "resource store identity already used");
  }
  Resource r;
  r.cell_.epoch_independent = true;
  r.spec_ = s;
  r.domain_ = d;
  r.store_ = store;
  A channels(s.capacity, Value(std::uint64_t(0))), records;
  records.reserve(s.reservation_capacity);
  for (std::size_t i = 0; i < s.reservation_capacity; ++i)
    records.emplace_back(A{Value(std::uint64_t(0)), Value(Handle{}), Value(std::uint64_t(0)),
                           Value(std::uint64_t(0)), Value(std::uint64_t(0)),
                           Value(std::uint64_t(0)), Value(false)});
  r.cell_.value = Value(A{Value(std::uint64_t(0)), Value(std::uint64_t(0)), Value(std::uint64_t(0)),
                          Value(channels), Value(records)});
  return r;
}
Expected<Grant> Resource::reserve(EventTxn &t, Handle owner, Tick earliest, Duration duration) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  if (t.context().domain != domain_ || owner.domain != domain_)
    return fail(ErrorCode::WrongDomain, "resource owner domain");
  if (t.context().owner != owner.owner)
    return fail(ErrorCode::WrongOwner, "resource caller owner");
  if (owner.kind != HandleKind::Transaction && owner.kind != HandleKind::Access)
    return fail(ErrorCode::WrongOwner, "resource owner kind");
  if (!owner.generation)
    return fail(ErrorCode::StaleHandle, "resource owner generation");
  if (earliest < t.context().ready.time)
    return fail(ErrorCode::TimeRegression, "earliest precedes arrival");
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto state = arr(v.value());
  if (t.context().ready.time.value < u(state[2]))
    return fail(ErrorCode::TimeRegression, "resource arrival regression");
  auto &records = arr(state[4]);
  std::size_t slot = records.size();
  for (std::size_t i = 0; i < records.size(); ++i)
    if (!u(arr(records[i])[0]) && std::get<Handle>(arr(records[i])[1].data).generation !=
                                      std::numeric_limits<std::uint64_t>::max()) {
      slot = i;
      break;
    }
  if (slot == records.size())
    return fail(ErrorCode::Capacity, "resource reservation metadata full");
  auto &channels = arr(state[3]);
  std::size_t channel = 0;
  for (std::size_t i = 1; i < channels.size(); ++i)
    if (u(channels[i]) < u(channels[channel]))
      channel = i;
  auto start = std::max({earliest.value, u(state[1]), u(channels[channel]),
                         spec_.kind == ResourceKind::Pipeline ? u(state[0]) : std::uint64_t(0)});
  auto finish = checked_add(start, duration.value);
  if (!finish)
    return finish.error();
  auto next = checked_add(
      start, spec_.kind == ResourceKind::Pipeline ? spec_.initiation_interval.value : 0);
  if (!next)
    return next.error();
  auto previous = std::get<Handle>(arr(records[slot])[1].data);
  Handle ticket{HandleKind::ResourceTicket, domain_,    store_, static_cast<std::uint32_t>(slot),
                previous.generation + 1,    owner.owner};
  Grant g{Tick{start}, Tick{finish.value()}, Duration{start - earliest.value}, ticket, channel};
  records[slot] =
      Value(A{Value(std::uint64_t(1)), Value(ticket), Value(start), Value(finish.value()),
              Value(start - earliest.value), Value(std::uint64_t(channel)), Value(false)});
  channels[channel] = Value(finish.value());
  state[0] = Value(next.value());
  state[1] = Value(start);
  state[2] = Value(t.context().ready.time.value);
  auto w = t.buffer(cell_, Value(std::move(state)));
  if (!w)
    return w.error();
  return g;
}
Expected<ResourceCancelDisposition> Resource::cancel_pending(EventTxn &t, Handle h) {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "resource context domain");
  if (t.context().owner != h.owner)
    return fail(ErrorCode::WrongOwner, "resource ticket caller owner");
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto state = arr(v.value());
  auto &records = arr(state[4]);
  if (h.kind != HandleKind::ResourceTicket || h.domain != domain_ || h.store != store_ ||
      h.slot >= records.size())
    return fail(ErrorCode::StaleHandle, "resource ticket identity");
  auto &r = arr(records[h.slot]);
  if (std::get<Handle>(r[1].data) != h)
    return fail(ErrorCode::StaleHandle, "resource ticket generation/owner");
  if (!u(r[0]))
    return ResourceCancelDisposition::AlreadySettled;
  if (std::get<bool>(r[6].data))
    return ResourceCancelDisposition::AlreadyCancelled;
  r[6] = Value(true);
  auto result = t.context().ready.time.value >= u(r[2])
                    ? ResourceCancelDisposition::CancellationDeferred
                    : ResourceCancelDisposition::CancelledRetained;
  const auto &committed = arr(cell_.value);
  const auto &old_record = arr(arr(committed[4])[h.slot]);
  if (!u(old_record[0]) || std::get<Handle>(old_record[1].data) != h) {
    r[0] = Value(std::uint64_t(0));
    state[0] = committed[0];
    state[1] = committed[1];
    state[3] = committed[3];
    auto &channels = arr(state[3]);
    for (const auto &value : records) {
      const auto &record = arr(value);
      if (!u(record[0]))
        continue;
      auto channel = static_cast<std::size_t>(u(record[5]));
      channels[channel] = Value(std::max(u(channels[channel]), u(record[3])));
      state[1] = Value(std::max(u(state[1]), u(record[2])));
      auto next = checked_add(
          u(record[2]), spec_.kind == ResourceKind::Pipeline ? spec_.initiation_interval.value : 0);
      if (!next)
        return next.error();
      state[0] = Value(std::max(u(state[0]), next.value()));
    }
    result = ResourceCancelDisposition::CancelledUnpublished;
  }
  auto w = t.buffer(cell_, Value(std::move(state)));
  if (!w)
    return w.error();
  return result;
}
Expected<bool> Resource::complete(EventTxn &t, Handle h, Tick at) {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "resource context domain");
  if (t.context().owner != h.owner)
    return fail(ErrorCode::WrongOwner, "resource ticket caller owner");
  if (at != t.context().ready.time)
    return fail(ErrorCode::InvalidArgument, "resource completion must use current effective time");
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto state = arr(v.value());
  auto &records = arr(state[4]);
  if (h.kind != HandleKind::ResourceTicket || h.domain != domain_ || h.store != store_ ||
      h.slot >= records.size())
    return fail(ErrorCode::StaleHandle, "resource ticket identity");
  auto &r = arr(records[h.slot]);
  if (std::get<Handle>(r[1].data) != h)
    return fail(ErrorCode::StaleHandle, "resource ticket generation/owner");
  if (!u(r[0]))
    return false;
  if (at.value < u(r[3]))
    return fail(ErrorCode::NotReady, "resource finish not reached");
  bool business = !std::get<bool>(r[6].data);
  r[0] = Value(std::uint64_t(0));
  auto w = t.buffer(cell_, Value(std::move(state)));
  if (!w)
    return w.error();
  return business;
}
Expected<ResourceSnapshot> Resource::inspect(EventTxn &t) const {
  if (t.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "resource context domain");
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto &s = arr(v.value());
  ResourceSnapshot out;
  out.next_issue = Tick{u(s[0])};
  for (auto &rv : arr(s[4])) {
    auto &r = arr(rv);
    if (!u(r[0]))
      continue;
    out.reservations.push_back(grant(r));
    if (std::get<bool>(r[6].data))
      ++out.cancelled;
    auto now = t.context().ready.time.value;
    if (now < u(r[2]))
      ++out.scheduled;
    else if (now < u(r[3]))
      ++out.servicing;
  }
  return out;
}
} // namespace leanat
