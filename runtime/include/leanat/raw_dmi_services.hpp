#pragma once
#include "runtime_services.hpp"
namespace leanat {
struct RawDmiGrant {
  bool granted{};
  std::uint64_t region{}, start{}, end{}, permission{}, read_latency{}, write_latency{},
      generation{};
};
struct RawDmiRegion {
  std::uint64_t id{}, start{}, end{}, read_latency{}, write_latency{}, generation{1}, owner{};
  DomainId domain;
  std::size_t capacity{128};
  // Called only after commit by publish(). Pointer installation and native callbacks belong here.
  std::function<Expected<void>(std::uint64_t, Command)> grant;
  std::function<Expected<void>(std::uint64_t, std::uint64_t)> invalidate;
};
struct RawDmiObservation {
  bool invalidation{};
  RawDmiGrant grant;
};
class RawDmiServices {
  struct Impl;
  std::shared_ptr<Impl> impl_;

public:
  explicit RawDmiServices(bool enabled);
  Expected<void> add_region(RawDmiRegion);
  Expected<void> register_into(CoreRuntimeBackend &, const exec::Project &);
  // Like transport publication, failure is a committed host failure and is never retried
  // implicitly.
  Expected<void> publish();
  const std::vector<RawDmiObservation> &observations() const;
  const std::vector<RawDmiGrant> &grants() const;
};
Expected<exec::ServiceSignature> raw_dmi_signature(const exec::Project &, std::uint32_t id,
                                                   exec::Op, std::vector<std::uint32_t> inputs,
                                                   std::uint32_t output);
} // namespace leanat
