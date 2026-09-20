#pragma once
#include "common.hpp"
#include "event_txn.hpp"
namespace leanat {
enum class ProcessState { Runnable, Executing, Suspended, ResumeQueued, Terminal };
enum class SingleWaitKind {
  Response,
  Terminal,
  Phase,
  After,
  Until,
  Internal,
  InputChanged,
  RequestGateReady
};
enum class SingleWaitStatus { Ready, Timeout, Cancelled, Skipped, Terminated, Error };
struct SingleWaitSpec {
  SingleWaitKind kind{SingleWaitKind::Until};
  Handle source{};
  ConnectionId connection{};
  PhaseId phase{};
  PortId port{};
  Duration after{};
  Tick until{};
  std::uint64_t sequence{};
};
struct SingleWaitOutcome {
  SingleWaitStatus status{SingleWaitStatus::Ready};
  Value value;
  ReadyKey source;
  std::uint64_t source_sequence{};
};
struct ResumeFrame {
  BlockId resume_block{};
  std::vector<Value> live;
};
struct SuspensionToken {
  Handle process, wait;
  std::uint64_t ordinal{};
  friend bool operator==(const SuspensionToken &a, const SuspensionToken &b) {
    return a.process == b.process && a.wait == b.wait && a.ordinal == b.ordinal;
  }
};
struct ResumeAction {
  SuspensionToken token;
  ResumeFrame frame;
  SingleWaitOutcome outcome;
};
struct ProcessSnapshot {
  ProgramId program{};
  InstanceId instance{};
  bool instance_bound{};
  ProcessState state{ProcessState::Runnable};
  std::uint64_t suspension_ordinal{};
  std::optional<SuspensionToken> suspension;
};
// One atomic API installs both wait and owned frame; no external callback or yield occurs.
class ProcessStore {
  friend class PreparedProcessMutation;
  friend class StructuredServices;
  ProcessStore(const ProcessStore &) = default;
  ProcessStore &operator=(const ProcessStore &) = delete;
  struct Frame {
    Handle handle;
    ProcessSnapshot info;
    ResumeFrame resume;
  };
  struct Wait {
    Handle handle;
    SingleWaitSpec spec;
    SuspensionToken token;
    std::optional<SingleWaitOutcome> outcome;
  };
  DomainId domain_;
  std::uint32_t store_;
  std::size_t frames_limit_, waits_limit_, live_limit_;
  std::uint64_t generation_{1};
  std::map<Handle, Frame> frames_;
  std::map<Handle, Wait> waits_;
  bool transaction_pending_{};
  EventTxn *pending_txn_{};
  ProcessStore *pending_view_{};
  Expected<Frame *> frame(Handle);
  Expected<void> authorize(Handle, const ExecutionContext &);
  Expected<void> stage_copy(std::unique_ptr<ProcessStore>, EventTxn &);
  Expected<std::unique_ptr<ProcessStore>> draft(EventTxn &);
  // Only the scope adapter may use these after validating scope ownership.
  // Domain, instance and complete process/wait identities remain mandatory.
  Expected<Handle> create_controlled(ProgramId, std::uint64_t owner, EventTxn &);
  Expected<void> cancel_controlled(Handle process, EventTxn &);
  Expected<void> cancel_wait_controlled(Handle wait, Handle expected_process, EventTxn &);

public:
  struct StagingFootprint {
    std::size_t mutation_bytes{}, frame_entry_bytes{}, wait_bytes{};
  };
  // Fixed terms of reserved_bytes(); owning live/outcome Values are additional.
  static StagingFootprint staging_footprint() noexcept;
  ProcessStore(ProcessStore &&) = delete;
  ProcessStore &operator=(ProcessStore &&) = delete;
  ProcessStore(DomainId, std::uint32_t store, std::size_t frame_capacity, std::size_t wait_capacity,
               std::size_t live_capacity);
  Expected<Handle> create(ProgramId, std::uint64_t owner);
  Expected<Handle> create(ProgramId, std::uint64_t owner, EventTxn &);
  Expected<void> begin(Handle);
  Expected<void> begin(Handle, EventTxn &);
  Expected<SuspensionToken> suspend(Handle, const SingleWaitSpec &, ResumeFrame, ReadyKey,
                                    std::optional<SingleWaitOutcome> latched = {});
  Expected<SuspensionToken> suspend(Handle, const SingleWaitSpec &, ResumeFrame, ReadyKey,
                                    EventTxn &, std::optional<SingleWaitOutcome> latched = {});
  Expected<Handle> register_wait(Handle, const SingleWaitSpec &, EventTxn &);
  Expected<SuspensionToken> suspend_registered(Handle wait, ResumeFrame, EventTxn &);
  Expected<SuspensionToken> suspension_for_wait(Handle) const;
  Expected<SingleWaitSpec> wait_spec(Handle, EventTxn *prepared = nullptr) const;
  Expected<ResumeAction> notify(const SuspensionToken &, SingleWaitOutcome);
  Expected<ResumeAction> notify(const SuspensionToken &, SingleWaitOutcome, EventTxn &);
  std::vector<SuspensionToken> waiting_on(SingleWaitKind, Handle source) const;
  Expected<ResumeAction> take_resume(const SuspensionToken &);
  Expected<ResumeAction> take_resume(const SuspensionToken &, EventTxn &);
  Expected<SingleWaitOutcome> read_wait_result(Handle) const;
  Expected<void> cancel(Handle);
  Expected<void> complete(Handle);
  Expected<void> complete(Handle, EventTxn &);
  Expected<void> cancel(Handle, EventTxn &);
  Expected<ProcessSnapshot> inspect(Handle) const;
  std::size_t frame_count() const {
    return frames_.size();
  }
  std::size_t wait_count() const {
    return waits_.size();
  }
  struct CounterSnapshot {
    DomainId domain;
    std::uint32_t store{};
    std::uint64_t next_generation{};
    std::size_t frame_capacity{}, wait_capacity{}, live_capacity{}, frames{}, waits{};
  };
  CounterSnapshot counter_snapshot() const {
    return {domain_,      store_,      generation_,    frames_limit_,
            waits_limit_, live_limit_, frames_.size(), waits_.size()};
  }
};
} // namespace leanat
