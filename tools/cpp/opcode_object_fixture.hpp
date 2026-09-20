#pragma once
#include "../../tests/conformance/opcode_protocol_fixture.hpp"
#include "../../tests/support/event_queue_test_access.hpp"
#include "opcode_fixture.hpp"
#include <leanat/memory.hpp>
#include <leanat/object_services.hpp>
#include <leanat/pipeline.hpp>
#include <leanat/queue.hpp>
#include <leanat/register_bank.hpp>
#include <leanat/resource.hpp>

namespace leanat::opcode_test {
namespace object_fixture_detail {
using A = Value::Array;
inline const A &array(const Value &v) {
  auto p = std::get_if<A>(&v.data);
  if (!p)
    throw std::invalid_argument("object fixture array");
  return *p;
}
inline std::uint64_t number(const Value &v) {
  auto p = std::get_if<std::uint64_t>(&v.data);
  if (!p)
    throw std::invalid_argument("object fixture u64");
  return *p;
}
inline const Bytes &bytes(const Value &v) {
  auto p = std::get_if<Bytes>(&v.data);
  if (!p)
    throw std::invalid_argument("object fixture bytes");
  return *p;
}
inline bool boolean(const Value &v) {
  auto p = std::get_if<bool>(&v.data);
  if (!p)
    throw std::invalid_argument("object fixture bool");
  return *p;
}
inline Value option(std::optional<Value> v) {
  return Value(A{Value(std::uint64_t(v ? 1 : 0)), Value(v ? A{*v} : A{})});
}
inline std::uint64_t relative_identity(std::uint64_t candidate, std::uint64_t logical,
                                       std::uint64_t actual, std::uint64_t maximum) {
  if (candidate >= logical) {
    const auto delta = candidate - logical;
    if (actual > maximum || delta > maximum - actual)
      throw std::invalid_argument("object identity relative offset overflow");
    return actual + delta;
  }
  const auto delta = logical - candidate;
  if (delta > actual)
    throw std::invalid_argument("object identity relative offset underflow");
  return actual - delta;
}
// Map the entire logical store namespace, including stale and forged capabilities.
// Exact-live replacement alone can accidentally turn an unchanged forged generation live.
inline void remap_identity(Value &value, const Handle &logical, const Handle &actual) {
  if (auto candidate = std::get_if<Handle>(&value.data)) {
    if (candidate->kind != logical.kind || candidate->store != logical.store)
      return;
    candidate->store = actual.store;
    candidate->slot = static_cast<std::uint32_t>(
        relative_identity(candidate->slot, logical.slot, actual.slot, UINT32_MAX));
    if (candidate->generation)
      candidate->generation = relative_identity(candidate->generation, logical.generation,
                                                actual.generation, UINT64_MAX);
    // Owner and domain remain exactly as supplied, including intentionally invalid authority.
  } else if (auto children = std::get_if<A>(&value.data)) {
    for (auto &child : *children)
      remap_identity(child, logical, actual);
  }
}
inline std::uint64_t configuration(const FixtureConfig &c, const char *key) {
  auto found = c.environment.find(key);
  if (found == c.environment.end())
    throw std::invalid_argument(std::string("object fixture missing ") + key);
  return number(found->second);
}
struct Owned {
  StandardObject object;
  ObjectDesc desc;
  ExecutionContext context;
  A original;
  std::shared_ptr<ObjectServices> services;
  std::size_t kind{};
  std::optional<ReservedResult> seeded_result;
  std::shared_ptr<CancelScopeStore> scopes;
  std::optional<Handle> logical_scope;
  std::optional<OwnedDrainSeed> drain_seed;
  std::optional<Handle> logical_drain;
  Expected<Value> snapshot() const {
    auto result = original;
    auto read_context = context;
    read_context.kind = ContextKind::Timed;
    read_context.instance = desc.instance;
    EventTxn read({}, read_context);
    if (kind == 0) {
      const auto &memory = *std::get<std::shared_ptr<Memory>>(object);
      result[1] = Value(Bytes(memory.data(), memory.data() + memory.size()));
      result[3] = Value(memory.version());
      result[4] = Value(false);
    } else if (kind == 1) {
      auto data = std::get<std::shared_ptr<RegisterBank>>(object)->snapshot(read);
      if (!data)
        return data.error();
      result[1] = data.value();
    } else if (kind == 2 || kind == 4) {
      auto &resource = kind == 2 ? *std::get<std::shared_ptr<Resource>>(object)
                                 : std::get<std::shared_ptr<Pipeline>>(object)->resource();
      auto state = resource.snapshot(read);
      if (!state)
        return state.error();
      const auto &s = array(state.value());
      result[6] = Value(std::uint64_t(resource.identity_store()));
      result[7] = s[0];
      result[8] = s[1];
      result[9] = s[2];
      result[10] = s[3];
      A records;
      for (const auto &v : array(s[4])) {
        auto row = array(v);
        const auto &ticket = std::get<Handle>(row[1].data);
        if (!ticket.generation)
          break; // Never allocated tail slots have no reference record.
        row[0] = Value(number(row[0]) != 0);
        records.emplace_back(std::move(row));
      }
      result[11] = Value(std::move(records));
    } else {
      auto state = std::get<std::shared_ptr<BoundedQueue>>(object)->snapshot(read);
      if (!state)
        return state.error();
      const auto &s = array(state.value());
      result[5] = s[0];
      A rows;
      for (const auto &v : array(s[1])) {
        const auto &r = array(v);
        bool owned = std::holds_alternative<A>(r[4].data);
        auto scope = std::get_if<Handle>(&r[2].data);
        rows.emplace_back(
            A{r[0], option(owned ? std::optional<Value>{} : std::optional<Value>{r[1]}),
              option(scope ? std::optional<Value>{Value(*scope)} : std::optional<Value>{}), r[3],
              option(owned ? std::optional<Value>{r[4]} : std::optional<Value>{})});
      }
      result[6] = Value(std::move(rows));
    }
    return Value(std::move(result));
  }
};
} // namespace object_fixture_detail

// Source configuration describes real initial storage, never expected results.
// Resource/Queue/Pipeline setup currently requires a fresh store. Unsupported
// non-fresh catalogs fail explicitly instead of projecting a supplied answer.
inline Expected<ProviderFixture> make_object_fixture(Runtime &runtime, CoreRuntimeBackend &backend,
                                                     const exec::Project &project,
                                                     const FixtureConfig &config) {
  using namespace object_fixture_detail;
  try {
    auto node_bytes = config.environment.find("objects.valueNodeBytes");
    if (node_bytes == config.environment.end() || number(node_bytes->second) != sizeof(Value))
      return fail(ErrorCode::Schema, "object fixture Value ABI size differs from source catalog");
    auto kind = configuration(config, "objects.kind"), id = configuration(config, "objects.id");
    if (kind > 4 || id > UINT32_MAX)
      return fail(ErrorCode::Schema, "object fixture kind/id");
    auto found = config.environment.find("objects.record");
    if (found == config.environment.end())
      return fail(ErrorCode::Schema, "object fixture record missing");
    auto owned = std::make_shared<Owned>();
    owned->kind = kind;
    owned->context = config.context;
    owned->original = array(found->second);
    auto &v = owned->original;
    if (v.empty() || number(v[0]) > UINT32_MAX)
      return fail(ErrorCode::Schema, "object fixture instance");
    owned->desc = {ObjectId{std::uint32_t(id)}, std::uint32_t(35 + kind), config.context.domain,
                   InstanceId{std::uint32_t(number(v[0]))}};
    if (kind == 0) {
      if (v.size() != 5 || bytes(v[1]).size() != bytes(v[2]).size() || boolean(v[4]))
        return fail(ErrorCode::Schema, "memory fixture backing");
      auto made = Memory::make(bytes(v[2]).size(), bytes(v[2]));
      if (!made)
        return made.error();
      auto memory = std::make_shared<Memory>(std::move(made.value()));
      if (bytes(v[1]) != bytes(v[2])) {
        EventTxn seed(
            {}, ExecutionContext{ContextKind::Timed, config.context.domain, owned->desc.instance});
        auto staged = memory->write_bytes(seed, 0, bytes(v[1]));
        if (!staged)
          return staged.error();
        auto committed = seed.commit();
        if (!committed)
          return committed.error();
      }
      if (memory->version() != number(v[3]))
        return fail(ErrorCode::Schema, "memory initial version differs from actual setup");
      owned->object = memory;
    } else if (kind == 1) {
      if (v.size() != 4 || bytes(v[1]).size() != bytes(v[2]).size())
        return fail(ErrorCode::Schema, "register fixture backing");
      std::vector<RegisterSpec> specs;
      std::size_t base = 0;
      for (const auto &value : array(v[3])) {
        const auto &r = array(value);
        if (r.size() != 6)
          return fail(ErrorCode::Schema, "register fixture spec");
        RegisterSpec spec;
        spec.offset = number(r[0]);
        spec.width_bits = number(r[1]);
        if (!spec.width_bits || spec.width_bits > 4096 || spec.width_bits % 8 ||
            base + spec.width_bits / 8 > bytes(v[2]).size())
          return fail(ErrorCode::Schema, "register fixture width");
        for (const auto &n : array(r[2]))
          spec.allowed_access_bytes.push_back(number(n));
        if (number(r[3]))
          spec.alignment_bytes = number(r[3]);
        spec.endian = boolean(r[4]) ? RegisterEndian::Big : RegisterEndian::Little;
        for (const auto &field : array(r[5])) {
          const auto &f = array(field);
          if (f.size() != 4 || number(f[0]) > UINT32_MAX || number(f[3]) > 5)
            return fail(ErrorCode::Schema, "register fixture field");
          FieldSpec fs;
          fs.id = FieldId{std::uint32_t(number(f[0]))};
          fs.lsb = number(f[1]);
          fs.width = number(f[2]);
          fs.access = RegisterAccess(number(f[3]));
          if (!fs.width || fs.lsb + fs.width > spec.width_bits)
            return fail(ErrorCode::Schema, "register fixture field bounds");
          fs.reset.resize((fs.width + 7) / 8);
          for (std::size_t bit = 0; bit < fs.width; ++bit) {
            auto logical = fs.lsb + bit;
            auto physical =
                base + (spec.endian == RegisterEndian::Big ? spec.width_bits / 8 - 1 - logical / 8
                                                           : logical / 8);
            if ((bytes(v[2])[physical] >> (logical % 8)) & 1)
              fs.reset[bit / 8] |= std::uint8_t(1U << (bit % 8));
          }
          spec.fields.push_back(std::move(fs));
        }
        base += spec.width_bits / 8;
        specs.push_back(std::move(spec));
      }
      if (base != bytes(v[2]).size())
        return fail(ErrorCode::Schema, "register fixture packed length");
      auto made = RegisterBank::make(specs);
      if (!made)
        return made.error();
      auto bank = std::make_shared<RegisterBank>(std::move(made.value()));
      auto c = config.context;
      c.kind = ContextKind::Debug;
      c.instance = owned->desc.instance;
      EventTxn seed({}, c);
      base = 0;
      for (const auto &spec : specs) {
        auto count = spec.width_bits / 8;
        Bytes part(bytes(v[1]).begin() + base, bytes(v[1]).begin() + base + count);
        auto seeded = bank->peek_poke(seed, Command::Write, spec.offset, part);
        if (!seeded)
          return seeded.error();
        base += count;
      }
      auto committed = seed.commit();
      if (!committed)
        return committed.error();
      owned->object = bank;
    } else if (kind == 2 || kind == 4) {
      if (v.size() != 12 || number(v[1]) > 2 || !number(v[4]) || !number(v[5]) ||
          number(v[6]) > UINT32_MAX || number(v[7]) || number(v[8]) || number(v[9]) ||
          !array(v[11]).empty())
        return fail(ErrorCode::Schema, "resource fixture requires fresh state");
      if (array(v[10]).size() != number(v[4]) ||
          std::any_of(array(v[10]).begin(), array(v[10]).end(),
                      [](const Value &x) { return number(x) != 0; }))
        return fail(ErrorCode::Schema, "resource initial channels");
      if (kind == 2) {
        auto made = Resource::make({ResourceKind(number(v[1])), std::size_t(number(v[4])),
                                    Duration{number(v[2])}, Duration{number(v[3])},
                                    std::size_t(number(v[5]))},
                                   config.context.domain);
        if (!made)
          return made.error();
        owned->object = std::make_shared<Resource>(std::move(made.value()));
        v[6] = Value(
            std::uint64_t(std::get<std::shared_ptr<Resource>>(owned->object)->identity_store()));
      } else {
        if (number(v[1]) != 2)
          return fail(ErrorCode::Schema, "pipeline kind");
        auto made = Pipeline::make(Duration{number(v[2])}, Duration{number(v[3])}, number(v[4]),
                                   runtime.queue(), number(v[5]), config.context.domain);
        if (!made)
          return made.error();
        owned->object = std::make_shared<Pipeline>(std::move(made.value()));
        v[6] = Value(std::uint64_t(
            std::get<std::shared_ptr<Pipeline>>(owned->object)->resource().identity_store()));
      }
    } else {
      if (v.size() != 7 || !number(v[1]) || !number(v[2]) || !number(v[3]) ||
          number(v[5]) != array(v[6]).size() + 1)
        return fail(ErrorCode::Schema, "queue fixture sequence requires contiguous initial rows");
      owned->object = std::make_shared<BoundedQueue>(number(v[1]), number(v[2]), number(v[3]),
                                                     config.context.domain);
      auto queue = std::get<std::shared_ptr<BoundedQueue>>(owned->object);
      queue->set_consumer_store(runtime.results(), number(v[4]));
      auto scope_seed = config.environment.find("objects.scopeSeed");
      if (scope_seed != config.environment.end()) {
        owned->logical_scope = std::get<Handle>(scope_seed->second.data);
        if (owned->logical_scope->kind != HandleKind::Scope || owned->logical_scope->owner != 0 ||
            owned->logical_scope->domain != config.context.domain)
          return fail(ErrorCode::Schema, "queue root scope seed");
        owned->scopes = std::make_shared<CancelScopeStore>(config.context.domain, 130, 16, 64, 256);
        queue->set_scope_store(*owned->scopes);
      }
      auto drain_seed = config.environment.find("objects.drainSeed");
      if (drain_seed != config.environment.end()) {
        const auto &seed = array(drain_seed->second);
        if (seed.size() != 5 || number(seed[0]) != config.context.connection.value)
          return fail(ErrorCode::Schema, "queue drain seed codec/connection");
        PayloadSnapshot payload;
        payload.command = Command::Read;
        payload.data = bytes(seed[2]);
        payload.streaming_width = std::max<std::size_t>(1, payload.data.size());
        auto made = seed_owned_protocol_drain(runtime, config.context, config.context.connection,
                                              TransportId{number(seed[1])}, payload);
        if (!made)
          return made.error();
        owned->drain_seed = std::move(made.value());
        owned->logical_drain = std::get<Handle>(seed[4].data);
        queue->set_owner_validator(
            [&runtime, scopes = owned->scopes](Handle owner) -> Expected<void> {
              if (owner.kind == HandleKind::Scope) {
                if (!scopes)
                  return fail(ErrorCode::Unsupported, "queue scope store missing");
                return scopes->validate_owner(owner, false);
              }
              auto status = runtime.drains().inspect(owner);
              if (!status)
                return status.error();
              return {};
            });
      }
      auto setup_context = config.context;
      setup_context.kind = ContextKind::Timed;
      EventTxn setup({}, setup_context);
      A mapped_rows;
      std::uint64_t next = 1;
      for (const auto &value : array(v[6])) {
        auto row = array(value);
        if (row.size() != 5 || number(row[0]) != next || number(array(row[1]).at(0)) != 1 ||
            number(array(row[4]).at(0)) != 0)
          return fail(ErrorCode::Schema, "queue seed requires ValueOnly contiguous rows");
        const auto &element = array(array(row[1]).at(1));
        if (element.size() != 1)
          return fail(ErrorCode::Schema, "queue seed element option");
        std::optional<Handle> owner;
        if (number(array(row[2]).at(0)) == 1) {
          auto logical = std::get<Handle>(array(array(row[2]).at(1)).at(0).data);
          if (!owned->scopes || logical != *owned->logical_scope)
            return fail(ErrorCode::Schema, "queue seed scope identity");
          owner = owned->scopes->root();
          row[2] = option(Value(*owner));
        }
        auto pushed = queue->try_push(setup, element[0], owner);
        if (!pushed)
          return pushed.error();
        if (!pushed.value())
          return fail(ErrorCode::Capacity, "queue seed push rejected");
        if (boolean(row[3])) {
          auto marked = queue->mark_published(setup, next);
          if (!marked)
            return marked.error();
        }
        mapped_rows.emplace_back(std::move(row));
        ++next;
      }
      auto committed = setup.commit();
      if (!committed)
        return committed.error();
      v[6] = Value(std::move(mapped_rows));
    }
    auto initialized = owned->snapshot();
    if (!initialized || initialized.value() != Value(owned->original))
      return fail(ErrorCode::Schema,
                  "object fixture initial state cannot be represented by native provider");
    ObjectServiceConfig settings;
    settings.max_bytes = configuration(config, "objects.maxBytes");
    settings.max_entries = configuration(config, "objects.maxEntries");
    if (kind == 3) {
      auto element = configuration(config, "objects.elementType");
      if (element >= project.types.size())
        return fail(ErrorCode::Schema, "queue fixture element type");
      settings.queue_element_types[owned->desc.id] = std::uint32_t(element);
      // This fixture's source catalog declares UInt64 ValueOnly elements. Build the host
      // contract independently, so a loaded descriptor cannot choose its own expected hash.
      exec::Project host_element;
      host_element.types = {{exec::TypeKind::Bits, 64, {}, {}}};
      auto element_hash = ObjectServices::type_hash(host_element, 0);
      if (!element_hash)
        return element_hash.error();
      settings.queue_element_hashes[owned->desc.id] = element_hash.value();
    }
    auto services =
        ObjectServices::bind_existing(project, {{owned->desc, owned->object}}, settings);
    if (!services)
      return services.error();
    owned->services = services.value();
    auto registered = services.value()->register_into(backend);
    if (!registered)
      return registered.error();
    ProviderFixture fixture;
    fixture.inputs = config.inputs;
    if (owned->drain_seed)
      for (auto &input : fixture.inputs)
        remap_identity(input, *owned->logical_drain, owned->drain_seed->receipt);
    auto pipeline_seed = config.environment.find("objects.pipelineSeed");
    if (pipeline_seed != config.environment.end()) {
      const auto &seed = array(pipeline_seed->second);
      if (kind != 4 || seed.size() != 5)
        return fail(ErrorCode::Schema, "pipeline seed shape");
      const auto source = std::get<Handle>(seed[0].data);
      const auto &logical = array(seed[4]);
      if (logical.size() != 4 || array(logical[0]).size() != 5)
        return fail(ErrorCode::Schema, "pipeline logical ticket shape");
      const auto logical_event = std::get<Handle>(logical[2].data);
      auto setup_context = config.context;
      setup_context.kind = ContextKind::Timed;
      setup_context.ready = {Tick{number(seed[2])}, number(seed[3])};
      EventTxn setup({}, setup_context);
      auto pending = std::get<std::shared_ptr<Pipeline>>(owned->object)
                         ->submit(setup, source, Tick{number(seed[1])});
      if (!pending)
        return pending.error();
      auto committed = setup.commit();
      if (!committed)
        return committed.error();
      auto batch = runtime.queue().pop_batch(pending.value().grant.finish, 1);
      if (!batch || !batch.value())
        return fail(ErrorCode::InvalidState, "pipeline seed did not dispatch");
      auto active = runtime.queue().active_event(pending.value().ready_event);
      if (!active || active.value().key.time != config.context.ready.time ||
          active.value().key.turn != config.context.ready.turn)
        return fail(ErrorCode::Schema, "pipeline seed context is not active event");
      const auto &p = pending.value();
      Value native_ticket(A{Value(A{Value(p.grant.start.value), Value(p.grant.finish.value),
                                    Value(p.grant.wait.value), Value(p.grant.ticket),
                                    Value(std::uint64_t(p.grant.channel))}),
                            Value(p.owner), Value(p.ready_event), Value(p.epoch)});
      const auto &logical_grant = array(logical[0]);
      const auto &native_grant = array(array(native_ticket)[0]);
      for (auto index : {0u, 1u, 2u, 4u})
        if (logical_grant[index] != native_grant[index])
          return fail(ErrorCode::Schema,
                      "pipeline seed grant data differs from native initialization");
      if (logical[1] != Value(p.owner) || logical[3] != Value(p.epoch))
        return fail(ErrorCode::Schema,
                    "pipeline seed owner or epoch differs from native initialization");
      const auto logical_resource = std::get<Handle>(logical_grant[3].data);
      for (auto &input : fixture.inputs) {
        remap_identity(input, logical_resource, p.grant.ticket);
        remap_identity(input, logical_event, p.ready_event);
      }
    }
    if (owned->logical_scope) {
      for (auto &input : fixture.inputs)
        remap_identity(input, *owned->logical_scope, owned->scopes->root());
    }
    auto seed = config.environment.find("objects.resultSeed");
    if (seed != config.environment.end()) {
      const auto &fields = array(seed->second);
      if (kind != 3 || fields.size() != 5 || number(fields[1]) >= project.types.size())
        return fail(ErrorCode::Schema, "queue result seed schema");
      auto source = std::get<Handle>(fields[0].data);
      auto logical_result = std::get<Handle>(fields[3].data);
      auto logical_consumer = std::get<Handle>(fields[4].data);
      if (source.domain != config.context.domain || source.owner != config.context.owner ||
          logical_result.kind != HandleKind::Result ||
          logical_consumer.kind != HandleKind::Consumer)
        return fail(ErrorCode::Schema, "queue result seed identity");
      auto reserved = runtime.results().reserve({source, TypeId{std::uint32_t(number(fields[1]))},
                                                 std::size_t(number(fields[2])),
                                                 config.context.owner, config.context.owner});
      if (!reserved)
        return reserved.error();
      owned->seeded_result = reserved.value();
      for (auto &input : fixture.inputs) {
        remap_identity(input, logical_result, reserved.value().consumer.result);
        remap_identity(input, logical_consumer, reserved.value().consumer.consumer);
      }
    }
    fixture.lifetime.push_back(owned);
    fixture.snapshot = [owned, &runtime]() {
      auto actual = owned->snapshot();
      if (!actual)
        return Json::object({{"error", observe(actual.error())}});
      static const char *tags[] = {"reference.object.memory", "reference.object.register",
                                   "reference.object.resource", "reference.object.queue",
                                   "reference.object.pipeline"};
      auto object = Json::object({{"objectId", observe(owned->desc.id)},
                                  {"domain", observe(owned->desc.domain)},
                                  {"instance", observe(owned->desc.instance)},
                                  {"providerKind", owned->kind},
                                  {"tag", tags[owned->kind]},
                                  {"valueSchema", tags[owned->kind]},
                                  {"value", observe(actual.value())},
                                  {"alive", true}});
      auto queue = testing::EventQueueTestAccess::snapshot(runtime.queue());
      std::vector<Json> slots;
      for (const auto &slot : queue.slots)
        slots.push_back(Json::object({{"state", slot.state}, {"event", observe(slot.queued)}}));
      auto results = runtime.results().snapshot();
      std::vector<Json> scope_rows;
      Json scope_metadata;
      if (owned->scopes) {
        auto scopes = owned->scopes->snapshot();
        if (!scopes)
          return Json::object({{"error", observe(scopes.error())}});
        std::vector<Json> observers;
        for (const auto &o : scopes.value().observers)
          observers.push_back(Json::object({{"id", o.id},
                                            {"source", observe(o.source)},
                                            {"process", observe(o.process)},
                                            {"wait", observe(o.wait)}}));
        scope_metadata =
            Json::object({{"sequence", scopes.value().sequence},
                          {"nextAllocationGeneration", owned->scopes->next_allocation_generation()},
                          {"capacity", owned->scopes->control_capacity()},
                          {"observers", Json::array(observers)}});
        for (const auto &s : scopes.value().scopes) {
          std::vector<Json> owned_rows;
          for (const auto &o : s.owned)
            owned_rows.push_back(Json::object({{"identity", observe(o.handle)},
                                               {"kind", observe(o.kind)},
                                               {"published", o.published}}));
          Json plan;
          if (s.plan) {
            std::vector<Json> actions;
            for (const auto &a : s.plan->actions)
              actions.push_back(Json::object({{"id", a.id},
                                              {"scope", observe(a.scope)},
                                              {"identity", observe(a.owned.handle)},
                                              {"ownedKind", observe(a.owned.kind)},
                                              {"published", a.owned.published},
                                              {"kind", observe(a.kind)},
                                              {"applied", a.applied}}));
            plan = Json::object({{"scope", observe(s.plan->scope)},
                                 {"reason", s.plan->reason},
                                 {"actions", Json::array(actions)}});
          }
          scope_rows.push_back(Json::object({{"identity", observe(s.identity)},
                                             {"parent", observe(s.parent)},
                                             {"state", observe(s.state)},
                                             {"owned", Json::array(owned_rows)},
                                             {"plan", plan},
                                             {"hasPlan", bool(s.plan)}}));
        }
      }
      std::vector<Json> result_slots, consumer_slots;
      for (const auto &s : results.result_slots)
        result_slots.push_back(Json::object({{"identity", observe(s.identity)},
                                             {"alive", s.alive},
                                             {"producerAlive", s.producer_alive},
                                             {"publishing", s.publishing},
                                             {"value", observe(s.value)},
                                             {"ready", observe(s.ready)},
                                             {"consumerCount", s.consumer_count},
                                             {"pinCount", s.pin_count},
                                             {"source", observe(s.create.source)},
                                             {"type", observe(s.create.type)},
                                             {"maxBytes", s.create.max_bytes},
                                             {"producerOwner", s.create.producer_owner},
                                             {"consumerOwner", s.create.consumer_owner}}));
      for (const auto &s : results.consumer_slots)
        consumer_slots.push_back(Json::object({{"identity", observe(s.identity)},
                                               {"result", observe(s.result)},
                                               {"active", s.active}}));
      Json drain_receipt;
      if (owned->drain_seed) {
        auto status = runtime.drains().inspect(owned->drain_seed->receipt);
        if (!status)
          return Json::object({{"error", observe(status.error())}});
        std::vector<Json> hops;
        for (const auto &hop : status.value().hops)
          hops.push_back(Json::object({{"hop", observe(hop.hop)},
                                       {"wireTerminal", hop.wire_terminal},
                                       {"timingConsumed", hop.timing_consumed},
                                       {"cleanupReturned", hop.cleanup_returned},
                                       {"callPin", hop.call_pin}}));
        drain_receipt = Json::object({{"identity", observe(owned->drain_seed->receipt)},
                                      {"transaction", observe(owned->drain_seed->transaction)},
                                      {"state", static_cast<std::uint64_t>(status.value().state)},
                                      {"reason", static_cast<std::uint64_t>(status.value().reason)},
                                      {"hops", Json::array(hops)}});
      }
      return Json::object(
          {{"objects", Json::array({object})},
           {"protocol", owned->drain_seed ? owned->drain_seed->state->snapshot() : Json()},
           {"protocolIdentities",
            owned->drain_seed ? owned->drain_seed->state->identities() : Json()},
           {"drainReceipt", drain_receipt},
           {"scopes", Json::array(scope_rows)},
           {"scopeMetadata", scope_metadata},
           {"results", Json::array(result_slots)},
           {"consumers", Json::array(consumer_slots)},
           {"nextResultGeneration", results.next_result_generation},
           {"nextConsumerGeneration", results.next_consumer_generation},
           {"resultPins", results.pin_count},
           {"resultPinLimit", results.pin_limit},
           {"events", Json::array(slots)},
           {"eventOccupied", runtime.queue().occupied()},
           {"frontier", observe(queue.frontier)},
           {"batch", observe(queue.batch)},
           {"batchReady", observe(queue.batch_ready)},
           {"members", observe(queue.members)},
           {"cursor", queue.cursor},
           {"resolverCursor", queue.resolver_cursor},
           {"reclaimCursor", queue.reclaim_cursor},
           {"resolverTotal", observe(queue.resolver_total)},
           {"nextBatch", queue.next_batch},
           {"nextSequence", queue.next_sequence},
           {"maxEventBytes", queue.max_event_bytes}});
    };
    fixture.identities = [owned]() {
      auto actual = owned->snapshot();
      std::vector<Json> tickets;
      if (actual && (owned->kind == 2 || owned->kind == 4))
        for (const auto &r : array(array(actual.value())[11]))
          tickets.push_back(observe(array(r)[1]));
      return Json::object(
          {{"objectId", observe(owned->desc.id)},
           {"tickets", Json::array(tickets)},
           {"resourceStore", actual && (owned->kind == 2 || owned->kind == 4)
                                 ? observe(array(actual.value())[6])
                                 : Json()},
           {"result",
            owned->seeded_result ? observe(owned->seeded_result->consumer.result) : Json()},
           {"consumer",
            owned->seeded_result ? observe(owned->seeded_result->consumer.consumer) : Json()},
           {"scope", owned->scopes ? observe(owned->scopes->root()) : Json()}});
    };
    return fixture;
  } catch (const std::exception &error) {
    return fail(ErrorCode::Schema, std::string("object fixture: ") + error.what());
  }
}
} // namespace leanat::opcode_test
