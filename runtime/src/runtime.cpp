#include "leanat/runtime.hpp"
#include "leanat/admission.hpp"
#include "leanat/storage_identity.hpp"
#include <set>
namespace leanat {
namespace {
bool outbound_data_equal(const SendIntent &intent, const PayloadSnapshot &actual) {
  if (intent.phase == begin_resp && intent.payload.command != Command::Read && actual.data.empty())
    return true;
  if (intent.payload.data.size() != actual.data.size())
    return false;
  if (intent.phase != begin_resp || intent.payload.command != Command::Read)
    return intent.payload.data == actual.data;
  for (std::size_t n = 0; n < actual.data.size(); ++n)
    if ((intent.payload.byte_enable.empty() ||
         intent.payload.byte_enable[n % intent.payload.byte_enable.size()] != 0) &&
        intent.payload.data[n] != actual.data[n])
      return false;
  return true;
}
bool payload_fits(const PayloadSnapshot &p, std::size_t capacity) {
  std::size_t used = 0;
  auto add = [&](std::size_t n) {
    if (n > capacity - used)
      return false;
    used += n;
    return true;
  };
  if (!add(p.data.size()) || !add(p.byte_enable.size()))
    return false;
  for (auto &e : p.extensions)
    if (!add(e.first.size()) || !add(e.second.size()))
      return false;
  return true;
}
} // namespace
Runtime::Runtime(const RuntimeConfig &c, RuntimeHost &h)
    : config_(c), host_(h), events_(c.event_capacity, c.domain),
      protocol_(c.domain, c.instance, c.ledger_capacity, c.call_capacity, c.calls_per_ledger),
      results_(c.result_capacity, c.consumer_capacity, c.pin_capacity, c.domain),
      processes_(c.domain, 10, c.frame_capacity, c.wait_capacity, c.live_capacity),
      drains_(c.domain, c.drain_capacity, c.hops_per_transaction, c.instance_capacity) {
  business_store_ = storage_detail::allocate_store_incarnation();
}
Expected<ProtocolEngine *> Runtime::protocol(InstanceId side) {
  if (side == config_.instance)
    return &protocol_;
  auto i = protocols_.find(side);
  if (i == protocols_.end())
    return fail(ErrorCode::WrongOwner, "unknown local protocol side");
  return i->second.get();
}
Expected<InstanceId> Runtime::call_side(ConnectionId connection, Flow flow, bool outgoing) const {
  for (auto &binding : config_.connection_bindings)
    if (binding.connection == connection) {
      auto side = (flow == Flow::Forward) == outgoing ? binding.initiator : binding.target;
      if (side != config_.instance && !protocols_.count(side))
        return fail(ErrorCode::WrongOwner, "call endpoint is external to this runtime");
      return side;
    }
  if (!config_.connection_bindings.empty())
    return fail(ErrorCode::WrongOwner, "unbound connection");
  if (std::find(config_.connections.begin(), config_.connections.end(), connection) ==
      config_.connections.end())
    return fail(ErrorCode::WrongOwner, "unbound connection");
  return config_.instance;
}
Expected<ProtocolEngine *> Runtime::engine_for_hop(Handle hop) {
  auto primary = protocol_.inspect(hop);
  if (primary)
    return &protocol_;
  for (auto &entry : protocols_)
    if (entry.second->inspect(hop))
      return entry.second.get();
  return fail(ErrorCode::StaleHandle, "unknown local hop");
}
Expected<WireSnapshot> Runtime::inspect_hop(Handle hop) {
  auto engine = engine_for_hop(hop);
  if (!engine)
    return engine.error();
  return engine.value()->inspect(hop);
}
Expected<void> Runtime::operational() const {
  if (!started_)
    return fail(ErrorCode::NotStarted, "runtime not started");
  if (stopped_)
    return fail(ErrorCode::InvalidState, "runtime stopped: " + stop_detail_);
  return {};
}
Expected<Handle>
Runtime::submit_blocking(ConnectionId connection, PayloadSnapshot payload, Tick arrival,
                         std::function<void(Expected<ResponseSnapshot>)> completion) {
  Entry entry(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (!blocking_handler_ || !completion)
    return fail(ErrorCode::Unsupported, "direct blocking service is not bound");
  if (blocking_.size() >= config_.blocking_capacity)
    return fail(ErrorCode::Capacity, "blocking business pool full");
  if (!payload_fits(payload, config_.ingress_bytes))
    return fail(ErrorCode::Capacity, "blocking snapshot byte capacity");
  if (arrival < now_)
    return fail(ErrorCode::TimeRegression, "blocking arrival before host time");
  if (next_business_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "blocking business identity exhausted");
  auto side = call_side(connection, Flow::Forward, false);
  if (!side)
    return side.error();
  auto epoch = drains_.epoch(side.value());
  if (!epoch)
    return epoch.error();
  std::uint32_t slot = 0;
  for (;; ++slot) {
    bool used = false;
    for (auto &r : blocking_)
      if (r.first.slot == slot)
        used = true;
    if (!used)
      break;
  }
  auto generation = next_business_++;
  Handle token{
      HandleKind::Transaction, config_.domain, business_store_, slot, generation, generation};
  bool deferred = !drains_.admits_root(side.value());
  std::optional<EventReservation> reserved;
  if (!deferred) {
    auto ready = events_.successor(arrival);
    if (!ready)
      return ready.error();
    EventDraft event;
    event.key = {ready.value().time, ready.value().turn, EventStage::Input,
                 side.value(),       connection,         0};
    event.owner = token.owner;
    event.epoch = epoch.value();
    auto r = events_.prepare(std::move(event));
    if (!r)
      return r.error();
    reserved.emplace(std::move(r.value()));
  }
  Responsibility responsibility{token, side.value(), epoch.value(), {}, {}, false, false};
  auto registered = deferred ? drains_.defer_root(responsibility)
                             : drains_.register_responsibility(responsibility);
  if (!registered)
    return registered.error();
  BlockingRequest request{token,   connection,   side.value(), std::move(payload),
                          arrival, epoch.value()};
  blocking_.emplace(token, BlockingRecord{std::move(request), std::move(completion), {}, deferred});
  if (reserved) {
    auto event = events_.enqueue(std::move(*reserved));
    if (!event) {
      stop(event.error());
      return event.error();
    }
    blocking_events_.emplace(event.value(), std::make_pair(token, false));
  }
  dirty_ = true;
  return token;
}
Expected<void> Runtime::complete_blocking(Handle token, ResponseSnapshot response) {
  Entry entry(*this);
  auto record = blocking_.find(token);
  if (record == blocking_.end())
    return fail(ErrorCode::StaleHandle, "blocking business token");
  if (record->second.response)
    return fail(ErrorCode::Duplicate, "blocking completion already queued");
  auto &request = record->second.request;
  if (response.status == ResponseStatus::Incomplete)
    return fail(ErrorCode::ProtocolViolation, "blocking response incomplete");
  if ((request.request.command == Command::Read &&
       response.data.size() != request.request.data.size()) ||
      (request.request.command != Command::Read && !response.data.empty()))
    return fail(ErrorCode::Schema, "blocking response data shape");
  PayloadSnapshot bounded;
  bounded.data = response.data;
  bounded.extensions = response.extensions;
  if (!payload_fits(bounded, config_.ingress_bytes))
    return fail(ErrorCode::Capacity, "blocking response snapshot capacity");
  auto ready = events_.successor(now_);
  if (!ready)
    return ready.error();
  EventDraft event;
  event.key = {ready.value().time, ready.value().turn, EventStage::Output,
               request.instance,   request.connection, 0};
  event.owner = token.owner;
  event.epoch = request.epoch;
  auto reserved = events_.prepare(std::move(event));
  if (!reserved)
    return reserved.error();
  auto queued = events_.enqueue(std::move(reserved.value()));
  if (!queued)
    return queued.error();
  record->second.response = std::move(response);
  blocking_events_.emplace(queued.value(), std::make_pair(token, true));
  dirty_ = true;
  return {};
}
Expected<void> Runtime::cancel_blocking(Handle token) {
  auto record = blocking_.find(token);
  if (record == blocking_.end())
    return fail(ErrorCode::StaleHandle, "blocking business token");
  auto cancelled = drains_.cancel_local(token, CancelReason::User);
  if (!cancelled)
    return cancelled.error();
  if (record->second.response)
    return {};
  ResponseSnapshot response;
  response.status = ResponseStatus::GenericError;
  if (record->second.request.request.command == Command::Read)
    response.data = record->second.request.request.data;
  return complete_blocking(token, std::move(response));
}
Expected<void> Runtime::dispatch_blocking(EventToken event, ReadyKey ready) {
  auto action = blocking_events_.find(event);
  if (action == blocking_events_.end())
    return fail(ErrorCode::Integrity, "missing blocking event");
  auto token = action->second.first;
  bool completion = action->second.second;
  blocking_events_.erase(action);
  auto record = blocking_.find(token);
  if (record == blocking_.end())
    return {};
  if (completion) {
    if (!record->second.response)
      return fail(ErrorCode::Integrity, "missing blocking response");
    auto response = *record->second.response;
    auto callback = std::move(record->second.completion);
    blocking_.erase(record);
    auto finished = drains_.finish_local(token);
    if (!finished && finished.error().code != ErrorCode::StaleHandle)
      return finished.error();
    (void)drains_.retire_responsibility(token);
    try {
      callback(std::move(response));
    } catch (...) {
      return fail(ErrorCode::ExternalFailure, "blocking completion callback threw");
    }
    return {};
  }
  if (record->second.response)
    return {};
  auto epoch = drains_.epoch(record->second.request.instance);
  if (!epoch)
    return epoch.error();
  if (drains_.is_cancelled(token) || record->second.request.epoch != epoch.value()) {
    ResponseSnapshot response;
    response.status = ResponseStatus::GenericError;
    if (record->second.request.request.command == Command::Read)
      response.data = record->second.request.request.data;
    return complete_blocking(token, std::move(response));
  }
  record->second.request.arrival = ready.time;
  return blocking_handler_(record->second.request, *this);
}
Expected<void> Runtime::promote_blocking() {
  for (auto &entry : blocking_) {
    auto &r = entry.second;
    if (!r.deferred || drains_.is_deferred(entry.first))
      continue;
    auto epoch = drains_.epoch(r.request.instance);
    if (!epoch)
      return epoch.error();
    auto tick = now_ < r.request.arrival ? r.request.arrival : now_;
    auto ready = events_.successor(tick);
    if (!ready)
      return ready.error();
    EventDraft event;
    event.key = {ready.value().time, ready.value().turn,   EventStage::Input,
                 r.request.instance, r.request.connection, 0};
    event.owner = entry.first.owner;
    event.epoch = epoch.value();
    auto token = events_.enqueue(event);
    if (!token)
      return token.error();
    r.request.epoch = epoch.value();
    r.deferred = false;
    blocking_events_.emplace(token.value(), std::make_pair(entry.first, false));
    dirty_ = true;
  }
  return {};
}
Expected<EventToken> Runtime::stage_output(EventTxn &txn, PortId port, Value value) {
  auto ready = events_.successor(txn.context().ready.time);
  if (!ready)
    return ready.error();
  EventDraft event;
  event.key = {
      ready.value().time, ready.value().turn, EventStage::Output, txn.context().instance, {}, 0};
  event.owner = txn.context().owner;
  event.epoch = txn.context().epoch;
  event.value = Value{Value::Array{Value{std::uint64_t{0x4c41544f}},
                                   Value{std::uint64_t{port.value}}, std::move(value)}};
  return txn.stage_event(events_, std::move(event));
}
void Runtime::stop(Error e) {
  if (stopped_) {
    dirty_ = true;
    return;
  }
  stopped_ = true;
  stop_detail_ = e.message;
  dirty_ = true;
  TraceEvent trace;
  trace.kind = "runtime.stopped";
  trace.ready = {now_, 0};
  trace.instance = config_.instance;
  trace.detail = stop_detail_;
  for (auto &call : outgoing_)
    trace.values.push_back(Value{call.first.value});
  try {
    host_.emit_trace(trace);
  } catch (...) {
  }
  for (auto &entry : blocking_)
    if (entry.second.completion) {
      auto callback = std::move(entry.second.completion);
      try {
        callback(fail(ErrorCode::ExternalFailure, stop_detail_));
      } catch (...) {
      }
    }
}
void Runtime::report_host_failure(Error e) {
  Entry entry(*this);
  stop(std::move(e));
}
void Runtime::reconcile() {
  if (!dirty_)
    return;
  dirty_ = false;
  if (wake_generation_ == UINT64_MAX) {
    stopped_ = true;
    stop_detail_ = "wakeup generation exhausted";
  } else
    ++wake_generation_;
  try {
    host_.arm(next_wakeup());
  } catch (...) {
    stopped_ = true;
    stop_detail_ = "Host arm threw";
  }
}
std::optional<WakePoint> Runtime::next_wakeup() const {
  if (!started_ || stopped_)
    return {};
  auto w = events_.next_wakeup();
  for (auto &i : intents_) {
    WakePoint v{i.second.not_before < now_ ? now_ : i.second.not_before, 0, wake_generation_};
    if (!w || std::tie(v.time, v.turn) < std::tie(w->time, w->turn))
      w = v;
  }
  if (w)
    w->generation = wake_generation_ + (dirty_ && wake_generation_ != UINT64_MAX ? 1 : 0);
  return w;
}
Expected<void> Runtime::start(const HostBindingManifest &m) {
  Entry e(*this);
  if (started_)
    return fail(ErrorCode::Duplicate, "runtime already started");
  auto a = m.connections, b = config_.connections;
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  if (m.domain != config_.domain || m.descriptor_identity != config_.descriptor_identity ||
      m.descriptor_identity.empty() || a != b || !m.clock_grid_valid || !m.capabilities_valid)
    return fail(ErrorCode::Integrity, "host binding manifest mismatch");
  auto expected_instances = config_.instances;
  if (expected_instances.empty())
    expected_instances.push_back(config_.instance);
  auto actual_instances = m.instances;
  if (actual_instances.empty() && config_.instances.empty())
    actual_instances.push_back(config_.instance);
  std::sort(expected_instances.begin(), expected_instances.end());
  std::sort(actual_instances.begin(), actual_instances.end());
  auto expected_bindings = config_.connection_bindings, actual_bindings = m.connection_bindings;
  auto order = [](const RuntimeConnectionBinding &a, const RuntimeConnectionBinding &b) {
    return a.connection < b.connection;
  };
  std::sort(expected_bindings.begin(), expected_bindings.end(), order);
  std::sort(actual_bindings.begin(), actual_bindings.end(), order);
  if (expected_instances != actual_instances || expected_bindings != actual_bindings ||
      expected_instances.size() > config_.instance_capacity ||
      std::adjacent_find(expected_instances.begin(), expected_instances.end()) !=
          expected_instances.end() ||
      !std::binary_search(expected_instances.begin(), expected_instances.end(), config_.instance))
    return fail(ErrorCode::Integrity, "host instance/local-side binding mismatch");
  for (std::size_t n = 0; n < expected_bindings.size(); ++n) {
    auto &binding = expected_bindings[n];
    if ((n && expected_bindings[n - 1].connection == binding.connection) ||
        !std::binary_search(b.begin(), b.end(), binding.connection))
      return fail(ErrorCode::Integrity, "connection side binding");
  }
  if (std::adjacent_find(a.begin(), a.end()) != a.end() || !config_.event_capacity ||
      !config_.ledger_capacity || !config_.call_capacity || !config_.intent_capacity ||
      !config_.drain_capacity || !config_.max_events || !config_.max_events_per_tick ||
      !config_.nesting_capacity)
    return fail(ErrorCode::InvalidArgument, "invalid finite capacity or duplicate binding");
  for (auto instance : expected_instances) {
    auto r = drains_.add_instance(instance);
    if (!r)
      return r.error();
    if (instance != config_.instance)
      protocols_.emplace(instance, std::make_unique<ProtocolEngine>(
                                       config_.domain, instance, config_.ledger_capacity,
                                       config_.call_capacity, config_.calls_per_ledger));
  }
  started_ = true;
  dirty_ = true;
  return {};
}
Expected<CallId> Runtime::allocate_call_id(CallOrigin origin) {
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (allocated_.size() >= config_.call_capacity)
    return fail(ErrorCode::Capacity, "call identity reservations full");
  if (next_call_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "call identity exhausted");
  CallId id{next_call_++};
  allocated_.emplace(id, origin);
  return id;
}
Expected<void> Runtime::release_call_id(CallId id) {
  if (intents_.count(id) || outgoing_.count(id) || prepared_intents_.count(id))
    return fail(ErrorCode::NotReady, "call identity is owned");
  auto i = allocated_.find(id);
  if (i == allocated_.end())
    return fail(ErrorCode::StaleHandle, "call identity");
  allocated_.erase(i);
  return {};
}
class PreparedCallIdentity final : public PreparedParticipant {
  Runtime &runtime_;
  CallId id_;
  bool active_{true};

public:
  PreparedCallIdentity(Runtime &runtime, CallId id) : runtime_(runtime), id_(id) {}
  ~PreparedCallIdentity() override {
    discard();
  }
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this);
  }
  Expected<void> validate() const override {
    return {};
  }
  void apply() noexcept override {
    active_ = false;
  }
  void discard() noexcept override {
    if (active_) {
      (void)runtime_.release_call_id(id_);
      active_ = false;
    }
  }
};
Expected<CallId> Runtime::stage_allocate_call_id(EventTxn &txn, CallOrigin origin) {
  if (txn.context().domain != config_.domain)
    return fail(ErrorCode::WrongDomain, "call reservation context");
  auto id = allocate_call_id(origin);
  if (!id)
    return id.error();
  auto staged = txn.stage_participant(std::make_unique<PreparedCallIdentity>(*this, id.value()));
  if (!staged)
    return staged.error();
  return id.value();
}
class PreparedRequestPermit final : public PreparedParticipant {
  Runtime &runtime_;
  std::map<CallId, Runtime::IntentHooks> node_;
  std::map<std::pair<InstanceId, ConnectionId>, AdmissionStore *> lanes_;
  bool active_{true};
  EventTxn &transaction_;
  AdmissionStore &store_;
  RequestPermit permit_;
  Handle txn_;
  ConnectionId connection_;

public:
  PreparedRequestPermit(Runtime &r, CallId id, AdmissionStore &store, RequestPermit permit,
                        EventTxn &transaction, Handle txn, ConnectionId connection)
      : runtime_(r), transaction_(transaction), store_(store), permit_(permit), txn_(txn),
        connection_(connection) {
    Runtime::IntentHooks hooks;
    hooks.start = [&store, permit]() -> Expected<void> {
      auto sent = store.request_sent(permit);
      if (!sent)
        return sent.error();
      return store.retire_request_ticket(RequestGateTicket{permit.handle});
    };
    hooks.cancel = [&store, permit](ReadyKey ready) -> Expected<void> {
      auto cancelled = store.cancel_request_permit(permit, ready);
      if (!cancelled)
        return cancelled.error();
      return store.retire_request_ticket(RequestGateTicket{permit.handle});
    };
    node_.emplace(id, std::move(hooks));
    lanes_.emplace(std::make_pair(store.local_side(), connection), &store);
    r.prepared_hooks_.emplace(id, true);
  }
  ~PreparedRequestPermit() override {
    discard();
  }
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this) + sizeof(Runtime::IntentHooks) +
           2 * (sizeof(RequestPermit) + sizeof(AdmissionStore *));
  }
  Expected<void> validate() const override {
    if (!active_ || !runtime_.prepared_hooks_.count(node_.begin()->first))
      return fail(ErrorCode::InvalidState, "request permit hook lost");
    AdmissionStore *view = &store_;
    if (auto update = dynamic_cast<AdmissionUpdate *>(transaction_.participant(&store_)))
      view = &update->draft();
    return view->validate_request_permit(permit_, txn_, connection_);
  }
  void apply() noexcept override {
    auto id = node_.begin()->first;
    runtime_.intent_hooks_.merge(node_);
    runtime_.admission_bindings_.merge(lanes_);
    runtime_.prepared_hooks_.erase(id);
    active_ = false;
  }
  void discard() noexcept override {
    if (active_) {
      runtime_.prepared_hooks_.erase(node_.begin()->first);
      active_ = false;
    }
  }
};
Expected<void> Runtime::stage_request_permit(EventTxn &txn, CallId id, AdmissionStore &store,
                                             RequestPermit permit) {
  if (!allocated_.count(id))
    return fail(ErrorCode::InvalidArgument, "request permit call identity");
  if (permit.handle.domain != txn.context().domain || permit.handle.owner != txn.context().owner)
    return fail(ErrorCode::WrongOwner, "request permit context");
  const SendIntent *intent = nullptr;
  if (auto i = prepared_intents_.find(id); i != prepared_intents_.end())
    intent = i->second;
  else if (auto i = intents_.find(id); i != intents_.end())
    intent = &i->second;
  if (!intent || intent->phase != begin_req || intent->flow != Flow::Forward)
    return fail(ErrorCode::InvalidState, "request permit needs committed BEGIN_REQ intent");
  if (intent_hooks_.count(id) || prepared_hooks_.count(id))
    return fail(ErrorCode::Duplicate, "request permit hook exists");
  if (intent_hooks_.size() + prepared_hooks_.size() >= config_.intent_capacity)
    return fail(ErrorCode::Capacity, "request permit hooks full");
  AdmissionStore *view = &store;
  if (auto update = dynamic_cast<AdmissionUpdate *>(txn.participant(&store)))
    view = &update->draft();
  if (store.domain() != txn.context().domain || store.local_side() != txn.context().instance)
    return fail(ErrorCode::WrongOwner, "request permit store context");
  auto existing = admission_bindings_.find({store.local_side(), intent->connection});
  if (existing != admission_bindings_.end() && existing->second != &store)
    return fail(ErrorCode::WrongOwner, "conflicting admission binding");
  auto valid = view->validate_request_permit(permit, intent->txn, intent->connection);
  if (!valid)
    return valid.error();
  return txn.stage_participant(std::make_unique<PreparedRequestPermit>(
      *this, id, store, permit, txn, intent->txn, intent->connection));
}
class PreparedIntent final : public PreparedParticipant {
  Runtime *runtime_;
  std::map<CallId, SendIntent> node_;
  bool active_{true};

public:
  PreparedIntent(Runtime &r, SendIntent i) : runtime_(&r) {
    node_.emplace(i.call_id, std::move(i));
    r.prepared_intents_.emplace(node_.begin()->first, &node_.begin()->second);
  }
  ~PreparedIntent() override {
    discard();
  }
  std::size_t reserved_bytes() const noexcept override {
    if (node_.empty())
      return sizeof(*this);
    const auto &p = node_.begin()->second.payload;
    std::size_t bytes = sizeof(*this) + sizeof(SendIntent) + p.data.size() + p.byte_enable.size();
    for (auto &e : p.extensions)
      bytes += e.first.size() + e.second.size();
    return bytes;
  }
  Expected<void> validate() const override {
    auto ok = runtime_->operational();
    if (!ok)
      return ok.error();
    auto id = node_.begin()->first;
    if (!active_ || !runtime_->prepared_intents_.count(id) || runtime_->intents_.count(id) ||
        !runtime_->allocated_.count(id))
      return fail(ErrorCode::InvalidState, "intent preparation lost");
    return {};
  }
  void apply() noexcept override {
    auto id = node_.begin()->first;
    runtime_->intents_.merge(node_);
    runtime_->prepared_intents_.erase(id);
    runtime_->dirty_ = true;
    active_ = false;
  }
  void discard() noexcept override {
    if (active_) {
      runtime_->prepared_intents_.erase(node_.begin()->first);
      active_ = false;
    }
  }
};
Expected<void> Runtime::stage_publish(EventTxn &txn, SendIntent intent) {
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (txn.context().domain != config_.domain || txn.context().owner != intent.txn.owner)
    return fail(ErrorCode::WrongOwner, "intent transaction context");
  if (intents_.size() + prepared_intents_.size() >= config_.intent_capacity)
    return fail(ErrorCode::Capacity, "intent pool full");
  if (!allocated_.count(intent.call_id) || allocated_.at(intent.call_id) != CallOrigin::Outgoing)
    return fail(ErrorCode::InvalidArgument, "unreserved outgoing identity");
  if (intents_.count(intent.call_id) || prepared_intents_.count(intent.call_id))
    return fail(ErrorCode::Duplicate, "intent already owned");
  if (intent.not_before < now_)
    return fail(ErrorCode::TimeRegression, "intent in past");
  return txn.stage_participant(std::make_unique<PreparedIntent>(*this, std::move(intent)));
}
Expected<CommittedActions> Runtime::commit_segment(EventTxn &txn) {
  Entry e(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (txn.context().domain != config_.domain)
    return fail(ErrorCode::WrongDomain, "segment domain");
  auto epoch =
      drains_.epoch(txn.context().instance.value ? txn.context().instance : config_.instance);
  if (!epoch)
    return epoch.error();
  auto committed = txn.commit(epoch.value());
  if (committed)
    dirty_ = true;
  return committed;
}
bool Runtime::has_unstarted_intent(Handle txn) const {
  for (auto &i : intents_)
    if (i.second.txn == txn && !outgoing_.count(i.first))
      return true;
  return false;
}
Expected<CancelDisposition> Runtime::stage_cancel_local(EventTxn &txn, Handle h,
                                                        CancelReason reason) {
  auto many = stage_cancel_local_many(txn, {h}, reason);
  if (!many)
    return many.error();
  return many.value().front();
}
Expected<std::vector<CancelDisposition>>
Runtime::stage_cancel_local_many(EventTxn &txn, const std::vector<Handle> &handles,
                                 CancelReason reason) {
  if (handles.size() > config_.drain_capacity)
    return fail(ErrorCode::Capacity, "cancel cohort capacity");
  for (auto h : handles)
    if (txn.context().domain != h.domain || txn.context().owner != h.owner)
      return fail(ErrorCode::WrongOwner, "cancel segment context");
  auto update = drains_.prepare(txn);
  if (!update)
    return update.error();
  std::vector<CancelDisposition> outcomes;
  for (auto h : handles) {
    bool retired_owned = false, all_retired = true;
    for (auto &r : update.value()->view().stop_report())
      if (r.transaction == h)
        for (auto &hop : r.hops) {
          const CleanupBinding *binding = nullptr;
          auto live = cleanup_.find(hop.hop);
          if (live != cleanup_.end()) binding = &live->second;
          auto pending = prepared_cleanup_.find(hop.hop);
          if (!binding && pending != prepared_cleanup_.end() && pending->second.first == &txn)
            binding = pending->second.second;
          if (binding && binding->transaction != h)
            return fail(ErrorCode::WrongOwner, "cleanup responsibility transaction mismatch");
          if (binding && binding->admission) {
            auto marked = stage_mark_cleanup_cancelled(txn, hop.hop);
            if (!marked) return marked.error();
          }
          auto engine = protocol(r.instance);
          if (!engine)
            return engine.error();
          auto wire = engine.value()->prepared_inspect(txn, hop.hop);
          if (wire && wire.value().state == WireState::Idle && !wire.value().call_ordinal) {
            auto dropped =
                update.value()->view().drop_unstarted_hop(h, hop.hop, *engine.value(), &txn);
            if (!dropped)
              return dropped.error();
            if (binding && binding->admission) {
              auto retired =
                  binding->admission->stage_retire_unstarted(txn, hop.hop, *engine.value());
              if (!retired) {
                if (retired.error().code != ErrorCode::InvalidState ||
                    retired.error().message != "unstarted hop retains a request gate")
                  return retired.error();
                all_retired = false;
              } else {
                auto untracked = stage_untrack_cleanup(txn, hop.hop);
                if (!untracked)
                  return untracked.error();
                retired_owned = true;
              }
            } else
              all_retired = false;
          } else
            all_retired = false;
        }
    auto cancelled = update.value()->view().cancel_local(h, reason);
    if (!cancelled)
      return cancelled.error();
    outcomes.push_back(cancelled.value());
    bool has_intent = false;
    for (const auto &entry : intents_)
      if (entry.second.txn == h)
        has_intent = true;
    for (const auto &entry : prepared_intents_)
      if (entry.second->txn == h)
        has_intent = true;
    if (retired_owned && all_retired && !has_intent && !cancelled.value().receipt) {
      auto retired = update.value()->view().retire_responsibility(h);
      if (!retired && retired.error().code != ErrorCode::NotReady)
        return retired.error();
    }
  }
  auto staged = txn.stage_participant(std::move(update.value()));
  if (!staged)
    return staged.error();
  return outcomes;
}
Expected<EventToken> Runtime::stage_resume(EventTxn &txn, const SuspensionToken &t,
                                           ReadyKey ready) {
  auto successor = events_.successor(ready.time);
  if (!successor)
    return successor.error();
  if (ready < successor.value())
    ready = successor.value();
  EventDraft event;
  event.key = {ready.time, ready.turn, EventStage::Resume, txn.context().instance, {}, 0};
  event.owner = t.process.owner;
  event.epoch = txn.context().epoch;
  event.value = Value{Value::Array{Value{std::uint64_t{0x4c415452}}, Value{t.process},
                                   Value{t.wait}, Value{t.ordinal}}};
  return txn.stage_event(events_, std::move(event));
}
Expected<void> Runtime::notify_process(const SuspensionToken &t, SingleWaitOutcome outcome) {
  Entry entry(*this);
  auto frame = processes_.inspect(t.process);
  if (!frame)
    return frame.error();
  auto instance = frame.value().instance;
  if (!protocol(instance))
    instance = config_.instance;
  auto ready = events_.successor(outcome.source.time < now_ ? now_ : outcome.source.time);
  if (!ready)
    return ready.error();
  EventDraft event;
  event.key = {ready.value().time, ready.value().turn, EventStage::Resume, instance, {}, 0};
  event.owner = t.process.owner;
  auto epoch = drains_.epoch(instance);
  if (!epoch)
    return epoch.error();
  event.epoch = epoch.value();
  event.value = Value{Value::Array{Value{std::uint64_t{0x4c415452}}, Value{t.process},
                                   Value{t.wait}, Value{t.ordinal}}};
  auto reserved = events_.prepare(std::move(event));
  if (!reserved)
    return reserved.error();
  auto notified = processes_.notify(t, std::move(outcome));
  if (!notified)
    return notified.error();
  auto queued = events_.enqueue(std::move(reserved.value()));
  if (!queued) {
    stop(queued.error());
    return queued.error();
  }
  dirty_ = true;
  return {};
}
Expected<Handle> Runtime::schedule(EventDraft d) {
  Entry e(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (d.key.time < now_)
    return fail(ErrorCode::TimeRegression, "event before host time");
  auto r = events_.enqueue(std::move(d));
  if (r)
    dirty_ = true;
  return r;
}
Expected<std::vector<EventReservation>> Runtime::reserve_return_events() {
  std::vector<EventReservation> slots;
  for (int n = 0; n < 4; ++n) {
    auto ready = events_.successor(now_);
    if (!ready)
      return ready.error();
    EventDraft d;
    d.key.time = ready.value().time;
    d.key.turn = ready.value().turn;
    auto r = events_.prepare(d);
    if (!r)
      return r.error();
    slots.push_back(std::move(r.value()));
  }
  return slots;
}
Expected<void> Runtime::enqueue_exchange(const ExchangeResult &x,
                                         std::vector<EventReservation> &reserved) {
  if (x.milestones.size() > reserved.size())
    return fail(ErrorCode::Capacity, "exchange exceeds reserved milestone budget");
  for (std::size_t n = 0; n < x.milestones.size(); ++n) {
    auto &m = x.milestones[n];
    auto ready = events_.successor(m.time);
    if (!ready)
      return ready.error();
    EventDraft d;
    d.key = {ready.value().time,
             ready.value().turn,
             EventStage::Internal,
             inspect_hop(x.hop).value().identity.local_side,
             {},
             0};
    auto changed = events_.retarget(reserved[n], d);
    if (!changed)
      return changed.error();
    auto token = events_.enqueue(std::move(reserved[n]));
    if (!token)
      return token.error();
    wire_events_.emplace(token.value(), RuntimeMilestone{x.hop, x.call_id, m, ready.value()});
  }
  drains_.observe_wire(x.hop, x.wire_terminal, false);
  auto wire = inspect_hop(x.hop);
  if (!wire)
    return wire.error();
  auto key = std::make_pair(wire.value().identity.local_side, wire.value().identity.connection);
  auto binding = admission_bindings_.find(key);
  if (binding != admission_bindings_.end()) {
    if (reserved.size() <= x.milestones.size())
      return fail(ErrorCode::Capacity, "request lane resolver reservation");
    auto ready = events_.successor(now_);
    if (!ready)
      return ready.error();
    EventDraft draft;
    draft.key = {ready.value().time, ready.value().turn, EventStage::WaitResolve,
                 key.first,          key.second,         0};
    auto &slot = reserved[x.milestones.size()];
    auto moved = events_.retarget(slot, draft);
    if (!moved)
      return moved.error();
    auto token = events_.enqueue(std::move(slot));
    if (!token)
      return token.error();
    admission_events_.emplace(token.value(), key);
  }
  if (x.wire_terminal)
    open_wires_.erase(x.hop);
  else {
    if (!open_wires_.count(x.hop) &&
        open_wires_.size() >= config_.ledger_capacity * (protocols_.size() + 1))
      return fail(ErrorCode::Capacity, "open wire capacity");
    open_wires_[x.hop] = true;
  }
  dirty_ = true;
  return {};
}
Expected<WireReturn> Runtime::ingress(const WireCall &c) {
  Entry entry(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (depth_ > config_.nesting_capacity)
    return fail(ErrorCode::Capacity, "ingress nesting limit");
  if (c.call_time < now_)
    return fail(ErrorCode::TimeRegression, "ingress time regression");
  std::size_t bytes = c.request.data.size() + c.request.byte_enable.size();
  if (bytes > config_.ingress_bytes)
    return fail(ErrorCode::Capacity, "ingress snapshot limit");
  for (auto &ext : c.request.extensions) {
    if (ext.first.size() > config_.ingress_bytes - bytes)
      return fail(ErrorCode::Capacity, "ingress extension name limit");
    bytes += ext.first.size();
    if (ext.second.size() > config_.ingress_bytes - bytes)
      return fail(ErrorCode::Capacity, "ingress extension limit");
    bytes += ext.second.size();
  }
  auto side = call_side(c.connection, c.flow, false);
  if (!side)
    return side.error();
  auto selected = protocol(side.value());
  if (!selected)
    return selected.error();
  auto engine = selected.value();
  auto id = allocated_.find(c.id);
  if (id == allocated_.end())
    return fail(ErrorCode::InvalidArgument, "call identity was not allocated in this domain");
  auto time = add_time(c.call_time, c.incoming_delay);
  if (!time)
    return time.error();
  now_ = c.call_time;
  auto slots = reserve_return_events();
  if (!slots)
    return slots.error();
  IngressPlan plan;
  plan.reply.sync = Sync::Accepted;
  plan.reply.outgoing_delay = c.incoming_delay;
  if (ingress_policy_) {
    auto proposed = ingress_policy_(c);
    if (!proposed)
      return proposed.error();
    plan = std::move(proposed.value());
  }
  if (plan.complete_now &&
      (plan.reply.sync != Sync::Completed || c.phase != begin_req || c.incoming_delay.value != 0 ||
       plan.reply.outgoing_delay.value != 0 || !plan.prepared_service))
    return fail(ErrorCode::InvalidArgument,
                "CompleteNow requires prepared BEGIN_REQ service and COMPLETED");
  if (!plan.complete_now && plan.reply.sync != Sync::Accepted)
    return fail(ErrorCode::Unsupported,
                "nondeferred ingress requires prepared CompleteNow service");
  if (plan.prepared_service) {
    auto valid = plan.prepared_service->validate();
    if (!valid) {
      plan.prepared_service->discard();
      return valid.error();
    }
  }
  std::optional<EventReservation> input;
  if (!plan.complete_now) {
    auto ready = events_.successor(time.value());
    if (!ready)
      return ready.error();
    EventDraft d;
    d.key = {ready.value().time, ready.value().turn, EventStage::Input,
             side.value(),       c.connection,       0};
    auto epoch = drains_.epoch(side.value());
    if (!epoch)
      return epoch.error();
    d.epoch = epoch.value();
    auto r = events_.prepare(d);
    if (!r)
      return r.error();
    input.emplace(std::move(r.value()));
  }
  auto ticket = engine->begin_call(c);
  if (!ticket)
    return ticket.error();
  auto preview = engine->validate_end_call(ticket.value(), plan.reply);
  if (!preview) {
    engine->fail_call(ticket.value(), preview.error());
    stop(preview.error());
    return preview.error();
  }
  if (plan.prepared_service)
    plan.prepared_service->apply();
  auto exchange = engine->end_call(ticket.value(), plan.reply);
  if (!exchange) {
    stop(exchange.error());
    return exchange.error();
  }
  if (input && !exchange.value().ignored) {
    auto token = events_.enqueue(std::move(*input));
    if (!token) {
      stop(token.error());
      return token.error();
    }
    wire_events_.emplace(token.value(), c);
  }
  auto queued = enqueue_exchange(exchange.value(), slots.value());
  if (!queued) {
    stop(queued.error());
    return queued.error();
  }
  if (id->second == CallOrigin::ExternalIngress)
    allocated_.erase(id);
  dirty_ = true;
  return plan.reply;
}
Expected<void> Runtime::start_outbound(const SendIntent &i, const WireCall &c) {
  Entry e(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (!host_calls_ || !intents_.count(i.call_id))
    return fail(ErrorCode::InvalidState, "outbound requires active committed Host call");
  auto id = allocated_.find(i.call_id);
  if (id == allocated_.end() || id->second != CallOrigin::Outgoing)
    return fail(ErrorCode::InvalidArgument, "outgoing identity missing");
  if (outgoing_.count(i.call_id))
    return fail(ErrorCode::Duplicate, "outgoing already started");
  const auto &committed = intents_.at(i.call_id);
  if (i.txn != committed.txn || i.connection != committed.connection ||
      i.transport != committed.transport || i.flow != committed.flow ||
      i.phase != committed.phase || i.not_before != committed.not_before ||
      c.id != committed.call_id || c.connection != committed.connection ||
      c.transport != committed.transport || c.flow != committed.flow ||
      c.phase != committed.phase || c.call_time != now_ || c.call_time < committed.not_before ||
      c.incoming_delay.value != 0 || c.request.command != committed.payload.command ||
      c.request.address != committed.payload.address ||
      !outbound_data_equal(committed, c.request) ||
      c.request.byte_enable != committed.payload.byte_enable ||
      c.request.streaming_width != committed.payload.streaming_width ||
      c.request.status != committed.payload.status ||
      c.request.dmi_hint != committed.payload.dmi_hint ||
      c.request.extensions != committed.payload.extensions)
    return fail(ErrorCode::ProtocolViolation, "actual outbound identity/time mismatch");
  auto reserved = reserve_return_events();
  if (!reserved)
    return reserved.error();
  auto side = call_side(c.connection, c.flow, true);
  if (!side)
    return side.error();
  auto selected = protocol(side.value());
  if (!selected)
    return selected.error();
  auto engine = selected.value();
  auto ticket = engine->begin_call(c);
  if (!ticket)
    return ticket.error();
  auto hook = intent_hooks_.find(c.id);
  if (hook != intent_hooks_.end()) {
    auto sent = hook->second.start();
    if (!sent) {
      engine->fail_call(ticket.value(), sent.error());
      stop(sent.error());
      return sent.error();
    }
    intent_hooks_.erase(hook);
  }
  outgoing_.emplace(c.id, Outgoing{ticket.value(), std::move(reserved.value()), engine});
  return {};
}
Expected<void> Runtime::record_return(CallId id, const WireReturn &r) {
  Entry e(*this);
  if (!started_)
    return fail(ErrorCode::NotStarted, "runtime not started");
  auto i = outgoing_.find(id);
  if (i == outgoing_.end())
    return fail(ErrorCode::InvalidState, "unknown/already recorded outgoing call");
  auto exchange = i->second.engine->end_call(i->second.ticket, r);
  if (!exchange) {
    stop(exchange.error());
    return exchange.error();
  }
  auto queued = enqueue_exchange(exchange.value(), i->second.reserved);
  if (!queued) {
    stop(queued.error());
    return queued.error();
  }
  outgoing_.erase(i);
  allocated_.erase(id);
  try {
    host_.recorded_return(id, exchange.value().wire_terminal);
  } catch (...) {
    auto error = fail(ErrorCode::ExternalFailure, "Host recorded_return threw");
    stop(error);
    return error;
  }
  dirty_ = true;
  return {};
}
Expected<void> Runtime::publish(SendIntent intent) {
  Entry e(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (intents_.size() + prepared_intents_.size() >= config_.intent_capacity)
    return fail(ErrorCode::Capacity, "intent pool full");
  if (!allocated_.count(intent.call_id) || allocated_.at(intent.call_id) != CallOrigin::Outgoing)
    return fail(ErrorCode::InvalidArgument, "unreserved outgoing identity");
  if (intents_.count(intent.call_id) || prepared_intents_.count(intent.call_id))
    return fail(ErrorCode::Duplicate, "intent already published");
  if (intent.not_before < now_)
    return fail(ErrorCode::TimeRegression, "intent in past");
  intents_.emplace(intent.call_id, std::move(intent));
  dirty_ = true;
  return {};
}
Expected<void> Runtime::track_cleanup(Handle txn, Handle hop, bool initiator,
                                      PayloadSnapshot request, AdmissionStore *admission) {
  if (cleanup_.count(hop))
    return fail(ErrorCode::Duplicate, "cleanup binding exists");
  if (cleanup_.size() >= config_.drain_capacity * config_.hops_per_transaction)
    return fail(ErrorCode::Capacity, "cleanup bindings full");
  auto wire = inspect_hop(hop);
  if (!wire)
    return wire.error();
  cleanup_.emplace(hop, CleanupBinding{txn, hop, initiator, std::move(request), false, admission});
  return {};
}
class PreparedCleanup final : public PreparedParticipant {
  Runtime *runtime_;
  std::map<Handle, Runtime::CleanupBinding> node_;
  bool active_{true};

public:
  PreparedCleanup(Runtime &r, Runtime::CleanupBinding b, EventTxn &txn) : runtime_(&r) {
    node_.emplace(b.hop, std::move(b));
    r.prepared_cleanup_.emplace(node_.begin()->first, std::make_pair(&txn, &node_.begin()->second));
  }
  ~PreparedCleanup() override {
    discard();
  }
  std::size_t reserved_bytes() const noexcept override {
    if (node_.empty())
      return sizeof(*this);
    auto &p = node_.begin()->second.request;
    std::size_t bytes =
        sizeof(*this) + sizeof(Runtime::CleanupBinding) + p.data.size() + p.byte_enable.size();
    for (auto &e : p.extensions)
      bytes += e.first.size() + e.second.size();
    return bytes;
  }
  Expected<void> validate() const override {
    if (!active_ || !runtime_->prepared_cleanup_.count(node_.begin()->first) ||
        runtime_->cleanup_.count(node_.begin()->first))
      return fail(ErrorCode::InvalidState, "cleanup preparation lost");
    return {};
  }
  void apply() noexcept override {
    auto id = node_.begin()->first;
    runtime_->cleanup_.merge(node_);
    runtime_->prepared_cleanup_.erase(id);
    active_ = false;
  }
  void discard() noexcept override {
    if (active_) {
      runtime_->prepared_cleanup_.erase(node_.begin()->first);
      active_ = false;
    }
  }
};
Expected<void> Runtime::stage_track_cleanup(EventTxn &txn, Handle transaction, Handle hop,
                                            bool initiator, PayloadSnapshot request,
                                            AdmissionStore *admission) {
  if (transaction.domain != txn.context().domain || transaction.owner != txn.context().owner ||
      hop.domain != transaction.domain || hop.kind != HandleKind::Hop)
    return fail(ErrorCode::WrongOwner, "cleanup context");
  if (cleanup_.count(hop) || prepared_cleanup_.count(hop))
    return fail(ErrorCode::Duplicate, "cleanup binding exists");
  if (cleanup_.size() + prepared_cleanup_.size() >=
      config_.drain_capacity * config_.hops_per_transaction)
    return fail(ErrorCode::Capacity, "cleanup bindings full");
  return txn.stage_participant(std::make_unique<PreparedCleanup>(
      *this, CleanupBinding{transaction, hop, initiator, std::move(request), false, admission},
      txn));
}
class PreparedCleanupRetirement final : public PreparedParticipant {
  Runtime *runtime_;
  EventTxn *txn_;
  Handle hop_;
  bool erase_;

public:
  PreparedCleanupRetirement(Runtime &r, EventTxn &txn, Handle hop, bool erase = true)
      : runtime_(&r), txn_(&txn), hop_(hop), erase_(erase) {}
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this);
  }
  Expected<void> validate() const override {
    if (runtime_->cleanup_.count(hop_))
      return {};
    auto found = runtime_->prepared_cleanup_.find(hop_);
    if (found != runtime_->prepared_cleanup_.end() && found->second.first == txn_)
      return {};
    return fail(ErrorCode::StaleHandle, "cleanup retirement binding");
  }
  void apply() noexcept override {
    if (erase_) runtime_->cleanup_.erase(hop_);
    else {
      auto found = runtime_->cleanup_.find(hop_);
      if (found != runtime_->cleanup_.end()) found->second.cancelled = true;
    }
  }
  void discard() noexcept override {}
};
Expected<void> Runtime::stage_mark_cleanup_cancelled(EventTxn &txn, Handle hop) {
  return txn.stage_participant(std::make_unique<PreparedCleanupRetirement>(*this, txn, hop, false));
}
Expected<void> Runtime::stage_untrack_cleanup(EventTxn &txn, Handle hop) {
  const CleanupBinding *binding = nullptr;
  auto live = cleanup_.find(hop);
  if (live != cleanup_.end())
    binding = &live->second;
  auto pending = prepared_cleanup_.find(hop);
  if (!binding && pending != prepared_cleanup_.end() && pending->second.first == &txn)
    binding = pending->second.second;
  if (!binding || binding->transaction.owner != txn.context().owner ||
      binding->transaction.domain != txn.context().domain)
    return fail(ErrorCode::WrongOwner, "cleanup retirement context");
  auto engine = protocol(txn.context().instance);
  if (!engine)
    return engine.error();
  auto wire = engine.value()->prepared_inspect(txn, hop);
  if (!wire)
    return wire.error();
  if (wire.value().state != WireState::Idle || wire.value().call_ordinal || wire.value().pending)
    return fail(ErrorCode::NotReady, "cleanup retirement reached wire");
  return txn.stage_participant(std::make_unique<PreparedCleanupRetirement>(*this, txn, hop));
}
Expected<void> Runtime::untrack_cleanup(Handle hop) {
  auto binding = cleanup_.find(hop);
  if (binding == cleanup_.end())
    return fail(ErrorCode::StaleHandle, "cleanup binding");
  auto wire = inspect_hop(hop);
  if (wire && (wire.value().state != WireState::Idle || wire.value().call_ordinal))
    return fail(ErrorCode::NotReady, "cleanup reached wire");
  if (open_wires_.count(hop))
    return fail(ErrorCode::NotReady, "open wire retains cleanup");
  for (auto &r : drains_.stop_report())
    for (auto &h : r.hops)
      if (h.hop == hop)
        return fail(ErrorCode::NotReady, "drain retains cleanup");
  cleanup_.erase(binding);
  return {};
}
Expected<RuntimeMilestone> Runtime::milestone(Handle hop, MilestoneKind kind) const {
  auto i = latches_.find({hop, kind});
  if (i == latches_.end())
    return fail(ErrorCode::NotReady, "milestone not effective");
  return i->second;
}
Expected<SingleWaitOutcome> Runtime::response_wait_outcome(Handle hop) const {
  auto observed = milestone(hop, MilestoneKind::ResponseReady);
  if (!observed)
    return observed.error();
  if (!observed.value().milestone.response)
    return fail(ErrorCode::Integrity, "response milestone has no owning snapshot");
  const auto &response = *observed.value().milestone.response;
  return SingleWaitOutcome{SingleWaitStatus::Ready,
                           Value{Value::Array{Value{static_cast<std::uint64_t>(response.status)},
                                              Value{response.data}, Value{response.dmi_hint}}},
                           observed.value().ready, 0};
}
Expected<ProtocolEngine *> Runtime::retirement_engine(Handle hop) {
  auto terminal = milestone(hop, MilestoneKind::Terminal);
  if (!terminal)
    return terminal.error();
  for (auto &e : wire_events_) {
    if (auto m = std::get_if<RuntimeMilestone>(&e.second)) {
      if (m->hop == hop)
        return fail(ErrorCode::NotReady, "hop has queued timing");
    } else {
      auto &c = std::get<WireCall>(e.second);
      auto wire = inspect_hop(hop);
      if (wire && c.transport == wire.value().identity.transport &&
          c.connection == wire.value().identity.connection)
        return fail(ErrorCode::NotReady, "hop has queued input");
    }
  }
  for (auto &r : drains_.stop_report())
    for (auto &h : r.hops)
      if (h.hop == hop)
        return fail(ErrorCode::NotReady, "hop has drain responsibility");
  auto engine = engine_for_hop(hop);
  if (!engine)
    return engine.error();
  return engine.value();
}
Expected<void> Runtime::retire_hop(Handle hop) {
  auto engine = retirement_engine(hop);
  if (!engine) return engine.error();
  auto retired = engine.value()->retire_ledger(hop);
  if (!retired)
    return retired.error();
  cleanup_.erase(hop);
  for (auto i = latches_.begin(); i != latches_.end();)
    if (i->first.first == hop)
      i = latches_.erase(i);
    else
      ++i;
  return {};
}
Expected<void> Runtime::progress_cleanup(Handle hop) {
  auto i = cleanup_.find(hop);
  if (i == cleanup_.end() || !drains_.is_cancelled(i->second.transaction))
    return {};
  auto &b = i->second;
  auto state = inspect_hop(hop);
  if (!state)
    return state.error();
  if (state.value().state == WireState::Terminal) {
    b.scheduled = false;
    return {};
  }
  if (b.scheduled)
    return {};
  PhaseId phase;
  Flow flow;
  if (b.initiator) {
    if (state.value().state != WireState::Response)
      return {};
    phase = end_resp;
    flow = Flow::Forward;
  } else {
    if (state.value().state == WireState::Request) {
      phase = end_req;
      flow = Flow::Backward;
    } else if (state.value().state == WireState::RequestReleased) {
      phase = begin_resp;
      flow = Flow::Backward;
    } else
      return {};
  }
  auto id = allocate_call_id(CallOrigin::Outgoing);
  if (!id)
    return id.error();
  SendIntent intent;
  intent.connection = state.value().identity.connection;
  intent.txn = b.transaction;
  intent.flow = flow;
  intent.phase = phase;
  intent.not_before = now_ < state.value().last_timing ? state.value().last_timing : now_;
  intent.call_id = id.value();
  intent.transport = state.value().identity.transport;
  intent.payload = b.request;
  if (phase == begin_resp)
    intent.payload.status = ResponseStatus::GenericError;
  auto p = publish(std::move(intent));
  if (!p) {
    allocated_.erase(id.value());
    return p.error();
  }
  cleanup_intents_[id.value()] = true;
  b.scheduled = true;
  return {};
}
Expected<void> Runtime::boundary() {
  auto cancels = std::move(pending_cancel_);
  pending_cancel_.clear();
  for (auto &p : cancels) {
    auto r = cancel_local(p.first, p.second);
    if (!r)
      return r.error();
  }
  auto resets = std::move(pending_reset_);
  pending_reset_.clear();
  for (auto &p : resets) {
    auto r = reset(p.first, p.second);
    if (!r)
      return r.error();
  }
  return {};
}
Expected<void> Runtime::dispatch_intents() {
  for (auto i = intents_.begin(); i != intents_.end();)
    if (drains_.is_cancelled(i->second.txn) && !cleanup_intents_.count(i->first) &&
        !outgoing_.count(i->first)) {
      auto hook = intent_hooks_.find(i->first);
      if (hook != intent_hooks_.end()) {
        auto cancelled = hook->second.cancel(ReadyKey{now_, 0});
        if (!cancelled) {
          stop(cancelled.error());
          return cancelled.error();
        }
        intent_hooks_.erase(hook);
      }
      allocated_.erase(i->first);
      i = intents_.erase(i);
    } else
      ++i;
  for (auto binding = cleanup_.begin(); binding != cleanup_.end();) {
    if (binding->second.admission && binding->second.cancelled) {
      auto engine = engine_for_hop(binding->first);
      if (!engine) return engine.error();
      auto wire = engine.value()->inspect(binding->first);
      if (wire && wire.value().state == WireState::Terminal) {
        auto ready = retirement_engine(binding->first);
        if (ready) {
          auto record = binding->second.admission->lookup(binding->second.transaction, wire.value().identity.connection);
          if (!record) return record.error();
          if (!record.value().semantic_terminal) {
            auto synced = binding->second.admission->sync_terminal(binding->first, *ready.value(), wire.value().last_timing);
            if (!synced) return synced.error();
          }
          auto retired = binding->second.admission->retire(binding->first);
          if (retired) {
            auto hop = binding->first;
            ++binding;
            auto removed = retire_hop(hop);
            if (!removed) return removed.error();
            continue;
          }
          if (retired.error().code != ErrorCode::InvalidState) return retired.error();
        } else if (ready.error().code != ErrorCode::NotReady) return ready.error();
      }
    }
    if (binding->second.admission && drains_.is_cancelled(binding->second.transaction) &&
        !has_unstarted_intent(binding->second.transaction)) {
      auto engine = engine_for_hop(binding->first);
      if (!engine)
        return engine.error();
      auto wire = engine.value()->inspect(binding->first);
      if (wire && wire.value().state == WireState::Idle && !wire.value().call_ordinal &&
          !wire.value().pending) {
        auto retired = binding->second.admission->retire_unstarted(binding->first, *engine.value());
        if (retired) {
          auto transaction = binding->second.transaction;
          binding = cleanup_.erase(binding);
          auto responsibility = drains_.retire_responsibility(transaction);
          if (!responsibility && responsibility.error().code != ErrorCode::NotReady)
            return responsibility.error();
          continue;
        }
        if (retired.error().code != ErrorCode::InvalidState)
          return retired.error();
      }
    }
    auto p = progress_cleanup(binding->first);
    if (!p)
      return p.error();
    ++binding;
  }
  std::vector<CallId> due;
  for (auto &i : intents_)
    if (!(now_ < i.second.not_before))
      due.push_back(i.first);
  for (auto id : due) {
    auto i = intents_.find(id);
    if (i == intents_.end())
      continue;
    auto intent = i->second;
    ++host_calls_;
    Expected<WireReturn> ret = fail(ErrorCode::ExternalFailure, "Host transport threw");
    try {
      ret = host_.transport(intent);
    } catch (...) {
    }
    --host_calls_;
    if (!ret) {
      if (outgoing_.count(id))
        outgoing_.at(id).engine->fail_call(outgoing_.at(id).ticket, ret.error());
      stop(ret.error());
      return ret.error();
    }
    if (!outgoing_.count(id)) {
      auto err = fail(ErrorCode::ProtocolViolation, "Host returned without one start_outbound");
      stop(err);
      return err;
    }
    auto recorded = record_return(id, ret.value());
    if (!recorded)
      return recorded.error();
    intents_.erase(id);
    cleanup_intents_.erase(id);
    for (auto &binding : cleanup_)
      if (binding.second.transaction == intent.txn)
        binding.second.scheduled = false;
    auto b = boundary();
    if (!b) {
      stop(b.error());
      return b.error();
    }
  }
  return {};
}
Expected<PumpResult> Runtime::pump_batch(Tick now, std::uint32_t budget) {
  Entry e(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (pumping_)
    return fail(ErrorCode::InvalidState, "pump reentry");
  if (!budget)
    return fail(ErrorCode::InvalidArgument, "zero dispatch budget");
  if (now < now_)
    return fail(ErrorCode::TimeRegression, "host time regression");
  now_ = now;
  pumping_ = true;
  struct Guard {
    bool &b;
    ~Guard() {
      b = false;
    }
  } guard{pumping_};
  dirty_ = true;
  auto sent = dispatch_intents();
  if (!sent)
    return sent.error();
  auto batch = events_.pop_batch(now, budget);
  if (!batch)
    return batch.error();
  PumpResult result;
  if (batch.value()) {
    auto &s = *batch.value();
    result.closed_batch_id = s.id;
    for (auto &m : s.members) {
      if (event_count_ >= config_.max_events) {
        stop(fail(ErrorCode::FuelExhausted, "cumulative event fuel exhausted"));
        break;
      }
      if (count_tick_ != s.ready.time) {
        count_tick_ = s.ready.time;
        tick_events_ = 0;
      }
      if (tick_events_ >= config_.max_events_per_tick) {
        stop(fail(ErrorCode::ZenoDetected, "events per tick exhausted"));
        break;
      }
      ++event_count_;
      ++tick_events_;
      auto live = events_.is_active(m.token);
      if (!live) {
        stop(live.error());
        break;
      }
      m.cancelled = !live.value();
      Expected<void> execution;
      auto typed = wire_events_.find(m.token);
      bool cleanup_event = (typed != wire_events_.end() &&
                            std::holds_alternative<RuntimeMilestone>(typed->second)) ||
                           blocking_events_.count(m.token) || admission_events_.count(m.token) ||
                           m.event.key.stage == EventStage::Reset ||
                           m.event.key.stage == EventStage::Cancel;
      auto epoch =
          drains_.epoch(m.event.key.instance.value ? m.event.key.instance : config_.instance);
      if (!cleanup_event && epoch && m.event.epoch != epoch.value())
        m.cancelled = true;
      if (!m.cancelled) {
        if (admission_events_.count(m.token)) {
          auto key = admission_events_.at(m.token);
          admission_events_.erase(m.token);
          auto binding = admission_bindings_.find(key);
          auto engine = protocol(key.first);
          if (binding == admission_bindings_.end() || !engine)
            execution = fail(ErrorCode::Integrity, "admission resolver binding");
          else
            execution = binding->second->update_request_lane(
                key.second, engine.value()->request_lane_free(key.second),
                ReadyKey{m.event.key.time, m.event.key.turn});
        } else if (blocking_events_.count(m.token))
          execution = dispatch_blocking(m.token, ReadyKey{m.event.key.time, m.event.key.turn});
        else if (typed != wire_events_.end()) {
          ReadyKey ready{m.event.key.time, m.event.key.turn};
          if (auto call = std::get_if<WireCall>(&typed->second)) {
            if (input_handler_)
              execution = input_handler_(*call, ready, *this);
          } else {
            auto milestone = std::get<RuntimeMilestone>(typed->second);
            if (!latches_.count({milestone.hop, milestone.milestone.kind}) &&
                latches_.size() >= config_.ledger_capacity * (protocols_.size() + 1) * 3) {
              stop(fail(ErrorCode::Capacity, "milestone latch capacity"));
              break;
            }
            latches_[{milestone.hop, milestone.milestone.kind}] = milestone;
            if (milestone.milestone.kind == MilestoneKind::ResponseReady) {
              auto outcome = response_wait_outcome(milestone.hop);
              if (!outcome)
                execution = outcome.error();
              else
                for (const auto &token :
                     processes_.waiting_on(SingleWaitKind::Response, milestone.hop)) {
                  execution = notify_process(token, outcome.value());
                  if (!execution)
                    break;
                }
            }
            auto observed_wire = inspect_hop(milestone.hop);
            if (!observed_wire)
              execution = observed_wire.error();
            else {
              try {
                host_.observe_milestone(RuntimeMilestoneObservation{
                    m.event.key, m.token, observed_wire.value().identity, milestone});
              } catch (...) {
                execution = fail(ErrorCode::ExternalFailure, "Host milestone observer threw");
              }
            }
            if (execution && milestone.milestone.kind == MilestoneKind::Terminal)
              execution = drains_.consume_terminal(milestone.hop);
            if (execution)
              execution = progress_cleanup(milestone.hop);
            if (execution && milestone_handler_)
              execution = milestone_handler_(milestone, ready, *this);
          }
        } else {
          auto values = std::get_if<Value::Array>(&m.event.value.data);
          bool resume_event = m.event.key.stage == EventStage::Resume && values &&
                              values->size() == 4 &&
                              std::holds_alternative<std::uint64_t>((*values)[0].data) &&
                              std::get<std::uint64_t>((*values)[0].data) == 0x4c415452;
          bool output_event = m.event.key.stage == EventStage::Output && values &&
                              values->size() == 3 &&
                              std::holds_alternative<std::uint64_t>((*values)[0].data) &&
                              std::get<std::uint64_t>((*values)[0].data) == 0x4c41544f;
          if (output_event) {
            auto port = std::get_if<std::uint64_t>(&(*values)[1].data);
            if (!port || *port > UINT32_MAX)
              execution = fail(ErrorCode::TypeMismatch, "output port");
            else
              try {
                host_.publish_output(m.event.key.instance,
                                     PortId{static_cast<std::uint32_t>(*port)}, (*values)[2]);
              } catch (...) {
                execution = fail(ErrorCode::ExternalFailure, "Host output threw");
              }
          } else if (resume_event) {
            auto process = std::get_if<Handle>(&(*values)[1].data);
            auto wait = std::get_if<Handle>(&(*values)[2].data);
            auto ordinal = std::get_if<std::uint64_t>(&(*values)[3].data);
            if (!process || !wait || !ordinal || process->owner != m.event.owner)
              execution = fail(ErrorCode::Integrity, "malformed process resume event");
            else {
              SuspensionToken token{*process, *wait, *ordinal};
              auto frame = processes_.inspect(*process);
              if (!frame || !frame.value().suspension || !(*frame.value().suspension == token))
                m.cancelled = true;
              else {
                ReadyKey ready{m.event.key.time, m.event.key.turn};
                if (frame.value().state == ProcessState::Suspended) {
                  auto notify = processes_.notify(
                      token, SingleWaitOutcome{SingleWaitStatus::Ready, {}, ready});
                  if (!notify)
                    execution = notify.error();
                }
                if (execution) {
                  if (resume_handler_)
                    execution = resume_handler_(token, ready, *this);
                  else
                    execution = fail(ErrorCode::Unsupported, "process resume handler absent");
                }
              }
            }
          } else if (handler_)
            execution = handler_(m, *this);
        }
      }
      if (typed != wire_events_.end())
        wire_events_.erase(typed);
      auto ack = events_.ack_executed(s.id, m.token,
                                      m.cancelled ? ExecutionDisposition::Cancelled
                                      : execution ? ExecutionDisposition::Committed
                                                  : ExecutionDisposition::Failed);
      ++result.executed_events;
      if (!ack) {
        stop(ack.error());
        break;
      }
      if (!execution) {
        stop(execution.error());
        break;
      }
    }
    if (!stopped_ && s.members_complete && batch_epilogue_ && epilogue_done_ != s.id) {
      auto epilogue = batch_epilogue_(s.id, s.ready, *this);
      if (!epilogue)
        stop(epilogue.error());
      else
        epilogue_done_ = s.id;
    }
    if (!stopped_ && s.members_complete) {
      auto resolved = events_.resolver_step(s.id, budget, 0);
      if (!resolved) {
        stop(resolved.error());
      } else if (resolved.value()) {
        auto f = events_.finish_batch(s.id);
        if (!f)
          stop(f.error());
        else
          result.batch_complete = true;
      }
    }
    result.stop_reason =
        result.batch_complete ? PumpStopReason::BatchComplete : PumpStopReason::LimitReached;
  } else
    result.stop_reason = (drains_.outstanding() || !blocking_.empty() || !open_wires_.empty() ||
                          !outgoing_.empty() || !intents_.empty())
                             ? PumpStopReason::WaitingForEnvironment
                             : PumpStopReason::Quiescent;
  if (!stopped_) {
    auto reset_progress = drains_.advance_reset(config_.instance);
    for (auto &side : protocols_) {
      auto progress = drains_.advance_reset(side.first);
      if (!progress && progress.error().code != ErrorCode::InvalidState)
        stop(progress.error());
    }
    if (!reset_progress && reset_progress.error().code != ErrorCode::InvalidState)
      stop(reset_progress.error());
    if (!stopped_) {
      auto promoted = promote_blocking();
      if (!promoted)
        stop(promoted.error());
    }
  }
  if (!stopped_) {
    auto post = dispatch_intents();
    if (!post)
      return post.error();
  }
  if (stopped_) {
    result.progress = ProgressDisposition::Stopped;
    result.stop_reason = PumpStopReason::LimitReached;
    result.limit_detail = stop_detail_;
  }
  result.next_wake = next_wakeup();
  return result;
}
Expected<CancelDisposition> Runtime::cancel_local(Handle h, CancelReason reason) {
  Entry e(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (host_calls_) {
    if (pending_cancel_.size() + pending_reset_.size() >= config_.drain_capacity)
      return fail(ErrorCode::Capacity, "boundary work full");
    pending_cancel_.emplace_back(h, reason);
    return CancelDisposition{false, {}};
  }
  for (auto &responsibility : drains_.stop_report())
    if (responsibility.transaction == h)
      for (auto &hop : responsibility.hops) {
        auto wire = inspect_hop(hop.hop);
        if (wire && wire.value().state == WireState::Idle && !wire.value().call_ordinal) {
          auto drop = drains_.drop_unstarted_hop(h, hop.hop, *engine_for_hop(hop.hop).value());
          if (!drop)
            return drop.error();
        }
      }
  auto r = drains_.cancel_local(h, reason);
  if (!r)
    return r.error();
  for (auto i = intents_.begin(); i != intents_.end();)
    if (i->second.txn == h && !outgoing_.count(i->first)) {
      auto hook = intent_hooks_.find(i->first);
      if (hook != intent_hooks_.end()) {
        auto cancelled = hook->second.cancel(ReadyKey{now_, 0});
        if (!cancelled) {
          stop(cancelled.error());
          return cancelled.error();
        }
        intent_hooks_.erase(hook);
      }
      allocated_.erase(i->first);
      i = intents_.erase(i);
    } else
      ++i;
  for (auto &binding : cleanup_)
    if (binding.second.transaction == h) {
      auto c = progress_cleanup(binding.first);
      if (!c) {
        stop(c.error());
        return c.error();
      }
    }
  dirty_ = true;
  return r;
}
Expected<ResetDisposition> Runtime::reset(InstanceId id, ResetPolicy p) {
  Entry e(*this);
  auto ok = operational();
  if (!ok)
    return ok.error();
  if (host_calls_) {
    if (pending_cancel_.size() + pending_reset_.size() >= config_.drain_capacity)
      return fail(ErrorCode::Capacity, "boundary work full");
    auto epoch = drains_.epoch(id);
    if (!epoch)
      return epoch.error();
    if (epoch.value() == UINT64_MAX)
      return fail(ErrorCode::Overflow, "reset epoch");
    pending_reset_.emplace_back(id, p);
    return ResetDisposition{0, p, epoch.value(), epoch.value() + 1, ResetState::Draining};
  }
  auto r = drains_.reset(id, p);
  if (r) {
    if (p == ResetPolicy::AbortLocalAndDrain) {
      for (auto i = intents_.begin(); i != intents_.end();)
        if (drains_.is_cancelled(i->second.txn) && !outgoing_.count(i->first)) {
          auto hook = intent_hooks_.find(i->first);
          if (hook != intent_hooks_.end()) {
            auto cancelled = hook->second.cancel(ReadyKey{now_, 0});
            if (!cancelled) {
              stop(cancelled.error());
              return cancelled.error();
            }
            intent_hooks_.erase(hook);
          }
          allocated_.erase(i->first);
          i = intents_.erase(i);
        } else
          ++i;
      for (auto &b : cleanup_) {
        auto c = progress_cleanup(b.first);
        if (!c) {
          stop(c.error());
          return c.error();
        }
      }
    }
    dirty_ = true;
  }
  return r;
}
} // namespace leanat
