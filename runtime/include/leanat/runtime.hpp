#pragma once
#include "drain.hpp"
#include "event_queue.hpp"
#include "process.hpp"
#include "protocol.hpp"
#include "result_store.hpp"
namespace leanat {
class AdmissionStore;
struct RequestPermit;
struct RuntimeConnectionBinding {
  ConnectionId connection;
  InstanceId initiator, target;
  friend bool operator==(const RuntimeConnectionBinding &a, const RuntimeConnectionBinding &b) {
    return a.connection == b.connection && a.initiator == b.initiator && a.target == b.target;
  }
};
struct RuntimeConfig {
  DomainId domain{};
  InstanceId instance{};
  std::string descriptor_identity;
  std::vector<ConnectionId> connections;
  std::vector<InstanceId> instances;
  std::vector<RuntimeConnectionBinding> connection_bindings;
  std::size_t instance_capacity{64};
  std::size_t event_capacity{256}, ledger_capacity{128}, call_capacity{256}, calls_per_ledger{64},
      intent_capacity{128}, frame_capacity{64}, wait_capacity{128}, live_capacity{64},
      drain_capacity{128}, hops_per_transaction{8};
  std::size_t result_capacity{128}, consumer_capacity{256}, pin_capacity{128};
  std::uint64_t max_events{100000}, max_events_per_tick{10000};
  std::size_t ingress_bytes{65536}, nesting_capacity{16};
  std::size_t blocking_capacity{128};
};
struct HostBindingManifest {
  DomainId domain{};
  std::string descriptor_identity;
  std::vector<ConnectionId> connections;
  bool clock_grid_valid{}, capabilities_valid{};
  std::vector<InstanceId> instances;
  std::vector<RuntimeConnectionBinding> connection_bindings;
};
enum class CallOrigin { Outgoing, ExternalIngress };
enum class ProgressDisposition { ContinueDispatch, Stopped };
enum class PumpStopReason { Quiescent, WaitingForEnvironment, BatchComplete, LimitReached };
struct PumpResult {
  std::uint32_t executed_events{};
  std::optional<WakePoint> next_wake;
  PumpStopReason stop_reason{PumpStopReason::Quiescent};
  ProgressDisposition progress{ProgressDisposition::ContinueDispatch};
  std::optional<BatchId> closed_batch_id;
  bool batch_complete{};
  std::string limit_detail;
};
struct RuntimeMilestone {
  Handle hop;
  CallId call_id;
  ProtocolMilestone milestone;
  ReadyKey ready;
};
// Observation of a real queue dispatch, retaining its owning protocol snapshot.
// The EventKey is the scheduler key; identity.connection is the wire connection.
struct RuntimeMilestoneObservation {
  EventKey key;
  EventToken event;
  LedgerIdentity identity;
  RuntimeMilestone causal;
};
struct IngressPlan {
  WireReturn reply;
  bool complete_now{};
  std::unique_ptr<PreparedParticipant> prepared_service;
};
struct BlockingRequest {
  Handle token;
  ConnectionId connection;
  InstanceId instance;
  PayloadSnapshot request;
  Tick arrival;
  std::uint64_t epoch{};
};
class RuntimeHost {
public:
  virtual ~RuntimeHost() = default;
  virtual Expected<WireReturn> transport(const SendIntent &) = 0;
  virtual void arm(std::optional<WakePoint>) = 0;
  virtual void publish_output(PortId, const Value &) = 0;
  virtual void publish_output(InstanceId, PortId port, const Value &value) {
    publish_output(port, value);
  }
  virtual void emit_trace(const TraceEvent &) = 0;
  virtual void recorded_return(CallId, bool) {}
  virtual void observe_milestone(const RuntimeMilestoneObservation &) {}
};
class Runtime {
  friend class PreparedIntent;
  friend class PreparedCleanup;
  friend class PreparedCleanupRetirement;
  friend class PreparedRequestPermit;
  const RuntimeConfig config_;
  RuntimeHost &host_;
  EventQueue events_;
  ProtocolEngine protocol_;
  std::map<InstanceId, std::unique_ptr<ProtocolEngine>> protocols_;
  std::uint32_t business_store_{};
  std::uint64_t next_business_{1};
  ResultStore results_;
  ProcessStore processes_;
  DrainStore drains_;
  bool started_{}, stopped_{}, pumping_{}, dirty_{};
  std::size_t depth_{}, host_calls_{};
  Tick now_{};
  std::uint64_t next_call_{1}, wake_generation_{}, event_count_{}, tick_events_{};
  Tick count_tick_{};
  struct Outgoing {
    CallTicket ticket;
    std::vector<EventReservation> reserved;
    ProtocolEngine *engine{};
  };
  struct CleanupBinding {
    Handle transaction, hop;
    bool initiator{};
    PayloadSnapshot request;
    bool scheduled{};
    AdmissionStore *admission{};
    bool cancelled{};
  };
  std::map<CallId, CallOrigin> allocated_;
  std::map<CallId, Outgoing> outgoing_;
  std::map<CallId, SendIntent> intents_;
  std::map<CallId, const SendIntent *> prepared_intents_;
  std::map<CallId, bool> cleanup_intents_;
  struct IntentHooks {
    std::function<Expected<void>()> start;
    std::function<Expected<void>(ReadyKey)> cancel;
  };
  std::map<CallId, IntentHooks> intent_hooks_;
  std::map<CallId, bool> prepared_hooks_;
  std::map<std::pair<InstanceId, ConnectionId>, AdmissionStore *> admission_bindings_;
  std::map<EventToken, std::pair<InstanceId, ConnectionId>> admission_events_;
  std::map<Handle, bool> open_wires_;
  std::map<Handle, std::variant<WireCall, RuntimeMilestone>> wire_events_;
  std::map<std::pair<Handle, MilestoneKind>, RuntimeMilestone> latches_;
  std::map<Handle, CleanupBinding> cleanup_;
  std::map<Handle, std::pair<EventTxn *, const CleanupBinding *>> prepared_cleanup_;
  Expected<void> stage_untrack_cleanup(EventTxn &, Handle hop);
  Expected<void> stage_mark_cleanup_cancelled(EventTxn &, Handle hop);
  Expected<ProtocolEngine *> retirement_engine(Handle hop);
  struct BlockingRecord {
    BlockingRequest request;
    std::function<void(Expected<ResponseSnapshot>)> completion;
    std::optional<ResponseSnapshot> response;
    bool deferred{};
  };
  std::map<Handle, BlockingRecord> blocking_;
  std::map<EventToken, std::pair<Handle, bool>> blocking_events_;
  std::function<Expected<void>(const BlockingRequest &, Runtime &)> blocking_handler_;
  std::vector<std::pair<Handle, CancelReason>> pending_cancel_;
  std::vector<std::pair<InstanceId, ResetPolicy>> pending_reset_;
  std::function<Expected<void>(const QueuedEvent &, Runtime &)> handler_;
  std::string stop_detail_;
  std::function<Expected<void>(const WireCall &, ReadyKey, Runtime &)> input_handler_;
  std::function<Expected<void>(const RuntimeMilestone &, ReadyKey, Runtime &)> milestone_handler_;
  std::function<Expected<void>(const SuspensionToken &, ReadyKey, Runtime &)> resume_handler_;
  std::function<Expected<void>(BatchId, ReadyKey, Runtime &)> batch_epilogue_;
  std::optional<BatchId> epilogue_done_;
  std::function<Expected<IngressPlan>(const WireCall &)> ingress_policy_;
  struct Entry {
    Runtime &runtime;
    explicit Entry(Runtime &r) : runtime(r) {
      ++runtime.depth_;
    }
    ~Entry() {
      if (!--runtime.depth_)
        runtime.reconcile();
    }
  };
  void reconcile();
  void stop(Error);
  Expected<void> operational() const;
  Expected<void> dispatch_intents();
  Expected<void> boundary();
  Expected<std::vector<EventReservation>> reserve_return_events();
  Expected<void> enqueue_exchange(const ExchangeResult &, std::vector<EventReservation> &);
  Expected<void> progress_cleanup(Handle);
  Expected<void> promote_blocking();
  Expected<void> dispatch_blocking(EventToken, ReadyKey);
  Expected<ProtocolEngine *> engine_for_hop(Handle);

public:
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Runtime(Runtime &&) = delete;
  Runtime &operator=(Runtime &&) = delete;
  Runtime(const RuntimeConfig &, RuntimeHost &);
  Expected<InstanceId> call_side(ConnectionId, Flow, bool outgoing) const;
  Expected<ProtocolEngine *> protocol(InstanceId);
  Expected<WireSnapshot> inspect_hop(Handle);
  Expected<void> start(const HostBindingManifest &);
  Expected<CallId> allocate_call_id(CallOrigin);
  Expected<CallId> stage_allocate_call_id(EventTxn &, CallOrigin);
  Expected<void> release_call_id(CallId);
  Expected<WireReturn> ingress(const WireCall &);
  Expected<void> start_outbound(const SendIntent &, const WireCall &);
  Expected<void> record_return(CallId, const WireReturn &);
  void report_host_failure(Error);
  Expected<PumpResult> pump_batch(Tick, std::uint32_t budget);
  Expected<Handle> schedule(EventDraft);
  Expected<void> publish(SendIntent);
  Expected<void> stage_publish(EventTxn &, SendIntent);
  Expected<void> stage_request_permit(EventTxn &, CallId, AdmissionStore &, RequestPermit);
  Expected<CommittedActions> commit_segment(EventTxn &);
  Expected<void> track_cleanup(Handle transaction, Handle hop, bool local_initiator,
                               PayloadSnapshot request, AdmissionStore *admission = nullptr);
  Expected<void> stage_track_cleanup(EventTxn &, Handle transaction, Handle hop,
                                     bool local_initiator, PayloadSnapshot request,
                                     AdmissionStore *admission = nullptr);
  Expected<void> untrack_cleanup(Handle hop);
  Expected<RuntimeMilestone> milestone(Handle, MilestoneKind) const;
  Expected<SingleWaitOutcome> response_wait_outcome(Handle) const;
  Expected<void> retire_hop(Handle);
  Expected<CancelDisposition> cancel_local(Handle, CancelReason);
  Expected<CancelDisposition> stage_cancel_local(EventTxn &, Handle, CancelReason);
  Expected<std::vector<CancelDisposition>>
  stage_cancel_local_many(EventTxn &, const std::vector<Handle> &, CancelReason);
  bool has_unstarted_intent(Handle transaction) const;
  std::vector<SendIntent> pending_intents() const {
    std::vector<SendIntent> snapshots;
    snapshots.reserve(intents_.size());
    for (const auto &entry : intents_)
      snapshots.push_back(entry.second);
    return snapshots;
  }
  struct CounterSnapshot {
    std::uint64_t next_call{}, wake_generation{};
    std::size_t allocated_calls{};
    std::size_t intent_capacity{}, call_capacity{}, ledger_capacity{}, drain_capacity{};
    std::size_t prepared_intents{}, committed_intents{};
  };
  CounterSnapshot counter_snapshot() const {
    return {next_call_,
            wake_generation_,
            allocated_.size(),
            config_.intent_capacity,
            config_.call_capacity,
            config_.ledger_capacity,
            config_.drain_capacity,
            prepared_intents_.size(),
            intents_.size()};
  }
  Expected<EventToken> stage_resume(EventTxn &, const SuspensionToken &, ReadyKey);
  Expected<void> notify_process(const SuspensionToken &, SingleWaitOutcome);
  Expected<Handle> submit_blocking(ConnectionId, PayloadSnapshot, Tick,
                                   std::function<void(Expected<ResponseSnapshot>)>);
  Expected<void> complete_blocking(Handle, ResponseSnapshot);
  Expected<void> cancel_blocking(Handle);
  void
  set_blocking_handler(std::function<Expected<void>(const BlockingRequest &, Runtime &)> handler) {
    blocking_handler_ = std::move(handler);
  }
  Expected<EventToken> stage_output(EventTxn &, PortId, Value);
  Expected<ResetDisposition> reset(InstanceId, ResetPolicy);
  Expected<Value> take_result(ResultHandle h) {
    return results_.take(h);
  }
  std::optional<WakePoint> next_wakeup() const;
  void set_handler(std::function<Expected<void>(const QueuedEvent &, Runtime &)> h) {
    handler_ = std::move(h);
  }
  void set_input_handler(std::function<Expected<void>(const WireCall &, ReadyKey, Runtime &)> h) {
    input_handler_ = std::move(h);
  }
  void set_milestone_handler(
      std::function<Expected<void>(const RuntimeMilestone &, ReadyKey, Runtime &)> h) {
    milestone_handler_ = std::move(h);
  }
  void set_ingress_policy(std::function<Expected<IngressPlan>(const WireCall &)> h) {
    ingress_policy_ = std::move(h);
  }
  void set_resume_handler(
      std::function<Expected<void>(const SuspensionToken &, ReadyKey, Runtime &)> h) {
    resume_handler_ = std::move(h);
  }
  void set_batch_epilogue(std::function<Expected<void>(BatchId, ReadyKey, Runtime &)> h) {
    batch_epilogue_ = std::move(h);
  }
  EventQueue &queue() {
    return events_;
  }
  ProtocolEngine &protocol() {
    return protocol_;
  }
  ResultStore &results() {
    return results_;
  }
  ProcessStore &processes() {
    return processes_;
  }
  DrainStore &drains() {
    return drains_;
  }
  Tick now() const {
    return now_;
  }
  bool stopped() const {
    return stopped_;
  }
  const std::string &stop_detail() const {
    return stop_detail_;
  }
};
} // namespace leanat
