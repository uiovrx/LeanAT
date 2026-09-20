#include "leanat/runtime_services.hpp"
namespace leanat {
Expected<WireReturn> decode_transport_return(const Value &value) {
  auto fields = std::get_if<Value::Array>(&value.data);
  if (!fields || fields->size() != 4)
    return fail(ErrorCode::TypeMismatch, "transport return record");
  auto sync = std::get_if<std::uint64_t>(&(*fields)[0].data);
  auto delay = std::get_if<std::uint64_t>(&(*fields)[2].data);
  if (!sync || *sync > 2 || !delay)
    return fail(ErrorCode::TypeMismatch, "transport sync/delay");
  auto option = [](const Value &v) -> Expected<std::optional<Value>> {
    auto pair = std::get_if<Value::Array>(&v.data);
    if (!pair || pair->size() != 2)
      return fail(ErrorCode::TypeMismatch, "optional value variant");
    auto tag = std::get_if<std::uint64_t>(&(*pair)[0].data);
    auto payload = std::get_if<Value::Array>(&(*pair)[1].data);
    if (!tag || !payload || *tag > 1 || payload->size() != *tag)
      return fail(ErrorCode::TypeMismatch, "optional constructor payload");
    if (!*tag)
      return std::optional<Value>{};
    return std::optional<Value>{payload->front()};
  };
  auto phase = option((*fields)[1]);
  auto response = option((*fields)[3]);
  if (!phase)
    return phase.error();
  if (!response)
    return response.error();
  WireReturn result;
  result.sync = static_cast<Sync>(*sync);
  result.outgoing_delay = Duration{*delay};
  if (phase.value()) {
    auto n = std::get_if<std::uint64_t>(&phase.value()->data);
    if (!n || *n > UINT32_MAX)
      return fail(ErrorCode::TypeMismatch, "return phase");
    result.phase = PhaseId{static_cast<std::uint32_t>(*n)};
  }
  if (response.value()) {
    auto record = std::get_if<Value::Array>(&response.value()->data);
    if (!record || record->size() != 3)
      return fail(ErrorCode::TypeMismatch, "response record");
    auto status = std::get_if<std::uint64_t>(&(*record)[0].data);
    auto bytes = std::get_if<Bytes>(&(*record)[1].data);
    auto dmi = std::get_if<bool>(&(*record)[2].data);
    if (!status || *status > static_cast<std::uint64_t>(ResponseStatus::ByteEnableError) ||
        !bytes || !dmi)
      return fail(ErrorCode::TypeMismatch, "response fields");
    result.response = ResponseSnapshot{static_cast<ResponseStatus>(*status), *bytes, *dmi, {}};
  }
  return result;
}
namespace {
bool same_signature(const exec::ServiceSignature &a, const exec::ServiceSignature &b) {
  return a.id == b.id && a.op == b.op && a.input_types == b.input_types &&
         a.result_types == b.result_types && a.context_mask == b.context_mask &&
         a.effect_mask == b.effect_mask && a.extra_fuel == b.extra_fuel &&
         a.provider_key == b.provider_key && a.provider_version == b.provider_version &&
         a.abi_hash == b.abi_hash;
}
Expected<std::uint64_t> number(const Value &v) {
  auto p = std::get_if<std::uint64_t>(&v.data);
  if (!p)
    return fail(ErrorCode::TypeMismatch, "core service expects U64");
  return *p;
}
Expected<Handle> handle(const Value &v, HandleKind kind, const ExecutionContext &c) {
  auto h = std::get_if<Handle>(&v.data);
  if (!h || h->kind != kind)
    return fail(ErrorCode::TypeMismatch, "core service handle kind");
  if (h->domain != c.domain)
    return fail(ErrorCode::WrongDomain, "core service handle domain");
  if (h->owner != c.owner)
    return fail(ErrorCode::WrongOwner, "core service handle owner");
  return *h;
}
} // namespace
CoreRuntimeBackend::CoreRuntimeBackend(Runtime &r, std::size_t c) : runtime_(r), capacity_(c) {}
CoreRuntimeBackend::CoreRuntimeBackend(Runtime &r, const exec::Project &p, std::size_t capacity)
    : runtime_(r), capacity_(capacity), core_types_(&p) {}
Expected<void> CoreRuntimeBackend::register_core(exec::ServiceSignature signature,
                                                 const exec::Project &types) {
  if (core_types_ && core_types_ != &types)
    return fail(ErrorCode::InvalidArgument, "core type table changed");
  core_types_ = &types;
  return register_core(std::move(signature));
}
Expected<void> CoreRuntimeBackend::bind_current_process(Handle process) {
  auto frame = runtime_.processes().inspect(process);
  if (!frame)
    return frame.error();
  current_process_ = process;
  return {};
}
Expected<void> CoreRuntimeBackend::prepare_suspend(Handle wait, BlockId block,
                                                   std::vector<Value> live, TypeId,
                                                   const ExecutionContext &ctx, EventTxn &txn) {
  auto spec = runtime_.processes().wait_spec(wait, &txn);
  if (!spec)
    return spec.error();
  auto token =
      runtime_.processes().suspend_registered(wait, ResumeFrame{block, std::move(live)}, txn);
  if (!token)
    return token.error();
  if (spec.value().kind == SingleWaitKind::After || spec.value().kind == SingleWaitKind::Until) {
    auto tick = spec.value().until < ctx.ready.time ? ctx.ready.time : spec.value().until;
    auto event = runtime_.stage_resume(txn, token.value(), ReadyKey{tick, 0});
    if (!event)
      return event.error();
  } else if (spec.value().kind == SingleWaitKind::Response) {
    auto outcome = runtime_.response_wait_outcome(spec.value().source);
    if (outcome) {
      auto notified = runtime_.processes().notify(token.value(), outcome.value(), txn);
      if (!notified)
        return notified.error();
      auto event = runtime_.stage_resume(txn, token.value(), ctx.ready);
      if (!event)
        return event.error();
    } else if (outcome.error().code != ErrorCode::NotReady)
      return outcome.error();
  }
  return {};
}
Expected<exec::ResumeInput> CoreRuntimeBackend::prepare_resume(const SuspensionToken &t,
                                                               const ExecutionContext &,
                                                               EventTxn &txn) {
  auto info = runtime_.processes().inspect(t.process);
  if (!info)
    return info.error();
  auto action = runtime_.processes().take_resume(t, txn);
  if (!action)
    return action.error();
  current_process_ = t.process;
  if (info.value().instance != txn.context().instance)
    return fail(ErrorCode::WrongOwner, "resume instance mismatch");
  exec::ResumeInput input;
  input.program = info.value().program;
  input.block = action.value().frame.resume_block;
  input.arguments.push_back(action.value().outcome.value);
  for (auto &v : action.value().frame.live)
    input.arguments.push_back(v);
  return input;
}
Expected<void> CoreRuntimeBackend::register_provider(exec::ServiceSignature s, Provider provider) {
  if (frozen_)
    return fail(ErrorCode::InvalidState, "service registry frozen");
  if (bindings_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "service registry full");
  if (!provider || s.provider_key.empty() || s.provider_version.empty())
    return fail(ErrorCode::InvalidArgument, "missing actual provider identity");
  if (bindings_.count(s.id))
    return fail(ErrorCode::Duplicate, "service id already bound");
  bindings_.emplace(s.id, Binding{std::move(s), std::move(provider)});
  return {};
}
Expected<void> CoreRuntimeBackend::register_providers(
    std::vector<std::pair<exec::ServiceSignature, Provider>> providers) {
  if (frozen_)
    return fail(ErrorCode::InvalidState, "service registry frozen");
  if (providers.size() > capacity_ - bindings_.size())
    return fail(ErrorCode::Capacity, "service registry full");
  std::map<std::uint32_t, Binding> prepared;
  for (auto &entry : providers) {
    auto &s = entry.first;
    if (!entry.second || s.provider_key.empty() || s.provider_version.empty())
      return fail(ErrorCode::InvalidArgument, "missing actual provider identity");
    if (bindings_.count(s.id) || prepared.count(s.id))
      return fail(ErrorCode::Duplicate, "service id already bound");
    prepared.emplace(s.id, Binding{std::move(s), std::move(entry.second)});
  }
  bindings_.merge(prepared);
  return {};
}
Expected<void> CoreRuntimeBackend::freeze() {
  if (frozen_)
    return fail(ErrorCode::Duplicate, "service registry already frozen");
  frozen_ = true;
  return {};
}
Expected<void> CoreRuntimeBackend::check_signature(const exec::ServiceSignature &s) const {
  if (!frozen_)
    return fail(ErrorCode::InvalidState, "service registry not frozen");
  auto b = bindings_.find(s.id);
  if (b == bindings_.end() || !same_signature(s, b->second.signature))
    return fail(ErrorCode::Integrity, "descriptor service disagrees with actual frozen provider");
  return {};
}
Expected<std::vector<Value>> CoreRuntimeBackend::invoke(const exec::ServiceSignature &s,
                                                        const std::vector<Value> &args,
                                                        const ExecutionContext &ctx,
                                                        EventTxn &txn) {
  auto checked = check_signature(s);
  if (!checked)
    return checked.error();
  const auto context_bit = std::uint32_t{1} << static_cast<unsigned>(ctx.kind);
  if (!(s.context_mask & context_bit) || !(exec::allowed_contexts(s.op) & context_bit))
    return fail(ErrorCode::InvalidState, "service context disallowed");
  if (ctx.domain != txn.context().domain || ctx.owner != txn.context().owner ||
      ctx.epoch != txn.context().epoch || ctx.instance != txn.context().instance ||
      ctx.kind != txn.context().kind || ctx.connection != txn.context().connection ||
      ctx.ready != txn.context().ready)
    return fail(ErrorCode::WrongOwner, "service execution context mismatch");
  if (args.size() != s.input_types.size())
    return fail(ErrorCode::TypeMismatch, "service arity");
  return bindings_.at(s.id).provider(args, ctx, txn);
}
Expected<void> CoreRuntimeBackend::register_core(exec::ServiceSignature s) {
  using exec::Op;
  const bool response_wait =
      s.op == Op::RegisterWait && s.provider_key == "leanat.core.wait.response";
  if (!core_types_)
    return fail(ErrorCode::InvalidArgument,
                "core registration requires actual executed type table");
  std::string key, hex;
  switch (s.op) {
  case Op::GetContextField:
    key = "leanat.core.context.process";
    hex = "f36277e80b561e7f2fccbe5e1fde0aa9b6599153f09130bd1f1c96f12133a0da";
    break;
  case Op::RegisterWait:
    key = response_wait ? "leanat.core.wait.response" : "leanat.core.wait.timer";
    hex = response_wait ? "7f0dfc014f1a8c07de8d55c2e40a52b85fc7d534be8df44f48d31c2962db1a04"
                        : "783dda9c9ea7bd22ee2b2b075230e110219632fb463886aed1579583d2aea36f";
    break;
  case Op::SetTransportReturn:
    key = "leanat.core.transport.return";
    hex = "eb4a92463bf8fc5c11181b492b8ad37f91671b8f703bdc5370604ef49ace0ffe";
    break;
  case Op::BufferOutputWrite:
    key = "leanat.core.output.write";
    hex = "87d06714cd584408dbba0e953b91928c11867e65713f710a4256be0632bebdbc";
    break;
  case Op::ScheduleEvent:
    key = "leanat.core.event.schedule";
    hex = "bffe39a0efb9a6f7c75fbb789e564aac1286c4f9b6c00e6f2971d896c39b8705";
    break;
  case Op::CancelEvent:
    key = "leanat.core.event.cancel";
    hex = "214aeb1ccb48bbaeabe889924cbb909b6f2049b64b7fd587fff0fb25e8ffc479";
    break;
  case Op::ResultGet:
    key = "leanat.core.result.get";
    hex = "f40fcd8a22c18448a9b8b9bef88c7eca9dc150e28444ab9e4b2465ee6de587d0";
    break;
  case Op::ResultRelease:
    key = "leanat.core.result.release";
    hex = "21c278f5ed404ee7e67528ae212eab663d65b69ea2c08440980a21a244d673e7";
    break;
  case Op::ReadWaitResult:
    key = "leanat.core.wait.result";
    hex = "24ad58653d80da7b299d174f9f6f1c9fbb9f6b14159ca7846741b8214f981184";
    break;
  case Op::CancelLocal:
    key = "leanat.core.transaction.cancel";
    hex = "fd95191dfcd83497a02c4edcca1089d64b98ba014a288bf1800bc12df81a6956";
    break;
  default:
    return fail(ErrorCode::Unsupported, "no canonical core provider");
  }
  std::array<std::uint8_t, 32> expected{};
  auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < 32; ++i)
    expected[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  const std::uint64_t expected_fuel =
      (s.op == Op::GetContextField || s.op == Op::RegisterWait || s.op == Op::SetTransportReturn ||
       s.op == Op::BufferOutputWrite)
          ? 1
          : 0;
  if (s.provider_key != key || s.provider_version != "1" || s.abi_hash != expected ||
      s.extra_fuel != expected_fuel || !s.context_mask ||
      (s.context_mask & ~exec::allowed_contexts(s.op)) ||
      s.effect_mask != exec::required_effect(s.op))
    return fail(ErrorCode::Integrity, "core provider canonical ABI mismatch");
  std::size_t inputs = 0, outputs = 1;
  switch (s.op) {
  case Op::SetTransportReturn:
    inputs = 1;
    outputs = 0;
    break;
  case Op::BufferOutputWrite:
    inputs = 2;
    outputs = 0;
    break;
  case Op::GetContextField:
    inputs = 0;
    break;
  case Op::RegisterWait:
    inputs = response_wait ? 2 : 3;
    break;
  case Op::ScheduleEvent:
    inputs = 2;
    break;
  case Op::CancelEvent:
    inputs = 1;
    break;
  case Op::ResultGet:
  case Op::ResultRelease:
    inputs = 2;
    break;
  case Op::ReadWaitResult:
    inputs = 1;
    break;
  case Op::CancelLocal:
    inputs = 2;
    break;
  default:
    return fail(ErrorCode::Unsupported,
                "operation requires its concrete subsystem provider binding");
  }
  if (s.input_types.size() != inputs || s.result_types.size() != outputs)
    return fail(ErrorCode::Schema, "core service ABI arity mismatch");
  auto type = [&](std::uint32_t id) -> const exec::Type * {
    return id < core_types_->types.size() ? &core_types_->types[id] : nullptr;
  };
  auto u64 = [&](std::uint32_t id) {
    auto t = type(id);
    return t && t->kind == exec::TypeKind::Bits && t->bound == 64;
  };
  auto handle_type = [&](std::uint32_t id, HandleKind kind) {
    auto t = type(id);
    return t && t->kind == exec::TypeKind::Handle && t->bound == static_cast<std::uint64_t>(kind);
  };
  auto unit = [&](std::uint32_t id) {
    auto t = type(id);
    return t && t->kind == exec::TypeKind::Unit;
  };
  bool shape = true;
  for (auto id : s.input_types)
    shape = shape && type(id);
  for (auto id : s.result_types)
    shape = shape && type(id);
  if (shape)
    switch (s.op) {
    case Op::GetContextField:
      shape = handle_type(s.result_types[0], HandleKind::Process);
      break;
    case Op::RegisterWait:
      shape = handle_type(s.input_types[0], HandleKind::Process) &&
              (response_wait ? handle_type(s.input_types[1], HandleKind::Hop)
                             : (u64(s.input_types[1]) && u64(s.input_types[2]))) &&
              handle_type(s.result_types[0], HandleKind::Wait);
      break;
    case Op::ScheduleEvent:
      shape = u64(s.input_types[0]) && handle_type(s.result_types[0], HandleKind::Event);
      break;
    case Op::CancelEvent:
      shape = handle_type(s.input_types[0], HandleKind::Event) &&
              type(s.result_types[0])->kind == exec::TypeKind::Bool;
      break;
    case Op::BufferOutputWrite:
      shape = u64(s.input_types[0]) && (type(s.input_types[1])->kind == exec::TypeKind::Bool ||
                                        type(s.input_types[1])->kind == exec::TypeKind::Bits);
      break;
    case Op::ResultGet:
    case Op::ResultRelease:
      shape = handle_type(s.input_types[0], HandleKind::Result) &&
              handle_type(s.input_types[1], HandleKind::Consumer) &&
              (s.op == Op::ResultGet || unit(s.result_types[0]));
      break;
    case Op::CancelLocal:
      shape = handle_type(s.input_types[0], HandleKind::Transaction) && u64(s.input_types[1]) &&
              unit(s.result_types[0]);
      break;
    case Op::ReadWaitResult:
      shape = type(s.input_types[0])->kind == exec::TypeKind::Variant;
      break;
    case Op::SetTransportReturn: {
      auto record = type(s.input_types[0]);
      shape = record->kind == exec::TypeKind::Record && record->fields.size() == 4;
      if (shape) {
        shape = u64(record->fields[0]) && u64(record->fields[2]);
        for (auto index : {1, 3}) {
          auto option = type(record->fields[index]);
          shape = shape && option && option->kind == exec::TypeKind::Variant &&
                  option->constructors.size() == 2 && option->constructors[0].empty() &&
                  option->constructors[1].size() == 1;
        }
        if (shape) {
          auto phase = type(record->fields[1]);
          auto response = type(type(record->fields[3])->constructors[1][0]);
          shape = u64(phase->constructors[1][0]) && response &&
                  response->kind == exec::TypeKind::Record && response->fields.size() == 3;
          if (shape)
            shape = u64(response->fields[0]) && type(response->fields[1]) &&
                    type(response->fields[1])->kind == exec::TypeKind::Bytes &&
                    type(response->fields[2]) &&
                    type(response->fields[2])->kind == exec::TypeKind::Bool;
        }
      }
      break;
    }
    default:
      break;
    }
  if (!shape)
    return fail(ErrorCode::Schema, "core provider concrete type ABI mismatch");
  auto op = s.op;
  return register_provider(
      std::move(s),
      [this, op, response_wait](const std::vector<Value> &a, const ExecutionContext &ctx,
                                EventTxn &txn) -> Expected<std::vector<Value>> {
        switch (op) {
        case Op::SetTransportReturn: {
          if (ctx.kind != ContextKind::Transport)
            return fail(ErrorCode::InvalidState, "transport return outside callback");
          auto returned = decode_transport_return(a[0]);
          if (!returned)
            return returned.error();
          return std::vector<Value>{};
        }
        case Op::BufferOutputWrite: {
          auto port = number(a[0]);
          if (!port || port.value() > UINT32_MAX)
            return fail(ErrorCode::TypeMismatch, "output port");
          auto staged =
              runtime_.stage_output(txn, PortId{static_cast<std::uint32_t>(port.value())}, a[1]);
          if (!staged)
            return staged.error();
          return std::vector<Value>{};
        }
        case Op::GetContextField: {
          if (!current_process_ || current_process_->owner != ctx.owner ||
              current_process_->domain != ctx.domain)
            return fail(ErrorCode::WrongOwner, "current process context is not bound");
          auto frame = runtime_.processes().inspect(*current_process_);
          if (!frame) return frame.error();
          if (frame.value().instance_bound && frame.value().instance != ctx.instance)
            return fail(ErrorCode::WrongOwner, "current process instance mismatch");
          return std::vector<Value>{Value{*current_process_}};
        }
        case Op::RegisterWait: {
          auto process = handle(a[0], HandleKind::Process, ctx);
          if (!process)
            return process.error();
          if (!current_process_ || *current_process_ != process.value())
            return fail(ErrorCode::WrongOwner, "wait process is not current");
          if (response_wait) {
            auto hop = handle(a[1], HandleKind::Hop, ctx);
            if (!hop)
              return hop.error();
            auto source = runtime_.inspect_hop(hop.value());
            if (!source)
              return source.error();
            if (source.value().identity.local_side != ctx.instance)
              return fail(ErrorCode::WrongOwner, "response wait source instance");
            SingleWaitSpec spec;
            spec.kind = SingleWaitKind::Response;
            spec.source = hop.value();
            spec.connection = source.value().identity.connection;
            auto wait = runtime_.processes().register_wait(process.value(), spec, txn);
            if (!wait)
              return wait.error();
            return std::vector<Value>{Value{wait.value()}};
          }
          auto kind = number(a[1]);
          auto tick = number(a[2]);
          if (!kind || !tick)
            return fail(ErrorCode::TypeMismatch, "timer wait operands");
          if (kind.value() != static_cast<std::uint64_t>(SingleWaitKind::After) &&
              kind.value() != static_cast<std::uint64_t>(SingleWaitKind::Until))
            return fail(ErrorCode::Unsupported, "core wait service requires timer kind");
          SingleWaitSpec spec;
          spec.kind = static_cast<SingleWaitKind>(kind.value());
          spec.after = Duration{tick.value()};
          spec.until = Tick{tick.value()};
          auto wait = runtime_.processes().register_wait(process.value(), spec, txn);
          if (!wait)
            return wait.error();
          return std::vector<Value>{Value{wait.value()}};
        }
        case Op::ScheduleEvent: {
          auto tick = number(a[0]);
          if (!tick)
            return tick.error();
          auto ready = runtime_.queue().successor(Tick{tick.value()});
          if (!ready)
            return ready.error();
          if (Tick{tick.value()} < ctx.ready.time)
            return fail(ErrorCode::TimeRegression, "scheduled event before segment");
          EventDraft d;
          d.key = {ready.value().time, ready.value().turn, EventStage::Internal,
                   ctx.instance,       ctx.connection,     0};
          d.owner = ctx.owner;
          d.epoch = ctx.epoch;
          d.value = a[1];
          auto event = txn.stage_event(runtime_.queue(), std::move(d));
          if (!event)
            return event.error();
          return std::vector<Value>{Value{event.value()}};
        }
        case Op::CancelEvent: {
          auto event = handle(a[0], HandleKind::Event, ctx);
          if (!event)
            return event.error();
          auto possible = txn.can_cancel(runtime_.queue(), event.value());
          if (!possible)
            return possible.error();
          if (possible.value()) {
            auto cancel = txn.stage_cancel(runtime_.queue(), event.value());
            if (!cancel)
              return cancel.error();
          }
          return std::vector<Value>{Value{possible.value()}};
        }
        case Op::ResultGet:
        case Op::ResultRelease: {
          ExecutionContext producer = ctx;
          if (auto raw = std::get_if<Handle>(&a[0].data))
            producer.owner = raw->owner;
          auto result = handle(a[0], HandleKind::Result, producer);
          if (!result)
            return result.error();
          auto consumer = handle(a[1], HandleKind::Consumer, ctx);
          if (!consumer)
            return consumer.error();
          ResultHandle h{result.value(), consumer.value()};
          if (op == Op::ResultGet) {
            auto value = runtime_.results().prepare_read(txn, h);
            if (!value)
              return value.error();
            return std::vector<Value>{value.value()};
          }
          auto released = runtime_.results().prepare_release(txn, h);
          if (!released)
            return released.error();
          return std::vector<Value>{Value{}};
        }
        case Op::ReadWaitResult: {
          auto outcome = std::get_if<Value::Array>(&a[0].data);
          if (!outcome || outcome->size() != 2)
            return fail(ErrorCode::TypeMismatch, "owning wait outcome is {tag,value}");
          auto tag = number((*outcome)[0]);
          if (!tag || tag.value() != 0)
            return fail(ErrorCode::NotReady, "wait outcome is not successful");
          return std::vector<Value>{(*outcome)[1]};
        }
        case Op::CancelLocal: {
          auto h = handle(a[0], HandleKind::Transaction, ctx);
          if (!h)
            return h.error();
          auto reason = number(a[1]);
          if (!reason || reason.value() > 3)
            return fail(ErrorCode::InvalidArgument, "cancel reason");
          auto disposition = runtime_.stage_cancel_local(txn, h.value(),
                                                         static_cast<CancelReason>(reason.value()));
          if (!disposition)
            return disposition.error();
          if (disposition.value().receipt) {
            auto drain = runtime_.drains().prepare(txn);
            if (!drain)
              return drain.error();
            auto released = drain.value()->view().release_receipt(*disposition.value().receipt);
            if (!released)
              return released.error();
            auto staged = txn.stage_participant(std::move(drain.value()));
            if (!staged)
              return staged.error();
          }
          return std::vector<Value>{Value{}};
        }
        default:
          return fail(ErrorCode::Unsupported, "unbound core operation");
        }
      });
}
} // namespace leanat
