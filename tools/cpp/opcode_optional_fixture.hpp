#pragma once
#include "opcode_fixture.hpp"
#include <leanat/optional_services.hpp>
#include <leanat/raw_dmi_services.hpp>
#ifdef LEANAT_OPCODE_SYSTEMC
#include <leanat/systemc/raw_dmi_services.hpp>
#endif
namespace leanat::opcode_test {
namespace optional_fixture_detail {
inline Expected<std::uint64_t> number(const Value &v) {
  if (auto n = std::get_if<std::uint64_t>(&v.data))
    return *n;
  return fail(ErrorCode::TypeMismatch, "optional setup number");
}
inline Expected<const Value::Array *> record(const FixtureConfig &c, const std::string &key,
                                             std::size_t size) {
  auto f = c.environment.find(key);
  if (f == c.environment.end())
    return fail(ErrorCode::NotReady, "optional setup missing " + key);
  auto v = std::get_if<Value::Array>(&f->second.data);
  if (!v || v->size() != size)
    return fail(ErrorCode::TypeMismatch, "optional setup shape " + key);
  return v;
}
inline std::string text(const Value &v) {
  auto &b = std::get<Bytes>(v.data);
  return std::string(b.begin(), b.end());
}
struct MappedRaw {
  std::shared_ptr<Bytes> bytes;
  std::uint64_t base{}, end{};
  std::uint8_t *pointer{};
  std::vector<std::pair<std::uint64_t, std::uint64_t>> grants, invalidated;
  Expected<void> grant(std::uint64_t address, Command command) {
    if (address < base || address > end || (command != Command::Read && command != Command::Write))
      return fail(ErrorCode::InvalidArgument, "mapped raw request");
    pointer = bytes->data();
    grants.emplace_back(base, end);
    return {};
  }
  Expected<void> invalidate(std::uint64_t start, std::uint64_t stop) {
    for (auto g : grants)
      if (g.first <= stop && start <= g.second)
        invalidated.push_back(g);
    grants.erase(std::remove_if(grants.begin(), grants.end(),
                                [&](auto g) { return g.first <= stop && start <= g.second; }),
                 grants.end());
    if (grants.empty())
      pointer = nullptr;
    return {};
  }
};
inline std::int32_t native_add1(const std::uint8_t *in, std::uint32_t count, std::uint8_t *out,
                                std::uint32_t cap, std::uint32_t *written) {
  if (count != 8 || cap < 8)
    return 1;
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i)
    v |= std::uint64_t(in[i]) << (8 * i);
  if (v == UINT64_MAX)
    return 2;
  ++v;
  for (unsigned i = 0; i < 8; ++i)
    out[i] = std::uint8_t(v >> (8 * i));
  *written = 8;
  return 0;
}
} // namespace optional_fixture_detail
inline Expected<ProviderFixture> make_optional_fixture(Runtime &runtime,
                                                       CoreRuntimeBackend &backend,
                                                       const exec::Project &p,
                                                       const FixtureConfig &config) {
  using namespace optional_fixture_detail;
  ProviderFixture fixture;
  fixture.inputs = config.inputs;
  bool managed = false, raw = false, external = false;
  for (auto &s : p.services) {
    managed |= s.provider_key.rfind("leanat.managed.", 0) == 0;
    raw |= s.provider_key.rfind("leanat.raw-dmi.", 0) == 0;
    external |= s.provider_key.rfind("leanat.external.", 0) == 0;
  }
  if (unsigned(managed) + unsigned(raw) + unsigned(external) != 1)
    return fail(ErrorCode::Unsupported, "optional fixture requires one family");
  try {
    if (raw) {
      auto source = record(config, "raw.region", 8);
      if (!source)
        return source.error();
      auto &v = *source.value();
      auto backing = std::make_shared<Bytes>(std::get<Bytes>(v[7].data));
      std::uint64_t n[7];
      for (unsigned i = 0; i < 7; ++i) {
        auto x = number(v[i]);
        if (!x)
          return x.error();
        n[i] = x.value();
      }
      if (n[2] < n[1] || n[2] - n[1] + 1 != backing->size() || n[3] != 3 || n[6] != 1)
        return fail(ErrorCode::Schema, "raw mapping setup range");
      auto service = std::make_shared<RawDmiServices>(true);
      RawDmiRegion binding;
#ifdef LEANAT_OPCODE_SYSTEMC
      auto host = std::make_shared<leanat::systemc::RawDmiServiceHost>(
          backing, n[1], leanat::systemc::TimeCodec{}, Duration{n[4]}, Duration{n[5]});
      binding = host->binding(n[0], config.context.domain, config.context.owner);
      fixture.lifetime.push_back(host);
#else
      auto host = std::make_shared<MappedRaw>();
      host->bytes = backing;
      host->base = n[1];
      host->end = n[2];
      binding.id = n[0];
      binding.start = n[1];
      binding.end = n[2];
      binding.read_latency = n[4];
      binding.write_latency = n[5];
      binding.generation = n[6];
      binding.owner = config.context.owner;
      binding.domain = config.context.domain;
      binding.grant = [host](auto address, auto command) { return host->grant(address, command); };
      binding.invalidate = [host](auto a, auto b) { return host->invalidate(a, b); };
      fixture.lifetime.push_back(host);
#endif
      if (auto entry = config.environment.find("raw.capacity"); entry != config.environment.end()) {
        auto capacity = number(entry->second);
        if (!capacity)
          return capacity.error();
        binding.capacity = capacity.value();
      }
      auto added = service->add_region(binding);
      if (!added)
        return added.error();
      if (auto seed = config.environment.find("raw.seedGrant");
          seed != config.environment.end() && std::get<bool>(seed->second.data)) {
        auto staged_project = p;
        std::uint32_t u64_type = UINT32_MAX, grant_type = UINT32_MAX;
        for (std::uint32_t i = 0; i < p.types.size(); ++i) {
          if (p.types[i].kind == exec::TypeKind::Bits && p.types[i].bound == 64)
            u64_type = i;
          if (p.types[i].kind == exec::TypeKind::Record && p.types[i].fields.size() == 8)
            grant_type = i;
        }
        auto seed_signature = raw_dmi_signature(p, UINT32_MAX, exec::Op::GrantRawDmi,
                                                {u64_type, u64_type, u64_type}, grant_type);
        if (!seed_signature)
          return seed_signature.error();
        staged_project.services.push_back(seed_signature.value());
        CoreRuntimeBackend seed_backend(runtime, staged_project);
        auto binding_result = service->register_into(seed_backend, staged_project);
        if (!binding_result)
          return binding_result.error();
        auto freeze = seed_backend.freeze();
        if (!freeze)
          return freeze.error();
        auto context = config.context;
        context.kind = ContextKind::Dmi;
        EventTxn seed_txn({}, context);
        auto seeded = seed_backend.invoke(seed_signature.value(),
                                          {Value(n[0]), Value(n[1]), Value(std::uint64_t(0))},
                                          context, seed_txn);
        if (!seeded)
          return seeded.error();
        auto commit = seed_txn.commit();
        if (!commit)
          return commit.error();
        auto publication = service->publish();
        if (!publication)
          return publication.error();
      }
      auto bound = service->register_into(backend, p);
      if (!bound)
        return bound.error();
      fixture.lifetime.push_back(service);
      fixture.after_commit = [service]() { return service->publish(); };
      fixture.snapshot = [service, backing, binding]() {
        std::vector<Json> observations;
        for (auto &o : service->observations())
          observations.push_back(
              Json::object({{"kind", o.invalidation ? Json("raw.invalidate") : Json("raw.grant")},
                            {"region", Json(o.grant.region)},
                            {"start", Json(o.grant.start)},
                            {"end", Json(o.grant.end)},
                            {"permission", Json(o.grant.permission)},
                            {"readLatency", Json(o.grant.read_latency)},
                            {"writeLatency", Json(o.grant.write_latency)},
                            {"generation", Json(o.grant.generation)}}));
        std::vector<Json> grants;
        for (const auto &g : service->grants())
          grants.push_back(Json::array({Json(g.granted), Json(g.region), Json(g.start), Json(g.end),
                                        Json(g.permission), Json(g.read_latency),
                                        Json(g.write_latency), Json(g.generation)}));
        return Json::object(
            {{"backing", observe(Value(*backing))},
             {"region", Json::array({Json(binding.id), Json(binding.start), Json(binding.end),
                                     Json(std::uint64_t(3)), Json(binding.read_latency),
                                     Json(binding.write_latency), Json(binding.generation)})},
             {"grants", Json::array(grants)},
             {"rawCapacity", Json(binding.capacity)},
             {"observations", Json::array(observations)}});
      };
      fixture.identities = []() { return Json::object({}); };
      return fixture;
    }
    if (managed) {
      auto source = record(config, "managed.region", 9);
      if (!source)
        return source.error();
      auto &v = *source.value();
      std::uint64_t n[7];
      for (unsigned i = 0; i < 7; ++i) {
        auto x = number(v[i]);
        if (!x)
          return x.error();
        n[i] = x.value();
      }
      auto backing = std::make_shared<Bytes>(std::get<Bytes>(v[8].data));
      if (backing->empty() || n[0] > UINT32_MAX || n[2] < 1 || n[2] > 3)
        return fail(ErrorCode::Schema, "managed region setup");
      ManagedLimits limits;
      if (auto entry = config.environment.find("managed.limits");
          entry != config.environment.end()) {
        auto array = std::get_if<Value::Array>(&entry->second.data);
        if (!array || array->size() != 3)
          return fail(ErrorCode::Schema, "managed limits setup");
        auto leases = number((*array)[0]), operations = number((*array)[1]),
             bytes = number((*array)[2]);
        if (!leases || !operations || !bytes)
          return fail(ErrorCode::TypeMismatch, "managed limits fields");
        limits.leases = leases.value();
        limits.operations = operations.value();
        limits.input_bytes = bytes.value();
      }
      if (auto entry = config.environment.find("managed.scheduledChanges");
          entry != config.environment.end()) {
        auto capacity = number(entry->second);
        if (!capacity)
          return capacity.error();
        limits.scheduled_changes = capacity.value();
      }
      auto manager = std::make_shared<ManagedAccessManager>(
          runtime.results(), config.context.domain, true, limits, 401);
      ManagedRegionDesc region;
      region.id = RegionId{std::uint32_t(n[0])};
      region.start = n[1];
      auto end = checked_add(n[1], backing->size() - 1);
      if (!end)
        return end.error();
      region.end = end.value();
      region.permission = static_cast<ManagedPermission>(n[2]);
      region.version = n[3];
      region.backing = backing;
      region.allowed_connections = {config.context.connection};
      region.invalidation = std::get<bool>(v[7].data) ? InFlightPolicy::CancelBeforeCommit
                                                      : InFlightPolicy::AllowToComplete;
      region.service = [latency = n[4]](Command, std::size_t) -> Expected<Duration> {
        return Duration{latency};
      };
      auto added = manager->add_region(region);
      if (!added)
        return added.error();
      if (n[5] != 0)
        return fail(ErrorCode::Unsupported, "nonzero initial managed resource frontier");
      std::optional<Handle> lease;
      std::optional<Handle> logical_lease;
      if (auto seed = config.environment.find("managed.seedLease");
          seed != config.environment.end()) {
        auto h = std::get_if<Handle>(&seed->second.data);
        if (!h || h->kind != HandleKind::Lease || h->domain != config.context.domain ||
            h->owner != config.context.owner || !h->generation) {
          return fail(ErrorCode::InvalidArgument, "managed seed lease authority");
        }
        logical_lease = *h;
        ManagedRequest request;
        request.region = region.id;
        request.connection = config.context.connection;
        request.start = region.start;
        request.end = region.end;
        request.permission = region.permission;
        request.domain = config.context.domain;
        request.owner = config.context.owner;
        auto prepared = manager->request(request);
        if (!prepared) {
          return prepared.error();
        }
        lease = prepared.value();
      }
      for (auto &input : fixture.inputs) {
        if (auto h = std::get_if<Handle>(&input.data);
            h && lease && logical_lease && h->kind == logical_lease->kind &&
            h->store == logical_lease->store && h->slot == logical_lease->slot) {
          // Translate the pool role while preserving owner/domain and the exact
          // relative generation displacement, including forged/stale inputs.
          h->generation = lease->generation + (h->generation - logical_lease->generation);
          h->store = lease->store;
          h->slot = lease->slot;
        }
      }
      for (auto &s : p.services) {
        auto bound = register_managed_service(backend, p, s, *manager, runtime.results(),
                                              {true, config.context.owner});
        if (!bound)
          return bound.error();
      }
      fixture.lifetime.push_back(manager);
      auto until = config.environment.find("managed.advanceTo");
      if (until != config.environment.end()) {
        auto time = number(until->second);
        if (!time)
          return time.error();
        fixture.after_commit = [manager, time = time.value()]() {
          return manager->advance(Tick{time});
        };
      }
      fixture.snapshot = [manager, backing, &runtime, region_id = n[0], latency = n[4],
                          domain = config.context.domain.value]() {
        auto next = manager->next_ready();
        auto state = manager->snapshot();
        auto stored = runtime.results().snapshot();
        std::vector<Json> allocation, result_slots, consumer_slots;
        auto counter = [&](const std::string &group, Json slot, std::uint64_t last) {
          auto next_generation =
              last == UINT64_MAX ? std::string("18446744073709551616") : std::to_string(last + 1);
          allocation.push_back(Json::object({{"group", Json(group)},
                                             {"domain", Json(domain)},
                                             {"slot", slot},
                                             {"nextGeneration", Json(next_generation)},
                                             {"persistent", Json(true)},
                                             {"retired", Json(false)}}));
        };
        for (std::size_t slot = 0; slot < state.lease_generations.size(); ++slot)
          if (state.lease_generations[slot])
            counter("managed.lease", Json(slot), state.lease_generations[slot]);
        for (std::size_t slot = 0; slot < state.access_generations.size(); ++slot)
          if (state.access_generations[slot])
            counter("managed.access", Json(slot), state.access_generations[slot]);
        counter("storage.result", Json{}, stored.next_result_generation);
        counter("storage.consumer", Json{}, stored.next_consumer_generation);
        for (const auto &slot : stored.result_slots)
          result_slots.push_back(
              Json::object({{"identity", observe(slot.identity)}, {"alive", Json(slot.alive)}}));
        for (const auto &slot : stored.consumer_slots)
          consumer_slots.push_back(Json::object({{"identity", observe(slot.identity)},
                                                 {"active", Json(slot.active)},
                                                 {"result", observe(slot.result)}}));
        auto policy = [](std::size_t capacity, bool per_slot) {
          return Json::object({{"capacity", Json(capacity)},
                               {"perSlot", Json(per_slot)},
                               {"persistent", Json(true)},
                               {"allowMax", Json(true)}});
        };
        std::vector<Json> results, consumers;
        for (const auto &r : stored.result_slots)
          if (r.alive)
            results.push_back(Json::object({{"identity", observe(r.identity)},
                                            {"source", observe(r.create.source)},
                                            {"type", Json(r.create.type.value)},
                                            {"maxBytes", Json(r.create.max_bytes)},
                                            {"producerAlive", Json(r.producer_alive)},
                                            {"publishing", Json(r.publishing)},
                                            {"value", r.value ? observe(*r.value) : Json{}},
                                            {"ready", r.ready ? observe(*r.ready) : Json{}},
                                            {"pins", Json(r.pin_count)},
                                            {"consumers", Json(r.consumer_count)}}));
        for (const auto &r : stored.consumer_slots)
          if (r.active)
            consumers.push_back(
                Json::object({{"identity", observe(r.identity)}, {"result", observe(r.result)}}));
        return Json::object(
            {{"backing", observe(Value(*backing))},
             {"resources",
              Json::array({Json::array({Json(region_id), Json(latency), Json(state.free_at)})})},
             {"lastSequence", Json(state.sequence)},
             {"allocation", Json::array(allocation)},
             {"allocationPolicies",
              Json::object({{"leases", policy(state.limits.leases, true)},
                            {"accesses", policy(state.limits.operations, true)},
                            {"results", policy(stored.result_slots.size(), false)},
                            {"consumers", policy(stored.consumer_slots.size(), false)}})},
             {"resultSlots", Json::array(result_slots)},
             {"consumerSlots", Json::array(consumer_slots)},
             {"pinLimit", Json(stored.pin_limit)},
             {"inputByteLimit", Json(state.limits.input_bytes)},
             {"pendingInputBytes", Json(state.pending_input_bytes)},
             {"records",
              observe(Value(Value::Array{Value(state.regions), Value(state.leases),
                                         Value(state.operations), Value(state.events)}))},
             {"results", Json::array(results)},
             {"consumers", Json::array(consumers)},
             {"leaseGenerations",
              [&]() {
                std::vector<Json> values;
                for (auto generation : state.lease_generations)
                  values.emplace_back(generation);
                return Json::array(values);
              }()},
             {"accessGenerations",
              [&]() {
                std::vector<Json> values;
                for (auto generation : state.access_generations)
                  values.emplace_back(generation);
                return Json::array(values);
              }()},
             {"resultCount", Json(runtime.results().occupied())},
             {"pins", Json(manager->backing_pins())},
             {"next", next ? observe(*next) : Json{}}});
      };
      fixture.identities = [manager]() {
        auto state = manager->snapshot();
        std::vector<Json> names;
        for (const auto &v : state.leases) {
          auto &a = std::get<Value::Array>(v.data);
          auto h = std::get<Handle>(a[0].data);
          names.push_back(Json::object(
              {{"name", Json("lease." + std::to_string(h.slot))}, {"identity", observe(h)}}));
        }
        for (const auto &v : state.operations) {
          auto &a = std::get<Value::Array>(v.data);
          auto h = std::get<Handle>(a[0].data);
          for (auto index : {0, 6, 7})
            names.push_back(Json::object({{"name", Json(std::string(index == 0   ? "access."
                                                                    : index == 6 ? "result."
                                                                                 : "consumer.") +
                                                        std::to_string(h.slot))},
                                          {"identity", observe(std::get<Handle>(a[index].data))}}));
        }
        return Json::array(names);
      };
      return fixture;
    }
    auto source = record(config, "external.descriptor", 7);
    if (!source)
      return source.error();
    auto &v = *source.value();
    auto id = number(v[0]);
    if (!id || id.value() > UINT32_MAX)
      return fail(ErrorCode::Schema, "external call ID");
    ExternCallDesc desc;
    desc.id = std::uint32_t(id.value());
    desc.logical_id = text(v[1]);
    desc.reference_hash = text(v[2]);
    desc.symbol_id = text(v[3]);
    desc.build_id = text(v[4]);
    desc.contract_hash = text(v[5]);
    desc.select_native = std::get<bool>(v[6].data);
    desc.input = {ExternalLayoutKind::U64LE, 8};
    desc.output = {ExternalLayoutKind::U64LE, 8};
    desc.effects = {true, true, true, true, true, true};
    if (desc.logical_id != "add1" || desc.symbol_id != "add1")
      return fail(ErrorCode::Unsupported, "external fixture owns add1 ABI only");
    auto gate = std::make_shared<ExternalCallGate>(true);
    auto registered = gate->register_reference(
        desc,
        [](ExternalCallGate &, const Value &input) -> Expected<Value> {
          auto n = number(input);
          if (!n)
            return n.error();
          auto sum = checked_add(n.value(), 1);
          if (!sum)
            return sum.error();
          return Value(sum.value());
        },
        [](const Value &v) -> Expected<bool> {
          auto n = number(v);
          if (!n)
            return n.error();
          return n.value() != UINT64_MAX;
        });
    if (!registered)
      return registered.error();
    if (desc.select_native) {
      auto native = gate->register_native(desc.id, desc.symbol_id, desc.build_id, 1, native_add1);
      if (!native)
        return native.error();
    }
    auto sealed = gate->seal();
    if (!sealed)
      return sealed.error();
    for (auto &s : p.services) {
      auto bound = register_external_service(backend, p, s, *gate, desc.id);
      if (!bound)
        return bound.error();
    }
    fixture.lifetime.push_back(gate);
    fixture.snapshot = []() { return Json::object({}); };
    fixture.identities = []() { return Json::object({}); };
    return fixture;
  } catch (const std::exception &e) {
    return fail(ErrorCode::Schema, std::string("optional fixture setup: ") + e.what());
  }
}
} // namespace leanat::opcode_test
