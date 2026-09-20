#include "leanat/structured_services.hpp"
namespace leanat {
namespace {
std::size_t charge(std::size_t base, std::size_t count, std::size_t each) noexcept {
  if (count && each > (SIZE_MAX - base) / count)
    return SIZE_MAX;
  return base + count * each;
}
} // namespace
class PreparedScopes final : public PreparedParticipant {
public:
  CancelScopeStore &source;
  std::unique_ptr<CancelScopeStore> snapshot;
  bool done{};
  PreparedScopes(CancelScopeStore &s, const CancelScopeStore &base, EventTxn &tx)
      : source(s), snapshot(new CancelScopeStore(base)) {
    ++source.transaction_pending_;
    snapshot->transaction_ = &tx;
    snapshot->transaction_pending_ = 0;
  }
  ~PreparedScopes() {
    if (!done)
      discard();
  }
  const void *identity() const noexcept override {
    return &source;
  }
  std::size_t reserved_bytes() const noexcept override {
    auto record =
        charge(sizeof(CancelScopeStore::Record) + 256, snapshot->entries_, sizeof(ScopeOwned));
    record = charge(record, snapshot->actions_, sizeof(ScopeCancelAction));
    auto n = charge(sizeof(*this), snapshot->records_.size(), record);
    return charge(n, snapshot->observers_, sizeof(CancelScopeStore::Observer));
  }
  Expected<void> validate() const override {
    if (done || !source.transaction_pending_)
      return fail(ErrorCode::InvalidState, "scope transaction lost");
    return {};
  }
  void apply() noexcept override {
    source.records_.swap(snapshot->records_);
    source.observations_.swap(snapshot->observations_);
    source.sequence_ = snapshot->sequence_;
    --source.transaction_pending_;
    done = true;
  }
  void discard() noexcept override {
    if (!done) {
      --source.transaction_pending_;
      done = true;
    }
  }
};
Expected<CancelScopeStore *> CancelScopeStore::prepare(EventTxn &tx) {
  auto prior = static_cast<PreparedScopes *>(tx.participant(this));
  if (transaction_pending_ && !prior)
    return fail(ErrorCode::InvalidState, "scope owned by another segment");
  auto p = std::make_unique<PreparedScopes>(*this, prior ? *prior->snapshot : *this, tx);
  auto raw = p->snapshot.get();
  auto staged = tx.stage_participant(std::move(p));
  if (!staged)
    return staged.error();
  return raw;
}
class PreparedTasks final : public PreparedParticipant {
public:
  TaskPool &source;
  std::unique_ptr<TaskPool> snapshot;
  bool done{};
  PreparedTasks(TaskPool &s, const TaskPool &base, EventTxn &tx)
      : source(s), snapshot(new TaskPool(base)) {
    ++source.transaction_pending_;
    snapshot->transaction_ = &tx;
    snapshot->transaction_pending_ = 0;
  }
  ~PreparedTasks() {
    if (!done)
      discard();
  }
  const void *identity() const noexcept override {
    return &source;
  }
  std::size_t reserved_bytes() const noexcept override {
    auto task_bytes = charge(sizeof(TaskPool::Task), 1, snapshot->desc_.args_bytes);
    auto n = charge(sizeof(*this), snapshot->tasks_.size(), task_bytes);
    n = charge(n, snapshot->tickets_.size(), sizeof(TaskPool::Ticket) + sizeof(Handle));
    return charge(n, snapshot->desc_.queue_depth, sizeof(Handle));
  }
  Expected<void> validate() const override {
    if (done || !source.transaction_pending_)
      return fail(ErrorCode::InvalidState, "task transaction lost");
    return {};
  }
  void apply() noexcept override {
    source.tasks_.swap(snapshot->tasks_);
    source.tickets_.swap(snapshot->tickets_);
    source.queue_.swap(snapshot->queue_);
    source.pending_.swap(snapshot->pending_);
    source.frames_ = snapshot->frames_;
    source.grants_ = snapshot->grants_;
    --source.transaction_pending_;
    done = true;
  }
  void discard() noexcept override {
    if (!done) {
      --source.transaction_pending_;
      done = true;
    }
  }
};
Expected<TaskPool *> TaskPool::prepare(EventTxn &tx) {
  auto checkpoint = tx.checkpoint();
  if (!checkpoint)
    return checkpoint.error();
  auto prior = static_cast<PreparedTasks *>(tx.participant(this));
  if (transaction_pending_ && !prior)
    return fail(ErrorCode::InvalidState, "task pool owned by another segment");
  auto p = std::make_unique<PreparedTasks>(*this, prior ? *prior->snapshot : *this, tx);
  if (scopes_) {
    auto s = scopes_->prepare(tx);
    if (!s) {
      tx.rollback(std::move(checkpoint.value()));
      return s.error();
    }
    p->snapshot->scopes_ = s.value();
  }
  auto raw = p->snapshot.get();
  auto staged = tx.stage_participant(std::move(p));
  if (!staged) {
    tx.rollback(std::move(checkpoint.value()));
    return staged.error();
  }
  return raw;
}
Expected<const TaskPool *> TaskPool::dependency_view(EventTxn *tx) const {
  if (tx) {
    auto p = static_cast<PreparedTasks *>(tx->participant(this));
    if (p)
      return p->snapshot.get();
  }
  if (transaction_pending_)
    return fail(ErrorCode::InvalidState, "dependency pool owned by another segment");
  return this;
}
Expected<ReservedResult> TaskPool::reserve_result(ResultCreate c) {
  return transaction_ ? results_.prepare_reserve(*transaction_, std::move(c))
                      : results_.reserve(std::move(c));
}
Expected<PublicationReceipt> TaskPool::publish_result(ResultOwnerHandle h, Value v,
                                                      PublicationContext c) {
  return transaction_ ? results_.prepare_publish_in(*transaction_, h, std::move(v), c)
                      : results_.publish(h, std::move(v), c);
}
Expected<void> TaskPool::release_cap(ResultHandle h, ReleasePolicy p) {
  return transaction_ ? results_.prepare_release(*transaction_, h) : results_.release(h, p);
}
Expected<void> TaskPool::release_producer(ResultOwnerHandle h) {
  return transaction_ ? results_.prepare_release_owner(*transaction_, h)
                      : results_.release_owner(h);
}
Expected<Value> TaskPool::read_result(ResultHandle h) {
  return transaction_ ? results_.prepare_read(*transaction_, h) : results_.read(h);
}
class PreparedWaitGroups final : public PreparedParticipant {
public:
  WaitGroupStore &source;
  std::unique_ptr<WaitGroupStore> snapshot;
  bool done{};
  PreparedWaitGroups(WaitGroupStore &s, const WaitGroupStore &base, EventTxn &tx)
      : source(s), snapshot(new WaitGroupStore(base)) {
    ++source.transaction_pending_;
    snapshot->transaction_ = &tx;
    snapshot->transaction_pending_ = 0;
  }
  ~PreparedWaitGroups() {
    if (!done)
      discard();
  }
  const void *identity() const noexcept override {
    return &source;
  }
  std::size_t reserved_bytes() const noexcept override {
    auto branch = charge(sizeof(WaitGroupStore::Branch) + 256, 1, snapshot->candidate_bytes_);
    auto group = charge(sizeof(WaitGroupStore::Group) + 128, snapshot->branch_limit_, branch);
    return charge(sizeof(*this), snapshot->groups_.size(), group);
  }
  Expected<void> validate() const override {
    if (done || !source.transaction_pending_)
      return fail(ErrorCode::InvalidState, "wait transaction lost");
    return {};
  }
  void apply() noexcept override {
    source.groups_.swap(snapshot->groups_);
    --source.transaction_pending_;
    done = true;
  }
  void discard() noexcept override {
    if (!done) {
      --source.transaction_pending_;
      done = true;
    }
  }
};
Expected<WaitGroupStore *> WaitGroupStore::prepare(EventTxn &tx) {
  auto prior = static_cast<PreparedWaitGroups *>(tx.participant(this));
  if (transaction_pending_ && !prior)
    return fail(ErrorCode::InvalidState, "wait store owned by another segment");
  auto p = std::make_unique<PreparedWaitGroups>(*this, prior ? *prior->snapshot : *this, tx);
  auto raw = p->snapshot.get();
  auto staged = tx.stage_participant(std::move(p));
  if (!staged)
    return staged.error();
  return raw;
}
Expected<ReservedResult> WaitGroupStore::reserve_result(ResultCreate c) {
  return transaction_ ? results_.prepare_reserve(*transaction_, std::move(c))
                      : results_.reserve(std::move(c));
}
Expected<PublicationReceipt> WaitGroupStore::publish_result(ResultOwnerHandle h, Value v,
                                                            PublicationContext c) {
  return transaction_ ? results_.prepare_publish_in(*transaction_, h, std::move(v), c)
                      : results_.publish(h, std::move(v), c);
}
Expected<void> WaitGroupStore::release_cap(ResultHandle h, ReleasePolicy p) {
  return transaction_ ? results_.prepare_release(*transaction_, h) : results_.release(h, p);
}
Expected<void> WaitGroupStore::release_producer(ResultOwnerHandle h) {
  return transaction_ ? results_.prepare_release_owner(*transaction_, h)
                      : results_.release_owner(h);
}
Expected<ResultHandle> WaitGroupStore::retain_cap(ResultHandle h, std::uint64_t owner) {
  return transaction_ ? results_.prepare_retain(*transaction_, h, owner)
                      : results_.retain(h, owner);
}
Expected<Value> WaitGroupStore::owning_result(Handle h, ResultHandle r) {
  auto g = get(h);
  if (!g)
    return g.error();
  if (r.result != g.value()->result.consumer.result)
    return fail(ErrorCode::WrongOwner, "wait result capability");
  return transaction_ ? results_.prepare_read(*transaction_, r) : results_.read(r);
}
Expected<Value> TaskPool::owning_result(Handle h, ResultHandle r) {
  auto t = get(h);
  if (!t)
    return t.error();
  if (r.result != t.value()->result.consumer.result)
    return fail(ErrorCode::WrongOwner, "task result capability");
  return read_result(r);
}
Expected<bool> WaitGroupStore::fully_armed(Handle h) {
  auto g = get(h);
  if (!g)
    return g.error();
  return g.value()->branches.size() == g.value()->desc.branch_count;
}
Expected<void> WaitGroupStore::refresh_sources(BatchId batch, ReadyKey now) {
  if (transaction_pending_)
    return fail(ErrorCode::InvalidState, "wait transaction pending");
  for (auto &g : groups_)
    if (g.used && g.committed && !g.resolved)
      for (auto &b : g.branches)
        if (!b.candidate && b.consumer) {
          auto ready = results_.published_ready(*b.consumer);
          if (!ready) {
            if (ready.error().code == ErrorCode::NotReady)
              continue;
            return ready.error();
          }
          if (now < ready.value())
            continue;
          auto value = results_.read(*b.consumer);
          if (!value)
            return value.error();
          if (owned_value_bytes(value.value()) > candidate_bytes_)
            return fail(ErrorCode::Capacity, "wait candidate bytes");
          bool failed = false;
          if (b.desc.source.kind == HandleKind::Task)
            if (auto a = std::get_if<Value::Array>(&value.value().data); a && !a->empty())
              if (auto tag = std::get_if<std::uint64_t>(&(*a)[0].data))
                failed = *tag != 0;
          auto pin = results_.pin(*b.consumer);
          if (!pin)
            return pin.error();
          b.pin = pin.value();
          b.candidate = Candidate{ready.value(), batch, std::move(value.value()), failed};
        }
  return {};
}
class PreparedStructuredBindings final : public PreparedParticipant {
public:
  StructuredServices &source;
  StructuredServices::State snapshot;
  bool done{};
  PreparedStructuredBindings(StructuredServices &s, const StructuredServices::State &base)
      : source(s), snapshot(base) {
    ++source.pending_;
  }
  ~PreparedStructuredBindings() {
    if (!done)
      discard();
  }
  const void *identity() const noexcept override {
    return &source;
  }
  std::size_t reserved_bytes() const noexcept override {
    auto n = charge(sizeof(*this), source.tasks_.descriptor().task_capacity,
                    2 * sizeof(std::pair<Handle, Handle>) + sizeof(std::pair<Handle, ProgramId>) +
                        12 * sizeof(void *));
    return charge(n, source.waits_.control_capacity(),
                  sizeof(std::pair<Handle, Handle>) +
                      sizeof(std::pair<Handle, StructuredServices::WaitOwnership>) +
                      8 * sizeof(void *));
  }
  Expected<void> validate() const override {
    if (done || !source.pending_)
      return fail(ErrorCode::InvalidState, "structured binding transaction lost");
    return {};
  }
  void apply() noexcept override {
    source.state_.waits.swap(snapshot.waits);
    source.state_.wait_owners.swap(snapshot.wait_owners);
    source.state_.processes.swap(snapshot.processes);
    source.state_.starts.swap(snapshot.starts);
    source.state_.programs.swap(snapshot.programs);
    --source.pending_;
    done = true;
  }
  void discard() noexcept override {
    if (!done) {
      --source.pending_;
      done = true;
    }
  }
};
Expected<StructuredServices::State *> StructuredServices::prepare_bindings(EventTxn &tx) {
  auto prior = static_cast<PreparedStructuredBindings *>(tx.participant(this));
  if (pending_ && !prior)
    return fail(ErrorCode::InvalidState, "structured bindings owned by another segment");
  auto p = std::make_unique<PreparedStructuredBindings>(*this, prior ? prior->snapshot : state_);
  auto raw = &p->snapshot;
  auto r = tx.stage_participant(std::move(p));
  if (!r)
    return r.error();
  return raw;
}
Expected<Handle> StructuredServices::execution_process(Handle h) const {
  auto i = state_.processes.find(h);
  if (i == state_.processes.end())
    return fail(ErrorCode::StaleHandle, "task execution binding");
  return i->second;
}
Expected<void> StructuredServices::dispatch_ready(State &state, TaskPool &pool,
                                                  const ExecutionContext &ctx, EventTxn &tx) {
  if (!runtime_)
    return {};
  std::vector<Handle> ready;
  for (auto &p : state.programs)
    if (!state.processes.count(p.first)) {
      auto status = pool.state(p.first);
      if (!status)
        return status.error();
      if (status.value() == TaskState::Runnable)
        ready.push_back(p.first);
    }
  std::sort(ready.begin(), ready.end(),
            [](Handle a, Handle b) { return a.generation < b.generation; });
  for (auto task : ready) {
    auto process = runtime_->processes().create_controlled(state.programs.at(task), task.owner, tx);
    if (!process)
      return process.error();
    auto bound = pool.bind_execution(task, process.value());
    if (!bound)
      return bound.error();
    auto next = runtime_->queue().successor(ctx.ready.time);
    if (!next)
      return next.error();
    EventDraft event;
    event.key = {next.value().time, next.value().turn, EventStage::Internal,
                 ctx.instance,      ctx.connection,    0};
    event.owner = task.owner;
    event.epoch = ctx.epoch;
    event.value = Value{Value::Array{Value{task}, Value{process.value()}}};
    auto scheduled = tx.stage_event(runtime_->queue(), std::move(event));
    if (!scheduled)
      return scheduled.error();
    state.processes.emplace(task, process.value());
    state.starts.emplace(task, scheduled.value());
  }
  return {};
}
Expected<void> StructuredServices::cancel_execution(State &state, Handle task, EventTxn &tx) {
  if (!runtime_)
    return {};
  auto start = state.starts.find(task);
  if (start != state.starts.end()) {
    auto pending = tx.can_cancel(runtime_->queue(), start->second);
    if (!pending)
      return pending.error();
    if (pending.value()) {
      auto cancelled = tx.stage_cancel(runtime_->queue(), start->second);
      if (!cancelled)
        return cancelled.error();
    }
    state.starts.erase(start);
  }
  auto process = state.processes.find(task);
  if (process != state.processes.end()) {
    auto cancelled = runtime_->processes().cancel_controlled(process->second, tx);
    if (!cancelled)
      return cancelled.error();
    state.processes.erase(process);
  }
  state.programs.erase(task);
  return {};
}
Expected<StructuredServices::TaskStart>
StructuredServices::start_task(Handle task, const ExecutionContext &ctx, EventTxn &tx) {
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto result = [&]() -> Expected<TaskStart> {
    if (!runtime_)
      return fail(ErrorCode::Unsupported, "task execution needs Runtime");
    auto bindings = prepare_bindings(tx);
    if (!bindings)
      return bindings.error();
    auto process = bindings.value()->processes.find(task);
    if (process == bindings.value()->processes.end() || !bindings.value()->starts.count(task))
      return fail(ErrorCode::InvalidState, "task start already consumed or not scheduled");
    if (process->second.owner != ctx.owner)
      return fail(ErrorCode::WrongOwner, "task execution owner");
    auto pool = tasks_.prepare(tx);
    if (!pool)
      return pool.error();
    auto args = pool.value()->arguments(task);
    if (!args)
      return args.error();
    auto began = runtime_->processes().begin(process->second, tx);
    if (!began)
      return began.error();
    TaskStart out{process->second, bindings.value()->programs.at(task), std::move(args.value())};
    bindings.value()->starts.erase(task);
    return out;
  }();
  if (!result)
    tx.rollback(std::move(save.value()));
  return result;
}
Expected<void> StructuredServices::complete(Handle h, TaskOutcome outcome, ReadyKey key,
                                            EventTxn &tx) {
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto result = [&]() -> Expected<void> {
    if (key != tx.context().ready)
      return fail(ErrorCode::InvalidArgument, "task completion publication context");
    auto bindings = prepare_bindings(tx);
    if (!bindings)
      return bindings.error();
    auto task = tasks_.prepare(tx);
    if (!task)
      return task.error();
    auto i = bindings.value()->processes.find(h);
    if (i != bindings.value()->processes.end() && runtime_) {
      auto start = bindings.value()->starts.find(h);
      if (start != bindings.value()->starts.end()) {
        auto pending = tx.can_cancel(runtime_->queue(), start->second);
        if (!pending)
          return pending.error();
        if (pending.value()) {
          auto cancelled = tx.stage_cancel(runtime_->queue(), start->second);
          if (!cancelled)
            return cancelled.error();
        }
        bindings.value()->starts.erase(start);
      }
      auto done = runtime_->processes().complete(i->second, tx);
      if (!done)
        return done.error();
      bindings.value()->processes.erase(i);
    }
    bindings.value()->programs.erase(h);
    auto complete = task.value()->complete(h, std::move(outcome), key);
    if (!complete)
      return complete.error();
    return dispatch_ready(*bindings.value(), *task.value(), tx.context(), tx);
  }();
  if (!result)
    tx.rollback(std::move(save.value()));
  return result;
}
Expected<void> StructuredServices::resolve(BatchId b, ReadyKey key) {
  auto refreshed = waits_.refresh_sources(b, key);
  if (!refreshed)
    return refreshed.error();
  auto resolved = waits_.resolve_closed_batch(b, true);
  if (!resolved)
    return resolved.error();
  return {};
}
namespace {
Expected<Handle> service_handle(const Value &v, HandleKind k, const ExecutionContext &c) {
  auto h = std::get_if<Handle>(&v.data);
  if (!h || h->kind != k)
    return fail(ErrorCode::TypeMismatch, "structured handle kind");
  if (h->domain != c.domain)
    return fail(ErrorCode::WrongDomain, "structured handle domain");
  return *h;
}
Expected<std::uint64_t> service_number(const Value &v) {
  auto n = std::get_if<std::uint64_t>(&v.data);
  if (!n)
    return fail(ErrorCode::TypeMismatch, "structured integer operand");
  return *n;
}
Value except_handle(const Expected<Handle> &h) {
  return h ? Value{Value::Array{Value{std::uint64_t{0}}, Value{Value::Array{Value{h.value()}}}}}
           : Value{Value::Array{
                 Value{std::uint64_t{1}},
                 Value{Value::Array{Value{static_cast<std::uint64_t>(h.error().code)}}}}};
}
} // namespace
Expected<void> StructuredServices::bind(CoreRuntimeBackend &backend,
                                        exec::ServiceSignature signature,
                                        const exec::Project &project, ProgramId program) {
  if (signature.op == exec::Op::SpawnProcess || signature.op == exec::Op::TrySpawnProcess) {
    auto target = std::find_if(project.programs.begin(), project.programs.end(),
                               [&](const auto &p) { return p.id == program.value; });
    if (target == project.programs.end() || target->context != ContextKind::Process)
      return fail(ErrorCode::Schema, "spawn target process program");
    const bool explicit_policy = target->instruction_fuel || !target->owner_policy.empty() ||
                                 !target->result_lifetime_policy.empty();
    if (explicit_policy &&
        (project.schema_major < 5 || !target->instruction_fuel ||
         target->owner_policy != "caller" || target->result_lifetime_policy != "until-release"))
      return fail(ErrorCode::Schema, "unsupported process ownership or result lifetime policy");
  }
  return bind(backend, std::move(signature), program);
}
Expected<void> StructuredServices::bind(CoreRuntimeBackend &backend,
                                        exec::ServiceSignature signature, ProgramId program) {
  using exec::Op;
  std::size_t arity = 0;
  switch (signature.op) {
  case Op::ScopeNew:
  case Op::ScopeClose:
    arity = 1;
    break;
  case Op::ScopeTransfer:
    arity = 3;
    break;
  case Op::ScopeCancel:
  case Op::TrySpawnProcess:
  case Op::SpawnProcess:
  case Op::SubmitTask:
  case Op::CancelTask:
  case Op::TaskResultGet:
  case Op::TaskResultRelease:
  case Op::RequestTaskSlot:
  case Op::WaitResultGet:
  case Op::WaitGroupRelease:
    arity = 2;
    break;
  case Op::WaitGroupNew:
    arity = 7;
    break;
  case Op::WaitArm:
    arity = 5;
    break;
  default:
    return fail(ErrorCode::Unsupported, "structured service opcode");
  }
  if (signature.input_types.size() != arity || signature.result_types.size() != 1)
    return fail(ErrorCode::Schema, "structured service arity");
  auto op = signature.op;
  return backend.register_provider(
      std::move(signature),
      [this, op, program](const std::vector<Value> &a, const ExecutionContext &ctx,
                          EventTxn &tx) -> Expected<std::vector<Value>> {
        auto save = tx.checkpoint();
        if (!save)
          return save.error();
        auto invoke = [&]() -> Expected<std::vector<Value>> {
          auto unit = []() { return std::vector<Value>{Value{}}; };
          if (op == Op::ScopeNew || op == Op::ScopeClose || op == Op::ScopeTransfer ||
              op == Op::ScopeCancel) {
            auto s = scopes_.prepare(tx);
            if (!s)
              return s.error();
            if (op == Op::ScopeTransfer) {
              auto object = std::get_if<Handle>(&a[0].data);
              auto from = service_handle(a[1], HandleKind::Scope, ctx),
                   to = service_handle(a[2], HandleKind::Scope, ctx);
              if (!object || !from || !to)
                return fail(ErrorCode::TypeMismatch, "scope transfer operands");
              auto bindings = prepare_bindings(tx);
              if (!bindings)
                return bindings.error();
              for (const auto &entry : bindings.value()->wait_owners) {
                if (*object == entry.first || *object == entry.second.primary.consumer ||
                    *object == bindings.value()->waits.at(entry.first))
                  return fail(ErrorCode::Unsupported, "wait ownership requires paired transfer");
              }
              auto moved = s.value()->transfer(*object, from.value(), to.value());
              if (!moved)
                return moved.error();
              return unit();
            }
            auto scope = service_handle(a[0], HandleKind::Scope, ctx);
            if (!scope)
              return scope.error();
            if (op == Op::ScopeNew) {
              auto child = s.value()->create(scope.value());
              if (!child)
                return child.error();
              return std::vector<Value>{Value{child.value()}};
            }
            if (op == Op::ScopeClose) {
              auto closed = s.value()->close(scope.value());
              if (!closed)
                return closed.error();
              return unit();
            }
            auto reason = std::get_if<Bytes>(&a[1].data);
            if (!reason)
              return fail(ErrorCode::TypeMismatch, "scope cancel reason bytes");
            auto plan =
                s.value()->cancel(scope.value(), std::string(reason->begin(), reason->end()));
            if (!plan)
              return plan.error();
            auto tasks = tasks_.prepare(tx);
            if (!tasks)
              return tasks.error();
            auto final_scopes = scopes_.prepare(tx);
            if (!final_scopes)
              return final_scopes.error();
            auto bindings = prepare_bindings(tx);
            if (!bindings)
              return bindings.error();
            auto groups = waits_.prepare(tx);
            if (!groups)
              return groups.error();
            std::set<Handle> released_consumers;
            std::size_t remaining = scopes_.control_capacity();
            std::function<Expected<void>(Handle)> cancel_closure;
            cancel_closure = [&](Handle current) -> Expected<void> {
              if (!remaining--)
                return fail(ErrorCode::Integrity, "scope cancellation closure exceeds bound");
              auto current_plan = final_scopes.value()->cancel(current, plan.value().reason);
              if (!current_plan)
                return current_plan.error();
              for (auto action : current_plan.value().actions) {
                auto applied = final_scopes.value()->apply(
                    current, action.id, [&](const ScopeCancelAction &x) {
                      if (x.owned.kind == OwnedKind::ChildScope)
                        return cancel_closure(x.owned.handle);
                      if (x.owned.kind == OwnedKind::ResultConsumer &&
                          released_consumers.count(x.owned.handle))
                        return Expected<void>{};
                      if (x.owned.kind == OwnedKind::Wait) {
                        auto &state = *bindings.value();
                        auto binding =
                            std::find_if(state.waits.begin(), state.waits.end(),
                                         [&](const auto &v) { return v.second == x.owned.handle; });
                        if (binding == state.waits.end())
                          return Expected<void>{fail(ErrorCode::StaleHandle, "scope wait binding")};
                        auto owner = state.wait_owners.find(binding->first);
                        if (owner == state.wait_owners.end() || owner->second.scope != x.scope)
                          return Expected<void>{
                              fail(ErrorCode::WrongOwner, "scope wait ownership")};
                        auto cancelled = runtime_->processes().cancel_wait_controlled(
                            binding->first, owner->second.process, tx);
                        // This exact registered identity can already have been consumed or retired
                        // by its owning task.
                        if (!cancelled &&
                            !(cancelled.error().code == ErrorCode::StaleHandle &&
                              cancelled.error().message == "controlled wait cancel identity"))
                          return cancelled;
                        auto released = groups.value()->release(binding->second,
                                                                owner->second.primary, x.scope);
                        if (!released)
                          return released;
                        released_consumers.insert(owner->second.primary.consumer);
                        state.wait_owners.erase(owner);
                        state.waits.erase(binding);
                        return Expected<void>{};
                      }
                      if (x.owned.kind == OwnedKind::Task) {
                        auto cancelled = cancel_execution(*bindings.value(), x.owned.handle, tx);
                        if (!cancelled)
                          return cancelled;
                      }
                      return tasks.value()->execute_cancel_action(x, ctx.ready);
                    });
                if (!applied)
                  return applied.error();
              }
              return {};
            };
            auto cancelled_closure = cancel_closure(scope.value());
            if (!cancelled_closure)
              return cancelled_closure.error();
            auto dispatched = dispatch_ready(*bindings.value(), *tasks.value(), ctx, tx);
            if (!dispatched)
              return dispatched.error();
            return unit();
          }
          if (op == Op::TrySpawnProcess || op == Op::SpawnProcess || op == Op::SubmitTask) {
            auto scope = service_handle(a[0], HandleKind::Scope, ctx);
            auto args = std::get_if<Value::Array>(&a[1].data);
            if (!scope || !args)
              return fail(ErrorCode::TypeMismatch, "task spawn operands");
            auto pool = tasks_.prepare(tx);
            if (!pool)
              return pool.error();
            auto spawned = op == Op::SubmitTask
                               ? pool.value()->submit_queued(*args, scope.value(), ctx.kind)
                               : pool.value()->try_spawn(*args, scope.value(), ctx.kind);
            if (!spawned)
              return spawned.error();
            if (runtime_) {
              auto bindings = prepare_bindings(tx);
              if (!bindings)
                return bindings.error();
              bindings.value()->programs.emplace(spawned.value(), program);
              auto dispatched = dispatch_ready(*bindings.value(), *pool.value(), ctx, tx);
              if (!dispatched)
                return dispatched.error();
            }
            return std::vector<Value>{op == Op::SpawnProcess ? Value{spawned.value()}
                                                             : except_handle(spawned)};
          }
          if (op == Op::RequestTaskSlot) {
            if (!runtime_)
              return fail(ErrorCode::Unsupported, "task slot adapter needs Runtime");
            auto waiter = service_handle(a[0], HandleKind::Process, ctx),
                 scope = service_handle(a[1], HandleKind::Scope, ctx);
            if (!waiter || !scope)
              return fail(ErrorCode::TypeMismatch, "task slot operands");
            auto &processes = runtime_->processes();
            if (processes.transaction_pending_ && processes.pending_txn_ != &tx)
              return fail(ErrorCode::NotReady, "process transaction pending");
            const auto &view = processes.pending_txn_ == &tx && processes.pending_view_
                                   ? *processes.pending_view_ : processes;
            auto process = view.inspect(waiter.value());
            if (!process)
              return process.error();
            if (waiter.value().owner != ctx.owner ||
                (process.value().instance_bound && process.value().instance != ctx.instance))
              return fail(ErrorCode::WrongOwner, "task slot process context");
            auto pool = tasks_.prepare(tx);
            if (!pool)
              return pool.error();
            auto ticket = pool.value()->request_slot(waiter.value(), scope.value(), ctx.kind);
            if (!ticket)
              return ticket.error();
            return std::vector<Value>{except_handle(ticket)};
          }
          if (op == Op::CancelTask || op == Op::TaskResultGet || op == Op::TaskResultRelease) {
            auto task = service_handle(a[0], HandleKind::Task, ctx);
            if (!task)
              return task.error();
            auto pool = tasks_.prepare(tx);
            if (!pool)
              return pool.error();
            if (op == Op::CancelTask) {
              auto bytes = std::get_if<Bytes>(&a[1].data);
              if (!bytes)
                return fail(ErrorCode::TypeMismatch, "task cancel reason");
              auto done = pool.value()->cancel(
                  task.value(), std::string(bytes->begin(), bytes->end()), ctx.ready);
              if (!done)
                return done.error();
              if (runtime_) {
                auto b = prepare_bindings(tx);
                if (!b)
                  return b.error();
                auto cancelled = cancel_execution(*b.value(), task.value(), tx);
                if (!cancelled)
                  return cancelled.error();
                auto dispatched = dispatch_ready(*b.value(), *pool.value(), ctx, tx);
                if (!dispatched)
                  return dispatched.error();
              }
              return unit();
            }
            auto scope = service_handle(a[1], HandleKind::Scope, ctx);
            if (!scope)
              return scope.error();
            auto consumer = pool.value()->result_handle(task.value(), scope.value());
            if (!consumer)
              return consumer.error();
            if (op == Op::TaskResultGet) {
              auto v = pool.value()->owning_result(task.value(), consumer.value());
              if (!v)
                return v.error();
              return std::vector<Value>{std::move(v.value())};
            }
            auto released = pool.value()->release_result(task.value(), consumer.value());
            if (!released)
              return released.error();
            return unit();
          }
          if (op == Op::WaitGroupNew) {
            if (!runtime_)
              return fail(ErrorCode::Unsupported, "wait VM adapter needs Runtime");
            auto process = service_handle(a[0], HandleKind::Process, ctx),
                 scope = service_handle(a[1], HandleKind::Scope, ctx);
            auto mode = service_number(a[2]), count = service_number(a[3]),
                 policy = service_number(a[4]), type = service_number(a[5]),
                 bytes = service_number(a[6]);
            if (!process || !scope || !mode || !count || !policy || !type || !bytes ||
                mode.value() > 1 || policy.value() > 1 || type.value() > UINT32_MAX)
              return fail(ErrorCode::TypeMismatch, "wait create operands");
            if (process.value().owner != ctx.owner)
              return fail(ErrorCode::WrongOwner, "wait process owner");
            auto registered_scope = scopes_.prepare(tx);
            if (!registered_scope)
              return registered_scope.error();
            auto valid = registered_scope.value()->validate_owner(scope.value(), true);
            if (!valid)
              return valid.error();
            auto groups = waits_.prepare(tx);
            if (!groups)
              return groups.error();
            WaitCreate create;
            create.owner_process = process.value();
            create.owner_scope = scope.value();
            create.mode = static_cast<WaitMode>(mode.value());
            create.failure_policy = static_cast<WaitFailurePolicy>(policy.value());
            create.branch_count = count.value();
            create.result_type = TypeId{static_cast<std::uint32_t>(type.value())};
            create.result_bytes = bytes.value();
            auto wait_identity = std::make_shared<Handle>();
            create.resume = [this, wait_identity](const Value &v, ReadyKey key) -> Expected<void> {
              auto token = runtime_->processes().suspension_for_wait(*wait_identity);
              if (!token)
                return token.error();
              return runtime_->notify_process(token.value(), {SingleWaitStatus::Ready, v, key, 0});
            };
            auto group = groups.value()->create(std::move(create));
            if (!group)
              return group.error();
            SingleWaitSpec spec;
            spec.kind = SingleWaitKind::Response;
            spec.source = group.value();
            auto wait = runtime_->processes().register_wait(process.value(), spec, tx);
            if (!wait)
              return wait.error();
            *wait_identity = wait.value();
            auto bindings = prepare_bindings(tx);
            if (!bindings)
              return bindings.error();
            auto primary = groups.value()->result_handle(group.value(), scope.value());
            if (!primary)
              return primary.error();
            auto attached = registered_scope.value()->attach(
                {group.value(), OwnedKind::Wait, false}, scope.value());
            if (!attached)
              return attached.error();
            attached = registered_scope.value()->attach(
                {primary.value().consumer, OwnedKind::ResultConsumer, false}, scope.value());
            if (!attached)
              return attached.error();
            bindings.value()->waits.emplace(wait.value(), group.value());
            bindings.value()->wait_owners.emplace(
                wait.value(), WaitOwnership{process.value(), scope.value(), primary.value()});
            if (!count.value()) {
              auto sealed = groups.value()->commit(group.value());
              if (!sealed)
                return sealed.error();
            }
            return std::vector<Value>{Value{wait.value()}};
          }
          auto wait = service_handle(a[0], HandleKind::Wait, ctx);
          if (!wait)
            return wait.error();
          auto bindings = prepare_bindings(tx);
          if (!bindings)
            return bindings.error();
          auto entry = bindings.value()->waits.find(wait.value());
          if (entry == bindings.value()->waits.end())
            return fail(ErrorCode::StaleHandle, "wait group execution binding");
          auto groups = waits_.prepare(tx);
          if (!groups)
            return groups.error();
          if (op == Op::WaitArm) {
            auto ordinal = service_number(a[1]), priority = service_number(a[2]);
            auto task = service_handle(a[3], HandleKind::Task, ctx),
                 scope = service_handle(a[4], HandleKind::Scope, ctx);
            if (!ordinal || !priority || !task || !scope || ordinal.value() > UINT32_MAX ||
                priority.value() > INT32_MAX)
              return fail(ErrorCode::TypeMismatch, "wait branch operands");
            auto pool = tasks_.prepare(tx);
            if (!pool)
              return pool.error();
            auto result = pool.value()->result_handle(task.value(), scope.value());
            if (!result)
              return result.error();
            WaitBranch branch;
            branch.ordinal = static_cast<std::uint32_t>(ordinal.value());
            branch.priority = static_cast<std::int32_t>(priority.value());
            branch.source = task.value();
            branch.source_result = result.value();
            auto armed =
                groups.value()->arm(entry->second, std::move(branch), BatchId{}, ctx.ready);
            if (!armed)
              return armed.error();
            auto full = groups.value()->fully_armed(entry->second);
            if (!full)
              return full.error();
            if (full.value()) {
              auto committed = groups.value()->commit(entry->second);
              if (!committed)
                return committed.error();
            }
            return unit();
          }
          auto scope = service_handle(a[1], HandleKind::Scope, ctx);
          if (!scope)
            return scope.error();
          auto consumer = groups.value()->result_handle(entry->second, scope.value());
          if (!consumer)
            return consumer.error();
          if (op == Op::WaitResultGet) {
            auto v = groups.value()->owning_result(entry->second, consumer.value());
            if (!v)
              return v.error();
            return std::vector<Value>{std::move(v.value())};
          }
          auto owner = bindings.value()->wait_owners.find(wait.value());
          if (owner == bindings.value()->wait_owners.end())
            return fail(ErrorCode::Integrity, "wait ownership binding missing");
          auto suspension = runtime_->processes().suspension_for_wait(wait.value());
          if (suspension) {
            auto outcome = runtime_->processes().read_wait_result(wait.value());
            if (!outcome)
              return outcome.error();
          }
          // A queued resume owns its outcome independently of the group control.
          // Scope cancellation terminates that process; ordinary release does not.
          auto released = groups.value()->release(entry->second, consumer.value(), scope.value());
          if (!released)
            return released.error();
          auto owner_scopes = scopes_.prepare(tx);
          if (!owner_scopes)
            return owner_scopes.error();
          auto detached = owner_scopes.value()->detach_completed(entry->second, scope.value());
          if (!detached)
            return detached.error();
          detached =
              owner_scopes.value()->detach_completed(consumer.value().consumer, scope.value());
          if (!detached)
            return detached.error();
          bindings.value()->wait_owners.erase(wait.value());
          bindings.value()->waits.erase(entry);
          return unit();
        };
        auto r = invoke();
        if (!r) {
          auto error = r.error();
          auto rolled = tx.rollback(std::move(save.value()));
          if (!rolled)
            return rolled.error();
          if ((op == Op::TrySpawnProcess || op == Op::SubmitTask || op == Op::RequestTaskSlot) &&
              (error.code == ErrorCode::Capacity || error.code == ErrorCode::Cancelled ||
               error.code == ErrorCode::InvalidArgument))
            return std::vector<Value>{except_handle(Expected<Handle>{error})};
          return error;
        }
        return r;
      });
}
} // namespace leanat
