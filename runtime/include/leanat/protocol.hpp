#pragma once
#include "common.hpp"
#include "event_txn.hpp"
#include <set>
namespace leanat {
enum class WireState { Idle, Request, RequestReleased, Response, Terminal };
enum class MilestoneKind { RequestReleased, ResponseReady, Terminal };
struct ProtocolMilestone {
  MilestoneKind kind;
  Tick time;
  bool implicit{};
  std::optional<ResponseSnapshot> response;
};
struct LedgerIdentity {
  DomainId domain{};
  InstanceId local_side{};
  ConnectionId connection{};
  TransportId transport{};
  std::uint64_t transport_generation{1};
};
struct CallTicket {
  Handle handle;
};
struct WireSnapshot {
  LedgerIdentity identity;
  WireState state{WireState::Idle};
  Tick last_timing{};
  bool faulted{}, pending{};
  std::uint64_t call_ordinal{};
  std::uint32_t protocol_state{};
};
struct ExchangeResult {
  CallId call_id{};
  Handle hop;
  WireState next_state{};
  std::vector<ProtocolMilestone> milestones;
  std::vector<std::string> trace_tags;
  bool needs_ack{}, wire_terminal{}, ignored{};
  bool proves_terminal(Handle h) const {
    return authentic_ && proof_terminal_ && proof_hop_ == h;
  }
  Tick proven_terminal_time() const {
    return proof_time_;
  }

private:
  friend class ProtocolEngine;
  bool authentic_{}, proof_terminal_{};
  Handle proof_hop_;
  Tick proof_time_{};
};
struct ProtocolDrainPlan {
  Handle hop;
  bool wait_request_release{}, wait_response{}, send_end_response{}, terminal{};
};
struct CallFailureRecord {
  WireCall call;
  std::optional<WireReturn> returned;
  Error error;
};
// Finite, declarative extension rules: base phases cannot be overridden.
struct ProtocolRule {
  WireState predecessor;
  Flow flow;
  PhaseId phase;
  Sync sync;
  std::optional<PhaseId> returned_phase;
  WireState next_state;
  bool response_required{};
};
// Persistent descriptor mirror of LeanAT.Protocol.Package (M05), not callbacks.
enum class ProtocolGuardKind { Always, FieldEq, And, Not };
struct ProtocolGuard {
  ProtocolGuardKind kind{ProtocolGuardKind::Always};
  std::uint32_t field{};
  std::uint64_t value{};
  std::vector<ProtocolGuard> operands;
};
enum class ProtocolActionKind {
  OpenHop,
  RequestReleased,
  ResponseReady,
  CloseHop,
  AcquireLane,
  ReleaseLane,
  TraceTag
};
struct ProtocolAction {
  ProtocolActionKind kind;
  std::uint32_t lane{};
  std::string tag;
};
struct ProtocolPhase {
  std::string name;
  Flow flow;
};
struct FiniteProtocolRule {
  std::uint32_t pre{};
  Flow flow{};
  PhaseId phase{};
  Sync sync{};
  std::optional<PhaseId> returned;
  ProtocolGuard guard;
  std::optional<std::int32_t> priority;
  std::vector<std::uint32_t> entry_lanes;
  std::vector<ProtocolAction> call_actions, return_actions;
  std::uint32_t post{};
  ProtocolGuard call_guard;
};
struct ProtocolPackage {
  std::string name, version;
  std::vector<ProtocolPhase> phases;
  std::vector<std::string> states;
  std::uint32_t initial{};
  std::vector<std::uint32_t> terminal;
  std::vector<std::uint32_t> lanes;
  std::vector<FiniteProtocolRule> rules;
};
Expected<void> validate_protocol_package(const ProtocolPackage &);
class ProtocolBinding;
class ProtocolRetirement;
class ProtocolEngine {
  friend class ProtocolBinding;
  friend class ProtocolRetirement;
  ProtocolEngine(const ProtocolEngine &) = default;
  ProtocolEngine &operator=(const ProtocolEngine &) = default;
  struct Ledger {
    WireSnapshot snapshot;
    Handle handle;
    std::set<std::uint64_t> calls;
    std::optional<VersionedCell> ack;
    Command request_command{Command::Ignore};
    std::size_t request_length{};
  };
  struct Pending {
    Handle handle, hop;
    WireCall call;
    WireState pre;
    Tick input;
    bool ignored{};
    std::optional<WireReturn> failed_return;
    std::optional<Error> failure;
    std::optional<std::size_t> finite_rule;
    std::vector<ProtocolMilestone> draft;
  };
  DomainId domain_;
  InstanceId side_;
  std::uint32_t store_id_;
  std::size_t ledger_limit_, ticket_limit_, history_limit_;
  std::uint64_t generation_{1};
  std::map<Handle, Ledger> ledgers_;
  std::map<Handle, LedgerIdentity> prepared_bindings_;
  std::map<Handle, const EventTxn *> prepared_retirements_;
  std::map<Handle, Pending> pending_;
  std::map<ConnectionId, Handle> requests_, responses_;
  std::vector<ProtocolRule> rules_;
  std::set<std::uint32_t> ignorable_;
  std::optional<ProtocolPackage> package_;
  using LaneRegistry = std::map<std::pair<ConnectionId, std::uint32_t>, std::set<Handle>>;
  LaneRegistry finite_lanes_;
  Expected<void> finite_actions(const std::vector<ProtocolAction> &, Handle, const WireCall &,
                                const WireReturn *, Tick, WireState &, LaneRegistry &,
                                std::vector<ProtocolMilestone> &) const;
  Expected<Ledger *> ledger(Handle);
  void release(std::map<ConnectionId, Handle> &, ConnectionId, Handle);

public:
  struct CounterSnapshot {
    std::uint64_t next_generation;
    std::size_t ledgers, pending_calls, prepared_bindings, prepared_retirements;
    std::size_t ledger_capacity, ticket_capacity, calls_per_ledger;
  };
  CounterSnapshot counter_snapshot() const noexcept {
    return {generation_, ledgers_.size(), pending_.size(), prepared_bindings_.size(),
            prepared_retirements_.size(), ledger_limit_, ticket_limit_, history_limit_};
  }
  ProtocolEngine(DomainId, InstanceId, std::size_t ledger_capacity, std::size_t ticket_capacity,
                 std::size_t calls_per_ledger);
  Expected<Handle> create_ledger(ConnectionId, TransportId, std::uint64_t transport_generation = 1,
                                 std::uint64_t owner = 0);
  Expected<void> bind_ledger(Handle hop, ConnectionId, TransportId,
                             std::uint64_t transport_generation = 1);
  Expected<void> prepare_bind_ledger(EventTxn &, Handle, ConnectionId, TransportId,
                                     std::uint64_t transport_generation = 1);
  Expected<WireSnapshot> prepared_inspect(const EventTxn &, Handle) const;
  Expected<void> stage_retire_unstarted(EventTxn &, Handle);
  Expected<void> retire_ledger(Handle);
  Expected<void> discard_unstarted_ledger(Handle);
  Expected<void> install_rules(std::vector<ProtocolRule>);
  Expected<void> install_package(ProtocolPackage);
  Expected<void> allow_ignorable(PhaseId);
  Expected<CallTicket> begin_call(Handle, const WireCall &);
  Expected<CallTicket> begin_call(const WireCall &);
  Expected<Handle> find_ledger(ConnectionId, TransportId) const;
  Expected<ExchangeResult> end_call(CallTicket, const WireReturn &);
  Expected<ExchangeResult> validate_end_call(CallTicket, const WireReturn &) const;
  Expected<void> fail_call(CallTicket, Error);
  Expected<CallFailureRecord> inspect_failure(CallTicket) const;
  Expected<WireSnapshot> inspect(Handle) const;
  Expected<VersionedCell *> ack_cell(Handle, std::uint64_t epoch);
  Expected<ProtocolDrainPlan> request_local_cancel(Handle);
  bool request_lane_free(ConnectionId) const;
  bool response_lane_free(ConnectionId) const;
  bool base_protocol() const {
    return !package_;
  }
};
using WireLedger = ProtocolEngine;
} // namespace leanat
