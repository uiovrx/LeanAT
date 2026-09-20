#pragma once
#include "opcode_fixture.hpp"
#include <leanat/sideband_services.hpp>
namespace leanat::opcode_test {
// Inputs are actual source-catalog environment samples, never expected results.
inline Expected<ProviderFixture> bind_sideband_fixture(Runtime &, CoreRuntimeBackend &backend,
                                                       const exec::Project &project,
                                                       const FixtureConfig &config) {
  auto rows = [&](const char *name) -> Expected<Value::Array> {
    auto found = config.environment.find(name);
    if (found == config.environment.end())
      return fail(ErrorCode::InvalidArgument, std::string("missing ") + name);
    auto values = std::get_if<Value::Array>(&found->second.data);
    if (!values || values->size() > 65536)
      return fail(ErrorCode::TypeMismatch, "input fixture row bound");
    return *values;
  };
  auto metadata = rows("runtime.inputPorts"), samples = rows("runtime.inputs"),
       ownership = rows("runtime.owners");
  if (!metadata)
    return metadata.error();
  if (!samples)
    return samples.error();
  if (!ownership)
    return ownership.error();
  std::map<std::uint32_t, std::uint64_t> owners;
  for (const auto &row : ownership.value()) {
    auto fields = std::get_if<Value::Array>(&row.data);
    if (!fields || fields->size() != 2)
      return fail(ErrorCode::TypeMismatch, "input owner row");
    auto instance = std::get_if<std::uint64_t>(&(*fields)[0].data),
         owner = std::get_if<std::uint64_t>(&(*fields)[1].data);
    if (!instance || *instance > UINT32_MAX || !owner ||
        !owners.emplace(static_cast<std::uint32_t>(*instance), *owner).second)
      return fail(ErrorCode::Schema, "input owner identity");
  }
  struct Binding {
    InstanceId instance;
    std::uint32_t port;
    std::uint64_t owner;
    std::uint32_t type_id;
    exec::Type type;
  };
  auto bindings = std::make_shared<std::vector<Binding>>();
  auto store = std::make_shared<SidebandInputStore>(config.context.domain, metadata.value().size());
  for (const auto &row : metadata.value()) {
    auto fields = std::get_if<Value::Array>(&row.data);
    if (!fields || fields->size() != 3)
      return fail(ErrorCode::TypeMismatch, "input port metadata row");
    auto instance = std::get_if<std::uint64_t>(&(*fields)[0].data),
         port = std::get_if<std::uint64_t>(&(*fields)[1].data),
         type = std::get_if<std::uint64_t>(&(*fields)[2].data);
    if (!instance || *instance > UINT32_MAX || !port || *port > UINT32_MAX || !type ||
        *type >= project.types.size())
      return fail(ErrorCode::Schema, "input metadata identity/type");
    auto owner = owners.find(static_cast<std::uint32_t>(*instance));
    if (owner == owners.end())
      return fail(ErrorCode::Schema, "input owner metadata absent");
    auto defined = store->define_port(InstanceId{static_cast<std::uint32_t>(*instance)},
                                      owner->second, static_cast<std::uint32_t>(*port),
                                      exec::PortDirection::Input, project.types[*type]);
    if (!defined)
      return defined.error();
    bindings->push_back({InstanceId{static_cast<std::uint32_t>(*instance)},
                         static_cast<std::uint32_t>(*port), owner->second,
                         static_cast<std::uint32_t>(*type), project.types[*type]});
  }
  auto sealed = store->freeze();
  if (!sealed)
    return sealed.error();
  std::set<std::pair<std::uint32_t, std::uint32_t>> seeded;
  for (const auto &row : samples.value()) {
    auto fields = std::get_if<Value::Array>(&row.data);
    if (!fields || fields->size() != 3)
      return fail(ErrorCode::TypeMismatch, "input sample row");
    auto instance = std::get_if<std::uint64_t>(&(*fields)[0].data),
         port = std::get_if<std::uint64_t>(&(*fields)[1].data);
    if (!instance || *instance > UINT32_MAX || !port || *port > UINT32_MAX)
      return fail(ErrorCode::Schema, "input sample identity");
    auto key =
        std::make_pair(static_cast<std::uint32_t>(*instance), static_cast<std::uint32_t>(*port));
    if (!seeded.insert(key).second)
      return fail(ErrorCode::Duplicate, "duplicate input sample");
    auto updated = store->update(InstanceId{key.first}, key.second, (*fields)[2]);
    if (!updated)
      return updated.error();
  }
  auto registered = register_sideband_services(backend, *store, project);
  if (!registered)
    return registered.error();
  ProviderFixture fixture;
  fixture.inputs = config.inputs;
  fixture.lifetime = {store, bindings};
  fixture.snapshot = [store, bindings, domain = config.context.domain]() {
    std::vector<Json> rows;
    for (const auto &binding : *bindings) {
      ExecutionContext context;
      context.domain = domain;
      context.instance = binding.instance;
      context.owner = binding.owner;
      auto value = store->read(context, binding.port, binding.type);
      rows.push_back(Json::object({{"instance", observe(binding.instance)},
                                   {"port", observe(binding.port)},
                                   {"owner", observe(binding.owner)},
                                   {"typeId", observe(binding.type_id)},
                                   {"value", value ? observe(value.value()) : Json()},
                                   {"ready", Json(bool(value))}}));
    }
    return Json::object({{"inputs", Json::array(rows)}});
  };
  fixture.identities = []() {
    return Json::object({});
  }; // Ports are instance/local IDs, not allocated handles.
  return fixture;
}
} // namespace leanat::opcode_test
