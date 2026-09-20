#include "leanat/objects.hpp"
#include "leanat/memory.hpp"
#include "leanat/pipeline.hpp"
#include "leanat/queue.hpp"
#include "leanat/register_bank.hpp"
#include "leanat/resource.hpp"
namespace leanat {
namespace {
bool intrinsic_type(TypeId type) {
  return type.value >= 1 && type.value <= 5;
}
bool intrinsic_value(TypeId type, const Value &value) {
  switch (type.value) {
  case 1:
    return std::holds_alternative<std::uint64_t>(value.data);
  case 2:
    return std::holds_alternative<Bytes>(value.data);
  case 3:
    return std::holds_alternative<Handle>(value.data);
  case 4:
    return true; // The owning Value ABI deliberately permits every alternative.
  case 5:
    return std::holds_alternative<bool>(value.data);
  default:
    return false;
  }
}
} // namespace
Expected<void> object_context(const EventTxn &t, bool debug) {
  auto k = t.context().kind;
  if (debug ? k != ContextKind::Debug : (k != ContextKind::Timed && k != ContextKind::Process))
    return fail(ErrorCode::InvalidState, "object context not permitted");
  return {};
}
Expected<void> ObjectRegistry::register_provider(ProviderDesc d,
                                                 std::shared_ptr<ObjectProvider> p) {
  if (frozen_)
    return fail(ErrorCode::InvalidState, "registry frozen");
  if (!p || d.abi != 1 || d.key.empty() || d.version.empty() || d.hash.empty() || d.methods.empty())
    return fail(ErrorCode::Schema, "invalid provider ABI/descriptor");
  if (d.evidence == "proved" && d.reference.empty())
    return fail(ErrorCode::Integrity, "proved provider needs reference evidence");
  if (entries_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "provider capacity");
  for (auto &e : entries_)
    if (e.first == d.kind || (e.second.desc.key == d.key && e.second.desc.version == d.version))
      return fail(ErrorCode::Duplicate, "provider conflict");
  std::map<MethodId, bool> ids;
  for (auto &m : d.methods) {
    if (!std::all_of(m.arguments.begin(), m.arguments.end(), intrinsic_type) ||
        !std::all_of(m.results.begin(), m.results.end(), intrinsic_type))
      return fail(ErrorCode::Schema, "unknown intrinsic TypeId");
    if (!ids.emplace(m.id, true).second || m.contexts.empty())
      return fail(ErrorCode::Schema, "method conflict/context missing");
    for (auto c : m.contexts)
      if (c == ContextKind::Debug && (m.effects & ~(ObjectPeek | ObjectPoke)))
        return fail(ErrorCode::InvalidState, "debug effect violation");
    if (m.effects & ObjectWait)
      return fail(ErrorCode::Unsupported, "waiting intrinsic must use process suspension");
  }
  auto k = d.kind;
  entries_.emplace(k, Entry{std::move(d), std::move(p)});
  return {};
}
Expected<void> ObjectRegistry::register_object(ObjectDesc d) {
  if (frozen_)
    return fail(ErrorCode::InvalidState, "registry frozen");
  if (!entries_.count(d.kind))
    return fail(ErrorCode::Unsupported, "unknown provider");
  if (objects_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "object capacity");
  if (!objects_.emplace(d.id, d).second)
    return fail(ErrorCode::Duplicate, "object conflict");
  return {};
}
Expected<void> ObjectRegistry::freeze() {
  frozen_ = true;
  return {};
}
Expected<MethodDesc> ObjectRegistry::lookup(std::uint32_t k, MethodId id) const {
  auto p = entries_.find(k);
  if (p == entries_.end())
    return fail(ErrorCode::Unsupported, "unknown provider");
  for (auto &m : p->second.desc.methods)
    if (m.id == id)
      return m;
  return fail(ErrorCode::Unsupported, "unknown intrinsic");
}
Expected<ObjectResult> ObjectRegistry::prepare(ObjectId id, MethodId m,
                                               const std::vector<Value> &args,
                                               const std::vector<TypeId> &types, EventTxn &t) {
  if (!frozen_)
    return fail(ErrorCode::InvalidState, "registry not frozen");
  auto o = objects_.find(id);
  if (o == objects_.end())
    return fail(ErrorCode::StaleHandle, "unknown object");
  if (o->second.domain != t.context().domain || o->second.instance != t.context().instance)
    return fail(ErrorCode::WrongDomain, "object outside instance/domain");
  auto d = lookup(o->second.kind, m);
  if (!d)
    return d.error();
  if (args.size() != types.size() || types != d.value().arguments)
    return fail(ErrorCode::TypeMismatch, "intrinsic signature mismatch");
  for (std::size_t i = 0; i < args.size(); ++i)
    if (!intrinsic_value(types[i], args[i]))
      return fail(ErrorCode::TypeMismatch, "intrinsic argument representation mismatch");
  if (std::find(d.value().contexts.begin(), d.value().contexts.end(), t.context().kind) ==
      d.value().contexts.end())
    return fail(ErrorCode::InvalidState, "intrinsic context violation");
  std::size_t bytes = 0;
  for (auto &v : args) {
    auto n = owned_value_bytes(v);
    if (n > d.value().max_argument_bytes - bytes)
      return fail(ErrorCode::Capacity, "intrinsic argument budget");
    bytes += n;
  }
  auto checkpoint = t.checkpoint();
  if (!checkpoint)
    return checkpoint.error();
  try {
    auto result = entries_.at(o->second.kind).provider->prepare(id, m, args, t);
    if (!result) {
      t.rollback(std::move(checkpoint.value()));
      return result.error();
    }
    if (result.value().values.size() != d.value().results.size()) {
      t.rollback(std::move(checkpoint.value()));
      return fail(ErrorCode::TypeMismatch, "intrinsic result signature mismatch");
    }
    std::size_t result_bytes = 0;
    for (std::size_t i = 0; i < result.value().values.size(); ++i) {
      const auto &value = result.value().values[i];
      auto n = owned_value_bytes(value);
      if (n > d.value().max_result_bytes - result_bytes) {
        t.rollback(std::move(checkpoint.value()));
        return fail(ErrorCode::Capacity, "intrinsic result budget");
      }
      result_bytes += n;
      if (!intrinsic_value(d.value().results[i], value)) {
        t.rollback(std::move(checkpoint.value()));
        return fail(ErrorCode::TypeMismatch, "intrinsic result representation mismatch");
      }
    }
    auto effects = t.validate_effects_since(
        checkpoint.value(), (d.value().effects & (ObjectWrite | ObjectPoke)) != 0,
        (d.value().effects & ObjectSchedule) != 0, (d.value().effects & ObjectSocket) != 0);
    if (!effects) {
      t.rollback(std::move(checkpoint.value()));
      return effects.error();
    }
    return result;
  } catch (const std::exception &e) {
    t.rollback(std::move(checkpoint.value()));
    return fail(ErrorCode::ExternalFailure,
                std::string("provider implementation fault: ") + e.what());
  } catch (...) {
    t.rollback(std::move(checkpoint.value()));
    return fail(ErrorCode::ExternalFailure, "provider implementation fault");
  }
}
} // namespace leanat

namespace leanat {
namespace {
using SI = StandardIntrinsic;
using A = Value::Array;
std::uint64_t uint_arg(const std::vector<Value> &a, std::size_t i) {
  return std::get<std::uint64_t>(a.at(i).data);
}
const Bytes &bytes_arg(const std::vector<Value> &a, std::size_t i) {
  return std::get<Bytes>(a.at(i).data);
}
Handle handle_arg(const std::vector<Value> &a, std::size_t i) {
  return std::get<Handle>(a.at(i).data);
}
FieldId field_arg(const std::vector<Value> &a, std::size_t i) {
  auto n = uint_arg(a, i);
  if (n > UINT32_MAX)
    throw std::out_of_range("field ID overflow");
  return FieldId{static_cast<std::uint32_t>(n)};
}
Command command_arg(const std::vector<Value> &a, std::size_t i) {
  auto n = uint_arg(a, i);
  return n > std::uint64_t(Command::Ignore) ? static_cast<Command>(3) : static_cast<Command>(n);
}
Value grant_value(const Grant &g) {
  return Value(A{Value(g.start.value), Value(g.finish.value), Value(g.wait.value), Value(g.ticket),
                 Value(std::uint64_t(g.channel))});
}
PayloadSnapshot payload_args(const std::vector<Value> &a) {
  PayloadSnapshot p;
  p.command = command_arg(a, 0);
  p.address = uint_arg(a, 1);
  p.data = bytes_arg(a, 2);
  p.streaming_width = uint_arg(a, 3);
  p.byte_enable = bytes_arg(a, 4);
  return p;
}
Expected<ObjectResult> transfer_result(Expected<MemoryTransferResult> r) {
  if (!r)
    return r.error();
  return ObjectResult{{Value(std::uint64_t(r.value().status)), Value(std::move(r.value().data))}};
}
Expected<ObjectResult> debug_result(Expected<MemoryDebugResult> r) {
  if (!r)
    return r.error();
  return ObjectResult{{Value(std::uint64_t(r.value().count)), Value(std::move(r.value().data))}};
}
Expected<ObjectResult> void_result(Expected<void> r) {
  if (!r)
    return r.error();
  return ObjectResult{};
}
Expected<ObjectResult> prepare_standard(Memory &m, SI id, const std::vector<Value> &a,
                                        EventTxn &t) {
  switch (id) {
  case SI::Transfer:
    return transfer_result(m.transfer(t, payload_args(a)));
  case SI::ReadBytes: {
    auto r = m.read_bytes(t, uint_arg(a, 0), uint_arg(a, 1));
    if (!r)
      return r.error();
    return ObjectResult{{Value(std::move(r.value()))}};
  }
  case SI::WriteBytes:
    return void_result(m.write_bytes(t, uint_arg(a, 0), bytes_arg(a, 1), bytes_arg(a, 2)));
  case SI::Clear:
    return void_result(m.clear(t));
  case SI::Reset:
    return void_result(m.reset(t));
  case SI::Debug:
    return debug_result(m.debug_transfer(t, command_arg(a, 0), uint_arg(a, 1), bytes_arg(a, 2)));
  default:
    return fail(ErrorCode::Unsupported, "memory method");
  }
}
Expected<ObjectResult> prepare_standard(RegisterBank &m, SI id, const std::vector<Value> &a,
                                        EventTxn &t) {
  switch (id) {
  case SI::Transfer:
    return transfer_result(m.access(t, payload_args(a)));
  case SI::ReadField: {
    auto r = m.read_field(t, field_arg(a, 0));
    if (!r)
      return r.error();
    return ObjectResult{{Value(std::move(r.value()))}};
  }
  case SI::UpdateField:
    return void_result(m.stage_field_update(t, field_arg(a, 0), bytes_arg(a, 1)));
  case SI::Reset:
    return void_result(m.reset(t));
  case SI::Debug:
    return debug_result(m.peek_poke(t, command_arg(a, 0), uint_arg(a, 1), bytes_arg(a, 2)));
  default:
    return fail(ErrorCode::Unsupported, "register method");
  }
}
Expected<ObjectResult> prepare_standard(Resource &m, SI id, const std::vector<Value> &a,
                                        EventTxn &t) {
  switch (id) {
  case SI::Reserve:
  case SI::ReserveDefault: {
    auto r = id == SI::Reserve
                 ? m.reserve(t, handle_arg(a, 0), Tick{uint_arg(a, 1)}, Duration{uint_arg(a, 2)})
                 : m.reserve_default(t, handle_arg(a, 0), Tick{uint_arg(a, 1)});
    if (!r)
      return r.error();
    return ObjectResult{{grant_value(r.value())}};
  }
  case SI::Cancel: {
    auto r = m.cancel_pending(t, handle_arg(a, 0));
    if (!r)
      return r.error();
    return ObjectResult{{Value(std::uint64_t(r.value()))}};
  }
  case SI::Complete: {
    auto r = m.complete(t, handle_arg(a, 0), Tick{uint_arg(a, 1)});
    if (!r)
      return r.error();
    return ObjectResult{{Value(r.value())}};
  }
  case SI::Inspect: {
    auto r = m.inspect(t);
    if (!r)
      return r.error();
    A rows;
    for (auto &g : r.value().reservations)
      rows.push_back(grant_value(g));
    return ObjectResult{{Value(std::move(rows))}};
  }
  default:
    return fail(ErrorCode::Unsupported, "resource method");
  }
}
Expected<ObjectResult> prepare_standard(BoundedQueue &m, SI id, const std::vector<Value> &a,
                                        EventTxn &t) {
  switch (id) {
  case SI::Push: {
    auto r = m.try_push(t, a.at(0));
    if (!r)
      return r.error();
    return ObjectResult{{Value(r.value())}};
  }
  case SI::Pop: {
    auto r = m.try_pop(t);
    if (!r)
      return r.error();
    if (!r.value())
      return ObjectResult{{Value{}}};
    const auto &e = *r.value();
    return ObjectResult{{Value(
        A{e.value,
          e.consumer ? Value(A{Value(e.consumer->result), Value(e.consumer->consumer)}) : Value{},
          e.owner ? Value(*e.owner) : Value{}, Value(e.id)})}};
  }
  case SI::PushOwned: {
    auto policy = uint_arg(a, 2);
    if (policy < 1 || policy > 2)
      return fail(ErrorCode::InvalidArgument, "queue ownership policy");
    auto r = m.try_push_owned(t, {handle_arg(a, 0), handle_arg(a, 1)}, {},
                              static_cast<QueueOwnership>(policy));
    if (!r)
      return r.error();
    return ObjectResult{{Value(r.value())}};
  }
  case SI::RemoveOwned:
  case SI::TransferDrain: {
    auto r = id == SI::RemoveOwned
                 ? m.remove_unpublished_owned(t, handle_arg(a, 0))
                 : m.transfer_drain_owned(t, handle_arg(a, 0), handle_arg(a, 1), uint_arg(a, 2));
    if (!r)
      return r.error();
    A ids;
    for (auto n : r.value().entry_ids)
      ids.push_back(Value(n));
    return ObjectResult{{Value(std::move(ids))}};
  }
  case SI::MarkPublished:
    return void_result(m.mark_published(t, uint_arg(a, 0)));
  case SI::Size: {
    auto r = m.size(t);
    if (!r)
      return r.error();
    return ObjectResult{{Value(std::uint64_t(r.value()))}};
  }
  default:
    return fail(ErrorCode::Unsupported, "queue method");
  }
}
Expected<ObjectResult> prepare_standard(Pipeline &m, SI id, const std::vector<Value> &a,
                                        EventTxn &t) {
  if (id == SI::Cancel || id == SI::Ready) {
    const auto &p = std::get<A>(a.at(0).data);
    if (p.size() != 4)
      return fail(ErrorCode::TypeMismatch, "pipeline ticket shape");
    const auto &g = std::get<A>(p[0].data);
    if (g.size() != 5)
      return fail(ErrorCode::TypeMismatch, "pipeline grant shape");
    PipelineTicket ticket{
        {Tick{std::get<std::uint64_t>(g[0].data)}, Tick{std::get<std::uint64_t>(g[1].data)},
         Duration{std::get<std::uint64_t>(g[2].data)}, std::get<Handle>(g[3].data),
         static_cast<std::size_t>(std::get<std::uint64_t>(g[4].data))},
        std::get<Handle>(p[1].data),
        std::get<Handle>(p[2].data),
        std::get<std::uint64_t>(p[3].data)};
    if (id == SI::Cancel) {
      auto r = m.cancel_pending(t, ticket);
      if (!r)
        return r.error();
      return ObjectResult{{Value(std::uint64_t(r.value()))}};
    }
    auto r = m.on_ready(t, ticket, handle_arg(a, 1));
    if (!r)
      return r.error();
    return ObjectResult{{Value(r.value())}};
  }
  if (id == SI::Submit) {
    auto r = m.submit(t, handle_arg(a, 0), Tick{uint_arg(a, 1)});
    if (!r)
      return r.error();
    auto &p = r.value();
    return ObjectResult{
        {Value(A{grant_value(p.grant), Value(p.owner), Value(p.ready_event), Value(p.epoch)})}};
  }
  if (id == SI::Inspect)
    return prepare_standard(m.resource(), id, a, t);
  return fail(ErrorCode::Unsupported, "pipeline method");
}
class StandardProvider final : public ObjectProvider {
  std::map<ObjectId, StandardObject> objects_;

public:
  explicit StandardProvider(std::map<ObjectId, StandardObject> o) : objects_(std::move(o)) {}
  Expected<ObjectResult> prepare(ObjectId object, MethodId method, const std::vector<Value> &args,
                                 EventTxn &t) override {
    auto found = objects_.find(object);
    if (found == objects_.end())
      return fail(ErrorCode::StaleHandle, "unbound standard object");
    try {
      return std::visit(
          [&](auto &p) -> Expected<ObjectResult> {
            if (!p)
              return fail(ErrorCode::StaleHandle, "null standard object");
            return prepare_standard(*p, static_cast<SI>(method.value), args, t);
          },
          found->second);
    } catch (const std::bad_variant_access &) {
      return fail(ErrorCode::TypeMismatch, "intrinsic value representation mismatch");
    } catch (const std::out_of_range &) {
      return fail(ErrorCode::TypeMismatch, "intrinsic argument missing");
    }
  }
};
ProviderDesc standard_desc(std::size_t kind, std::uint32_t id) {
  ProviderDesc d;
  d.kind = id;
  d.key = "leanat.stdlib." + std::to_string(kind);
  d.version = "1";
  d.hash = "leanat-stdlib-abi-v1";
  d.reference = "";
  if (kind == 0)
    d.reference = "LeanAT.ModelIR.Object.memoryTransfer;LeanAT.ModelIR.Object.memoryDebug";
  if (kind == 2)
    d.reference = "LeanAT.ModelIR.Object.resourceReserve (reservation only)";
  if (kind == 3)
    d.reference = "LeanAT.ModelIR.Object.queuePush;LeanAT.ModelIR.Object.queuePop (ValueOnly)";
  d.build_id = "cpp17";
  auto add = [&](SI m, std::initializer_list<std::uint32_t> args,
                 std::initializer_list<std::uint32_t> results, std::uint32_t effects,
                 bool debug = false) {
    MethodDesc x;
    x.id = MethodId{static_cast<std::uint32_t>(m)};
    for (auto a : args)
      x.arguments.push_back(TypeId{a});
    for (auto r : results)
      x.results.push_back(TypeId{r});
    x.effects = effects;
    x.contexts = debug ? std::vector<ContextKind>{ContextKind::Debug}
                       : std::vector<ContextKind>{ContextKind::Timed, ContextKind::Process};
    d.methods.push_back(std::move(x));
  };
  if (kind == 0 || kind == 1) {
    add(SI::Transfer, {1, 1, 2, 1, 2}, {1, 2}, ObjectRead | ObjectWrite);
    add(SI::Reset, {}, {}, ObjectWrite);
    add(SI::Debug, {1, 1, 2}, {1, 2}, ObjectPeek | ObjectPoke, true);
    if (kind == 0) {
      add(SI::ReadBytes, {1, 1}, {2}, ObjectRead);
      add(SI::WriteBytes, {1, 2, 2}, {}, ObjectWrite);
      add(SI::Clear, {}, {}, ObjectWrite);
    } else {
      add(SI::ReadField, {1}, {2}, ObjectRead);
      add(SI::UpdateField, {1, 2}, {}, ObjectWrite);
    }
  }
  if (kind == 2) {
    add(SI::Reserve, {3, 1, 1}, {4}, ObjectRead | ObjectWrite);
    add(SI::ReserveDefault, {3, 1}, {4}, ObjectRead | ObjectWrite);
    add(SI::Cancel, {3}, {1}, ObjectWrite);
    add(SI::Complete, {3, 1}, {5}, ObjectWrite);
    add(SI::Inspect, {}, {4}, ObjectRead);
  }
  if (kind == 3) {
    add(SI::Push, {4}, {5}, ObjectRead | ObjectWrite);
    add(SI::Pop, {}, {4}, ObjectRead | ObjectWrite);
    add(SI::Size, {}, {1}, ObjectRead);
    add(SI::PushOwned, {3, 3, 1}, {5}, ObjectRead | ObjectWrite);
    add(SI::RemoveOwned, {3}, {4}, ObjectRead | ObjectWrite);
    add(SI::TransferDrain, {3, 3, 1}, {4}, ObjectRead | ObjectWrite);
    add(SI::MarkPublished, {1}, {}, ObjectWrite);
  }
  if (kind == 4) {
    add(SI::Submit, {3, 1}, {4}, ObjectRead | ObjectWrite | ObjectSchedule);
    add(SI::Inspect, {}, {4}, ObjectRead);
    add(SI::Cancel, {4}, {1}, ObjectRead | ObjectWrite | ObjectSchedule);
    add(SI::Ready, {4, 3}, {5}, ObjectRead | ObjectWrite);
  }
  return d;
}
} // namespace
Expected<void> register_standard_objects(ObjectRegistry &r,
                                         const std::vector<StandardObjectBinding> &bindings) {
  auto next = r;
  std::map<std::uint32_t, std::map<ObjectId, StandardObject>> groups;
  std::map<std::uint32_t, std::size_t> kinds;
  for (auto &b : bindings) {
    auto i = b.object.index();
    auto found = kinds.find(b.desc.kind);
    if (found != kinds.end() && found->second != i)
      return fail(ErrorCode::Schema, "standard object kind conflict");
    kinds[b.desc.kind] = i;
    if (!groups[b.desc.kind].emplace(b.desc.id, b.object).second)
      return fail(ErrorCode::Duplicate, "standard object duplicate");
  }
  for (auto &g : groups) {
    auto added = next.register_provider(standard_desc(kinds[g.first], g.first),
                                        std::make_shared<StandardProvider>(g.second));
    if (!added)
      return added.error();
  }
  for (auto &b : bindings) {
    auto added = next.register_object(b.desc);
    if (!added)
      return added.error();
  }
  r = std::move(next);
  return {};
}
} // namespace leanat
