#include "leanat/payload_helpers.hpp"
namespace leanat {
Expected<void> PayloadBuilder::register_extension(OptionalExtensionSchema s) {
  if (s.key.empty() || s.schema.empty() || !s.ignorable)
    return fail(ErrorCode::InvalidArgument,
                "optional extension must have ignorable registered schema");
  auto i = schemas_.find(s.key);
  if (i != schemas_.end()) {
    auto &x = i->second;
    if (x.schema != s.schema || x.version != s.version || x.type != s.type)
      return fail(ErrorCode::Schema, "extension schema conflict");
    return {};
  }
  if (schemas_.size() >= limits_.extension_count || s.key.size() > limits_.extension_key_bytes)
    return fail(ErrorCode::Capacity, "optional extension registry bounds");
  schemas_.emplace(s.key, std::move(s));
  return {};
}
Expected<PayloadSnapshot> PayloadBuilder::read(std::uint64_t a, std::size_t n) const {
  if (n > limits_.data_bytes || n > UINT32_MAX)
    return fail(ErrorCode::Capacity, "payload data limit");
  PayloadSnapshot p;
  p.command = Command::Read;
  p.address = a;
  p.data.resize(n);
  p.streaming_width = std::max<std::uint64_t>(1, n);
  return p;
}
Expected<PayloadSnapshot> PayloadBuilder::write(std::uint64_t a, Bytes b) const {
  if (b.size() > limits_.data_bytes || b.size() > UINT32_MAX)
    return fail(ErrorCode::Capacity, "payload data limit");
  PayloadSnapshot p;
  p.command = Command::Write;
  p.address = a;
  p.streaming_width = std::max<std::uint64_t>(1, b.size());
  p.data = std::move(b);
  return p;
}
PayloadSnapshot PayloadBuilder::ignore(std::uint64_t a) const {
  PayloadSnapshot p;
  p.address = a;
  p.streaming_width = 0;
  return p;
}
Expected<PayloadSnapshot> PayloadBuilder::with_streaming_width(PayloadSnapshot p,
                                                               std::uint64_t w) const {
  if ((p.command != Command::Ignore && !w) || w > UINT32_MAX)
    return fail(ErrorCode::InvalidArgument, "invalid streaming width");
  p.streaming_width = w;
  return p;
}
Expected<PayloadSnapshot> PayloadBuilder::with_byte_enable(PayloadSnapshot p, Bytes b) const {
  if (b.size() > limits_.mask_bytes || b.size() > UINT32_MAX)
    return fail(ErrorCode::Capacity, "byte enable limit");
  p.byte_enable = std::move(b);
  return p;
}
Expected<PayloadSnapshot> PayloadBuilder::with_optional_extension(PayloadSnapshot p,
                                                                  const OptionalExtensionSchema &s,
                                                                  Bytes b) const {
  auto i = schemas_.find(s.key);
  if (i == schemas_.end())
    return fail(ErrorCode::Schema, "unregistered extension");
  auto &x = i->second;
  if (x.schema != s.schema || x.version != s.version || x.type != s.type || !s.ignorable)
    return fail(ErrorCode::Schema, "extension schema conflict");
  std::size_t total = b.size();
  if (total > limits_.extension_bytes)
    return fail(ErrorCode::Capacity, "extension limit");
  for (auto &e : p.extensions)
    if (e.first != s.key) {
      if (e.second.size() > limits_.extension_bytes - total)
        return fail(ErrorCode::Capacity, "extension limit");
      total += e.second.size();
    }
  if (!p.extensions.count(s.key) && p.extensions.size() >= limits_.extension_count)
    return fail(ErrorCode::Capacity, "optional extension count");
  p.extensions[s.key] = std::move(b);
  return p;
}
Expected<TransactionPlan> plan_transact(const ExecutionContext &c, ConnectionId endpoint,
                                        PayloadSnapshot p, const PayloadLimits &l, bool initiator,
                                        bool base) {
  if (c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "transact requires process context");
  if (!initiator)
    return fail(ErrorCode::InvalidArgument, "endpoint is not initiator");
  if (!base)
    return fail(ErrorCode::Unsupported, "helper requires base protocol");
  if (p.data.size() > l.data_bytes || p.byte_enable.size() > l.mask_bytes)
    return fail(ErrorCode::Capacity, "endpoint payload capacity");
  if (p.command != Command::Ignore && !p.streaming_width)
    return fail(ErrorCode::InvalidArgument, "zero streaming width");
  if (p.extensions.size() > l.extension_count)
    return fail(ErrorCode::Capacity, "endpoint extension count");
  std::size_t n = 0;
  for (auto &e : p.extensions) {
    if (e.first.size() > l.extension_key_bytes || e.second.size() > l.extension_bytes - n)
      return fail(ErrorCode::Capacity, "endpoint extension capacity");
    n += e.second.size();
  }
  return TransactionPlan{endpoint,
                         std::move(p),
                         {TransactionStep::CreateTransaction, TransactionStep::AwaitRequestGate,
                          TransactionStep::StageBeginRequest, TransactionStep::AwaitResponse,
                          TransactionStep::AckIfNeeded, TransactionStep::AwaitTerminal,
                          TransactionStep::CopyOwnedResult, TransactionStep::ReleaseOwnConsumer}};
}
} // namespace leanat
namespace leanat {
Expected<bool> ResponseAcknowledger::stage_ack(EventTxn &tx, Tick when, CallId call,
                                               PayloadSnapshot payload, Runtime *runtime,
                                               std::optional<Handle> transaction) {
  auto &ctx = tx.context();
  if (runtime && !transaction)
    return fail(ErrorCode::InvalidArgument,
                "runtime acknowledgement requires logical transaction identity");
  if (ctx.kind != ContextKind::Timed && ctx.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "ack requires timed or process context");
  if (hop_.owner != ctx.owner || hop_.domain != ctx.domain)
    return fail(ErrorCode::WrongOwner, "ack observation owner");
  auto state = protocol_.inspect(hop_);
  if (!state)
    return state.error();
  if (when < ctx.ready.time || when < state.value().last_timing)
    return fail(ErrorCode::TimeRegression, "ack precedes response");
  if (state.value().state == WireState::Terminal)
    return false;
  if (ctx.epoch != epoch_)
    return fail(ErrorCode::StaleHandle, "ack epoch");
  auto cell = protocol_.ack_cell(hop_, epoch_);
  if (!cell)
    return cell.error();
  auto a = tx.read(*cell.value());
  if (!a)
    return a.error();
  if (std::get<bool>(a.value().data))
    return false;
  if (state.value().state != WireState::Response || state.value().pending || state.value().faulted)
    return fail(ErrorCode::NotReady, "response not ready to acknowledge");
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  SendIntent intent{state.value().identity.connection,
                    transaction.value_or(hop_),
                    Flow::Forward,
                    end_resp,
                    when,
                    call,
                    state.value().identity.transport,
                    std::move(payload)};
  auto r =
      runtime ? runtime->stage_publish(tx, std::move(intent)) : tx.stage_action(std::move(intent));
  if (r)
    r = tx.buffer(*cell.value(), Value{true});
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return true;
}
} // namespace leanat

namespace leanat {
Value encode_txn_result(const TxnResult &r) {
  Value::Array extensions;
  for (auto &e : r.extensions)
    extensions.push_back(
        Value{Value::Array{Value{Bytes(e.first.begin(), e.first.end())}, Value{e.second}}});
  return Value{Value::Array{
      Value{static_cast<std::uint64_t>(r.status)}, Value{r.data}, Value{std::move(extensions)},
      r.terminal_effective_time ? Value{r.terminal_effective_time->value} : Value{},
      r.local_cancel ? Value{static_cast<std::uint64_t>(*r.local_cancel)} : Value{},
      r.drain ? Value{*r.drain} : Value{}}};
}
TransactOperation::TransactOperation(Runtime &r, AdmissionStore &a, PayloadLimits l,
                                     std::size_t bytes)
    : runtime_(r), admission_(a), limits_(l), result_bytes_(bytes) {}
Expected<void> TransactOperation::validate_context(const ExecutionContext &c,
                                                   bool cancellation) const {
  if (c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "transaction helper requires process context");
  if (c.domain != context_.domain || c.owner != context_.owner || c.instance != context_.instance)
    return fail(ErrorCode::WrongOwner, "transaction helper owner");
  if (!cancellation && c.epoch != context_.epoch)
    return fail(ErrorCode::StaleHandle, "transaction helper epoch");
  if (runtime_.now() < c.ready.time)
    return fail(ErrorCode::TimeRegression, "helper cannot execute ahead of Runtime");
  return {};
}
Expected<void> TransactOperation::start(const ExecutionContext &c, ConnectionId connection,
                                        TransportId transport, PayloadSnapshot payload,
                                        bool external) {
  if (state_ != TransactState::Fresh)
    return fail(ErrorCode::Duplicate, "transaction helper already started");
  auto plan = plan_transact(c, connection, payload, limits_);
  if (!plan)
    return plan.error();
  auto side = runtime_.call_side(connection, Flow::Forward, true);
  if (!side)
    return side.error();
  if (side.value() != c.instance)
    return fail(ErrorCode::WrongOwner, "transaction endpoint instance");
  auto entry_bytes = checked_add(3 * sizeof(Value), limits_.extension_key_bytes);
  if (!entry_bytes)
    return entry_bytes.error();
  auto entries = checked_mul(limits_.extension_count, entry_bytes.value());
  if (!entries)
    return entries.error();
  auto fixed = checked_add(7 * sizeof(Value), payload.data.size());
  if (!fixed)
    return fixed.error();
  auto extension_bound = checked_add(entries.value(), limits_.extension_bytes);
  if (!extension_bound)
    return extension_bound.error();
  auto maximum = checked_add(fixed.value(), extension_bound.value());
  if (!maximum)
    return maximum.error();
  if (maximum.value() > result_bytes_ || limits_.extension_count > (value_max_nodes - 7) / 3)
    return fail(ErrorCode::Capacity, "result capacity cannot hold the permitted response");
  auto epoch = runtime_.drains().epoch(c.instance);
  if (!epoch)
    return epoch.error();
  if (epoch.value() != c.epoch)
    return fail(ErrorCode::StaleHandle, "transaction epoch");
  if (runtime_.now() < c.ready.time)
    return fail(ErrorCode::TimeRegression, "helper ahead of Runtime");
  auto update = admission_.prepare();
  if (!update)
    return update.error();
  AdmissionRequest request{connection, transport, 1, c.owner, c.ready.time, {}, payload};
  auto admitted = update.value()->draft().create_initiator(request);
  if (!admitted)
    return admitted.error();
  auto bound = runtime_.protocol().bind_ledger(admitted.value().hop, connection, transport, 1);
  if (!bound)
    return bound.error();
  auto cleanup_ledger = [&]() {
    runtime_.protocol().discard_unstarted_ledger(admitted.value().hop);
  };
  auto reserved = runtime_.results().reserve(
      ResultCreate{admitted.value().txn, TypeId{}, result_bytes_, c.owner, c.owner});
  if (!reserved) {
    cleanup_ledger();
    return reserved.error();
  }
  auto release_result = [&]() {
    runtime_.results().release(reserved.value().consumer);
    runtime_.results().release_owner(reserved.value().reservation);
  };
  auto drains = runtime_.drains().prepare();
  if (!drains) {
    release_result();
    cleanup_ledger();
    return drains.error();
  }
  auto registered = drains.value()->view().register_responsibility(
      Responsibility{admitted.value().txn,
                     c.instance,
                     c.epoch,
                     {},
                     {{admitted.value().hop, false, false, false, false}},
                     false,
                     false});
  if (!registered) {
    release_result();
    cleanup_ledger();
    return registered.error();
  }
  std::optional<RequestGateTicket> gate;
  if (!external) {
    auto lane = update.value()->draft().update_request_lane(
        connection, runtime_.protocol().request_lane_free(connection), c.ready);
    if (!lane) {
      release_result();
      cleanup_ledger();
      return lane.error();
    }
    auto ticket =
        update.value()->draft().request_request_gate(admitted.value().txn, connection, c.ready);
    if (!ticket) {
      release_result();
      cleanup_ledger();
      return ticket.error();
    }
    gate = ticket.value();
  }
  EventTxn tx({}, c);
  auto staged =
      runtime_.stage_track_cleanup(tx, admitted.value().txn, admitted.value().hop, true, payload);
  if (staged)
    staged = tx.stage_participant(std::move(update.value()));
  if (staged)
    staged = tx.stage_participant(std::move(drains.value()));
  if (!staged) {
    tx.discard();
    release_result();
    cleanup_ledger();
    return staged.error();
  }
  auto committed = runtime_.commit_segment(tx);
  if (!committed) {
    tx.discard();
    release_result();
    cleanup_ledger();
    return committed.error();
  }
  context_ = c;
  connection_ = connection;
  transport_ = transport;
  transaction_ = admitted.value().txn;
  hop_ = admitted.value().hop;
  request_ = std::move(payload);
  storage_ = reserved.value();
  gate_ = gate;
  external_gate_ = external;
  state_ = TransactState::GateWaiting;
  return {};
}
Expected<void> TransactOperation::queue_request(const ExecutionContext &c,
                                                std::optional<RequestPermit> external) {
  auto valid = validate_context(c);
  if (!valid)
    return valid;
  if (state_ != TransactState::GateWaiting)
    return fail(ErrorCode::InvalidState, "request already queued");
  auto update = admission_.prepare();
  if (!update)
    return update.error();
  Expected<RequestPermit> permit = external ? Expected<RequestPermit>{*external}
                                            : update.value()->draft().consume_request_gate(*gate_);
  if (!permit)
    return permit.error();
  auto checked =
      update.value()->draft().validate_request_permit(permit.value(), transaction_, connection_);
  if (!checked)
    return checked.error();
  auto id = runtime_.allocate_call_id(CallOrigin::Outgoing);
  if (!id)
    return id.error();
  EventTxn tx({}, c);
  auto staged =
      runtime_.stage_publish(tx, SendIntent{connection_, transaction_, Flow::Forward, begin_req,
                                            c.ready.time, id.value(), transport_, request_});
  if (staged)
    staged = tx.stage_participant(std::move(update.value()));
  if (staged)
    staged = runtime_.stage_request_permit(tx, id.value(), admission_, permit.value());
  if (!staged) {
    tx.discard();
    runtime_.release_call_id(id.value());
    return staged.error();
  }
  auto committed = runtime_.commit_segment(tx);
  if (!committed) {
    tx.discard();
    runtime_.release_call_id(id.value());
    return committed.error();
  }
  permit_ = permit.value();
  gate_retired_ = true;
  gate_ = RequestGateTicket{permit.value().handle};
  state_ = TransactState::RequestQueued;
  return {};
}
Expected<void> TransactOperation::publish_with_permit(const ExecutionContext &c,
                                                      RequestPermit permit) {
  if (!external_gate_)
    return fail(ErrorCode::InvalidState, "operation owns its request gate");
  return queue_request(c, permit);
}
Expected<void> TransactOperation::observe_sent(const ExecutionContext &c) {
  if (!permit_)
    return {};
  auto wire = runtime_.protocol().inspect(hop_);
  if (!wire)
    return wire.error();
  if (!wire.value().call_ordinal)
    return {};
  wire_started_ = true;
  auto updated = admission_.update_request_lane(
      connection_, runtime_.protocol().request_lane_free(connection_), c.ready);
  if (!updated)
    return updated;
  if (state_ == TransactState::RequestQueued)
    state_ = TransactState::ResponseWaiting;
  return {};
}
Expected<void> TransactOperation::publish_result(TxnResult result, ReadyKey ready) {
  if (!storage_)
    return fail(ErrorCode::InvalidState, "missing result consumer");
  auto value = encode_txn_result(result);
  auto published = runtime_.results().publish(storage_->reservation, std::move(value),
                                              PublicationContext{transaction_, ready, true});
  if (!published)
    return published.error();
  auto owned = runtime_.results().take(storage_->consumer, result_bytes_);
  if (!owned)
    return owned.error();
  auto released = runtime_.results().release_owner(storage_->reservation);
  if (!released)
    return released;
  storage_.reset();
  result_ = std::move(result);
  return {};
}
Expected<bool> TransactOperation::advance(const ExecutionContext &c) {
  auto valid = validate_context(c);
  if (!valid)
    return valid.error();
  if (state_ == TransactState::Complete || state_ == TransactState::Cancelled)
    return true;
  if (state_ == TransactState::Fresh)
    return fail(ErrorCode::InvalidState, "transaction not started");
  if (runtime_.drains().is_cancelled(transaction_)) {
    auto cancelled = cancel(c, CancelReason::Reset);
    if (!cancelled)
      return cancelled.error();
    return true;
  }
  if (state_ == TransactState::GateWaiting) {
    if (external_gate_)
      return false;
    auto lane = admission_.update_request_lane(
        connection_, runtime_.protocol().request_lane_free(connection_), c.ready);
    if (!lane)
      return lane.error();
    auto gate = admission_.gate_state(*gate_);
    if (!gate)
      return gate.error();
    if (gate.value() == RequestGateState::Pending)
      return false;
    if (gate.value() != RequestGateState::Granted)
      return fail(ErrorCode::Cancelled, "request gate cancelled");
    auto queued = queue_request(c, {});
    if (!queued)
      return queued.error();
    return false;
  }
  auto sent = observe_sent(c);
  if (!sent)
    return sent.error();
  if (!response_) {
    auto response = runtime_.milestone(hop_, MilestoneKind::ResponseReady);
    if (!response) {
      if (response.error().code == ErrorCode::NotReady)
        return false;
      return response.error();
    }
    if (!response.value().milestone.response)
      return fail(ErrorCode::ProtocolViolation, "response milestone has no safe snapshot");
    auto &s = *response.value().milestone.response;
    if (s.extensions.size() > limits_.extension_count)
      return fail(ErrorCode::ProtocolViolation, "response extension count");
    std::size_t extension_bytes = 0;
    for (auto &e : s.extensions) {
      if (e.first.size() > limits_.extension_key_bytes ||
          e.second.size() > limits_.extension_bytes - extension_bytes)
        return fail(ErrorCode::ProtocolViolation, "response extension capacity");
      extension_bytes += e.second.size();
    }
    TxnResult result;
    result.status = s.status;
    result.data = request_.data;
    result.extensions = s.extensions;
    if (request_.command == Command::Read) {
      if (s.data.size() != request_.data.size())
        return fail(ErrorCode::ProtocolViolation, "response length changed");
      for (std::size_t i = 0; i < result.data.size(); ++i)
        if (request_.byte_enable.empty() ||
            request_.byte_enable[i % request_.byte_enable.size()] != 0)
          result.data[i] = s.data[i];
    }
    if (owned_value_bytes(encode_txn_result(result)) > result_bytes_)
      return fail(ErrorCode::Capacity, "durable response exceeds reserved capacity");
    response_ = std::move(result);
  }
  auto terminal = runtime_.milestone(hop_, MilestoneKind::Terminal);
  if (!terminal) {
    if (terminal.error().code != ErrorCode::NotReady)
      return terminal.error();
    auto wire = runtime_.protocol().inspect(hop_);
    if (!wire)
      return wire.error();
    if (wire.value().state == WireState::Response && state_ != TransactState::TerminalWaiting) {
      auto id = runtime_.allocate_call_id(CallOrigin::Outgoing);
      if (!id)
        return id.error();
      EventTxn tx({}, c);
      ResponseAcknowledger ack(runtime_.protocol(), hop_, context_.epoch);
      auto staged = ack.stage_ack(tx, c.ready.time, id.value(), request_, &runtime_, transaction_);
      if (!staged) {
        tx.discard();
        runtime_.release_call_id(id.value());
        return staged.error();
      }
      if (!staged.value())
        runtime_.release_call_id(id.value());
      auto committed = runtime_.commit_segment(tx);
      if (!committed) {
        tx.discard();
        runtime_.release_call_id(id.value());
        return committed.error();
      }
      state_ = TransactState::TerminalWaiting;
    }
    return false;
  }
  response_->terminal_effective_time = terminal.value().milestone.time;
  if (!result_) {
    auto published = publish_result(*response_, c.ready);
    if (!published)
      return published.error();
  }
  if (!admission_retired_) {
    auto synced =
        admission_.sync_terminal(hop_, runtime_.protocol(), *response_->terminal_effective_time);
    if (!synced)
      return synced.error();
    auto retired = admission_.retire(hop_);
    if (!retired)
      return retired.error();
    admission_retired_ = true;
  }
  if (!drain_finished_) {
    auto finished = runtime_.drains().finish_local(transaction_);
    if (!finished)
      return finished.error();
    drain_finished_ = true;
  }
  if (!ledger_retired_) {
    auto retired = runtime_.retire_hop(hop_);
    if (!retired) {
      if (retired.error().code == ErrorCode::NotReady)
        return false;
      return retired.error();
    }
    ledger_retired_ = true;
  }
  state_ = TransactState::Complete;
  return true;
}
Expected<CancelDisposition> TransactOperation::cancel(const ExecutionContext &c,
                                                      CancelReason reason) {
  auto valid = validate_context(c, true);
  if (!valid)
    return valid.error();
  if (state_ == TransactState::Complete)
    return CancelDisposition{true, {}};
  if (state_ == TransactState::Cancelled)
    return CancelDisposition{!result_->drain, result_->drain};
  if (state_ == TransactState::Fresh)
    return fail(ErrorCode::InvalidState, "transaction not started");
  auto wire = runtime_.protocol().inspect(hop_);
  if (!wire)
    return wire.error();
  const bool sent = wire.value().call_ordinal != 0;
  wire_started_ = wire_started_ || sent;
  if (gate_ && !gate_retired_) {
    auto gate = admission_.gate_state(*gate_);
    if (!gate)
      return gate.error();
    Expected<void> released;
    if (gate.value() == RequestGateState::Consumed) {
      if (sent)
        released = admission_.request_sent(*permit_);
      else
        released = admission_.cancel_request_permit(*permit_, c.ready);
    } else if (gate.value() == RequestGateState::Pending ||
               gate.value() == RequestGateState::Granted)
      released = admission_.cancel_request_gate(*gate_, c.ready);
    if (!released)
      return released.error();
    released = admission_.retire_request_ticket(*gate_);
    if (!released)
      return released.error();
    gate_retired_ = true;
  }
  if (sent) {
    auto tracked = runtime_.track_cleanup(transaction_, hop_, true, request_);
    if (!tracked && tracked.error().code != ErrorCode::Duplicate)
      return tracked.error();
  }
  auto cancelled = runtime_.cancel_local(transaction_, reason);
  if (!cancelled)
    return cancelled.error();
  if (!sent) {
    auto retired = admission_.retire_unstarted(hop_, runtime_.protocol());
    if (!retired)
      return retired.error();
    admission_retired_ = ledger_retired_ = true;
    auto responsibility = runtime_.drains().retire_responsibility(transaction_);
    if (!responsibility && responsibility.error().code != ErrorCode::StaleHandle)
      return responsibility.error();
    drain_finished_ = true;
    auto untracked = runtime_.untrack_cleanup(hop_);
    if (!untracked)
      return untracked.error();
  }
  TxnResult local = response_.value_or(TxnResult{});
  local.local_cancel = reason;
  local.drain = cancelled.value().receipt;
  auto terminal = runtime_.milestone(hop_, MilestoneKind::Terminal);
  if (terminal)
    local.terminal_effective_time = terminal.value().milestone.time;
  auto published = publish_result(local, c.ready);
  if (!published)
    return published.error();
  state_ = TransactState::Cancelled;
  return cancelled.value();
}
Expected<TxnResult> TransactOperation::result() const {
  if (!result_)
    return fail(ErrorCode::NotReady, "durable transaction result unavailable");
  return *result_;
}
Expected<ResultHandle> TransactOperation::subscribe_result(std::uint64_t owner) {
  if (!storage_)
    return fail(ErrorCode::NotReady, "result producer retired");
  return runtime_.results().retain(storage_->consumer, owner);
}
} // namespace leanat

namespace leanat {
bool TransactOperation::published() const {
  if (state_ == TransactState::Fresh)
    return false;
  if (wire_started_)
    return true;
  auto wire = runtime_.protocol().inspect(hop_);
  return wire ? wire.value().call_ordinal != 0
              : (state_ == TransactState::Complete ||
                 (result_ && result_->terminal_effective_time.has_value()));
}
Expected<bool> TransactOperation::reap(const ExecutionContext &c) {
  auto valid = validate_context(c, true);
  if (!valid)
    return valid.error();
  if (state_ != TransactState::Cancelled)
    return fail(ErrorCode::InvalidState, "reap requires cancelled transaction");
  if (ledger_retired_)
    return true;
  auto terminal = runtime_.milestone(hop_, MilestoneKind::Terminal);
  if (!terminal) {
    if (terminal.error().code == ErrorCode::NotReady)
      return false;
    return terminal.error();
  }
  if (!admission_retired_) {
    auto sync =
        admission_.sync_terminal(hop_, runtime_.protocol(), terminal.value().milestone.time);
    if (!sync)
      return sync.error();
    auto retired = admission_.retire(hop_);
    if (!retired)
      return retired.error();
    admission_retired_ = true;
  }
  auto retired = runtime_.retire_hop(hop_);
  if (!retired) {
    if (retired.error().code == ErrorCode::NotReady)
      return false;
    return retired.error();
  }
  ledger_retired_ = true;
  return true;
}
} // namespace leanat

namespace leanat {
Expected<TxnResult> decode_txn_result(const Value &v) {
  auto a = std::get_if<Value::Array>(&v.data);
  if (!a || a->size() != 6)
    return fail(ErrorCode::Schema, "transaction result shape");
  auto status = std::get_if<std::uint64_t>(&(*a)[0].data);
  auto data = std::get_if<Bytes>(&(*a)[1].data);
  auto extensions = std::get_if<Value::Array>(&(*a)[2].data);
  if (!status || *status > static_cast<std::uint64_t>(ResponseStatus::ByteEnableError) || !data ||
      !extensions)
    return fail(ErrorCode::Schema, "transaction result fields");
  TxnResult out;
  out.status = static_cast<ResponseStatus>(*status);
  out.data = *data;
  for (auto &e : *extensions) {
    auto fields = std::get_if<Value::Array>(&e.data);
    if (!fields || fields->size() != 2)
      return fail(ErrorCode::Schema, "result extension shape");
    auto key = std::get_if<Bytes>(&(*fields)[0].data),
         bytes = std::get_if<Bytes>(&(*fields)[1].data);
    if (!key || !bytes)
      return fail(ErrorCode::Schema, "result extension fields");
    if (!out.extensions.emplace(std::string(key->begin(), key->end()), *bytes).second)
      return fail(ErrorCode::Duplicate, "result extension key");
  }
  if (auto time = std::get_if<std::uint64_t>(&(*a)[3].data))
    out.terminal_effective_time = Tick{*time};
  else if (!std::holds_alternative<std::monostate>((*a)[3].data))
    return fail(ErrorCode::Schema, "terminal time shape");
  if (auto cancel = std::get_if<std::uint64_t>(&(*a)[4].data)) {
    if (*cancel > static_cast<std::uint64_t>(CancelReason::ParentCancelled))
      return fail(ErrorCode::Schema, "cancel reason");
    out.local_cancel = static_cast<CancelReason>(*cancel);
  } else if (!std::holds_alternative<std::monostate>((*a)[4].data))
    return fail(ErrorCode::Schema, "cancel shape");
  if (auto drain = std::get_if<Handle>(&(*a)[5].data)) {
    if (drain->kind != HandleKind::Drain)
      return fail(ErrorCode::Schema, "drain kind");
    out.drain = *drain;
  } else if (!std::holds_alternative<std::monostate>((*a)[5].data))
    return fail(ErrorCode::Schema, "drain shape");
  return out;
}
Expected<void> FinishResponseOperation::start(const ExecutionContext &c, ResultHandle token) {
  if (started_)
    return fail(ErrorCode::Duplicate, "finishResponse already started");
  if (c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "finishResponse requires process");
  auto create = runtime_.results().describe(token);
  if (!create)
    return create.error();
  if (create.value().source != producer_.transaction() || token.consumer.owner != c.owner)
    return fail(ErrorCode::WrongOwner, "finishResponse consumer/observation mismatch");
  if (create.value().max_bytes > destination_bytes_)
    return fail(ErrorCode::Capacity, "finishResponse destination capacity");
  auto observed = runtime_.milestone(producer_.hop(), MilestoneKind::ResponseReady);
  if (!observed)
    return observed.error();
  EventTxn tx({}, c);
  auto owned = runtime_.results().prepare_transfer(tx, token, c.owner);
  if (!owned)
    return owned.error();
  auto committed = runtime_.commit_segment(tx);
  if (!committed)
    return committed.error();
  consumer_ = owned.value();
  owner_ = c.owner;
  epoch_ = c.epoch;
  started_ = true;
  return {};
}
Expected<bool> FinishResponseOperation::advance(const ExecutionContext &c) {
  if (!started_)
    return fail(ErrorCode::InvalidState, "finishResponse not started");
  if (c.kind != ContextKind::Process || c.owner != owner_ || c.epoch != epoch_)
    return fail(ErrorCode::WrongOwner, "finishResponse owner/epoch");
  if (result_)
    return true;
  auto done = producer_.advance(c);
  if (!done)
    return done.error();
  if (!done.value())
    return false;
  auto value = runtime_.results().read(*consumer_, destination_bytes_);
  if (!value)
    return value.error();
  auto result = decode_txn_result(value.value());
  if (!result)
    return result.error();
  auto released = runtime_.results().release(*consumer_);
  if (!released)
    return released.error();
  consumer_.reset();
  result_ = std::move(result.value());
  return true;
}
Expected<void> FinishResponseOperation::cancel(const ExecutionContext &c) {
  if (!started_ || c.owner != owner_ || c.kind != ContextKind::Process)
    return fail(ErrorCode::WrongOwner, "finishResponse cancellation owner");
  if (result_)
    return {};
  auto cancelled = producer_.cancel(c, CancelReason::User);
  if (!cancelled)
    return cancelled.error();
  auto value = runtime_.results().read(*consumer_, destination_bytes_);
  if (!value)
    return value.error();
  auto result = decode_txn_result(value.value());
  if (!result)
    return result.error();
  auto released = runtime_.results().release(*consumer_);
  if (!released)
    return released.error();
  consumer_.reset();
  result_ = std::move(result.value());
  return {};
}
Expected<TxnResult> FinishResponseOperation::result() const {
  if (!result_)
    return fail(ErrorCode::NotReady, "finishResponse result not ready");
  return *result_;
}
} // namespace leanat

namespace leanat {
Expected<void> release_task_result_when_done(EventTxn &tx, Runtime &runtime, ResultHandle handle) {
  const auto &c = tx.context();
  if (c.kind != ContextKind::Timed && c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "task result release context");
  if (handle.consumer.owner != c.owner || handle.consumer.domain != c.domain)
    return fail(ErrorCode::WrongOwner, "task result consumer ownership");
  auto description = runtime.results().describe(handle);
  if (!description)
    return description.error();
  if (description.value().source.kind != HandleKind::Task)
    return fail(ErrorCode::InvalidArgument, "task result source required");
  return runtime.results().prepare_release(tx, handle);
}
} // namespace leanat
