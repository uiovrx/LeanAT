#include "leanat/interpreter.hpp"
#include "leanat/numeric.hpp"
namespace leanat::exec {
namespace {
template <class Invoke>
Expected<SegmentResult> bounded_segment(const Project &project, std::uint32_t id,
                                        FuelCounter &caller, Invoke invoke) {
  const Program *program = nullptr;
  for (const auto &p : project.programs)
    if (p.id == id)
      program = &p;
  if (!program || project.schema_major < 5 || !program->instruction_fuel)
    return invoke(caller);
  const auto available = std::min(caller.remaining, program->instruction_fuel);
  FuelCounter segment{available};
  auto result = invoke(segment);
  caller.remaining -= available - segment.remaining;
  return result;
}
} // namespace
Expected<SegmentResult> Interpreter::execute_segment(std::uint32_t id, const ExecutionContext &ctx,
                                                     const std::vector<Value> &args, EventTxn &txn,
                                                     FuelCounter &fuel) const {
  return bounded_segment(project_.get(), id, fuel, [&](FuelCounter &segment) {
    return execute_at(id, ctx, args, txn, segment, {});
  });
}
Expected<SegmentResult> Interpreter::resume_segment(const SuspensionToken &token,
                                                    const ExecutionContext &ctx, EventTxn &txn,
                                                    FuelCounter &fuel) const try {
  auto &tc = txn.context();
  if (ctx.kind != ContextKind::Process || ctx.kind != tc.kind || ctx.domain != tc.domain ||
      ctx.instance != tc.instance || ctx.owner != tc.owner || ctx.epoch != tc.epoch ||
      ctx.ready != tc.ready || ctx.connection != tc.connection) {
    (void)txn.discard();
    return fail(ErrorCode::WrongOwner, "resume execution/transaction context mismatch");
  }
  if (token.process.kind != HandleKind::Process || token.wait.kind != HandleKind::Wait) {
    (void)txn.discard();
    return fail(ErrorCode::TypeMismatch, "suspension token handle kinds");
  }
  if (token.process.domain != ctx.domain || token.wait.domain != ctx.domain) {
    (void)txn.discard();
    return fail(ErrorCode::WrongDomain, "suspension token domain");
  }
  if (!backend_) {
    (void)txn.discard();
    return fail(ErrorCode::Unsupported, "resume backend unavailable");
  }
  auto resumed = backend_->prepare_resume(token, ctx, txn);
  if (!resumed) {
    (void)txn.discard();
    return resumed.error();
  }
  return bounded_segment(
      project_.get(), resumed.value().program.value, fuel, [&](FuelCounter &segment) {
        return execute_at(resumed.value().program.value, ctx, resumed.value().arguments, txn,
                          segment, resumed.value().block.value);
      });
} catch (const std::bad_alloc &) {
  (void)txn.discard();
  return fail(ErrorCode::Capacity, "resume allocation failure");
} catch (const std::exception &e) {
  (void)txn.discard();
  return fail(ErrorCode::InvalidState, e.what());
} catch (...) {
  (void)txn.discard();
  return fail(ErrorCode::ExternalFailure, "resume provider threw a nonstandard exception");
}
Expected<SegmentResult> Interpreter::execute_at(std::uint32_t id, const ExecutionContext &ctx,
                                                const std::vector<Value> &args, EventTxn &txn,
                                                FuelCounter &fuel,
                                                std::optional<std::uint32_t> start,
                                                std::size_t call_depth, bool pure) const try {
  const auto &p = project_.get();
  const Program *pr = nullptr;
  for (auto &x : p.programs)
    if (x.id == id)
      pr = &x;
  std::string location = "program " + std::to_string(id);
  const OpcodeObservation *active_observation = nullptr;
  auto abort = [&](Error e, bool attach_location = true) -> Expected<SegmentResult> {
    (void)txn.discard();
    if (attach_location)
      e.message += " (" + location + ")";
    if (observer_ && active_observation) {
      auto failed = *active_observation;
      failed.stage = OpcodeObservation::Stage::Error;
      failed.fuel_after = fuel.remaining;
      failed.error = &e;
      observer_->record(failed);
      active_observation = nullptr;
    }
    return e;
  };
  if (!pr)
    return abort(fail(ErrorCode::InvalidArgument, "program not found"));
  if (p.schema_major >= 4 && (p.schema_major == 4 || p.system_metadata)) {
    if (!p.system_metadata || ctx.domain.value != p.system_metadata->runtime_domain)
      return abort(fail(ErrorCode::WrongDomain, "program runtime domain mismatch"));
    if (!pure) {
      const InstanceDesc *owner = nullptr;
      for (auto &i : p.instances)
        for (auto &h : i.handlers)
          if (h.program_id == id)
            owner = &i;
      if (!owner || ctx.instance.value != owner->id)
        return abort(fail(ErrorCode::WrongOwner, "program instance mismatch"));
    }
  }
  if (call_depth > 64)
    return abort(fail(ErrorCode::Capacity, "pure call depth bound"));
  if (ctx.epoch != txn.context().epoch || ctx.owner != txn.context().owner ||
      ctx.domain != txn.context().domain || ctx.kind != txn.context().kind ||
      ctx.instance != txn.context().instance || ctx.connection != txn.context().connection ||
      ctx.ready != txn.context().ready)
    return abort(fail(ErrorCode::WrongOwner, "execution/transaction context mismatch"));
  if (p.schema_major == 1 && ctx.kind != ContextKind::Timed && ctx.kind != ContextKind::Process)
    return abort(fail(ErrorCode::Unsupported, "v1 programs require a timed or process context"));
  if (p.schema_major >= 2 && !pure && ctx.kind != pr->context)
    return abort(fail(ErrorCode::InvalidState, "program context mismatch"));
  std::map<std::uint32_t, const ServiceSignature *> services;
  if (!pure)
    for (auto &s : p.services) {
      if (!backend_)
        return abort(fail(ErrorCode::Unsupported, "service backend unavailable"));
      auto binding = backend_->check_signature(s);
      if (!binding)
        return abort(binding.error());
      services.emplace(s.id, &s);
    }
  const Block *start_block = nullptr;
  for (auto &block : pr->blocks)
    if (block.id == start.value_or(pr->entry))
      start_block = &block;
  if (!start_block)
    return abort(fail(ErrorCode::InvalidState, "resume block missing"));
  if (args.size() != start_block->parameters.size())
    return abort(fail(ErrorCode::TypeMismatch, "input arity"));
  for (std::size_t i = 0; i < args.size(); ++i)
    if (!conforms(p, start_block->parameters[i].type, args[i]))
      return abort(fail(ErrorCode::TypeMismatch, "input type"));
  if (state_.size() != p.state_types.size())
    return abort(fail(ErrorCode::InvalidState, "state binding shape"));
  for (std::size_t i = 0; i < state_.size(); ++i)
    if (!state_[i] || !conforms(p, p.state_types[i], state_[i]->value))
      return abort(fail(ErrorCode::TypeMismatch, "state binding type"));
  std::map<std::uint32_t, Value> regs;
  std::map<std::uint32_t, const Block *> blocks;
  for (auto &b : pr->blocks)
    blocks.emplace(b.id, &b);
  auto *b = start_block;
  for (std::size_t i = 0; i < args.size(); ++i)
    regs[b->parameters[i].id] = args[i];
  auto initial = fuel.remaining;
  std::vector<TraceEvent> traces;
  std::size_t trace_bytes = 0;
  auto read = [&](Reg r) -> const Value & { return regs.at(r.id); };
  for (;;) {
    for (std::size_t instruction_index = 0; instruction_index < b->instructions.size();
         ++instruction_index) {
      const auto &i = b->instructions[instruction_index];
      OpcodeObservation observation{OpcodeObservation::Stage::Entered,
                                    id,
                                    b->id,
                                    instruction_index,
                                    call_depth,
                                    i,
                                    ctx,
                                    regs,
                                    fuel.remaining,
                                    fuel.remaining};
      if (observer_) {
        active_observation = &observation;
        observer_->record(observation);
      }
      try {
        location = "program " + std::to_string(id) + ", block " + std::to_string(b->id) +
                   ", opcode " + std::to_string(static_cast<unsigned>(i.op)) + ", " + i.source;
        auto f = fuel.consume();
        if (!f)
          return abort(f.error());
        Value out;
        auto arg = [&](std::size_t n) -> const Value & { return read(i.args[n]); };
        switch (i.op) {
        case Op::Unary:
        case Op::Compare:
        case Op::Convert: {
          auto &type = p.types[i.args[0].type];
          auto width = type.kind == TypeKind::Bits ? std::uint32_t(type.bound) : 0;
          Expected<Value> result =
              i.op == Op::Unary ? numeric::eval_unary(i.immediate, arg(0), width)
              : i.op == Op::Compare
                  ? numeric::eval_compare(i.immediate, arg(0), arg(1), width)
                  : numeric::eval_convert(i.immediate, arg(0), width,
                                          std::uint32_t(p.types[i.dest->type].bound));
          if (!result)
            return abort(result.error());
          out = std::move(result.value());
          break;
        }
        case Op::CallPure: {
          std::vector<Value> inputs;
          for (auto r : i.args)
            inputs.push_back(read(r));
          auto called = execute_at(i.immediate, ctx, inputs, txn, fuel, {}, call_depth + 1, true);
          if (!called)
            return abort(called.error());
          if (called.value().kind != SegmentResult::Kind::Returned ||
              called.value().values.size() != 1)
            return abort(
                fail(ErrorCode::InvalidState, "pure call failed: " + called.value().error));
          out = std::move(called.value().values[0]);
          break;
        }
        case Op::GetNow:
          out = Value{ctx.ready.time.value};
          break;
        case Op::Const:
          out = i.value;
          break;
        case Op::Move:
          out = arg(0);
          break;
        case Op::Binary: {
          if (i.binary == Binary::Eq) {
            out = Value{arg(0) == arg(1)};
            break;
          }
          if (i.binary == Binary::And || i.binary == Binary::Or) {
            auto x = std::get<bool>(arg(0).data), y = std::get<bool>(arg(1).data);
            out = Value{i.binary == Binary::And ? x && y : x || y};
            break;
          }
          auto x = std::get<std::uint64_t>(arg(0).data), y = std::get<std::uint64_t>(arg(1).data);
          if (i.binary == Binary::Lt) {
            out = Value{x < y};
            break;
          }
          auto w = p.types[i.args[0].type].bound;
          auto mask = w == 64 ? UINT64_MAX : (std::uint64_t{1} << w) - 1;
          std::uint64_t n = 0;
          switch (i.binary) {
          case Binary::AddWrap:
            n = (x + y) & mask;
            break;
          case Binary::SubWrap:
            n = (x - y) & mask;
            break;
          case Binary::MulWrap:
            n = (x * y) & mask;
            break;
          case Binary::AddChecked:
            if (y > mask - x)
              return abort(fail(ErrorCode::Overflow, "checked addition"));
            n = x + y;
            break;
          case Binary::DivChecked:
            if (!y)
              return abort(fail(ErrorCode::InvalidArgument, "division by zero"));
            n = x / y;
            break;
          case Binary::Lt:
            out = Value{x < y};
            break;
          default:
            return abort(fail(ErrorCode::Unsupported, "binary opcode"));
          }
          if (i.binary != Binary::Lt)
            out = Value{n};
          break;
        }
        case Op::SelectValue:
          out = arg(std::get<bool>(arg(0).data) ? 1 : 2);
          break;
        case Op::MakeRecord:
        case Op::MakeVec: {
          Value::Array a;
          for (auto r : i.args)
            a.push_back(read(r));
          out = Value{std::move(a)};
          break;
        }
        case Op::MakeVariant: {
          Value::Array a;
          for (auto r : i.args)
            a.push_back(read(r));
          out = Value{Value::Array{Value{std::uint64_t(i.immediate)}, Value{std::move(a)}}};
          break;
        }
        case Op::GetField:
          out = std::get<Value::Array>(arg(0).data).at(i.immediate);
          break;
        case Op::VariantTag:
          out = std::get<Value::Array>(arg(0).data)[0];
          break;
        case Op::VariantGet: {
          auto &a = std::get<Value::Array>(arg(0).data);
          if (std::get<std::uint64_t>(a[0].data) != (i.immediate >> 16))
            return abort(fail(ErrorCode::TypeMismatch, "variant tag mismatch"));
          out = std::get<Value::Array>(a[1].data).at(i.immediate & 65535);
          break;
        }
        case Op::VecGet:
        case Op::VecSet: {
          auto a = std::get<Value::Array>(arg(0).data);
          auto n = std::get<std::uint64_t>(arg(1).data);
          if (n >= a.size())
            return abort(fail(ErrorCode::InvalidArgument, "vector index out of range"));
          if (i.op == Op::VecGet)
            out = a[n];
          else {
            a[n] = arg(2);
            out = Value{std::move(a)};
          }
          break;
        }
        case Op::LoadState: {
          auto v = txn.read(*state_[i.immediate]);
          if (!v)
            return abort(v.error());
          out = std::move(v.value());
          break;
        }
        case Op::BufferStateWrite: {
          auto r = txn.buffer(*state_[i.immediate], arg(0));
          if (!r)
            return abort(r.error());
          break;
        }
        case Op::Check:
          if (!std::get<bool>(arg(0).data))
            return abort(fail(ErrorCode::InvalidState, i.text));
          break;
        case Op::Trace: {
          TraceEvent event;
          event.kind = i.text;
          event.ready = ctx.ready;
          event.instance = ctx.instance;
          event.connection = ctx.connection;
          if (traces.size() >= 65536 || i.text.size() > 1048576 - trace_bytes)
            return abort(fail(ErrorCode::Capacity, "trace budget"));
          trace_bytes += i.text.size();
          for (auto r : i.args) {
            // The validated layout bounds individual values. Account all trace copies too.
            std::function<std::size_t(const Value &)> size = [&](const Value &v) {
              std::size_t n = sizeof(Value);
              if (auto b = std::get_if<Bytes>(&v.data))
                n += b->size();
              if (auto a = std::get_if<Value::Array>(&v.data))
                for (auto &x : *a)
                  n += size(x);
              return n;
            };
            auto bytes = size(read(r));
            if (bytes > 1048576 - trace_bytes)
              return abort(fail(ErrorCode::Capacity, "trace value budget"));
            trace_bytes += bytes;
            event.values.push_back(read(r));
          }
          traces.push_back(std::move(event));
          break;
        }
        default:
          if (!backend_ || !services.count(i.immediate))
            return abort(fail(ErrorCode::Unsupported, "service binding unavailable"));
          {
            auto &s = *services.at(i.immediate);
            auto cost = fuel.consume(s.extra_fuel);
            if (!cost)
              return abort(cost.error());
            std::vector<Value> inputs;
            for (auto r : i.args)
              inputs.push_back(read(r));
            auto result = backend_->invoke(s, inputs, ctx, txn);
            if (!result)
              return abort(result.error());
            if (result.value().size() != s.result_types.size())
              return abort(fail(ErrorCode::TypeMismatch, "service result arity"));
            if (i.dest)
              out = std::move(result.value()[0]);
          }
          break;
        }
        if (i.dest) {
          if (!conforms(p, i.dest->type, out))
            return abort(fail(ErrorCode::TypeMismatch, "computed value type"));
          regs[i.dest->id] = std::move(out);
        }
        if (observer_) {
          observation.stage = OpcodeObservation::Stage::Completed;
          observation.fuel_after = fuel.remaining;
          observation.result = i.dest ? &regs.at(i.dest->id) : nullptr;
          observer_->record(observation);
          active_observation = nullptr;
        }
      } catch (const std::bad_alloc &) {
        return abort(fail(ErrorCode::Capacity, "interpreter allocation failure"), false);
      } catch (const std::exception &error) {
        return abort(
            fail(ErrorCode::InvalidState, std::string("interpreter invariant: ") + error.what()),
            false);
      } catch (...) {
        return abort(fail(ErrorCode::ExternalFailure, "provider threw a nonstandard exception"),
                     false);
      }
    }
    location =
        "program " + std::to_string(id) + ", block " + std::to_string(b->id) + ", terminator";
    auto f = fuel.consume();
    if (!f)
      return abort(f.error());
    auto &t = b->terminator;
    const Edge *edge = nullptr;
    switch (t.kind) {
    case TermKind::TransportReturn: {
      SegmentResult r;
      r.kind = SegmentResult::Kind::TransportReturned;
      r.values = {read(t.value)};
      r.fuel_used = initial - fuel.remaining;
      r.traces = std::move(traces);
      return r;
    }
    case TermKind::Suspend: {
      if (!backend_)
        return abort(fail(ErrorCode::Unsupported, "suspend backend unavailable"));
      auto wait = std::get<Handle>(read(t.value).data);
      std::vector<Value> live;
      for (auto reg : t.values)
        live.push_back(read(reg));
      auto prepared =
          backend_->prepare_suspend(wait, BlockId{t.yes.target}, live,
                                    TypeId{blocks.at(t.yes.target)->parameters[0].type}, ctx, txn);
      if (!prepared)
        return abort(prepared.error());
      SegmentResult r;
      r.kind = SegmentResult::Kind::Suspended;
      r.fuel_used = initial - fuel.remaining;
      r.traces = std::move(traces);
      r.suspended_wait = wait;
      r.resume_block = BlockId{t.yes.target};
      r.live_values = std::move(live);
      return r;
    }
    case TermKind::Jump:
      edge = &t.yes;
      break;
    case TermKind::Branch:
      edge = std::get<bool>(read(t.value).data) ? &t.yes : &t.no;
      break;
    case TermKind::Switch: {
      auto tag = std::get<std::uint64_t>(read(t.value).data);
      edge = &t.no;
      for (auto &c : t.cases)
        if (c.first == tag) {
          edge = &c.second;
          break;
        }
      break;
    }
    case TermKind::Return: {
      SegmentResult r;
      r.fuel_used = initial - fuel.remaining;
      r.traces = std::move(traces);
      for (auto v : t.values)
        r.values.push_back(read(v));
      return r;
    }
    case TermKind::Fail: {
      (void)txn.discard();
      SegmentResult r;
      r.kind = SegmentResult::Kind::Failed;
      r.error = t.error;
      r.fuel_used = initial - fuel.remaining;
      r.traces = std::move(traces);
      return r;
    }
    default:
      return abort(fail(ErrorCode::Unsupported, "terminator adapter unavailable"));
    }
    std::vector<Value> incoming;
    for (auto r : edge->args)
      incoming.push_back(read(r));
    b = blocks.at(edge->target);
    for (std::size_t i = 0; i < incoming.size(); ++i)
      regs[b->parameters[i].id] = std::move(incoming[i]);
  }
} catch (const std::bad_alloc &) {
  (void)txn.discard();
  return fail(ErrorCode::Capacity, "interpreter allocation failure");
} catch (const std::exception &e) {
  (void)txn.discard();
  return fail(ErrorCode::InvalidState, std::string("interpreter invariant: ") + e.what());
} catch (...) {
  (void)txn.discard();
  return fail(ErrorCode::ExternalFailure, "provider threw a nonstandard exception");
}
} // namespace leanat::exec
