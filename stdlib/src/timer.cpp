#include "leanat/timer.hpp"
namespace leanat {
namespace {
Value timer_state(Handle h = {}, std::uint64_t deadline = 0, bool active = false,
                  Value::Array settled = {}) {
  return Value{Value::Array{Value{h}, Value{deadline}, Value{active}, Value{std::move(settled)}}};
}
struct State {
  Handle token;
  std::uint64_t deadline;
  bool active;
  Value::Array settled;
};
Expected<State> read_state(const EventTxn &tx, const VersionedCell &c) {
  auto v = tx.read(c);
  if (!v)
    return v.error();
  auto &a = std::get<Value::Array>(v.value().data);
  return State{std::get<Handle>(a[0].data), std::get<std::uint64_t>(a[1].data),
               std::get<bool>(a[2].data), std::get<Value::Array>(a[3].data)};
}
Expected<EventToken> schedule(EventTxn &tx, EventQueue &q, Tick t, InstanceId instance,
                              std::uint64_t owner, std::uint64_t epoch, Value value) {
  auto key = q.successor(t);
  if (!key)
    return key.error();
  if (t < tx.context().ready.time)
    return fail(ErrorCode::TimeRegression, "past deadline");
  if (t == tx.context().ready.time && key.value().turn <= tx.context().ready.turn) {
    auto turn = checked_add(tx.context().ready.turn, 1);
    if (!turn)
      return turn.error();
    key.value().turn = turn.value();
  }
  return tx.stage_event(
      q, EventDraft{EventKey{t, key.value().turn, EventStage::Internal, instance, {}, 0},
                    std::move(value), owner, epoch});
}
Expected<void> allowed(const ExecutionContext &c, std::uint64_t owner, std::uint64_t epoch,
                       InstanceId instance) {
  if (c.kind != ContextKind::Timed && c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "timer requires timed or process context");
  if (c.owner != owner || c.instance != instance)
    return fail(ErrorCode::WrongOwner, "timer owner");
  if (c.epoch != epoch)
    return fail(ErrorCode::StaleHandle, "timer epoch");
  return {};
}
} // namespace
namespace {
class EpochReset final : public PreparedParticipant {
  std::uint64_t &epoch_;
  VersionedCell &state_;
  VersionedCell *output_;
  std::uint64_t old_, next_, version_, output_version_;
  Value next_state_, next_output_;

public:
  EpochReset(std::uint64_t &e, VersionedCell &s, VersionedCell *o, std::uint64_t next, bool reset)
      : epoch_(e), state_(s), output_(o), old_(e), next_(next), version_(s.version),
        output_version_(o ? o->version : 0), next_state_(timer_state()),
        next_output_(Value{reset}) {}
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this) + 4 * sizeof(Value);
  }
  Expected<void> validate() const override {
    if (epoch_ != old_ || state_.version != version_ ||
        (output_ && output_->version != output_version_))
      return fail(ErrorCode::InvalidState, "reset version conflict");
    if (next_ <= old_)
      return fail(ErrorCode::StaleHandle, "reset epoch must advance");
    if (version_ == UINT64_MAX || (output_ && output_version_ == UINT64_MAX))
      return fail(ErrorCode::Overflow, "reset version exhausted");
    return {};
  }
  void apply() noexcept override {
    state_.value.data.swap(next_state_.data);
    state_.epoch = next_;
    ++state_.version;
    if (output_) {
      output_->value.data.swap(next_output_.data);
      output_->epoch = next_;
      ++output_->version;
    }
    epoch_ = next_;
  }
  void discard() noexcept override {}
};
} // namespace
struct Interrupt::PulseCleanup {
  EventToken token;
  EventTxn transaction;
  Value inactive;
  PulseCleanup(EventToken h, ExecutionContext c)
      : token(h), transaction(SegmentBudget{2, 0, 2048, 0}, c),
        inactive(timer_state(h, c.ready.time.value, false)) {}
};
class Interrupt::PulseCommit final : public PreparedParticipant {
  Interrupt &target_;
  std::unique_ptr<PulseCleanup> replacement_;

public:
  PulseCommit(Interrupt &t, std::unique_ptr<PulseCleanup> p = {})
      : target_(t), replacement_(std::move(p)) {}
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this) + (replacement_ ? sizeof(PulseCleanup) + 2048 : 0);
  }
  Expected<void> validate() const override {
    return {};
  }
  void apply() noexcept override {
    target_.cleanup_.swap(replacement_);
    target_.publish_committed();
  }
  void discard() noexcept override {
    replacement_.reset();
  }
};
Interrupt::~Interrupt() = default;
void Interrupt::publish_committed() noexcept {
  auto level = committed_level();
  if (output_sink_ && level != last_published_) {
    try {
      output_sink_(output_, level);
    } catch (...) {
      std::terminate();
    }
  }
  last_published_ = level;
}
Expected<bool> Interrupt::dispatch_clear(EventQueue &q, EventToken token) {
  if (!cleanup_ || cleanup_->token != token)
    return false;
  auto active = q.is_active(token);
  if (!active)
    return active.error();
  if (!active.value())
    return false;
  auto &a = std::get<Value::Array>(state_.value.data);
  if (!std::get<bool>(a[2].data) || std::get<Handle>(a[0].data) != token)
    return false;
  auto cleanup = std::move(cleanup_);
  auto &tx = cleanup->transaction;
  auto r = tx.buffer(output_value_, Value{false});
  if (r)
    r = tx.buffer(state_, std::move(cleanup->inactive));
  if (!r)
    return r.error();
  auto committed = tx.commit(epoch_);
  if (!committed)
    return committed.error();
  publish_committed();
  return true;
}
Expected<void> Timer::reset_epoch(EventTxn &tx, EventQueue &q) {
  auto &c = tx.context();
  if (c.owner != owner_ || c.instance != instance_ || c.domain != domain_)
    return fail(ErrorCode::WrongOwner, "timer reset owner");
  if (c.kind != ContextKind::Timed && c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "reset context");
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto &a = std::get<Value::Array>(state_.value.data);
  auto token = std::get<Handle>(a[0].data);
  Expected<void> r;
  if (std::get<bool>(a[2].data)) {
    auto valid = q.validate_cancel(token);
    if (valid)
      r = tx.stage_cancel(q, token);
    else if (valid.error().code != ErrorCode::StaleHandle)
      return valid.error();
  }
  if (r)
    r = tx.stage_participant(std::make_unique<EpochReset>(epoch_, state_, nullptr, c.epoch, false));
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return {};
}
Expected<void> Interrupt::reset_epoch(EventTxn &tx, EventQueue &q) {
  auto &c = tx.context();
  if (c.owner != owner_ || c.instance != instance_ || c.domain != domain_)
    return fail(ErrorCode::WrongOwner, "interrupt reset owner");
  if (c.kind != ContextKind::Timed && c.kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "reset context");
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto &a = std::get<Value::Array>(state_.value.data);
  auto token = std::get<Handle>(a[0].data);
  Expected<void> r;
  if (std::get<bool>(a[2].data)) {
    auto valid = q.validate_cancel(token);
    if (valid)
      r = tx.stage_cancel(q, token);
    else if (valid.error().code != ErrorCode::StaleHandle)
      return valid.error();
  }
  if (r)
    r = tx.stage_participant(
        std::make_unique<EpochReset>(epoch_, state_, &output_value_, c.epoch, reset_value_));
  if (r)
    r = tx.stage_participant(std::make_unique<PulseCommit>(*this));
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return {};
}
Timer::Timer(TimerConfig c, std::uint64_t owner, std::uint64_t epoch, InstanceId instance,
             DomainId domain)
    : config_(c), owner_(owner), epoch_(epoch), instance_(instance), domain_(domain),
      state_{timer_state(), 0, epoch} {
  if (bool(c.deadline) == bool(c.period.value))
    throw std::invalid_argument("exactly one timer mode required");
}
Expected<void> Timer::context(const ExecutionContext &c) const {
  if (c.domain != domain_)
    return fail(ErrorCode::WrongDomain, "timer domain");
  return allowed(c, owner_, epoch_, instance_);
}
Expected<std::optional<EventToken>> Timer::active(const EventTxn &tx) const {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  return s.value().active ? std::optional<EventToken>{s.value().token}
                          : std::optional<EventToken>{};
}
Expected<EventToken> Timer::arm(EventTxn &tx, EventQueue &q) {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  Expected<Tick> time = config_.deadline ? Expected<Tick>{*config_.deadline}
                                         : add_time(tx.context().ready.time, config_.period);
  if (!time)
    return time.error();
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto token = schedule(tx, q, time.value(), instance_, owner_, epoch_, Value{std::uint64_t{41}});
  if (!token) {
    tx.rollback(std::move(save.value()));
    return token.error();
  }
  auto w = tx.buffer(state_, timer_state(token.value(), time.value().value, true));
  if (w && s.value().active)
    w = tx.stage_cancel(q, s.value().token);
  if (!w) {
    tx.rollback(std::move(save.value()));
    return w.error();
  }
  return token.value();
}
Expected<bool> Timer::cancel(EventTxn &tx, EventQueue &q, EventToken token) {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  if (token != s.value().token) {
    for (const auto &v : s.value().settled)
      if (std::get<Handle>(v.data) == token) {
        auto valid = q.validate_cancel(token);
        if (!valid)
          return valid.error();
        return false;
      }
    return fail(ErrorCode::StaleHandle, "token is not current timer generation");
  }
  if (!s.value().active) {
    auto valid = q.validate_cancel(token);
    if (!valid)
      return valid.error();
    return false;
  }
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto r = tx.stage_cancel(q, token);
  if (r)
    r = tx.buffer(state_, timer_state(token, s.value().deadline, false, s.value().settled));
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return true;
}
Expected<bool> Timer::cancel_active(EventTxn &tx, EventQueue &q) {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  if (!s.value().active)
    return false;
  return cancel(tx, q, s.value().token);
}
Expected<bool> Timer::on_timer(EventTxn &tx, EventQueue &q, EventToken token) {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  if (token != s.value().token || !s.value().active)
    return false;
  auto running = q.is_active(token);
  if (!running)
    return running.error();
  if (!running.value())
    return fail(ErrorCode::InvalidState, "timer event not active");
  if (tx.context().ready.time < Tick{s.value().deadline})
    return fail(ErrorCode::NotReady, "timer not due");
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  Handle next = token;
  auto deadline = s.value().deadline;
  Expected<void> r;
  if (config_.period.value) {
    auto t = add_time(Tick{deadline}, config_.period);
    if (!t)
      return t.error();
    auto n = schedule(tx, q, t.value(), instance_, owner_, epoch_, Value{std::uint64_t{41}});
    if (!n)
      return n.error();
    next = n.value();
    deadline = t.value().value;
  }
  Value::Array settled;
  for (const auto &v : s.value().settled)
    if (q.validate_cancel(std::get<Handle>(v.data)))
      settled.push_back(v);
  if (config_.period.value)
    settled.push_back(Value{token});
  r = tx.buffer(state_, timer_state(next, deadline, config_.period.value != 0, std::move(settled)));
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return true;
}
Expected<void> Timer::reset(EventTxn &tx, EventQueue &q) {
  auto r = cancel_active(tx, q);
  if (!r)
    return r.error();
  return {};
}
Interrupt::Interrupt(std::uint64_t owner, std::uint64_t epoch, InstanceId instance, PortId output,
                     bool reset, DomainId domain)
    : owner_(owner), epoch_(epoch), instance_(instance), domain_(domain), output_(output),
      reset_value_(reset), state_{timer_state(), 0, epoch}, output_value_{Value{reset}, 0, epoch},
      last_published_(reset) {}
Expected<void> Interrupt::context(const ExecutionContext &c) const {
  if (c.domain != domain_)
    return fail(ErrorCode::WrongDomain, "timer domain");
  return allowed(c, owner_, epoch_, instance_);
}
Expected<bool> Interrupt::shadow_level(const EventTxn &tx) const {
  auto v = tx.read(output_value_);
  if (!v)
    return v.error();
  return std::get<bool>(v.value().data);
}
Expected<void> Interrupt::set_level(EventTxn &tx, EventQueue &q, bool level) {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto r = tx.buffer(output_value_, Value{level});
  if (r)
    r = tx.buffer(state_, timer_state(s.value().token, s.value().deadline, false));
  if (r && s.value().active)
    r = tx.stage_cancel(q, s.value().token);
  if (r)
    r = tx.stage_participant(std::make_unique<PulseCommit>(*this));
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return {};
}
Expected<EventToken> Interrupt::pulse(EventTxn &tx, EventQueue &q, Duration d) {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  if (!d.value)
    return fail(ErrorCode::InvalidArgument, "zero pulse width");
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  auto level = shadow_level(tx);
  if (!level)
    return level.error();
  if (s.value().active || level.value())
    return fail(ErrorCode::InvalidState, "pulse busy");
  auto time = add_time(tx.context().ready.time, d);
  if (!time)
    return time.error();
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto token =
      schedule(tx, q, time.value(), instance_, owner_, epoch_,
               Value{output_.value == 0 ? std::uint64_t{0} : std::uint64_t{output_.value}});
  if (!token)
    return token.error();
  auto r = tx.buffer(output_value_, Value{true});
  if (r)
    r = tx.buffer(state_, timer_state(token.value(), time.value().value, true));
  if (r) {
    auto clear_context = tx.context();
    clear_context.ready = {time.value(), 0};
    r = tx.stage_participant(std::make_unique<PulseCommit>(
        *this, std::make_unique<PulseCleanup>(token.value(), clear_context)));
  }
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return token.value();
}
Expected<bool> Interrupt::on_clear(EventTxn &tx, EventQueue &q, EventToken token) {
  auto c = context(tx.context());
  if (!c)
    return c.error();
  auto s = read_state(tx, state_);
  if (!s)
    return s.error();
  if (!s.value().active || token != s.value().token)
    return false;
  auto running = q.is_active(token);
  if (!running)
    return running.error();
  if (!running.value())
    return fail(ErrorCode::InvalidState, "clear event not active");
  if (tx.context().ready.time < Tick{s.value().deadline})
    return fail(ErrorCode::NotReady, "pulse clear not due");
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto r = tx.buffer(output_value_, Value{false});
  if (r)
    r = tx.buffer(state_, timer_state(token, s.value().deadline, false));
  if (r)
    r = tx.stage_participant(std::make_unique<PulseCommit>(*this));
  if (!r) {
    tx.rollback(std::move(save.value()));
    return r.error();
  }
  return true;
}
Expected<void> Interrupt::reset(EventTxn &tx, EventQueue &q) {
  return set_level(tx, q, reset_value_);
}
} // namespace leanat
