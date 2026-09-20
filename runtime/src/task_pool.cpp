#include "leanat/task_pool.hpp"
#include "leanat/storage_identity.hpp"
namespace leanat {
Expected<TaskPool::Snapshot> TaskPool::snapshot() const {
  if(transaction_pending_)return fail(ErrorCode::NotReady,"task snapshot transaction pending");
  Snapshot out;out.frames=frames_;out.grants=grants_;
  out.queue.assign(queue_.begin(),queue_.end());out.pending.assign(pending_.begin(),pending_.end());
  for(std::size_t i=0;i<tasks_.size();++i){const auto&t=tasks_[i];if(t.used)out.tasks.push_back({{HandleKind::Task,domain_,id_,static_cast<std::uint32_t>(i),t.generation,t.result.consumer.result.owner},t.scope,t.execution,t.state,t.args,t.result,t.producer_released});}
  for(const auto&t:tickets_)if(t.used)out.tickets.push_back({t.info,t.waiting,t.consumer_released,t.reservation,t.task});
  return out;
}
Value encode_task_outcome(const TaskOutcome &o) {
  return Value{Value::Array{Value{static_cast<std::uint64_t>(o.kind)}, o.value}};
}
TaskPool::TaskPool(DomainId d, std::uint32_t id, TaskPoolDesc desc, ResultStore &r,
                   CancelScopeStore *s)
    : domain_(d), id_(storage_detail::allocate_store_incarnation()), desc_(desc), results_(r),
      scopes_(s), tasks_(desc.task_capacity), tickets_(desc.waiter_limit) {
  (void)id;
  if (!desc.max_instances || !desc.task_capacity || !desc.result_capacity || !desc.args_bytes ||
      !desc.result_bytes || (desc.overflow == TaskOverflow::AwaitSlot && !desc.waiter_limit) ||
      (desc.overflow == TaskOverflow::Queue && !desc.queue_depth))
    throw std::invalid_argument("task pool descriptor");
}
Expected<TaskPool::Task *> TaskPool::get(Handle h) {
  if (transaction_pending_ && !transaction_)
    return fail(ErrorCode::InvalidState, "task store transaction pending");
  if (h.domain != domain_)
    return fail(ErrorCode::WrongDomain, "task domain");
  if (h.kind != HandleKind::Task || h.store != id_ || h.slot >= tasks_.size() ||
      !tasks_[h.slot].used || tasks_[h.slot].generation != h.generation)
    return fail(ErrorCode::StaleHandle, "task identity");
  if (h.owner != tasks_[h.slot].result.consumer.result.owner)
    return fail(ErrorCode::WrongOwner, "task handle owner");
  return &tasks_[h.slot];
}
Expected<TaskPool::Ticket *> TaskPool::ticket(Handle h) {
  if (transaction_pending_ && !transaction_)
    return fail(ErrorCode::InvalidState, "ticket store transaction pending");
  if (h.domain != domain_)
    return fail(ErrorCode::WrongDomain, "ticket domain");
  if (h.kind != HandleKind::SpawnTicket || h.store != id_ || h.slot >= tickets_.size() ||
      !tickets_[h.slot].used || tickets_[h.slot].generation != h.generation)
    return fail(ErrorCode::StaleHandle, "ticket identity");
  if (h.owner != tickets_[h.slot].info.ticket.owner)
    return fail(ErrorCode::WrongOwner, "ticket handle owner");
  return &tickets_[h.slot];
}
void TaskPool::reap() {
  auto alive = [&](ResultOwnerHandle h) {
    if (!transaction_)
      return results_.alive(h);
    auto a = results_.prepare_alive(*transaction_, h);
    return a && a.value();
  };
  for (auto &t : tasks_)
    if (t.used && t.producer_released && !alive(t.result.reservation)) {
      t.used = false;
      t.args.clear();
      if (t.generation != UINT64_MAX)
        ++t.generation;
    }
}
Expected<void> TaskPool::check_args(const std::vector<Value> &args) const {
  std::size_t bytes = 0;
  for (auto &a : args) {
    auto n = owned_value_bytes(a);
    if (n > desc_.args_bytes - bytes)
      return fail(ErrorCode::Capacity, "task arguments bytes");
    bytes += n;
  }
  return {};
}
Expected<Handle> TaskPool::spawn(std::vector<Value> args, Handle scope, bool queued,
                                 bool reserved) {
  if (transaction_pending_ && !transaction_)
    return fail(ErrorCode::InvalidState, "task store transaction pending");
  reap();
  auto valid = check_args(args);
  if (!valid)
    return valid.error();
  if (scope.domain != domain_)
    return fail(ErrorCode::WrongDomain, "task scope domain");
  if (scope.kind != HandleKind::Scope || !scope.generation)
    return fail(ErrorCode::InvalidArgument, "task scope identity kind");
  if (scopes_) {
    auto s = scopes_->state(scope);
    if (!s)
      return s.error();
    if (s.value() != ScopeState::Open)
      return fail(ErrorCode::Cancelled, "scope not open");
  }
  std::size_t used = 0;
  for (auto &t : tasks_)
    used += t.used;
  if (used + grants_ - (reserved ? 1 : 0) >= desc_.result_capacity)
    return fail(ErrorCode::Capacity, "task results full");
  if (!queued && frames_ + grants_ - (reserved ? 1 : 0) >= desc_.max_instances)
    return fail(ErrorCode::Capacity, "task frames full");
  if (queued && queue_.size() >= desc_.queue_depth)
    return fail(ErrorCode::Capacity, "task queue full");
  for (std::size_t i = 0; i < tasks_.size(); ++i)
    if (!tasks_[i].used && !tasks_[i].reserved && tasks_[i].generation != UINT64_MAX) {
      auto &t = tasks_[i];
      if (*allocation_generation_ == UINT64_MAX)
        return fail(ErrorCode::Overflow, "task generation");
      t.generation = (*allocation_generation_)++;
      Handle h{HandleKind::Task, domain_,          id_, static_cast<std::uint32_t>(i),
               t.generation,     scope.slot + 1ULL};
      auto r = reserve_result({h, desc_.result_type, desc_.result_bytes, h.owner, h.owner});
      if (!r)
        return r.error();
      if (scopes_) {
        auto a = scopes_->attach({h, OwnedKind::Task, false}, scope);
        if (!a) {
          release_cap(r.value().consumer, ReleasePolicy::DropOnTerminal);
          release_producer(r.value().reservation);
          return a.error();
        }
        auto ac =
            scopes_->attach({r.value().consumer.consumer, OwnedKind::ResultConsumer, false}, scope);
        if (!ac) {
          scopes_->detach_completed(h, scope);
          release_cap(r.value().consumer);
          release_producer(r.value().reservation);
          return ac.error();
        }
      }
      t.used = true;
      t.scope = scope;
      t.execution = {};
      t.args = std::move(args);
      t.result = r.value();
      t.producer_released = false;
      t.state = queued ? TaskState::Queued : TaskState::Runnable;
      if (queued)
        queue_.push_back(h);
      else
        ++frames_;
      return h;
    }
  return fail(ErrorCode::Capacity, "task identities full");
}
Expected<Handle> TaskPool::try_spawn(std::vector<Value> a, Handle s, ContextKind c) {
  if (c != ContextKind::Process && c != ContextKind::Timed)
    return fail(ErrorCode::InvalidState, "spawn context");
  schedule();
  return spawn(std::move(a), s, false, false);
}
Expected<Handle> TaskPool::submit_queued(std::vector<Value> a, Handle s, ContextKind c) {
  if (c != ContextKind::Process && c != ContextKind::Timed)
    return fail(ErrorCode::InvalidState, "submit context");
  if (desc_.overflow != TaskOverflow::Queue)
    return fail(ErrorCode::Unsupported, "pool not Queue");
  schedule();
  auto r = spawn(std::move(a), s, frames_ >= desc_.max_instances, false);
  return r;
}
void TaskPool::schedule() {
  if (transaction_pending_ && !transaction_)
    return;
  reap();
  while (!queue_.empty() && frames_ + grants_ < desc_.max_instances) {
    auto h = queue_.front();
    queue_.pop_front();
    auto t = get(h);
    if (t && t.value()->state == TaskState::Queued) {
      t.value()->state = TaskState::Runnable;
      ++frames_;
    }
  }
  std::size_t used = 0;
  for (auto &t : tasks_)
    used += t.used;
  while (!pending_.empty() && frames_ + grants_ < desc_.max_instances &&
         used + grants_ < desc_.result_capacity) {
    auto h = pending_.front();
    auto t = ticket(h);
    if (!t || t.value()->info.state != SpawnTicketState::Pending) {
      pending_.pop_front();
      continue;
    }
    std::size_t slot = 0;
    while (slot < tasks_.size() &&
           (tasks_[slot].used || tasks_[slot].reserved || tasks_[slot].generation == UINT64_MAX))
      ++slot;
    if (slot == tasks_.size())
      break;
    if (*allocation_generation_ == UINT64_MAX)
      break;
    tasks_[slot].generation = (*allocation_generation_)++;
    Handle task{HandleKind::Task,
                domain_,
                id_,
                static_cast<std::uint32_t>(slot),
                tasks_[slot].generation,
                t.value()->info.scope.slot + 1ULL};
    auto reservation =
        reserve_result({task, desc_.result_type, desc_.result_bytes, task.owner, task.owner});
    if (!reservation)
      break;
    if (scopes_) {
      auto own =
          scopes_->attach({reservation.value().consumer.consumer, OwnedKind::ResultConsumer, false},
                          t.value()->info.scope);
      if (!own) {
        release_cap(reservation.value().consumer);
        release_producer(reservation.value().reservation);
        break;
      }
    }
    tasks_[slot].reserved = true;
    t.value()->reservation = reservation.value();
    t.value()->consumer_released = false;
    t.value()->task = task;
    t.value()->info.state = SpawnTicketState::Granted;
    ++grants_;
    pending_.pop_front();
  }
}
Expected<Handle> TaskPool::request_slot(Handle waiter, Handle scope, ContextKind c) {
  if (transaction_pending_ && !transaction_)
    return fail(ErrorCode::InvalidState, "ticket store transaction pending");
  if (c != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "await slot requires process");
  if (desc_.overflow != TaskOverflow::AwaitSlot)
    return fail(ErrorCode::Unsupported, "pool not AwaitSlot");
  if (scope.domain != domain_ || waiter.domain != domain_)
    return fail(ErrorCode::WrongDomain, "ticket owner domain");
  if (scope.kind != HandleKind::Scope || waiter.kind != HandleKind::Process || !scope.generation ||
      !waiter.generation)
    return fail(ErrorCode::InvalidArgument, "ticket scope/waiter identity kind");
  if (scopes_) {
    auto s = scopes_->state(scope);
    if (!s)
      return s.error();
    if (s.value() != ScopeState::Open)
      return fail(ErrorCode::Cancelled, "ticket scope frozen");
  }
  for (std::size_t i = 0; i < tickets_.size(); ++i)
    if (!tickets_[i].used && tickets_[i].generation != UINT64_MAX) {
      auto &t = tickets_[i];
      if (*allocation_generation_ == UINT64_MAX)
        return fail(ErrorCode::Overflow, "ticket generation");
      t.generation = (*allocation_generation_)++;
      Handle h{HandleKind::SpawnTicket,       domain_,      id_,
               static_cast<std::uint32_t>(i), t.generation, scope.slot + 1ULL};
      if (scopes_) {
        auto a = scopes_->attach({h, OwnedKind::SpawnTicket, false}, scope);
        if (!a)
          return a.error();
      }
      t.used = true;
      t.waiting = false;
      t.info = {h, waiter, scope, SpawnTicketState::Pending, {}};
      pending_.push_back(h);
      schedule();
      return h;
    }
  return fail(ErrorCode::Capacity, "slot waiters full");
}
Expected<Handle> TaskPool::spawn_reserved(Handle h, Handle waiter, Handle scope,
                                          std::vector<Value> args) {
  auto tr = ticket(h);
  if (!tr)
    return tr.error();
  auto &t = *tr.value();
  if (t.info.waiter != waiter || t.info.scope != scope)
    return fail(ErrorCode::WrongOwner, "ticket consume owner");
  if (t.info.state != SpawnTicketState::Granted)
    return fail(ErrorCode::InvalidState, "ticket not granted");
  if (scopes_) {
    auto open = scopes_->validate_owner(scope, true);
    if (!open)
      return open.error();
  }
  auto valid = check_args(args);
  if (valid && scopes_) {
    auto st = scopes_->state(scope);
    if (!st)
      valid = st.error();
    else if (st.value() != ScopeState::Open)
      valid = fail(ErrorCode::Cancelled, "scope frozen");
    else
      valid = scopes_->replace_owned(h, {*t.task, OwnedKind::Task, false}, scope);
  }
  auto task = *t.task;
  auto &q = tasks_[task.slot];
  q.reserved = false;
  --grants_;
  if (!valid) {
    if (scopes_)
      scopes_->detach_completed(t.reservation->consumer.consumer, scope);
    release_cap(t.reservation->consumer);
    release_producer(t.reservation->reservation);
    t.info.state = SpawnTicketState::Failed;
    t.info.error = valid.error();
    t.reservation.reset();
    t.task.reset();
    schedule();
    return valid.error();
  }
  q.used = true;
  q.scope = scope;
  q.execution = {};
  q.state = TaskState::Runnable;
  q.args = std::move(args);
  q.result = *t.reservation;
  q.producer_released = false;
  ++frames_;
  t.info.state = SpawnTicketState::Consumed;
  t.reservation.reset();
  t.task.reset();
  schedule();
  return task;
}
Expected<SpawnTicketInfo> TaskPool::ticket_info(Handle h) {
  schedule();
  auto t = ticket(h);
  if (!t)
    return t.error();
  return t.value()->info;
}
Expected<void> TaskPool::bind_execution(Handle task, Handle process) {
  auto t = get(task);
  if (!t)
    return t.error();
  if (process.kind != HandleKind::Process || !process.generation || process.domain != domain_)
    return fail(ErrorCode::InvalidArgument, "task execution process identity");
  if (t.value()->state != TaskState::Runnable && t.value()->state != TaskState::Waiting)
    return fail(ErrorCode::InvalidState, "task has no active frame");
  if (t.value()->execution.generation && t.value()->execution != process)
    return fail(ErrorCode::WrongOwner, "task already bound to execution process");
  for (const auto &other : tasks_)
    if (&other != t.value() && other.used && other.execution == process &&
        (other.state == TaskState::Runnable || other.state == TaskState::Waiting))
      return fail(ErrorCode::Duplicate, "process already holds a task frame");
  t.value()->execution = process;
  return {};
}
Expected<void> TaskPool::begin_slot_wait(Handle h, Handle process, Handle scope) {
  auto t = ticket(h);
  if (!t)
    return t.error();
  if (t.value()->info.waiter != process || t.value()->info.scope != scope)
    return fail(ErrorCode::WrongOwner, "slot wait owner");
  if (t.value()->info.state == SpawnTicketState::Granted)
    return {};
  if (t.value()->info.state != SpawnTicketState::Pending)
    return fail(ErrorCode::InvalidState, "slot wait ticket not pending");
  if (t.value()->waiting)
    return fail(ErrorCode::Duplicate, "slot wait already installed");
  auto checked = dependencies_ ? TaskPoolDependencies::check(dependencies_->pools_, *this, process)
                               : TaskPoolDependencies::check({this}, *this, process);
  if (!checked)
    return checked.error();
  t.value()->waiting = true;
  return {};
}
TaskPoolDependencies::~TaskPoolDependencies() {
  for (auto pool : pools_)
    if (pool->dependencies_ == this)
      pool->dependencies_ = nullptr;
}
Expected<void> TaskPoolDependencies::add(TaskPool &pool) {
  if (pool.transaction_pending_ || pool.transaction_)
    return fail(ErrorCode::InvalidState, "register dependency pool outside transaction");
  if (pool.dependencies_)
    return fail(ErrorCode::Duplicate, "pool dependency registry already assigned");
  if (pools_.size() == capacity_)
    return fail(ErrorCode::Capacity, "dependency pool bound");
  for (auto p : pools_)
    if (p->domain_ != pool.domain_)
      return fail(ErrorCode::WrongDomain, "dependency pool domain");
  pools_.push_back(&pool);
  pool.dependencies_ = this;
  return {};
}
Expected<void> TaskPoolDependencies::check(const std::vector<TaskPool *> &registered,
                                           TaskPool &target, Handle prospective) {
  std::vector<const TaskPool *> pools;
  std::size_t target_index = registered.size();
  for (auto p : registered) {
    if (p->id_ == target.id_) {
      target_index = pools.size();
      pools.push_back(&target);
    } else {
      auto view = p->dependency_view(target.transaction_);
      if (!view)
        return view.error();
      pools.push_back(view.value());
    }
  }
  if (target_index == pools.size())
    return fail(ErrorCode::Integrity, "dependency target absent");
  for (auto pool : pools)
    for (const auto &ticket : pool->tickets_)
      if (ticket.used && ticket.waiting && ticket.info.state == SpawnTicketState::Pending &&
          ticket.info.waiter == prospective)
        return fail(ErrorCode::Duplicate, "process already awaits a task slot");
  // A full pool remains blocked only if every actual frame holder waits for a
  // still-blocked pool. Repeatedly removing escape paths computes a closed knot.
  std::vector<bool> blocked(pools.size());
  for (std::size_t i = 0; i < pools.size(); ++i)
    blocked[i] = pools[i]->frames_ >= pools[i]->desc_.max_instances && !pools[i]->grants_;
  auto waiting_on = [&](Handle process) -> std::optional<std::size_t> {
    if (process == prospective)
      return target_index;
    if (!process.generation)
      return {};
    for (std::size_t i = 0; i < pools.size(); ++i)
      for (const auto &ticket : pools[i]->tickets_)
        if (ticket.used && ticket.waiting && ticket.info.state == SpawnTicketState::Pending &&
            ticket.info.waiter == process)
          return i;
    return {};
  };
  for (std::size_t pass = 0; pass < pools.size(); ++pass) {
    bool changed = false;
    for (std::size_t i = 0; i < pools.size(); ++i)
      if (blocked[i]) {
        for (const auto &task : pools[i]->tasks_)
          if (task.used &&
              (task.state == TaskState::Runnable || task.state == TaskState::Waiting)) {
            auto destination = waiting_on(task.execution);
            if (!destination || !blocked[*destination]) {
              blocked[i] = false;
              changed = true;
              break;
            }
          }
      }
    if (!changed)
      break;
  }
  if (blocked[target_index])
    return fail(ErrorCode::InvalidState, "TaskSlotDeadlock: closed task-frame dependency cycle");
  return {};
}
Expected<void> TaskPool::cancel_ticket(Handle h, Handle waiter, Handle scope) {
  auto t = ticket(h);
  if (!t)
    return t.error();
  auto &i = t.value()->info;
  if (i.waiter != waiter || i.scope != scope)
    return fail(ErrorCode::WrongOwner, "ticket cancellation owner");
  if (i.state == SpawnTicketState::Cancelled)
    return {};
  if (i.state == SpawnTicketState::Consumed || i.state == SpawnTicketState::Failed)
    return fail(ErrorCode::InvalidState, "ticket terminal");
  if (i.state == SpawnTicketState::Granted) {
    --grants_;
    tasks_[t.value()->task->slot].reserved = false;
    bool planned = false;
    if (scopes_) {
      auto st = scopes_->state(scope);
      planned = st && st.value() == ScopeState::Cancelling;
    }
    if (scopes_ && !planned)
      scopes_->detach_completed(t.value()->reservation->consumer.consumer, scope);
    if (!planned)
      release_cap(t.value()->reservation->consumer);
    release_producer(t.value()->reservation->reservation);
    if (!planned || t.value()->consumer_released)
      t.value()->reservation.reset();
    t.value()->task.reset();
  }
  i.state = SpawnTicketState::Cancelled;
  pending_.erase(std::remove(pending_.begin(), pending_.end(), h), pending_.end());
  schedule();
  return {};
}
Expected<void> TaskPool::release_ticket(Handle h) {
  auto t = ticket(h);
  if (!t)
    return t.error();
  if (t.value()->info.state == SpawnTicketState::Pending ||
      t.value()->info.state == SpawnTicketState::Granted || t.value()->reservation)
    return fail(ErrorCode::InvalidState, "ticket still active");
  if (scopes_ && t.value()->info.state != SpawnTicketState::Consumed) {
    auto st = scopes_->state(t.value()->info.scope);
    if (!st)
      return st.error();
    if (st.value() == ScopeState::Open) {
      auto d = scopes_->detach_completed(h, t.value()->info.scope);
      if (!d)
        return d.error();
    }
  }
  t.value()->used = false;
  if (t.value()->generation != UINT64_MAX)
    ++t.value()->generation;
  return {};
}
Expected<void> TaskPool::complete(Handle h, TaskOutcome o, ReadyKey key) {
  auto t = get(h);
  if (!t)
    return t.error();
  auto &q = *t.value();
  if (q.producer_released)
    return fail(ErrorCode::Duplicate, "task already terminal");
  if (q.state == TaskState::Queued && o.kind != TaskOutcome::Kind::Cancelled)
    return fail(ErrorCode::InvalidState, "queued task cannot execute");
  auto p = publish_result(q.result.reservation, encode_task_outcome(o), {h, key, true});
  if (!p)
    return p.error();
  auto released_producer = release_producer(q.result.reservation);
  if (!released_producer)
    return fail(ErrorCode::Integrity,
                "task publication producer release failed: " + released_producer.error().message);
  if (q.state == TaskState::Queued)
    queue_.erase(std::remove(queue_.begin(), queue_.end(), h), queue_.end());
  else
    --frames_;
  q.state = o.kind == TaskOutcome::Kind::Success ? TaskState::Completed
            : o.kind == TaskOutcome::Kind::Error ? TaskState::Failed
                                                 : TaskState::Cancelled;
  q.args.clear();
  q.producer_released = true;
  if (scopes_) {
    auto s = scopes_->state(q.scope);
    if (s && s.value() == ScopeState::Open)
      scopes_->detach_completed(h, q.scope);
  }
  schedule();
  return {};
}
Expected<void> TaskPool::cancel(Handle h, std::string reason, ReadyKey key) {
  auto t = get(h);
  if (!t)
    return t.error();
  if (t.value()->producer_released)
    return {};
  return complete(h, {TaskOutcome::Kind::Cancelled, Value{Bytes(reason.begin(), reason.end())}},
                  key);
}
Expected<void> TaskPool::execute_cancel_action(const ScopeCancelAction &action, ReadyKey key) {
  if (!scopes_)
    return fail(ErrorCode::InvalidState, "task pool has no scope registry");
  auto st = scopes_->state(action.scope);
  if (!st || st.value() != ScopeState::Cancelling)
    return fail(ErrorCode::InvalidState, "task cleanup requires active scope plan");
  if (action.owned.kind == OwnedKind::Task) {
    auto t = get(action.owned.handle);
    if (!t)
      return t.error();
    if (t.value()->scope != action.scope)
      return fail(ErrorCode::WrongOwner, "task cleanup scope");
    return cancel(action.owned.handle, "scope cancellation", key);
  }
  if (action.owned.kind == OwnedKind::SpawnTicket) {
    auto t = ticket(action.owned.handle);
    if (!t)
      return t.error();
    if (t.value()->info.scope != action.scope)
      return fail(ErrorCode::WrongOwner, "ticket cleanup scope");
    return cancel_ticket(action.owned.handle, t.value()->info.waiter, action.scope);
  }
  if (action.owned.kind == OwnedKind::ResultConsumer) {
    for (auto &t : tasks_)
      if (t.used && t.scope == action.scope && t.result.consumer.consumer == action.owned.handle)
        return release_cap(t.result.consumer);
    for (auto &t : tickets_)
      if (t.used && t.info.scope == action.scope && t.reservation &&
          t.reservation->consumer.consumer == action.owned.handle) {
        auto released = release_cap(t.reservation->consumer);
        if (!released)
          return released.error();
        t.consumer_released = true;
        // The ticket owns its producer reservation until its own cancellation action.
        if (t.info.state == SpawnTicketState::Cancelled || t.info.state == SpawnTicketState::Failed)
          t.reservation.reset();
        return {};
      }
    return fail(ErrorCode::WrongOwner, "task cleanup consumer");
  }
  return fail(ErrorCode::Unsupported, "cleanup action belongs to another store");
}
Expected<TaskState> TaskPool::state(Handle h) {
  auto t = get(h);
  if (!t)
    return t.error();
  return t.value()->state;
}
Expected<std::vector<Value>> TaskPool::arguments(Handle h) {
  auto t = get(h);
  if (!t)
    return t.error();
  return t.value()->args;
}
Expected<ResultHandle> TaskPool::result_handle(Handle h, Handle scope) {
  auto t = get(h);
  if (!t)
    return t.error();
  if (t.value()->scope != scope)
    return fail(ErrorCode::WrongOwner, "task creator consumer");
  return t.value()->result.consumer;
}
Expected<PinnedResultView> TaskPool::pin_result(Handle h, ResultHandle r) {
  auto t = get(h);
  if (!t)
    return t.error();
  if (t.value()->result.consumer.result != r.result)
    return fail(ErrorCode::WrongOwner, "task result mismatch");
  return results_.pin(r);
}
Expected<void> TaskPool::release_result(Handle h, ResultHandle r) {
  auto t = get(h);
  if (!t)
    return t.error();
  if (t.value()->result.consumer.result != r.result)
    return fail(ErrorCode::WrongOwner, "task result mismatch");
  auto done = release_cap(r);
  if (!done)
    return done.error();
  if (scopes_ && r.consumer == t.value()->result.consumer.consumer) {
    auto st = scopes_->state(t.value()->scope);
    if (st && st.value() != ScopeState::Cancelling)
      scopes_->detach_completed(r.consumer, t.value()->scope);
  }
  schedule();
  return {};
}
Expected<Value> TaskPool::consume_result(Handle h, ResultHandle r) {
  auto t = get(h);
  if (!t)
    return t.error();
  if (t.value()->result.consumer.result != r.result)
    return fail(ErrorCode::WrongOwner, "task result mismatch");
  auto v = read_result(r);
  if (!v)
    return v.error();
  auto rel = release_result(h, r);
  if (!rel)
    return rel.error();
  return v.value();
}
std::vector<Handle> TaskPool::members(Handle s) {
  std::vector<Handle> out;
  for (std::size_t i = 0; i < tasks_.size(); ++i)
    if (tasks_[i].used && tasks_[i].scope == s)
      out.push_back({HandleKind::Task, domain_, id_, static_cast<std::uint32_t>(i),
                     tasks_[i].generation, s.slot + 1ULL});
  return out;
}
} // namespace leanat
