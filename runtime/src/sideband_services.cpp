#include "leanat/sideband_services.hpp"
namespace leanat {
namespace {
bool scalar(const exec::Type &t) {
  return t.fields.empty() && t.constructors.empty() &&
         ((t.kind == exec::TypeKind::Bool && t.bound == 0) ||
          (t.kind == exec::TypeKind::Bits && t.bound >= 1 && t.bound <= 64));
}
bool same_type(const exec::Type &a, const exec::Type &b) {
  return a.kind == b.kind && a.bound == b.bound && a.fields == b.fields &&
         a.constructors == b.constructors;
}
bool scalar_value(const exec::Type &type, const Value &value) {
  if (!scalar(type))
    return false;
  if (type.kind == exec::TypeKind::Bool)
    return std::holds_alternative<bool>(value.data);
  auto n = std::get_if<std::uint64_t>(&value.data);
  return n && (type.bound == 64 || *n < (std::uint64_t{1} << type.bound));
}
} // namespace
Expected<void> SidebandInputStore::define_port(InstanceId instance, std::uint64_t owner,
                                               std::uint32_t id, exec::PortDirection direction,
                                               exec::Type type, std::optional<Value> initial) {
  if (frozen_)
    return fail(ErrorCode::InvalidState, "input metadata frozen");
  if (!scalar(type) ||
      (direction != exec::PortDirection::Input && direction != exec::PortDirection::Output))
    return fail(ErrorCode::Schema, "input port scalar metadata");
  if (initial && !scalar_value(type, *initial))
    return fail(ErrorCode::TypeMismatch, "input initial value");
  auto key = std::make_pair(instance.value, id);
  if (ports_.count(key))
    return fail(ErrorCode::Duplicate, "input port already defined");
  if (ports_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "input port capacity");
  ports_.emplace(key, Port{owner, direction, std::move(type), std::move(initial)});
  return {};
}
Expected<void> SidebandInputStore::freeze() {
  frozen_ = true;
  return {};
}
Expected<void> SidebandInputStore::update(InstanceId instance, std::uint32_t port, Value value) {
  if (!frozen_)
    return fail(ErrorCode::InvalidState, "input metadata not frozen");
  auto found = ports_.find({instance.value, port});
  if (found == ports_.end())
    return fail(ErrorCode::InvalidArgument, "unknown input port");
  if (found->second.direction != exec::PortDirection::Input)
    return fail(ErrorCode::InvalidState, "port is not input");
  if (!scalar_value(found->second.type, value))
    return fail(ErrorCode::TypeMismatch, "input value shape");
  found->second.value = std::move(value);
  return {};
}
Expected<void> SidebandInputStore::validate_binding(InstanceId instance, std::uint32_t port,
                                                    exec::PortDirection direction,
                                                    const exec::Type &type) const {
  if (!frozen_)
    return fail(ErrorCode::InvalidState, "input metadata not frozen");
  auto found = ports_.find({instance.value, port});
  if (found == ports_.end())
    return fail(ErrorCode::InvalidArgument, "host input binding absent");
  if (found->second.direction != direction || !same_type(found->second.type, type))
    return fail(ErrorCode::Integrity, "host input metadata differs from descriptor");
  return {};
}
Expected<Value> SidebandInputStore::read(const ExecutionContext &ctx, std::uint32_t port,
                                         const exec::Type &expected) const {
  if (!frozen_)
    return fail(ErrorCode::InvalidState, "input metadata not frozen");
  if (ctx.kind != ContextKind::Timed && ctx.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "input read context");
  if (ctx.domain != domain_)
    return fail(ErrorCode::WrongDomain, "input domain");
  auto found = ports_.find({ctx.instance.value, port});
  if (found == ports_.end())
    return fail(ErrorCode::InvalidArgument, "unknown instance input port");
  const auto &binding = found->second;
  if (ctx.owner != binding.owner)
    return fail(ErrorCode::WrongOwner, "input instance owner");
  if (binding.direction != exec::PortDirection::Input)
    return fail(ErrorCode::InvalidState, "port is not input");
  if (!same_type(binding.type, expected))
    return fail(ErrorCode::TypeMismatch, "input result type differs from port");
  if (!binding.value)
    return fail(ErrorCode::NotReady, "input has no current sample");
  return *binding.value;
}
Expected<exec::ServiceSignature> input_service_signature(std::uint32_t id,
                                                         const std::vector<exec::Type> &types,
                                                         std::uint32_t index,
                                                         std::uint32_t result) {
  if (index >= types.size() || result >= types.size() || !scalar(types[index]) ||
      types[index].kind != exec::TypeKind::Bits || types[index].bound != 64 ||
      !scalar(types[result]))
    return fail(ErrorCode::Schema, "input provider expects U64 port and scalar result");
  exec::ServiceSignature s;
  s.id = id;
  s.op = exec::Op::LoadInput;
  s.input_types = {index};
  s.result_types = {result};
  s.context_mask = 3;
  s.effect_mask = 0;
  s.extra_fuel = 0;
  s.provider_key = "leanat.core.input.read";
  s.provider_version = "1";
  const std::string hex = "e084d863eaf370dd7b8004942c1720fac8f02977fb9efa8d01ba85183012d30f";
  auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < s.abi_hash.size(); ++i)
    s.abi_hash[i] = static_cast<std::uint8_t>((digit(hex[2 * i]) << 4) | digit(hex[2 * i + 1]));
  return s;
}
Expected<void> register_sideband_services(CoreRuntimeBackend &backend, SidebandInputStore &store,
                                          const exec::Project &project) {
  if (!store.frozen())
    return fail(ErrorCode::InvalidState, "seal actual input metadata first");
  using PortKey = std::pair<std::uint32_t, std::uint32_t>;
  auto ports = std::make_shared<std::map<PortKey, exec::Type>>();
  const bool hierarchical = !project.instances.empty() || !project.components.empty();
  std::set<std::uint32_t> instance_ids;
  for (const auto &instance : project.instances) {
    if (!instance_ids.insert(instance.id).second)
      return fail(ErrorCode::Duplicate, "duplicate input instance");
    const exec::ComponentDesc *component = nullptr;
    for (const auto &candidate : project.components)
      if (candidate.id == instance.definition) {
        if (component)
          return fail(ErrorCode::Duplicate, "duplicate input component");
        component = &candidate;
      }
    if (!component)
      return fail(ErrorCode::Schema, "input instance component missing");
    std::set<std::uint32_t> ids;
    for (const auto &port : component->sidebands) {
      if (!ids.insert(port.id).second || port.type_id >= project.types.size())
        return fail(ErrorCode::Schema, "input metadata port id/type");
      if (port.direction != exec::PortDirection::Input)
        continue;
      if (ports->size() >= 65536)
        return fail(ErrorCode::Capacity, "input metadata bound");
      auto checked = store.validate_binding(InstanceId{instance.id}, port.id, port.direction,
                                            project.types[port.type_id]);
      if (!checked)
        return checked.error();
      ports->emplace(PortKey{instance.id, port.id}, project.types[port.type_id]);
    }
  }
  std::vector<std::pair<exec::ServiceSignature, CoreRuntimeBackend::Provider>> providers;
  for (const auto &s : project.services) {
    if (s.op != exec::Op::LoadInput && s.provider_key != "leanat.core.input.read")
      continue;
    if (s.input_types.size() != 1 || s.result_types.size() != 1)
      return fail(ErrorCode::Schema, "input service arity");
    auto expected =
        input_service_signature(s.id, project.types, s.input_types[0], s.result_types[0]);
    if (!expected)
      return expected.error();
    if (expected.value() != s)
      return fail(ErrorCode::Integrity, "input provider ABI differs from descriptor");
    auto result_type = project.types[s.result_types[0]];
    providers.emplace_back(
        s,
        [&store, ports, hierarchical, result_type](const std::vector<Value> &args,
                                                   const ExecutionContext &ctx,
                                                   EventTxn &) -> Expected<std::vector<Value>> {
          if (args.size() != 1)
            return fail(ErrorCode::TypeMismatch, "input read arity");
          auto index = std::get_if<std::uint64_t>(&args[0].data);
          if (!index || *index > UINT32_MAX)
            return fail(ErrorCode::TypeMismatch, "input port index bound");
          auto port = static_cast<std::uint32_t>(*index);
          if (hierarchical) {
            auto declared = ports->find({ctx.instance.value, port});
            if (declared == ports->end())
              return fail(ErrorCode::InvalidArgument, "undeclared instance input port");
            if (!same_type(declared->second, result_type))
              return fail(ErrorCode::TypeMismatch, "descriptor input port result type");
          }
          auto value = store.read(ctx, port, result_type);
          if (!value)
            return value.error();
          return std::vector<Value>{value.value()};
        });
  }
  return backend.register_providers(std::move(providers));
}
} // namespace leanat
