#pragma once
#include "leanat/runtime_services.hpp"

namespace leanat {
// Trusted host metadata is sealed separately from current sampled values.
// The store and backend must outlive all registered provider invocations.
class SidebandInputStore {
  struct Port {
    std::uint64_t owner;
    exec::PortDirection direction;
    exec::Type type;
    std::optional<Value> value;
  };
  DomainId domain_;
  std::size_t capacity_;
  bool frozen_{};
  std::map<std::pair<std::uint32_t, std::uint32_t>, Port> ports_;

public:
  explicit SidebandInputStore(DomainId domain, std::size_t capacity = 1024)
      : domain_(domain), capacity_(capacity) {}
  Expected<void> define_port(InstanceId, std::uint64_t owner, std::uint32_t port,
                             exec::PortDirection, exec::Type, std::optional<Value> initial = {});
  Expected<void> freeze();
  bool frozen() const {
    return frozen_;
  }
  Expected<void> update(InstanceId, std::uint32_t port, Value);
  Expected<void> validate_binding(InstanceId, std::uint32_t port, exec::PortDirection,
                                  const exec::Type &) const;
  Expected<Value> read(const ExecutionContext &, std::uint32_t port, const exec::Type &) const;
};
Expected<exec::ServiceSignature> input_service_signature(std::uint32_t id,
                                                         const std::vector<exec::Type> &,
                                                         std::uint32_t index_type,
                                                         std::uint32_t result_type);
Expected<void> register_sideband_services(CoreRuntimeBackend &, SidebandInputStore &,
                                          const exec::Project &);
} // namespace leanat
