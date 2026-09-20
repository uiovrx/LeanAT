#include "leanat/structured.hpp"
namespace leanat {
Expected<TaskGroupView> make_task_group(TaskPool &p, Handle s, std::uint64_t tag) {
  if (s.kind != HandleKind::Scope)
    return fail(ErrorCode::InvalidArgument, "group scope");
  return TaskGroupView{&p, s, tag, std::make_shared<std::vector<Handle>>()};
}
static void prune(TaskGroupView &g) {
  g.membership->erase(std::remove_if(g.membership->begin(), g.membership->end(),
                                     [&](Handle h) {
                                       auto s = g.pool->state(h);
                                       return !s && s.error().code == ErrorCode::StaleHandle;
                                     }),
                      g.membership->end());
}
Expected<Handle> group_try_spawn(TaskGroupView &g, std::vector<Value> args, ContextKind c) {
  if (!g.pool)
    return fail(ErrorCode::InvalidArgument, "group pool");
  prune(g);
  if (g.membership->size() >= g.pool->descriptor().task_capacity)
    return fail(ErrorCode::Capacity, "group membership");
  auto t = g.pool->try_spawn(std::move(args), g.owner, c);
  if (!t)
    return t.error();
  g.membership->push_back(t.value());
  return t;
}
Expected<Handle> group_submit(TaskGroupView &g, std::vector<Value> args, ContextKind c) {
  if (!g.pool)
    return fail(ErrorCode::InvalidArgument, "group pool");
  prune(g);
  if (g.membership->size() >= g.pool->descriptor().task_capacity)
    return fail(ErrorCode::Capacity, "group membership");
  auto t = g.pool->submit_queued(std::move(args), g.owner, c);
  if (!t)
    return t.error();
  g.membership->push_back(t.value());
  return t;
}
Expected<IdleSnapshot> snapshot_idle(const TaskGroupView &g, ContextKind c) {
  if (c != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "awaitIdle requires process");
  if (!g.pool)
    return fail(ErrorCode::InvalidArgument, "group pool");
  return IdleSnapshot{*g.membership};
}
Expected<bool> idle_ready(TaskPool &p, const IdleSnapshot &snapshot) {
  for (auto t : snapshot.tasks) {
    auto s = p.state(t);
    if (!s) {
      if (s.error().code == ErrorCode::StaleHandle)
        continue;
      return s.error();
    }
    if (s.value() == TaskState::Queued || s.value() == TaskState::Runnable ||
        s.value() == TaskState::Waiting)
      return false;
  }
  return true;
}
Expected<void> cancel_children(TaskGroupView &g, std::string reason, ReadyKey key) {
  if (!g.pool)
    return fail(ErrorCode::InvalidArgument, "group pool");
  auto members = *g.membership;
  for (auto h : members) {
    auto s = g.pool->state(h);
    if (!s && s.error().code == ErrorCode::StaleHandle)
      continue;
    auto c = g.pool->cancel(h, reason, key);
    if (!c)
      return c.error();
  }
  return {};
}
Expected<OwnedChildSet> fan_out(TaskGroupView &group, std::vector<std::vector<Value>> args,
                                ReadyKey key, ContextKind context) {
  if (!group.pool)
    return fail(ErrorCode::InvalidArgument, "fanout pool");
  if (args.size() > group.pool->descriptor().task_capacity)
    return fail(ErrorCode::Capacity, "fanout child bound");
  OwnedChildSet result;
  result.scope = group.owner;
  for (auto &a : args) {
    auto t = group_try_spawn(group, std::move(a), context);
    if (!t) {
      for (auto &child : result.children) {
        auto c = group.pool->cancel(child.task, "fanout allocation failure", key);
        if (!c)
          return c.error();
        auto rel = group.pool->release_result(child.task, child.consumer);
        if (!rel)
          return rel.error();
      }
      return t.error();
    }
    auto consumer = group.pool->result_handle(t.value(), group.owner);
    if (!consumer)
      return consumer.error();
    result.children.push_back({t.value(), consumer.value()});
  }
  return result;
}
Expected<std::vector<Value>> fan_in(TaskPool &p, OwnedChildSet &set, ContextKind c) {
  if (c != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "fanIn requires process");
  std::vector<PinnedResultView> pins;
  std::vector<Value> values;
  for (std::size_t i = 0; i < set.children.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j)
      if (set.children[i].consumer.consumer == set.children[j].consumer.consumer)
        return fail(ErrorCode::Duplicate, "fanIn duplicated owning consumer");
    auto pin = p.pin_result(set.children[i].task, set.children[i].consumer);
    if (!pin)
      return pin.error();
    values.push_back(pin.value().value());
    pins.push_back(std::move(pin.value()));
  }
  for (auto &child : set.children) {
    auto r = p.release_result(child.task, child.consumer);
    if (!r)
      return r.error();
  }
  set.children.clear();
  return values;
}
TimeoutOperation::TimeoutOperation(AdmissionStore &a, Handle t, ConnectionId c, Tick deadline,
                                   std::function<Expected<std::optional<Handle>>()> drain)
    : admission_(a), txn_(t), connection_(c), deadline_(deadline), drain_(std::move(drain)) {}
Expected<void> TimeoutOperation::expire() {
  if (state_ == TimeoutState::Success || state_ == TimeoutState::FinishingResponse)
    return fail(ErrorCode::InvalidState, "response already won");
  if (state_ == TimeoutState::TimedOut)
    return {};
  if (gate_) {
    auto s = admission_.gate_state(*gate_);
    if (!s)
      return s.error();
    if (s.value() == RequestGateState::Pending || s.value() == RequestGateState::Granted) {
      auto c = admission_.cancel_request_gate(*gate_, action_key_);
      if (!c)
        return c.error();
    }
    auto r = admission_.retire_request_ticket(*gate_);
    if (!r && s.value() != RequestGateState::Sent)
      return r.error();
    gate_.reset();
  }
  if (transferred_publication_ && wire_published_)
    result_.published = wire_published_();
  if (result_.published || transferred_publication_) {
    if (!drain_)
      return fail(ErrorCode::InvalidState, "published timeout requires drain executor");
    auto d = drain_();
    if (!d)
      return d.error();
    result_.drain = d.value();
  }
  state_ = TimeoutState::TimedOut;
  result_.state = state_;
  return {};
}
Expected<void> TimeoutOperation::start(ReadyKey now, ContextKind c) {
  action_key_ = now;
  if (c != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "timeout requires process");
  if (state_ != TimeoutState::Fresh)
    return fail(ErrorCode::Duplicate, "timeout already started");
  if (!(now.time < deadline_))
    return expire();
  auto g = admission_.request_request_gate(txn_, connection_, now);
  if (!g)
    return g.error();
  gate_ = g.value();
  state_ = TimeoutState::GateWaiting;
  result_.state = state_;
  return {};
}
Expected<bool>
TimeoutOperation::advance_gate(ReadyKey now, bool open,
                               const std::function<Expected<void>(RequestPermit)> &publish,
                               bool deferred_publication, std::function<bool()> wire_published) {
  action_key_ = now;
  if (state_ == TimeoutState::TimedOut)
    return false;
  if (state_ != TimeoutState::GateWaiting)
    return fail(ErrorCode::InvalidState, "not waiting for gate");
  if (!open || !(now.time < deadline_)) {
    auto e = expire();
    if (!e)
      return e.error();
    return false;
  }
  auto s = admission_.gate_state(*gate_);
  if (!s)
    return s.error();
  if (s.value() == RequestGateState::Pending)
    return false;
  if (s.value() != RequestGateState::Granted)
    return fail(ErrorCode::Cancelled, "gate cancelled");
  if (!publish)
    return fail(ErrorCode::InvalidArgument, "missing publish executor");
  if (deferred_publication && (!wire_published || !drain_))
    return fail(ErrorCode::InvalidArgument, "deferred publication needs wire observer and cleanup");
  auto p = admission_.consume_request_gate(*gate_);
  if (!p)
    return p.error();
  permit_ = p.value();
  state_ = TimeoutState::ResponseWaiting;
  result_.state = state_;
  auto sent = publish(*permit_);
  if (!sent) {
    auto cancelled = admission_.cancel_request_permit(*permit_, now);
    if (!cancelled)
      return cancelled.error();
    auto retired = admission_.retire_request_ticket(*gate_);
    if (!retired)
      return retired.error();
    gate_.reset();
    permit_.reset();
    state_ = TimeoutState::ModelError;
    result_.state = state_;
    return sent.error();
  }
  if (deferred_publication) {
    transferred_publication_ = true;
    wire_published_ = std::move(wire_published);
    result_.published = wire_published_();
    gate_.reset();
    permit_.reset();
  } else {
    auto marked = admission_.request_sent(*permit_);
    if (!marked)
      return marked.error();
    result_.published = true;
  }
  state_ = TimeoutState::ResponseWaiting;
  result_.state = state_;
  return true;
}
Expected<void> TimeoutOperation::record_response(Value value, ReadyKey key) {
  if (state_ != TimeoutState::ResponseWaiting)
    return fail(ErrorCode::InvalidState, "response registration inactive");
  if (response_)
    return {};
  response_ = WaitNotification{{}, 0, txn_, key, {}, std::move(value), false};
  return {};
}
Expected<void> TimeoutOperation::record_timeout(ReadyKey key) {
  if (key.time < deadline_)
    return fail(ErrorCode::InvalidArgument, "early timeout");
  if (state_ != TimeoutState::ResponseWaiting && state_ != TimeoutState::GateWaiting)
    return fail(ErrorCode::InvalidState, "timeout registration inactive");
  if (!timeout_)
    timeout_ = key;
  return {};
}
Expected<TimeoutResult> TimeoutOperation::resolve(bool closed) {
  if (!closed)
    return fail(ErrorCode::InvalidState, "timeout batch open");
  if (state_ == TimeoutState::TimedOut || state_ == TimeoutState::Success ||
      state_ == TimeoutState::FinishingResponse)
    return result_;
  if (response_ && (!timeout_ || !(*timeout_ < response_->ready))) {
    state_ = TimeoutState::FinishingResponse;
    result_.state = state_;
    result_.response = response_->outcome;
    timeout_.reset();
    return result_;
  }
  if (timeout_) {
    action_key_ = *timeout_;
    auto e = expire();
    if (!e)
      return e.error();
    return result_;
  }
  return fail(ErrorCode::NotReady, "no timeout candidates");
}
Expected<void> TimeoutOperation::finish_response(Value value) {
  if (state_ != TimeoutState::FinishingResponse)
    return fail(ErrorCode::InvalidState, "response not selected");
  if (gate_) {
    auto retired = admission_.retire_request_ticket(*gate_);
    if (!retired)
      return retired.error();
    gate_.reset();
    permit_.reset();
  }
  result_.response = std::move(value);
  state_ = TimeoutState::Success;
  result_.state = state_;
  return {};
}
RetryController::RetryController(RetryPolicy p, RetrySafety s,
                                 std::function<Expected<void>(const RetrySafety &)> verifier)
    : policy_(p), safety_(std::move(s)), verify_safety_(std::move(verifier)) {}
Expected<std::optional<RetryAttempt>> RetryController::next(Tick now, bool retryable, bool unsent) {
  if (awaiting_record_)
    return fail(ErrorCode::InvalidState, "previous attempt outcome not recorded");
  if (attempts_ && (!retryable || attempts_ > policy_.max_retries))
    return std::optional<RetryAttempt>{};
  if (!(now < policy_.deadline))
    return std::optional<RetryAttempt>{};
  if (attempts_) {
    if (!verify_safety_)
      return fail(ErrorCode::InvalidArgument, "retry requires a trusted contract verifier");
    auto verified = verify_safety_(safety_);
    if (!verified)
      return verified.error();
    if (!last_ || last_->state == TimeoutState::ModelError)
      return fail(ErrorCode::InvalidState, "runtime/model faults are not automatically retried");
    if (safety_.kind == RetrySafetyKind::Deduplicated && (!safety_.key || !safety_.epoch))
      return fail(ErrorCode::InvalidArgument, "deduplication key/epoch missing");
    if (safety_.kind == RetrySafetyKind::ProvenUnsent && (!unsent || !last_ || last_->published))
      return fail(ErrorCode::InvalidArgument, "previous attempt not proven unsent");
    auto start = add_time(now, policy_.backoff);
    if (!start)
      return start.error();
    now = start.value();
    if (!(now < policy_.deadline) || drains_.size() >= policy_.drain_capacity)
      return std::optional<RetryAttempt>{};
  }
  Tick deadline = policy_.deadline;
  if (policy_.attempt_timeout) {
    auto d = add_time(now, *policy_.attempt_timeout);
    if (!d)
      return d.error();
    if (d.value() < deadline)
      deadline = d.value();
  }
  if (attempts_ > UINT32_MAX)
    return fail(ErrorCode::Overflow, "retry attempt ordinal");
  RetryAttempt attempt{static_cast<std::uint32_t>(attempts_), now, deadline};
  ++attempts_;
  awaiting_record_ = true;
  return std::optional<RetryAttempt>{attempt};
}
Expected<void> RetryController::record(TimeoutResult r) {
  if (!attempts_ || !awaiting_record_)
    return fail(ErrorCode::InvalidState, "no retry attempt");
  if (r.state != TimeoutState::Success && r.state != TimeoutState::TimedOut &&
      r.state != TimeoutState::ModelError)
    return fail(ErrorCode::NotReady, "retry outcome is not terminal");
  if (r.drain) {
    if (drains_.size() >= policy_.drain_capacity)
      return fail(ErrorCode::Capacity, "retry cleanup capacity");
    drains_.push_back(*r.drain);
  }
  last_ = std::move(r);
  awaiting_record_ = false;
  return {};
}
Expected<Tick> transaction_deadline(Tick now, Duration duration) {
  return add_time(now, duration);
}
Expected<Value> with_scoped_operation(
    CancelScopeStore &scopes, TaskPool &pool, Handle parent, ReadyKey key,
    const std::function<Expected<Value>(Handle)> &body,
    const std::function<Expected<void>(const ScopeCancelAction &)> &external_cleanup) {
  if (!body)
    return fail(ErrorCode::InvalidArgument, "scoped operation body missing");
  auto child = scopes.create(parent);
  if (!child)
    return child.error();
  auto result = body(child.value());
  // Owning values are copied before epilogue; handles require a transfer plan.
  std::function<bool(const Value &)> has_handle = [&](const Value &v) {
    if (std::holds_alternative<Handle>(v.data))
      return true;
    if (auto a = std::get_if<Value::Array>(&v.data))
      for (const auto &x : *a)
        if (has_handle(x))
          return true;
    return false;
  };
  if (result && has_handle(result.value()))
    result = fail(ErrorCode::Unsupported, "scoped handle return requires explicit transfer");
  auto plan = scopes.cancel(child.value(), "scoped operation epilogue");
  if (!plan)
    return plan.error();
  for (auto action : plan.value().actions) {
    auto applied = scopes.apply(child.value(), action.id, [&](const ScopeCancelAction &a) {
      auto local = pool.execute_cancel_action(a, key);
      if (local || local.error().code != ErrorCode::Unsupported)
        return local;
      if (external_cleanup)
        return external_cleanup(a);
      return Expected<void>{fail(ErrorCode::Unsupported, "scoped resource needs cleanup executor")};
    });
    if (!applied)
      return applied.error();
  }
  auto closed = scopes.close(child.value());
  if (!closed)
    return closed.error();
  return result;
}
} // namespace leanat
