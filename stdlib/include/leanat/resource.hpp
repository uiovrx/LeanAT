#pragma once
#include "objects.hpp"
namespace leanat {
enum class ResourceKind { Serial, Capacity, Pipeline };
struct ResourceSpec {
  ResourceKind kind{ResourceKind::Serial};
  std::size_t capacity{1};
  Duration default_latency{}, initiation_interval{};
  std::size_t reservation_capacity{128};
};
struct Grant {
  Tick start{}, finish{};
  Duration wait{};
  Handle ticket;
  std::size_t channel{};
};
enum class ResourceCancelDisposition {
  CancelledUnpublished,
  CancelledRetained,
  CancellationDeferred,
  AlreadyCancelled,
  AlreadySettled
};
struct ResourceSnapshot {
  std::vector<Grant> reservations;
  Tick next_issue{};
  std::size_t servicing{}, scheduled{}, cancelled{};
};
class Resource {
  ResourceSpec spec_;
  DomainId domain_;
  std::uint32_t store_{};
  VersionedCell cell_;
  Resource() = default;

public:
  Resource(const Resource &) = delete;
  Resource &operator=(const Resource &) = delete;
  Resource(Resource &&) = default;
  Resource &operator=(Resource &&) = default;
  static Expected<Resource> make(ResourceSpec, DomainId domain = {}, std::uint32_t store = 0);
  static Expected<Resource> serial(Duration d, std::size_t records = 128) {
    return make({ResourceKind::Serial, 1, d, Duration{}, records});
  }
  static Expected<Resource> capacity(std::size_t n, Duration d, std::size_t records = 128) {
    return make({ResourceKind::Capacity, n, d, Duration{}, records});
  }
  static Expected<Resource> pipeline(Duration latency, Duration ii, std::size_t n,
                                     std::size_t records = 128) {
    return make({ResourceKind::Pipeline, n, latency, ii, records});
  }
  const ResourceSpec &spec() const {
    return spec_;
  }
  std::uint32_t identity_store() const noexcept {
    return store_;
  }
  Expected<Grant> reserve(EventTxn &, Handle owner, Tick earliest, Duration);
  Expected<Grant> reserve_default(EventTxn &t, Handle o, Tick e) {
    return reserve(t, o, e, spec_.default_latency);
  }
  Expected<ResourceCancelDisposition> cancel_pending(EventTxn &, Handle ticket);
  Expected<bool> complete(EventTxn &, Handle ticket, Tick at);
  Expected<ResourceSnapshot> inspect(EventTxn &) const;
  // Owning diagnostic snapshot, including retired ticket generations and scheduling state.
  Expected<Value> snapshot(EventTxn &t) const {
    auto checked = inspect(t);
    if (!checked)
      return checked.error();
    return t.read(cell_);
  }
};
} // namespace leanat
