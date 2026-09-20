#pragma once
#include "cancel_scope.hpp"
#include "result_store.hpp"
#include <deque>
namespace leanat {
class TaskPoolDependencies;
enum class TaskOverflow { Reject, AwaitSlot, Queue };
enum class TaskState { Queued, Runnable, Waiting, Completed, Failed, Cancelled };
enum class SpawnTicketState { Pending, Granted, Consumed, Cancelled, Failed };
struct TaskPoolDesc {
  std::size_t max_instances{1}, task_capacity{64}, result_capacity{64}, queue_depth{},
      waiter_limit{}, args_bytes{4096}, result_bytes{4096};
  TaskOverflow overflow{TaskOverflow::Reject};
  TypeId result_type{};
};
struct TaskOutcome {
  enum class Kind { Success, Error, Cancelled };
  Kind kind{Kind::Success};
  Value value;
};
struct SpawnTicketInfo {
  Handle ticket, waiter, scope;
  SpawnTicketState state;
  std::optional<Error> error;
};
class TaskPool {
  friend class PreparedTasks;
  friend class TaskPoolDependencies;
  TaskPool(const TaskPool &) = default;
  TaskPool &operator=(const TaskPool &) = delete;
  struct Task {
    bool used{}, reserved{};
    std::uint64_t generation{1};
    Handle scope;
    Handle execution;
    TaskState state;
    std::vector<Value> args;
    ReservedResult result;
    bool producer_released{};
  };
  struct Ticket {
    bool used{};
    std::uint64_t generation{1};
    SpawnTicketInfo info;
    bool consumer_released{};
    bool waiting{};
    std::optional<ReservedResult> reservation;
    std::optional<Handle> task;
  };
  DomainId domain_;
  std::uint32_t id_;
  TaskPoolDesc desc_;
  ResultStore &results_;
  CancelScopeStore *scopes_;
  std::vector<Task> tasks_;
  std::vector<Ticket> tickets_;
  std::deque<Handle> queue_, pending_;
  std::size_t frames_{}, grants_{};
  std::size_t transaction_pending_{};
  EventTxn *transaction_{};
  TaskPoolDependencies *dependencies_{};
  Expected<const TaskPool *> dependency_view(EventTxn *) const;
  std::shared_ptr<std::uint64_t> allocation_generation_{std::make_shared<std::uint64_t>(2)};
  Expected<ReservedResult> reserve_result(ResultCreate);
  Expected<PublicationReceipt> publish_result(ResultOwnerHandle, Value, PublicationContext);
  Expected<void> release_cap(ResultHandle, ReleasePolicy = ReleasePolicy::CurrentConsumer);
  Expected<void> release_producer(ResultOwnerHandle);
  Expected<Value> read_result(ResultHandle);
  Expected<Task *> get(Handle);
  Expected<Ticket *> ticket(Handle);
  void reap();
  void schedule();
  Expected<Handle> spawn(std::vector<Value>, Handle, bool, bool);
  Expected<void> check_args(const std::vector<Value> &) const;

public:
  struct TaskSnapshot { Handle identity,scope,process; TaskState state; std::vector<Value> arguments; ReservedResult result; bool producer_released; };
  struct TicketSnapshot { SpawnTicketInfo info; bool waiting,consumer_released; std::optional<ReservedResult> reservation; std::optional<Handle> task; };
  struct Snapshot { std::vector<TaskSnapshot> tasks; std::vector<TicketSnapshot> tickets; std::vector<Handle> queue,pending; std::size_t frames,grants; };
  Expected<Snapshot> snapshot() const;
  Expected<TaskPool *> prepare(EventTxn &);
  TaskPool(DomainId, std::uint32_t, TaskPoolDesc, ResultStore &, CancelScopeStore * = nullptr);
  Expected<Handle> try_spawn(std::vector<Value>, Handle scope, ContextKind = ContextKind::Process);
  Expected<Handle> submit_queued(std::vector<Value>, Handle scope,
                                 ContextKind = ContextKind::Process);
  Expected<Handle> request_slot(Handle waiter, Handle scope, ContextKind = ContextKind::Process);
  Expected<Handle> spawn_reserved(Handle ticket, Handle waiter, Handle scope, std::vector<Value>);
  Expected<SpawnTicketInfo> ticket_info(Handle);
  // Trusted runtime binding; a process can hold at most one frame in this pool.
  Expected<void> bind_execution(Handle task, Handle process);
  // Call immediately before suspending on a Pending ticket. Failure installs no wait.
  Expected<void> begin_slot_wait(Handle ticket, Handle process, Handle scope);
  Expected<void> cancel_ticket(Handle, Handle waiter, Handle scope);
  Expected<void> release_ticket(Handle);
  Expected<void> complete(Handle, TaskOutcome, ReadyKey);
  Expected<void> cancel(Handle, std::string reason, ReadyKey);
  Expected<void> execute_cancel_action(const ScopeCancelAction &, ReadyKey);
  Expected<TaskState> state(Handle);
  Expected<std::vector<Value>> arguments(Handle);
  Expected<ResultHandle> result_handle(Handle, Handle scope);
  Expected<PinnedResultView> pin_result(Handle, ResultHandle);
  Expected<Value> owning_result(Handle, ResultHandle);
  Expected<void> release_result(Handle, ResultHandle);
  Expected<Value> consume_result(Handle, ResultHandle);
  std::vector<Handle> members(Handle scope);
  std::size_t active_frames() const {
    return frames_;
  }
  std::size_t granted_reservations() const {
    return grants_;
  }
  const TaskPoolDesc &descriptor() const {
    return desc_;
  }
  std::uint64_t next_allocation_generation() const noexcept { return *allocation_generation_; }
};
// Host-owned bounded registry. Pools must outlive the registry and remain at stable addresses.
class TaskPoolDependencies {
  std::size_t capacity_;
  std::vector<TaskPool *> pools_;
  friend class TaskPool;
  static Expected<void> check(const std::vector<TaskPool *> &, TaskPool &, Handle);

public:
  explicit TaskPoolDependencies(std::size_t capacity) : capacity_(capacity) {}
  TaskPoolDependencies(const TaskPoolDependencies &) = delete;
  TaskPoolDependencies &operator=(const TaskPoolDependencies &) = delete;
  ~TaskPoolDependencies();
  Expected<void> add(TaskPool &);
};
Value encode_task_outcome(const TaskOutcome &);
} // namespace leanat
