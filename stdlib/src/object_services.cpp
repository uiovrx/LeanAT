#include "leanat/object_services.hpp"
#include "leanat/descriptor.hpp"
#include "leanat/memory.hpp"
#include "leanat/pipeline.hpp"
#include "leanat/queue.hpp"
#include "leanat/register_bank.hpp"
#include "leanat/resource.hpp"
namespace leanat {
namespace {
using A = Value::Array;
using SI = StandardIntrinsic;
using TK = exec::TypeKind;
Value option(const Value &v) {
  return std::holds_alternative<std::monostate>(v.data)
             ? Value(A{Value(std::uint64_t(0)), Value(A{})})
             : Value(A{Value(std::uint64_t(1)), Value(A{v})});
}
Value queue_result(Value value) {
  if (std::holds_alternative<std::monostate>(value.data))
    return option(value);
  auto &r = std::get<A>(value.data);
  if (r.size() != 4)
    throw std::logic_error("queue result shape");
  bool owned = !std::holds_alternative<std::monostate>(r[1].data);
  Value element = owned ? option(Value{}) : Value(A{Value(std::uint64_t(1)), Value(A{r[0]})});
  Value owner = Value{};
  if (auto h = std::get_if<Handle>(&r[2].data)) {
    if (h->kind != HandleKind::Scope && h->kind != HandleKind::Drain)
      throw std::logic_error("queue owner kind");
    owner =
        Value(A{Value(std::uint64_t(h->kind == HandleKind::Scope ? 0 : 1)), Value(A{Value(*h)})});
  }
  return option(Value(A{std::move(element), option(r[1]), option(owner), r[3]}));
}
std::uint32_t add_type(exec::Project &p, exec::Type t) {
  if (p.types.size() >= UINT32_MAX)
    throw std::overflow_error("type ID overflow");
  auto id = static_cast<std::uint32_t>(p.types.size());
  p.types.push_back(std::move(t));
  return id;
}
std::uint32_t record(exec::Project &p, std::vector<std::uint32_t> fields) {
  exec::Type t;
  t.kind = TK::Record;
  t.fields = std::move(fields);
  return add_type(p, std::move(t));
}
std::uint32_t optional(exec::Project &p, std::uint32_t type) {
  exec::Type t;
  t.kind = TK::Variant;
  t.constructors = {{}, {type}};
  return add_type(p, std::move(t));
}
std::uint32_t bounded(exec::Project &p, std::uint32_t type, std::size_t n) {
  exec::Type t;
  t.kind = TK::BoundedVec;
  t.bound = n;
  t.fields = {type};
  return add_type(p, std::move(t));
}
void encode_u64(Bytes &b, std::uint64_t n) {
  for (int i = 0; i < 8; ++i)
    b.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
}
void encode_type(Bytes &b, const exec::Project &p, std::uint32_t id, std::size_t depth = 64) {
  if (!depth || id >= p.types.size())
    throw std::invalid_argument("invalid service type graph");
  auto &t = p.types[id];
  encode_u64(b, static_cast<std::uint64_t>(t.kind));
  encode_u64(b, t.bound);
  encode_u64(b, t.fields.size());
  for (auto f : t.fields)
    encode_type(b, p, f, depth - 1);
  encode_u64(b, t.constructors.size());
  for (auto &c : t.constructors) {
    encode_u64(b, c.size());
    for (auto f : c)
      encode_type(b, p, f, depth - 1);
  }
}
} // namespace
struct ObjectServices::Impl {
  ObjectRegistry registry;
  exec::Project schema;
  ObjectServiceConfig config;
  std::vector<ObjectServiceEntry> entries;
  std::map<ObjectId, ObjectDesc> objects;
  std::map<ObjectId, std::shared_ptr<BoundedQueue>> queues;
  Expected<std::vector<Value>> invoke(const ObjectServiceEntry &e, const std::vector<Value> &args,
                                      const ExecutionContext &ctx, EventTxn &t) {
    if (ctx.domain != t.context().domain || ctx.instance != t.context().instance ||
        ctx.connection != t.context().connection || ctx.owner != t.context().owner ||
        ctx.epoch != t.context().epoch || ctx.kind != t.context().kind ||
        ctx.ready != t.context().ready)
      return fail(ErrorCode::WrongOwner, "stdlib service context mismatch");
    if (!(e.signature.context_mask & (1u << static_cast<unsigned>(ctx.kind))))
      return fail(ErrorCode::InvalidState, "stdlib service context forbidden");
    if (args.size() != e.signature.input_types.size())
      return fail(ErrorCode::TypeMismatch, "stdlib service argument arity");
    for (std::size_t i = 0; i < args.size(); ++i)
      if (!exec::conforms(schema, e.signature.input_types[i], args[i]))
        return fail(ErrorCode::TypeMismatch, "stdlib service argument schema");
    if (e.method == SI::ReadBytes && std::get<std::uint64_t>(args[1].data) > config.max_bytes)
      return fail(ErrorCode::Capacity, "stdlib service read bound");
    auto desc =
        registry.lookup(objects.at(e.object).kind, MethodId{static_cast<std::uint32_t>(e.method)});
    if (!desc)
      return desc.error();
    auto cp = t.checkpoint();
    if (!cp)
      return cp.error();
    auto result = registry.prepare(e.object, desc.value().id, args, desc.value().arguments, t);
    if (!result)
      return result.error();
    try {
      std::vector<Value> out;
      if (result.value().values.size() > 1)
        out.emplace_back(A(result.value().values));
      else
        out = std::move(result.value().values);
      if (e.method == SI::Pop && !out.empty())
        out[0] = queue_result(std::move(out[0]));
      if (out.size() != e.signature.result_types.size()) {
        t.rollback(std::move(cp.value()));
        return fail(ErrorCode::TypeMismatch, "stdlib service result arity");
      }
      for (std::size_t i = 0; i < out.size(); ++i)
        if (!exec::conforms(schema, e.signature.result_types[i], out[i])) {
          t.rollback(std::move(cp.value()));
          return fail(ErrorCode::TypeMismatch, "stdlib service result schema");
        }
      return out;
    } catch (const std::exception &) {
      t.rollback(std::move(cp.value()));
      return fail(ErrorCode::TypeMismatch, "stdlib service result representation");
    }
  }
};
Expected<std::shared_ptr<ObjectServices>>
ObjectServices::create(exec::Project &project, const std::vector<StandardObjectBinding> &bindings,
                       ObjectServiceConfig config) {
  if (!config.max_bytes || !config.max_entries)
    return fail(ErrorCode::InvalidArgument, "stdlib service bounds must be positive");
  auto draft = project;
  auto impl = std::make_shared<Impl>();
  impl->config = config;
  auto registered = register_standard_objects(impl->registry, bindings);
  if (!registered)
    return registered.error();
  auto frozen = impl->registry.freeze();
  if (!frozen)
    return frozen.error();
  try {
    auto u64 = add_type(draft, {TK::Bits, 64, {}, {}}),
         bytes = add_type(draft, {TK::Bytes, config.max_bytes, {}, {}}),
         boolean = add_type(draft, {TK::Bool, 0, {}, {}});
    std::map<HandleKind, std::uint32_t> handles;
    for (auto kind :
         {HandleKind::Transaction, HandleKind::ResourceTicket, HandleKind::Event, HandleKind::Scope,
          HandleKind::Drain, HandleKind::Result, HandleKind::Consumer})
      handles[kind] = add_type(draft, {TK::Handle, static_cast<std::uint64_t>(kind), {}, {}});
    auto grant = record(draft, {u64, u64, u64, handles[HandleKind::ResourceTicket], u64});
    auto pipeline =
        record(draft, {grant, handles[HandleKind::Transaction], handles[HandleKind::Event], u64});
    auto transfer = record(draft, {u64, bytes});
    auto report = bounded(draft, u64, config.max_entries);
    auto consumer = record(draft, {handles[HandleKind::Result], handles[HandleKind::Consumer]});
    std::uint64_t next = config.first_service_id;
    for (const auto &binding : bindings) {
      if (!std::visit([](const auto &p) { return bool(p); }, binding.object))
        return fail(ErrorCode::InvalidArgument, "null standard service object");
      impl->objects.emplace(binding.desc.id, binding.desc);
      auto kind = binding.object.index();
      std::optional<std::uint32_t> element, pop;
      if (kind == 3) {
        impl->queues.emplace(binding.desc.id,
                             std::get<std::shared_ptr<BoundedQueue>>(binding.object));
        auto f = config.queue_element_types.find(binding.desc.id);
        if (f == config.queue_element_types.end() || f->second >= project.types.size())
          return fail(ErrorCode::Schema, "queue service requires exact pre-existing element type");
        element = f->second;
        auto expected_element = config.queue_element_hashes.find(binding.desc.id);
        if (expected_element != config.queue_element_hashes.end()) {
          auto actual = type_hash(project, *element);
          if (!actual)
            return actual.error();
          if (actual.value() != expected_element->second)
            return fail(ErrorCode::Integrity, "queue element schema differs from host binding");
        }
        auto protocol = config.queue_protocol_fields.find(binding.desc.id);
        if (protocol != config.queue_protocol_fields.end()) {
          const auto &type = project.types[*element];
          auto fields = protocol->second;
          if (type.kind != TK::Record || fields.transaction_field >= type.fields.size() ||
              fields.hop_field >= type.fields.size())
            return fail(ErrorCode::Schema, "protocol queue requires transaction/hop record fields");
          auto transaction = project.types.at(type.fields[fields.transaction_field]),
               hop = project.types.at(type.fields[fields.hop_field]);
          if (transaction.kind != TK::Handle ||
              transaction.bound != std::uint64_t(HandleKind::Transaction) ||
              hop.kind != TK::Handle || hop.bound != std::uint64_t(HandleKind::Hop))
            return fail(ErrorCode::Schema, "protocol queue handle field schemas");
        }
        exec::Type owner;
        owner.kind = TK::Variant;
        owner.constructors = {{handles[HandleKind::Scope]}, {handles[HandleKind::Drain]}};
        auto owner_type = add_type(draft, std::move(owner));
        pop = optional(draft, record(draft, {optional(draft, *element), optional(draft, consumer),
                                             optional(draft, owner_type), u64}));
      }
      std::size_t records = config.max_entries;
      if (kind == 2)
        records = std::get<std::shared_ptr<Resource>>(binding.object)->spec().reservation_capacity;
      if (kind == 4)
        records = std::get<std::shared_ptr<Pipeline>>(binding.object)
                      ->resource()
                      .spec()
                      .reservation_capacity;
      auto grants = bounded(draft, grant, records);
      for (std::uint32_t mid = 1; mid <= 22; ++mid) {
        auto method = impl->registry.lookup(binding.desc.kind, MethodId{mid});
        if (!method)
          continue;
        auto id = static_cast<SI>(mid);
        std::vector<std::uint32_t> in, out;
        switch (id) {
        case SI::Transfer:
          in = {u64, u64, bytes, u64, bytes};
          out = {transfer};
          break;
        case SI::ReadBytes:
          in = {u64, u64};
          out = {bytes};
          break;
        case SI::WriteBytes:
          in = {u64, bytes, bytes};
          break;
        case SI::Clear:
        case SI::Reset:
          break;
        case SI::Debug:
          in = {u64, u64, bytes};
          out = {transfer};
          break;
        case SI::ReadField:
          in = {u64};
          out = {bytes};
          break;
        case SI::UpdateField:
          in = {u64, bytes};
          break;
        case SI::Reserve:
          in = {handles[HandleKind::Transaction], u64, u64};
          out = {grant};
          break;
        case SI::ReserveDefault:
          in = {handles[HandleKind::Transaction], u64};
          out = {grant};
          break;
        case SI::Cancel:
          in = {kind == 4 ? pipeline : handles[HandleKind::ResourceTicket]};
          out = {u64};
          break;
        case SI::Complete:
          in = {handles[HandleKind::ResourceTicket], u64};
          out = {boolean};
          break;
        case SI::Inspect:
          out = {grants};
          break;
        case SI::Push:
          in = {*element};
          out = {boolean};
          break;
        case SI::Pop:
          out = {*pop};
          break;
        case SI::Size:
          out = {u64};
          break;
        case SI::Submit:
          in = {handles[HandleKind::Transaction], u64};
          out = {pipeline};
          break;
        case SI::Ready:
          in = {pipeline, handles[HandleKind::Event]};
          out = {boolean};
          break;
        case SI::PushOwned:
          in = {handles[HandleKind::Result], handles[HandleKind::Consumer], u64};
          out = {boolean};
          break;
        case SI::RemoveOwned:
          if (config.queue_protocol_fields.count(binding.desc.id))
            continue;
          in = {handles[HandleKind::Scope]};
          out = {report};
          break;
        case SI::TransferDrain:
          if (config.queue_protocol_fields.count(binding.desc.id))
            continue;
          in = {handles[HandleKind::Scope], handles[HandleKind::Drain], u64};
          out = {report};
          break;
        case SI::MarkPublished:
          continue; // Publication is driven by the runtime integration, never a VM claim.
        }
        if (next > UINT32_MAX)
          return fail(ErrorCode::Overflow, "stdlib service ID overflow");
        exec::ServiceSignature sig;
        sig.id = static_cast<std::uint32_t>(next++);
        sig.op = id == SI::Debug ? exec::Op::DebugTransfer : exec::Op::ObjectCall;
        sig.input_types = std::move(in);
        sig.result_types = std::move(out);
        sig.context_mask = id == SI::Debug ? 8 : 3;
        sig.effect_mask = exec::Object;
        auto effects = method.value().effects;
        if (effects & (ObjectRead | ObjectPeek))
          sig.effect_mask |= exec::StateRead;
        if (effects & (ObjectWrite | ObjectPoke))
          sig.effect_mask |= exec::StateWrite;
        if (effects & ObjectSchedule)
          sig.effect_mask |= exec::Transaction;
        auto fuel = checked_add(config.max_bytes, config.max_entries);
        if (!fuel)
          return fuel.error();
        fuel = checked_add(fuel.value(), 1);
        if (!fuel)
          return fuel.error();
        sig.extra_fuel = fuel.value();
        sig.provider_key = "leanat.stdlib.object." + std::to_string(binding.desc.id.value) +
                           ".method." + std::to_string(mid);
        sig.provider_version = "2";
        Bytes canonical;
        encode_u64(canonical, kind);
        encode_u64(canonical, mid);
        encode_u64(canonical, sig.context_mask);
        encode_u64(canonical, sig.effect_mask);
        encode_u64(canonical, sig.extra_fuel);
        for (auto type : sig.input_types)
          encode_type(canonical, draft, type);
        canonical.push_back(255);
        for (auto type : sig.result_types)
          encode_type(canonical, draft, type);
        sig.abi_hash = exec::sha256(canonical);
        for (const auto &old : draft.services)
          if (old.id == sig.id)
            return fail(ErrorCode::Duplicate, "stdlib service ID conflict");
        draft.services.push_back(sig);
        impl->entries.push_back({binding.desc.id, id, std::move(sig)});
      }
    }
    draft.schema_major = std::max<std::uint16_t>(draft.schema_major, 2);
    impl->schema.types = draft.types;
    project = std::move(draft);
    return std::shared_ptr<ObjectServices>(new ObjectServices(std::move(impl)));
  } catch (const std::exception &e) {
    return fail(ErrorCode::Schema, std::string("stdlib service schema: ") + e.what());
  }
}
Expected<std::array<std::uint8_t, 32>> ObjectServices::type_hash(const exec::Project &project,
                                                                 std::uint32_t type) {
  try {
    Bytes bytes;
    encode_type(bytes, project, type);
    return exec::sha256(bytes);
  } catch (const std::exception &e) {
    return fail(ErrorCode::Schema, e.what());
  }
}
Expected<std::shared_ptr<ObjectServices>>
ObjectServices::bind_existing(const exec::Project &project,
                              const std::vector<StandardObjectBinding> &bindings,
                              ObjectServiceConfig config) {
  for (const auto &binding : bindings)
    if (binding.object.index() == 3 && !config.queue_element_hashes.count(binding.desc.id))
      return fail(ErrorCode::Schema,
                  "loaded queue service requires independent host element schema hash");
  exec::Project rebuilt;
  rebuilt.types = project.types;
  auto service = create(rebuilt, bindings, config);
  if (!service)
    return service.error();
  try {
    std::vector<ObjectServiceEntry> selected;
    for (auto &e : service.value()->impl_->entries) {
      const exec::ServiceSignature *declared = nullptr;
      for (const auto &s : project.services)
        if (s.provider_key == e.signature.provider_key) {
          if (declared)
            return fail(ErrorCode::Duplicate, "ambiguous stdlib service declaration");
          declared = &s;
        }
      if (!declared)
        continue;
      const auto &expected = e.signature;
      if (declared->provider_version != expected.provider_version ||
          declared->abi_hash != expected.abi_hash || declared->op != expected.op ||
          declared->context_mask != expected.context_mask ||
          declared->effect_mask != expected.effect_mask ||
          declared->extra_fuel != expected.extra_fuel ||
          declared->input_types.size() != expected.input_types.size() ||
          declared->result_types.size() != expected.result_types.size())
        return fail(ErrorCode::Integrity, "declared stdlib service differs from concrete provider");
      Bytes actual_types, expected_types;
      for (auto id : declared->input_types)
        encode_type(actual_types, project, id);
      actual_types.push_back(255);
      for (auto id : declared->result_types)
        encode_type(actual_types, project, id);
      for (auto id : expected.input_types)
        encode_type(expected_types, rebuilt, id);
      expected_types.push_back(255);
      for (auto id : expected.result_types)
        encode_type(expected_types, rebuilt, id);
      if (actual_types != expected_types)
        return fail(ErrorCode::TypeMismatch,
                    "declared stdlib service type graph differs from provider");
      e.signature = *declared;
      selected.push_back(e);
    }
    for (const auto &s : project.services)
      if (s.provider_key.rfind("leanat.stdlib.object.", 0) == 0) {
        bool found = false;
        for (const auto &e : selected)
          if (e.signature.provider_key == s.provider_key)
            found = true;
        if (!found)
          return fail(ErrorCode::Unsupported,
                      "declared stdlib object/method has no supplied binding");
      }
    service.value()->impl_->entries = std::move(selected);
    service.value()->impl_->schema.types = project.types;
    return service;
  } catch (const std::exception &e) {
    return fail(ErrorCode::Schema, std::string("stdlib service declaration: ") + e.what());
  }
}
Expected<void> ObjectServices::register_into(CoreRuntimeBackend &backend) {
  auto state = impl_;
  std::vector<std::pair<exec::ServiceSignature, CoreRuntimeBackend::Provider>> providers;
  for (const auto &entry : impl_->entries)
    providers.emplace_back(entry.signature,
                           [state, entry](const std::vector<Value> &a, const ExecutionContext &c,
                                          EventTxn &t) { return state->invoke(entry, a, c, t); });
  return backend.register_providers(std::move(providers));
}
const std::vector<ObjectServiceEntry> &ObjectServices::entries() const {
  return impl_->entries;
}
Expected<exec::ServiceSignature> ObjectServices::find(ObjectId object, SI method) const {
  for (const auto &e : impl_->entries)
    if (e.object == object && e.method == method)
      return e.signature;
  return fail(ErrorCode::Unsupported, "standard object service not registered");
}
namespace {
struct Publication {
  Handle transaction;
  bool started{};
};
Expected<Publication> publication(const QueueEntry &e,
                                  const ObjectServiceConfig::QueueProtocolFields &fields,
                                  Runtime &runtime, const ExecutionContext &context) {
  auto value = std::get_if<A>(&e.value.data);
  if (!value || fields.transaction_field >= value->size() || fields.hop_field >= value->size())
    return fail(ErrorCode::TypeMismatch, "protocol queue record");
  auto txn = std::get_if<Handle>(&(*value)[fields.transaction_field].data),
       hop = std::get_if<Handle>(&(*value)[fields.hop_field].data);
  if (!txn || !hop || txn->kind != HandleKind::Transaction || hop->kind != HandleKind::Hop)
    return fail(ErrorCode::TypeMismatch, "protocol queue handle kinds");
  if (txn->domain != context.domain || txn->owner != context.owner)
    return fail(ErrorCode::WrongOwner, "protocol queue transaction owner");
  bool owns = false;
  for (const auto &r : runtime.drains().stop_report())
    if (r.transaction == *txn)
      for (const auto &h : r.hops)
        if (h.hop == *hop)
          owns = true;
  if (!owns)
    return fail(ErrorCode::StaleHandle, "protocol queue transaction does not own hop");
  auto wire = runtime.protocol().inspect(*hop);
  if (!wire)
    return wire.error();
  if (wire.value().identity.local_side != context.instance)
    return fail(ErrorCode::WrongOwner, "protocol queue local instance");
  return Publication{*txn, wire.value().pending || wire.value().call_ordinal != 0 ||
                               wire.value().state != WireState::Idle};
}
} // namespace
Expected<void> ObjectServices::observe_queue_publication(ObjectId id, Runtime &runtime,
                                                         EventTxn &t) {
  auto queue = impl_->queues.find(id);
  auto fields = impl_->config.queue_protocol_fields.find(id);
  if (queue == impl_->queues.end() || fields == impl_->config.queue_protocol_fields.end())
    return fail(ErrorCode::Unsupported, "queue has no protocol binding");
  auto entries = queue->second->inspect_entries(t);
  if (!entries)
    return entries.error();
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  for (const auto &e : entries.value()) {
    auto actual = publication(e, fields->second, runtime, t.context());
    if (!actual) {
      t.rollback(std::move(cp.value()));
      return actual.error();
    }
    if (actual.value().started) {
      auto mark = queue->second->mark_published(t, e.id);
      if (!mark) {
        t.rollback(std::move(cp.value()));
        return mark.error();
      }
    }
  }
  return {};
}
Expected<ObjectServices::QueueCancelReport>
ObjectServices::cancel_queue_scope(ObjectId id, Runtime &runtime, EventTxn &t, Handle scope,
                                   Handle drain, std::size_t capacity) {
  auto queue = impl_->queues.find(id);
  auto fields = impl_->config.queue_protocol_fields.find(id);
  if (queue == impl_->queues.end() || fields == impl_->config.queue_protocol_fields.end())
    return fail(ErrorCode::Unsupported, "queue has no protocol binding");
  auto entries = queue->second->inspect_entries(t);
  if (!entries)
    return entries.error();
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  std::size_t started_count = 0;
  std::vector<Handle> transactions;
  for (const auto &e : entries.value()) {
    if (!e.owner || *e.owner != scope)
      continue;
    auto actual = publication(e, fields->second, runtime, t.context());
    if (!actual) {
      t.rollback(std::move(cp.value()));
      return actual.error();
    }
    if (std::find(transactions.begin(), transactions.end(), actual.value().transaction) ==
        transactions.end())
      transactions.push_back(actual.value().transaction);
    if (actual.value().started) {
      ++started_count;
      auto mark = queue->second->mark_published(t, e.id);
      if (!mark) {
        t.rollback(std::move(cp.value()));
        return mark.error();
      }
    }
  }
  if (!transactions.empty()) {
    auto cancel = runtime.stage_cancel_local_many(t, transactions, CancelReason::ParentCancelled);
    if (!cancel) {
      t.rollback(std::move(cp.value()));
      return cancel.error();
    }
  }
  auto removed = queue->second->remove_unpublished_owned(t, scope);
  if (!removed) {
    t.rollback(std::move(cp.value()));
    return removed.error();
  }
  if (!started_count)
    return QueueCancelReport{removed.value().entry_ids, {}};
  auto transferred = queue->second->transfer_drain_owned(t, scope, drain, capacity);
  if (!transferred) {
    t.rollback(std::move(cp.value()));
    return transferred.error();
  }
  return QueueCancelReport{removed.value().entry_ids, transferred.value().entry_ids};
}
} // namespace leanat
