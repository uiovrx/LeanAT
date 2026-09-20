#pragma once
#include "leanat/admission.hpp"
#include "leanat/drain.hpp"
#include "leanat/task_pool.hpp"
#include "leanat/wait_group.hpp"
namespace leanat {
struct TaskGroupView {
  TaskPool *pool{};
  Handle owner;
  std::uint64_t tag{};
  std::shared_ptr<std::vector<Handle>> membership{std::make_shared<std::vector<Handle>>()};
};
struct IdleSnapshot {
  std::vector<Handle> tasks;
};
Expected<TaskGroupView> make_task_group(TaskPool &, Handle, std::uint64_t tag);
Expected<Handle> group_try_spawn(TaskGroupView &, std::vector<Value>,
                                 ContextKind = ContextKind::Process);
Expected<Handle> group_submit(TaskGroupView &, std::vector<Value>,
                              ContextKind = ContextKind::Process);
Expected<IdleSnapshot> snapshot_idle(const TaskGroupView &, ContextKind = ContextKind::Process);
Expected<bool> idle_ready(TaskPool &, const IdleSnapshot &);
Expected<void> cancel_children(TaskGroupView &, std::string, ReadyKey);
struct OwnedChild {
  Handle task;
  ResultHandle consumer;
};
struct OwnedChildSet {
  Handle scope;
  std::vector<OwnedChild> children;
  OwnedChildSet() = default;
  OwnedChildSet(const OwnedChildSet &) = delete;
  OwnedChildSet &operator=(const OwnedChildSet &) = delete;
  OwnedChildSet(OwnedChildSet &&) = default;
  OwnedChildSet &operator=(OwnedChildSet &&) = default;
};
Expected<OwnedChildSet> fan_out(TaskGroupView &, std::vector<std::vector<Value>>, ReadyKey,
                                ContextKind = ContextKind::Process);
Expected<std::vector<Value>> fan_in(TaskPool &, OwnedChildSet &,
                                    ContextKind = ContextKind::Process);
enum class TimeoutState {
  Fresh,
  GateWaiting,
  ResponseWaiting,
  FinishingResponse,
  Success,
  TimedOut,
  ModelError
};
struct TimeoutResult {
  TimeoutState state;
  std::optional<Value> response;
  std::optional<Handle> drain;
  bool published{};
};
class TimeoutOperation {
  AdmissionStore &admission_;
  Handle txn_;
  ConnectionId connection_;
  Tick deadline_;
  ReadyKey action_key_{};
  bool transferred_publication_{};
  std::function<bool()> wire_published_;
  TimeoutState state_{TimeoutState::Fresh};
  std::optional<RequestGateTicket> gate_;
  std::optional<RequestPermit> permit_;
  std::optional<WaitNotification> response_;
  std::optional<ReadyKey> timeout_;
  TimeoutResult result_{TimeoutState::Fresh, {}, {}, false};
  std::function<Expected<std::optional<Handle>>()> drain_;
  Expected<void> expire();

public:
  TimeoutOperation(AdmissionStore &, Handle, ConnectionId, Tick,
                   std::function<Expected<std::optional<Handle>>()> drain);
  Expected<void> start(ReadyKey, ContextKind = ContextKind::Process);
  Expected<bool> advance_gate(ReadyKey, bool scope_open,
                              const std::function<Expected<void>(RequestPermit)> &publish,
                              bool deferred_publication = false,
                              std::function<bool()> wire_published = {});
  Expected<void> record_response(Value, ReadyKey);
  Expected<void> record_timeout(ReadyKey);
  Expected<TimeoutResult> resolve(bool closed_batch);
  Expected<void> finish_response(Value durable_result);
  const TimeoutResult &result() const {
    return result_;
  }
  Tick deadline() const {
    return deadline_;
  }
};
enum class RetrySafetyKind { Idempotent, Deduplicated, ProvenUnsent };
struct RetrySafety {
  RetrySafetyKind kind;
  std::string contract;
  std::optional<std::uint64_t> key, epoch;
  bool validated{};
};
struct RetryPolicy {
  std::uint32_t max_retries{};
  Duration backoff{};
  Tick deadline{};
  std::optional<Duration> attempt_timeout;
  std::size_t drain_capacity{16};
};
struct RetryAttempt {
  std::uint32_t ordinal;
  Tick start, deadline;
};
class RetryController {
  RetryPolicy policy_;
  RetrySafety safety_;
  std::uint64_t attempts_{};
  std::vector<Handle> drains_;
  std::optional<TimeoutResult> last_;
  bool awaiting_record_{};
  std::function<Expected<void>(const RetrySafety &)> verify_safety_;

public:
  RetryController(RetryPolicy, RetrySafety,
                  std::function<Expected<void>(const RetrySafety &)> verify_safety = {});
  Expected<std::optional<RetryAttempt>> next(Tick now, bool previous_retryable,
                                             bool proven_unsent = false);
  Expected<void> record(TimeoutResult);
  const std::vector<Handle> &outstanding_drains() const {
    return drains_;
  }
  std::uint64_t attempts() const {
    return attempts_;
  }
};
Expected<Tick> transaction_deadline(Tick, Duration);
Expected<Value> with_scoped_operation(
    CancelScopeStore &, TaskPool &, Handle parent, ReadyKey,
    const std::function<Expected<Value>(Handle child_scope)> &body,
    const std::function<Expected<void>(const ScopeCancelAction &)> &external_cleanup = {});
} // namespace leanat
