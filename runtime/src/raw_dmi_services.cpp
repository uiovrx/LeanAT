#include "leanat/raw_dmi_services.hpp"
#include "leanat/descriptor.hpp"
namespace leanat {
namespace {
void word(Bytes &b, std::uint64_t n) {
  for (unsigned i = 0; i < 8; ++i)
    b.push_back(std::uint8_t(n >> (8 * i)));
}
void words(Bytes &b, const std::vector<std::uint32_t> &v) {
  word(b, v.size());
  for (auto x : v)
    word(b, x);
}
Value value(const RawDmiGrant &g) {
  return Value(Value::Array{Value(g.granted), Value(g.region), Value(g.start), Value(g.end),
                            Value(g.permission), Value(g.read_latency), Value(g.write_latency),
                            Value(g.generation)});
}
bool u64(const exec::Project &p, std::uint32_t t) {
  return t < p.types.size() && p.types[t].kind == exec::TypeKind::Bits && p.types[t].bound == 64;
}
} // namespace
Expected<exec::ServiceSignature> raw_dmi_signature(const exec::Project &p, std::uint32_t id,
                                                   exec::Op op, std::vector<std::uint32_t> in,
                                                   std::uint32_t out) {
  if (out >= p.types.size())
    return fail(ErrorCode::Schema, "raw result type");
  bool invalidate = op == exec::Op::InvalidateRawDmi;
  if (op != exec::Op::GrantRawDmi && op != exec::Op::DenyDmi && !invalidate)
    return fail(ErrorCode::Unsupported, "raw opcode");
  if (in.size() != (op == exec::Op::DenyDmi ? 0 : 3) ||
      !std::all_of(in.begin(), in.end(), [&](auto t) { return u64(p, t); }))
    return fail(ErrorCode::Schema, "raw input shape");
  const auto &t = p.types[out];
  if (invalidate) {
    if (t.kind != exec::TypeKind::Unit)
      return fail(ErrorCode::Schema, "raw invalidation result");
  } else if (t.kind != exec::TypeKind::Record || t.fields.size() != 8 ||
             t.fields[0] >= p.types.size() || p.types[t.fields[0]].kind != exec::TypeKind::Bool ||
             !std::all_of(t.fields.begin() + 1, t.fields.end(), [&](auto f) { return u64(p, f); }))
    return fail(ErrorCode::Schema, "raw grant result");
  exec::ServiceSignature s;
  s.id = id;
  s.op = op;
  s.input_types = std::move(in);
  s.result_types = {out};
  s.context_mask = invalidate ? 3 : 16;
  s.effect_mask = exec::RawDmi;
  s.provider_key = "leanat.raw-dmi." + std::to_string(static_cast<unsigned>(op));
  s.provider_version = "1";
  Bytes bytes;
  std::string semantics = "raw-dmi-v1";
  word(bytes, semantics.size());
  bytes.insert(bytes.end(), semantics.begin(), semantics.end());
  word(bytes, static_cast<unsigned>(op));
  word(bytes, p.types.size());
  for (const auto &type : p.types) {
    word(bytes, static_cast<unsigned>(type.kind));
    word(bytes, type.bound);
    words(bytes, type.fields);
    word(bytes, type.constructors.size());
    for (const auto &c : type.constructors)
      words(bytes, c);
  }
  for (auto i : s.input_types)
    word(bytes, i);
  for (auto i : s.result_types)
    word(bytes, i);
  s.abi_hash = exec::sha256(bytes);
  return s;
}
struct RawDmiServices::Impl {
  struct Intent {
    std::uint64_t region{}, start{}, end{};
    Command command{};
    bool invalidation{};
    std::vector<RawDmiGrant> affected;
  };
  struct State {
    std::vector<RawDmiGrant> grants;
    std::vector<Intent> pending;
    std::vector<RawDmiObservation> observed;
    bool failed{};
  };
  bool enabled;
  std::map<std::uint64_t, RawDmiRegion> regions;
  std::shared_ptr<State> state{std::make_shared<State>()};
  struct Prepared final : PreparedParticipant {
    Impl &owner;
    std::shared_ptr<State> base, next;
    std::size_t bytes;
    Prepared(Impl &o, std::shared_ptr<State> b, std::shared_ptr<State> n)
        : owner(o), base(std::move(b)), next(std::move(n)),
          bytes(sizeof(*this) + sizeof(State) + next->grants.size() * sizeof(RawDmiGrant) +
                next->pending.size() * sizeof(Intent) +
                next->observed.capacity() * sizeof(RawDmiObservation)) {
      for (const auto &intent : next->pending)
        bytes += intent.affected.size() * sizeof(RawDmiGrant);
    }
    const void *identity() const noexcept override {
      return &owner;
    }
    std::size_t reserved_bytes() const noexcept override {
      return bytes;
    }
    Expected<void> validate() const override {
      if (owner.state != base || owner.state->failed)
        return fail(ErrorCode::InvalidState, "raw staged conflict");
      return {};
    }
    void apply() noexcept override {
      owner.state = next;
    }
    void discard() noexcept override {}
  };
  Expected<std::vector<Value>> invoke(exec::Op op, const std::vector<Value> &args,
                                      const ExecutionContext &c, EventTxn &txn) {
    if (!enabled)
      return fail(ErrorCode::Unsupported, "raw capability disabled");
    if (c.owner != txn.context().owner || c.domain != txn.context().domain ||
        c.instance != txn.context().instance || c.kind != txn.context().kind ||
        c.ready != txn.context().ready || c.epoch != txn.context().epoch ||
        c.connection != txn.context().connection)
      return fail(ErrorCode::WrongOwner, "raw segment context");
    if ((op == exec::Op::InvalidateRawDmi && c.kind != ContextKind::Timed &&
         c.kind != ContextKind::Process) ||
        (op != exec::Op::InvalidateRawDmi && c.kind != ContextKind::Dmi))
      return fail(ErrorCode::InvalidState, "raw service context kind");
    if (op == exec::Op::DenyDmi)
      return std::vector<Value>{value({})};
    std::uint64_t n[3];
    for (unsigned i = 0; i < 3; ++i) {
      auto v = std::get_if<std::uint64_t>(&args[i].data);
      if (!v)
        return fail(ErrorCode::TypeMismatch, "raw U64 argument");
      n[i] = *v;
    }
    auto found = regions.find(n[0]);
    if (found == regions.end())
      return fail(ErrorCode::Unsupported, "raw region missing");
    const auto &r = found->second;
    if (c.domain != r.domain || c.owner != r.owner)
      return fail(ErrorCode::WrongOwner, "raw region authority");
    auto latest = state;
    if (auto p = txn.participant(this))
      latest = static_cast<Prepared *>(p)->next;
    auto next = std::make_shared<State>(*latest);
    RawDmiGrant grant;
    if (op == exec::Op::GrantRawDmi) {
      if (n[1] < r.start || n[1] > r.end || n[2] > 1)
        return std::vector<Value>{value({})};
      auto count = std::count_if(next->grants.begin(), next->grants.end(),
                                 [&](const auto &g) { return g.region == r.id; });
      if (static_cast<std::size_t>(count) >= r.capacity)
        return std::vector<Value>{value({})};
      grant = {true, r.id, r.start, r.end, 3, r.read_latency, r.write_latency, r.generation};
      next->grants.push_back(grant);
      next->pending.push_back(
          {r.id, n[1], 0, n[2] == 0 ? Command::Read : Command::Write, false, {}});
      next->pending.back().affected.push_back(grant);
    } else {
      if (n[1] > n[2])
        return fail(ErrorCode::InvalidArgument, "raw invalidation range");
      next->pending.push_back({r.id, n[1], n[2], Command::Read, true, {}});
      for (const auto &g : next->grants)
        if (g.region == r.id && g.start <= n[2] && n[1] <= g.end)
          next->pending.back().affected.push_back(g);
      next->grants.erase(std::remove_if(next->grants.begin(), next->grants.end(),
                                        [&](const auto &g) {
                                          return g.region == r.id && g.start <= n[2] &&
                                                 n[1] <= g.end;
                                        }),
                         next->grants.end());
    }
    std::size_t effects = next->observed.size();
    for (const auto &pending : next->pending)
      effects += pending.affected.size();
    if (effects > 1024)
      return fail(ErrorCode::Capacity, "raw observation capacity");
    next->observed.reserve(effects);
    if (next->pending.size() > 1024)
      return fail(ErrorCode::Capacity, "raw publication capacity");
    auto staged = txn.stage_participant(std::make_unique<Prepared>(*this, state, next));
    if (!staged)
      return staged.error();
    return std::vector<Value>{op == exec::Op::GrantRawDmi ? value(grant) : Value{}};
  }
};
RawDmiServices::RawDmiServices(bool enabled) : impl_(std::make_shared<Impl>()) {
  impl_->enabled = enabled;
}
Expected<void> RawDmiServices::add_region(RawDmiRegion region) {
  if (!region.grant || !region.invalidate || region.start > region.end || !region.generation ||
      !region.capacity)
    return fail(ErrorCode::InvalidArgument, "raw concrete region");
  if (!impl_->regions.emplace(region.id, std::move(region)).second)
    return fail(ErrorCode::Duplicate, "raw region duplicate");
  return {};
}
Expected<void> RawDmiServices::register_into(CoreRuntimeBackend &backend, const exec::Project &p) {
  std::vector<std::pair<exec::ServiceSignature, CoreRuntimeBackend::Provider>> entries;
  auto impl = impl_;
  for (const auto &s : p.services) {
    if (s.provider_key.rfind("leanat.raw-dmi.", 0) != 0)
      continue;
    if (s.result_types.size() != 1)
      return fail(ErrorCode::Schema, "raw result arity");
    auto concrete = raw_dmi_signature(p, s.id, s.op, s.input_types, s.result_types[0]);
    if (!concrete)
      return concrete.error();
    if (concrete.value() != s)
      return fail(ErrorCode::Integrity, "raw frozen ABI");
    entries.emplace_back(s,
                         [impl, op = s.op](const std::vector<Value> &a, const ExecutionContext &c,
                                           EventTxn &t) { return impl->invoke(op, a, c, t); });
  }
  return backend.register_providers(std::move(entries));
}
Expected<void> RawDmiServices::publish() {
  auto publication = std::make_shared<Impl::State>(*impl_->state);
  impl_->state = publication;
  auto &s = *publication;
  if (s.failed)
    return fail(ErrorCode::InvalidState, "raw publication already failed");
  for (const auto &i : s.pending) {
    auto &r = impl_->regions.at(i.region);
    Expected<void> result;
    try {
      result = i.invalidation ? r.invalidate(i.start, i.end) : r.grant(i.start, i.command);
    } catch (...) {
      result = fail(ErrorCode::ExternalFailure, "raw native publication exception");
    }
    if (!result) {
      s.failed = true;
      return result.error();
    }
    for (const auto &g : i.affected)
      s.observed.push_back({i.invalidation, g});
  }
  s.pending.clear();
  return {};
}
const std::vector<RawDmiGrant> &RawDmiServices::grants() const {
  return impl_->state->grants;
}
const std::vector<RawDmiObservation> &RawDmiServices::observations() const {
  return impl_->state->observed;
}
} // namespace leanat
