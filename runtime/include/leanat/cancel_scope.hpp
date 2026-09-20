#pragma once
#include "common.hpp"
#include "event_txn.hpp"
#include <set>
namespace leanat {
enum class ScopeState { Open, Cancelling, Cancelled, Closed };
enum class OwnedKind {
  ChildScope,
  Task,
  Wait,
  BusinessEvent,
  TxnDemand,
  SpawnTicket,
  RequestGateTicket,
  Access,
  Lease,
  ResultConsumer
};
enum class CancelActionKind { DropLocal, CancelChild, ReleaseConsumer, TransferToDrain };
struct ScopeOwned {
  Handle handle;
  OwnedKind kind{OwnedKind::Task};
  bool published{};
};
struct ScopeCancelAction {
  std::uint64_t id{};
  Handle scope;
  ScopeOwned owned;
  CancelActionKind kind;
  bool applied{};
};
struct ScopeCancelPlan {
  Handle scope;
  std::string reason;
  std::vector<ScopeCancelAction> actions;
};
class CancelScopeStore {
  friend class PreparedScopes;
  CancelScopeStore(const CancelScopeStore &) = default;
  CancelScopeStore &operator=(const CancelScopeStore &) = delete;
  struct Record {
    std::uint64_t generation{1};
    bool used{};
    Handle parent;
    ScopeState state{ScopeState::Open};
    std::vector<ScopeOwned> entries;
    std::optional<ScopeCancelPlan> plan;
  };
  struct Observer {
    std::uint64_t id;
    Handle source, process, wait;
  };
  DomainId domain_;
  std::uint32_t store_;
  std::size_t entries_, actions_, observers_;
  std::uint64_t sequence_{};
  std::vector<Record> records_;
  std::vector<Observer> observations_;
  std::size_t transaction_pending_{};
  EventTxn *transaction_{};
  std::shared_ptr<std::uint64_t> allocation_generation_{std::make_shared<std::uint64_t>(2)};
  Expected<Record *> get(Handle);
  Expected<const Record *> get(Handle) const;
  std::optional<Handle> owner_of(Handle) const;
  bool inside(Handle, Handle) const;
  bool frozen(Handle) const;

public:
  struct ScopeSnapshot { Handle identity,parent; ScopeState state; std::vector<ScopeOwned> owned; std::optional<ScopeCancelPlan> plan; };
  struct ObserverSnapshot {std::uint64_t id;Handle source,process,wait;};
  struct Snapshot {std::vector<ScopeSnapshot> scopes;std::vector<ObserverSnapshot> observers;std::uint64_t sequence;};
  Expected<Snapshot> snapshot() const;
  std::size_t control_capacity() const noexcept {
    return records_.size();
  }
  Expected<CancelScopeStore *> prepare(EventTxn &);
  CancelScopeStore(DomainId, std::uint32_t, std::size_t scopes, std::size_t entries,
                   std::size_t actions, std::size_t observers = 64);
  Handle root() const;
  std::uint64_t next_allocation_generation() const noexcept { return *allocation_generation_; }
  Expected<Handle> create(Handle parent);
  Expected<void> attach(ScopeOwned, Handle);
  Expected<void> transfer(Handle object, Handle from, Handle to);
  Expected<void> replace_owned(Handle old, ScopeOwned replacement, Handle scope);
  Expected<void> detach_completed(Handle object, Handle scope);
  Expected<ScopeCancelPlan> cancel(Handle, std::string reason);
  Expected<void> apply(Handle, std::uint64_t action,
                       const std::function<Expected<void>(const ScopeCancelAction &)> &);
  Expected<void> close(Handle);
  Expected<ScopeState> state(Handle) const;
  Expected<void> validate_owner(Handle, bool require_open) const;
  Expected<std::uint64_t> observe_cancel(Handle observed, Handle process_owner, Handle wait_owner);
  Expected<void> release_observer(std::uint64_t);
  Expected<std::optional<std::string>> cancel_reason(Handle) const;
};
} // namespace leanat
