#pragma once
#include "result_store.hpp"
namespace leanat {
enum class WaitMode { Any, All };
enum class WaitFailurePolicy { CollectAll, FailFast };
struct WaitBranch {
  std::uint32_t ordinal{};
  std::int32_t priority{};
  Handle source;
  TypeId result_type{};
  std::string field_name;
  std::optional<ResultHandle> source_result;
  std::function<void()> unregister;
  std::function<Expected<void>()> transfer_winner;
};
struct WaitCreate {
  WaitMode mode{WaitMode::Any};
  WaitFailurePolicy failure_policy{WaitFailurePolicy::CollectAll};
  Handle owner_process, owner_scope;
  TypeId result_type{};
  std::size_t branch_count{}, result_bytes{4096};
  std::function<Expected<void>(const Value &, ReadyKey)> resume;
};
struct WaitNotification {
  Handle group;
  std::uint32_t ordinal{};
  Handle source;
  ReadyKey ready;
  BatchId delivery_batch;
  Value outcome;
  bool failed{};
};
struct WaitResolution {
  Handle group;
  ReadyKey ready;
  Value outcome;
};
class WaitGroupStore {
  friend class PreparedWaitGroups;
  WaitGroupStore(const WaitGroupStore &) = default;
  WaitGroupStore &operator=(const WaitGroupStore &) = delete;
  struct Candidate {
    ReadyKey ready;
    BatchId batch;
    Value outcome;
    bool failed;
  };
  struct Branch {
    WaitBranch desc;
    std::optional<Candidate> candidate;
    std::optional<ResultHandle> consumer;
    std::optional<PinnedResultView> pin;
    bool transferred{};
  };
  struct Group {
    bool used{}, committed{}, resolved{}, resume_pending{};
    std::uint64_t generation{1};
    WaitCreate desc;
    ReservedResult result;
    std::vector<Branch> branches;
  };
  DomainId domain_;
  std::uint32_t store_;
  std::size_t branch_limit_, candidate_bytes_;
  ResultStore &results_;
  std::vector<Group> groups_;
  std::size_t transaction_pending_{};
  EventTxn *transaction_{};
  std::shared_ptr<std::uint64_t> allocation_generation_{std::make_shared<std::uint64_t>(2)};
  Expected<ReservedResult> reserve_result(ResultCreate);
  Expected<PublicationReceipt> publish_result(ResultOwnerHandle, Value, PublicationContext);
  Expected<void> release_cap(ResultHandle, ReleasePolicy = ReleasePolicy::CurrentConsumer);
  Expected<void> release_producer(ResultOwnerHandle);
  Expected<ResultHandle> retain_cap(ResultHandle, std::uint64_t);
  Expected<Group *> get(Handle);
  Expected<void> cleanup(Group &);
  Expected<void> abort(Handle, Error);

public:
  struct BranchSnapshot {std::uint32_t ordinal;std::int32_t priority;Handle source;TypeId type;std::string field;std::optional<ResultHandle> consumer;std::optional<ReadyKey> ready;std::optional<Value> outcome;bool failed,pinned,transferred;};
  struct GroupSnapshot {Handle identity,process,scope;WaitMode mode;WaitFailurePolicy policy;std::size_t branch_count,result_bytes;TypeId result_type;ReservedResult result;bool committed,resolved,resume_pending;std::vector<BranchSnapshot> branches;};
  Expected<std::vector<GroupSnapshot>> snapshot() const;
  Expected<WaitGroupStore *> prepare(EventTxn &);
  Expected<Value> owning_result(Handle, ResultHandle);
  Expected<bool> fully_armed(Handle);
  Expected<void> refresh_sources(BatchId, ReadyKey);
  std::size_t control_capacity() const noexcept {
    return groups_.size();
  }
  std::uint64_t next_allocation_generation() const noexcept { return *allocation_generation_; }
  WaitGroupStore(DomainId, std::uint32_t, std::size_t groups, std::size_t branches,
                 std::size_t candidate_bytes, ResultStore &);
  Expected<Handle> create(WaitCreate);
  Expected<void> arm(Handle, WaitBranch, BatchId delivery_batch, ReadyKey now);
  Expected<void> commit(Handle);
  Expected<void> record(WaitNotification);
  Expected<std::vector<WaitResolution>> resolve_closed_batch(BatchId, bool batch_closed);
  Expected<ResultHandle> result_handle(Handle, Handle owner_scope);
  Expected<PinnedResultView> result(Handle, ResultHandle);
  Expected<ResultHandle> retain_wait_result(Handle, ResultHandle, std::uint64_t owner);
  Expected<void> release(Handle, ResultHandle, Handle owner_scope);
};
} // namespace leanat
