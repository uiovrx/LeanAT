#pragma once
#include "runtime_services.hpp"
#include "task_pool.hpp"
#include "wait_group.hpp"
namespace leanat {
// Explicit argument layouts are documented in docs/PROJECT.md.
class StructuredServices {
public:
  struct WaitOwnership {
    Handle process, scope;
    ResultHandle primary;
  };

private:
  friend class PreparedStructuredBindings;
  struct State {
    std::map<Handle, WaitOwnership> wait_owners;
    std::map<Handle, Handle> waits, processes, starts;
    std::map<Handle, ProgramId> programs;
  };
  State state_;
  std::size_t pending_{};
  Runtime *runtime_{};
  CancelScopeStore &scopes_;
  TaskPool &tasks_;
  WaitGroupStore &waits_;
  Expected<State *> prepare_bindings(EventTxn &);
  Expected<void> dispatch_ready(State &, TaskPool &, const ExecutionContext &, EventTxn &);
  Expected<void> cancel_execution(State &, Handle, EventTxn &);

public:
  struct Snapshot {
    std::map<Handle, Handle> waits, processes, starts;
    std::map<Handle, ProgramId> programs;
    std::map<Handle, WaitOwnership> wait_owners;
  };
  Expected<Snapshot> snapshot() const {
    if (pending_)
      return fail(ErrorCode::NotReady, "structured bindings transaction pending");
    return Snapshot{state_.waits, state_.processes, state_.starts, state_.programs,
                    state_.wait_owners};
  }
  StructuredServices(CancelScopeStore &s, TaskPool &t, WaitGroupStore &w)
      : scopes_(s), tasks_(t), waits_(w) {}
  StructuredServices(Runtime &r, CancelScopeStore &s, TaskPool &t, WaitGroupStore &w)
      : runtime_(&r), scopes_(s), tasks_(t), waits_(w) {}
  Expected<void> bind(CoreRuntimeBackend &, exec::ServiceSignature,
                      ProgramId spawned_program = ProgramId{});
  // Descriptor-bound process policy: caller supplies the owning scope capability;
  // its primary consumer survives completion until explicit release or scope cleanup.
  Expected<void> bind(CoreRuntimeBackend &, exec::ServiceSignature, const exec::Project &,
                      ProgramId spawned_program);
  Expected<void> complete(Handle, TaskOutcome, ReadyKey, EventTxn &);
  struct TaskStart {
    Handle process;
    ProgramId program;
    std::vector<Value> arguments;
  };
  Expected<TaskStart> start_task(Handle, const ExecutionContext &, EventTxn &);
  Expected<Handle> execution_process(Handle) const;
  Expected<void> resolve(BatchId, ReadyKey);
};
} // namespace leanat
