#pragma once
#include "harness.hpp"
#include "leanat/runtime_services.hpp"
#include "profile_context.hpp"
namespace conformance::profile {
inline Json process_observation(const ProcessSnapshot &p) {
  Json token;
  if (p.suspension)
    token = Json::object({{"process", observe(p.suspension->process)},
                          {"wait", observe(p.suspension->wait)},
                          {"ordinal", p.suspension->ordinal}});
  return Json::object({{"program", observe(p.program)},
                       {"instance", observe(p.instance)},
                       {"bound", p.instance_bound},
                       {"state", observe(p.state)},
                       {"ordinal", p.suspension_ordinal},
                       {"suspension", token}});
}
class RuntimeVmSession {
  ProfileContext &context_;
  Runtime &runtime_;
  Recorder &record_;
  CoreRuntimeBackend backend_;
  exec::Interpreter vm_;
  std::uint64_t fuel_;
  std::optional<Handle> process_;
  std::optional<Error> error_;
  std::optional<exec::SegmentResult> result_;
  unsigned resumes_{};
  std::uint64_t used_{};

public:
  RuntimeVmSession(ProfileContext &c, Runtime &r, Recorder &rec, std::uint64_t fuel = 1000)
      : context_(c), runtime_(r), record_(rec), backend_(r, c.project()),
        vm_(c.validated(), c.state_refs(), &backend_), fuel_(fuel) {}
  CoreRuntimeBackend &backend() {
    return backend_;
  }
  void bind_core() {
    for (const auto &s : context_.project().services)
      if (s.op != exec::Op::ObjectCall)
        take(backend_.register_core(s));
  }
  void freeze() {
    take(backend_.freeze());
  }
  unsigned resumes() const {
    return resumes_;
  }
  std::uint64_t fuel_used() const {
    return used_;
  }
  std::optional<Handle> process() const {
    return process_;
  }
  const std::optional<Error> &last_error() const {
    return error_;
  }
  const std::optional<exec::SegmentResult> &last_result() const {
    return result_;
  }
  Expected<exec::SegmentResult> execute(std::uint32_t id, const std::vector<Value> &args,
                                        ExecutionContext ctx,
                                        std::optional<SuspensionToken> resume = {}) {
    record_.add("execution", ctx);
    record_.add("arguments", args);
    record_.add("state.before", context_.state());
    EventTxn tx({}, ctx);
    exec::FuelCounter fuel{fuel_};
    if (process_) {
      auto bound = backend_.bind_current_process(*process_);
      if (!bound)
        return bound.error();
      if (!resume) {
        auto began = runtime_.processes().begin(*process_, tx);
        if (!began)
          return began.error();
      }
    }
    auto result = resume ? vm_.resume_segment(*resume, ctx, tx, fuel)
                         : vm_.execute_segment(id, ctx, args, tx, fuel);
    used_ += fuel_ - fuel.remaining;
    record_.add("fuel", Json::object({{"initial", fuel_},
                                      {"remaining", fuel.remaining},
                                      {"consumed", fuel_ - fuel.remaining}}));
    if (!result) {
      error_ = result.error();
      record_.add("vm.error", result.error());
      take(tx.discard());
      record_.add("transaction.discarded", true);
      record_.add("state.after", context_.state());
      return result.error();
    }
    record_.add("vm.result", result.value());
    result_ = result.value();
    if (result.value().kind == exec::SegmentResult::Kind::Failed) {
      take(tx.discard());
      return fail(ErrorCode::InvalidState, result.value().error);
    }
    if (process_ && result.value().kind == exec::SegmentResult::Kind::Returned) {
      auto finished = runtime_.processes().complete(*process_, tx);
      if (!finished)
        return finished.error();
    }
    auto commit = runtime_.commit_segment(tx);
    if (!commit) {
      error_ = commit.error();
      record_.add("commit.error", commit.error());
      return commit.error();
    }
    record_.add("transaction.committed", commit.value());
    record_.add("state.after", context_.state());
    if (process_) {
      auto p = runtime_.processes().inspect(*process_);
      if (p)
        record_.add("process", process_observation(p.value()));
    }
    return result;
  }
  void schedule_process(std::uint32_t program, const std::vector<Value> &args, ReadyKey ready) {
    require(context_.program(program).context == ContextKind::Process,
            "catalog process role context");
    runtime_.set_resume_handler(
        [this, program](const SuspensionToken &token, ReadyKey ready, Runtime &) -> Expected<void> {
          ++resumes_;
          record_.add("resume", Json::object({{"process", observe(token.process)},
                                              {"wait", observe(token.wait)},
                                              {"ordinal", token.ordinal},
                                              {"ready", observe(ready)}}));
          auto ctx = context_.execution(program, ready, {}, token.process.owner);
          auto result = execute(program, {}, ctx, token);
          if (!result)
            return result.error();
          return {};
        });
    runtime_.set_handler(
        [this, program, args](const QueuedEvent &event, Runtime &) -> Expected<void> {
          record_.add("start.event", event);
          auto ctx = context_.execution(program, {event.event.key.time, event.event.key.turn}, {},
                                        event.event.owner);
          EventTxn create({}, ctx);
          auto frame = runtime_.processes().create(ProgramId{program}, ctx.owner, create);
          if (!frame)
            return frame.error();
          auto committed = create.commit();
          if (!committed)
            return committed.error();
          process_ = frame.value();
          record_.add("process.created", *process_);
          auto result = execute(program, args, ctx);
          if (!result)
            return result.error();
          return {};
        });
    auto ctx = context_.execution(program, ready, {}, 7);
    EventDraft event;
    event.key = {ready.time, ready.turn, EventStage::Internal, ctx.instance, {}, 0};
    event.owner = ctx.owner;
    record_.add("process.startToken", take(runtime_.schedule(event)));
  }
};
} // namespace conformance::profile
