#pragma once
#include "common.hpp"
#include "event_txn.hpp"
#include "result_store.hpp"
namespace leanat {
using LeaseHandle = Handle;
using AccessHandle = Handle;
enum class ManagedPermission : unsigned { Read = 1, Write = 2, ReadWrite = 3 };
enum class InFlightPolicy { AllowToComplete, CancelBeforeCommit };
enum class AccessFailure { None, Invalidated, Cancelled, Reset, ServiceFailure };
enum class CommitDisposition { NotCommitted, ReadObserved, WriteCommitted };
struct ManagedRegionDesc {
  RegionId id{};
  std::vector<ConnectionId> allowed_connections{ConnectionId{0}};
  ObjectId backing_object{};
  std::uint64_t start{}, end{}, version{1};
  ManagedPermission permission{ManagedPermission::ReadWrite};
  InFlightPolicy invalidation{InFlightPolicy::AllowToComplete};
  std::shared_ptr<Bytes> backing;
  std::shared_ptr<VersionedCell> backing_cell;
  std::function<Expected<Duration>(Command, std::size_t)> service;
  bool timing_strict{true}, raw_alias{}, static_path{true};
};
struct ManagedRequest {
  RegionId region{};
  ConnectionId connection{};
  std::uint64_t start{}, end{}, owner{};
  ManagedPermission permission{ManagedPermission::ReadWrite};
  DomainId domain{};
};
struct OwnedAccessRequest {
  LeaseHandle lease;
  Command command{Command::Read};
  std::uint64_t address{};
  std::size_t count{};
  Bytes input;
  Tick arrival{};
  bool byte_enable{}, streaming{}, atomic{};
};
struct ManagedCompletion {
  AccessFailure failure{AccessFailure::None};
  CommitDisposition disposition{CommitDisposition::NotCommitted};
  ReadyKey ready{};
};
struct ManagedLimits {
  std::size_t regions{32}, leases{128}, operations{128}, input_bytes{65536}, scheduled_changes{128};
};
// Bounded single service resource. Shared AT callers must submit in effective-arrival order.
class ManagedServiceResource {
  Tick free_{};

public:
  Expected<Tick> reserve(Tick arrival, Duration service);
  Tick available() const {
    return free_;
  }
};
struct ManagedResourceGrant {
  Tick finish;
  Handle ticket;
};
struct ManagedResourceBinding {
  std::function<Expected<ManagedResourceGrant>(EventTxn &, Handle, Tick, Duration)> reserve;
  std::function<Expected<bool>(EventTxn &, Handle, Tick)> complete;
};
struct ManagedStateSnapshot {
  std::vector<Value> regions, leases, operations, events;
  std::vector<std::uint64_t> lease_generations, access_generations;
  ManagedLimits limits;
  std::uint64_t sequence{}, now{}, free_at{};
  std::size_t pending_input_bytes{};
};
class ManagedAccessManager {
  struct Impl;
  struct Staged;
  std::unique_ptr<Impl> impl_;
  explicit ManagedAccessManager(std::unique_ptr<Impl>);
  Expected<Staged *> stage(EventTxn &);
  Expected<void> advance_impl(Tick, std::size_t, std::optional<std::uint64_t>);

public:
  ManagedAccessManager(ResultStore &, DomainId, bool enabled, ManagedLimits = {},
                       std::uint32_t store = 0,
                       std::shared_ptr<ManagedServiceResource> resource = {});
  ~ManagedAccessManager();
  Expected<void> add_region(ManagedRegionDesc);
  template <class M>
  Expected<void> add_memory_region(ManagedRegionDesc region, std::shared_ptr<M> memory) {
    if (!memory) {
      return fail(ErrorCode::InvalidArgument, "null memory provider");
    }
    region.backing_cell = std::shared_ptr<VersionedCell>(memory, &memory->backing());
    region.backing = std::shared_ptr<Bytes>(region.backing_cell,
                                            &std::get<Bytes>(region.backing_cell->value.data));
    return add_region(std::move(region));
  }
  Expected<void> bind_resource(ManagedResourceBinding);
  template <class R> Expected<void> bind_shared_resource(R &resource) {
    return bind_resource(
        {[&resource](EventTxn &t, Handle o, Tick a, Duration d) -> Expected<ManagedResourceGrant> {
           auto g = resource.reserve(t, o, a, d);
           if (!g) {
             return g.error();
           }
           return ManagedResourceGrant{g.value().finish, g.value().ticket};
         },
         [&resource](EventTxn &t, Handle h, Tick at) { return resource.complete(t, h, at); }});
  }
  Expected<LeaseHandle> request(const ManagedRequest &);
  Expected<LeaseHandle> prepare_request(EventTxn &, const ManagedRequest &);
  Expected<AccessHandle> prepare_begin(EventTxn &, OwnedAccessRequest);
  Expected<void> prepare_release_lease(EventTxn &, LeaseHandle);
  Expected<void> prepare_invalidate(EventTxn &, RegionId, std::uint64_t, std::uint64_t, Tick);
  Expected<ResultHandle> prepare_result_handle(EventTxn &, AccessHandle);
  Expected<void> prepare_release_result(EventTxn &, AccessHandle);
  Expected<AccessHandle> begin(OwnedAccessRequest);
  Expected<void> advance(Tick);
  Expected<void> advance_one(ReadyKey);
  Expected<void> invalidate(RegionId, std::uint64_t start, std::uint64_t end, Tick effective);
  Expected<void> replace_backing(RegionId, std::shared_ptr<Bytes>, std::uint64_t new_version);
  Expected<void> release_lease(LeaseHandle);
  Expected<void> cancel(AccessHandle, AccessFailure reason = AccessFailure::Cancelled);
  Expected<void> reset();
  Expected<ManagedCompletion> completion(AccessHandle) const;
  Expected<ResultHandle> result_handle(AccessHandle) const;
  Expected<PinnedResultView> pin_result(AccessHandle, ConsumerToken);
  Expected<void> release_result(AccessHandle, ConsumerToken);
  bool standard_dmi_allowed(RegionId) const {
    return false;
  }
  std::size_t backing_pins() const;
  // Owning observation of actual records, including every live raw handle field.
  ManagedStateSnapshot snapshot() const;
  Tick now() const;
  bool uses_results(const ResultStore &) const;
  std::optional<ReadyKey> next_ready() const;
};
} // namespace leanat
