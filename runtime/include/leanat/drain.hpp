#pragma once
#include "common.hpp"
#include "event_txn.hpp"
#include "protocol.hpp"
#include <set>
namespace leanat {
enum class CancelReason { Timeout, User, Reset, ParentCancelled };
enum class DrainState { Pending, Complete, Failed };
enum class ResetPolicy { DrainThenReset, AbortLocalAndDrain };
enum class ResetState { Applied, Draining, Superseded };
struct DrainHop {
  Handle hop;
  bool wire_terminal{}, timing_consumed{}, cleanup_returned{}, call_pin{};
};
struct DrainStatus {
  DrainState state{DrainState::Pending};
  std::vector<DrainHop> hops;
  CancelReason reason{CancelReason::User};
};
struct CancelDisposition {
  bool local_only{};
  std::optional<Handle> receipt;
};
struct ResetDisposition {
  std::uint64_t reset_id{};
  ResetPolicy policy;
  std::uint64_t old_epoch{}, next_epoch{};
  ResetState state{ResetState::Draining};
};
struct Responsibility {
  Handle transaction;
  InstanceId instance;
  std::uint64_t epoch{};
  std::optional<Handle> parent;
  std::vector<DrainHop> hops;
  bool local_finished{}, cancelled{};
};
class DrainUpdate;
class DrainStore {
  friend class DrainUpdate;
  DrainStore(const DrainStore &) = default;
  DrainStore &operator=(const DrainStore &) = delete;
  struct Record {
    Responsibility responsibility;
    std::optional<Handle> receipt;
    CancelReason reason{CancelReason::User};
    bool receipt_released{}, deferred{}, retirement_requested{};
  };
  struct Instance {
    std::uint64_t epoch{};
    std::optional<ResetDisposition> reset;
    std::set<Handle> cohort;
  };
  DomainId domain_;
  std::size_t capacity_, hop_limit_, instance_limit_;
  std::uint64_t next_receipt_{1}, next_reset_{1};
  std::uint32_t physical_store_{};
  std::map<Handle, Record> records_;
  std::map<InstanceId, Instance> instances_;
  std::map<Handle, DrainStatus> receipts_;
  bool preparation_pending_{};
  DrainStore *pending_view_{};
  EventTxn *pending_txn_{};
  bool complete(const Record &) const;
  void refresh(Record &);
  void collect();

public:
  struct CounterSnapshot {
    DomainId domain;
    std::uint32_t store{};
    std::uint64_t next_receipt{}, next_reset{};
    std::size_t receipt_count{}, capacity{}, hop_limit{}, responsibilities{};
  };
  CounterSnapshot counter_snapshot() const {
    return {domain_,          physical_store_, next_receipt_, next_reset_,
            receipts_.size(), capacity_,       hop_limit_,    records_.size()};
  }
  DrainStore(DrainStore &&) = delete;
  DrainStore &operator=(DrainStore &&) = delete;
  DrainStore(DomainId, std::size_t responsibility_capacity, std::size_t hops_per_responsibility,
             std::size_t instance_capacity);
  Expected<std::unique_ptr<DrainUpdate>> prepare();
  Expected<std::unique_ptr<DrainUpdate>> prepare(EventTxn &);
  Expected<void> add_instance(InstanceId);
  Expected<void> register_responsibility(Responsibility,
                                         std::optional<Handle> verified_parent = {});
  Expected<void> defer_root(Responsibility);
  bool is_deferred(Handle) const;
  Expected<void> set_hop(Handle transaction, DrainHop);
  Expected<void> add_hop(Handle transaction, DrainHop);
  Expected<void> drop_unstarted_hop(Handle transaction, Handle hop, const ProtocolEngine &,
                                    const EventTxn *prepared = nullptr);
  Expected<void> observe_wire(Handle hop, bool terminal, bool pin_active);
  Expected<void> consume_terminal(Handle hop);
  bool is_cancelled(Handle transaction) const;
  Expected<void> finish_local(Handle transaction);
  Expected<void> retire_responsibility(Handle transaction);
  Expected<CancelDisposition> cancel_local(Handle, CancelReason);
  Expected<DrainStatus> inspect(Handle receipt) const;
  Expected<void> release_receipt(Handle);
  Expected<ResetDisposition> reset(InstanceId, ResetPolicy);
  Expected<ResetDisposition> advance_reset(InstanceId);
  Expected<std::uint64_t> epoch(InstanceId) const;
  bool admits_root(InstanceId) const;
  std::size_t outstanding() const;
  std::vector<Responsibility> stop_report() const;
};
class DrainUpdate final : public PreparedParticipant {
  friend class DrainStore;
  DrainStore *target_;
  std::unique_ptr<DrainStore> shadow_;
  DrainStore *previous_view_{};
  EventTxn *previous_txn_{};
  bool active_{true};
  explicit DrainUpdate(DrainStore &, EventTxn * = nullptr);

public:
  ~DrainUpdate() override;
  DrainStore &view() {
    return *shadow_;
  }
  std::size_t reserved_bytes() const noexcept override;
  const void *identity() const noexcept override {
    return target_;
  }
  Expected<void> validate() const override;
  void apply() noexcept override;
  void discard() noexcept override;
};
} // namespace leanat
