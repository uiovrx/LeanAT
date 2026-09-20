#include "leanat/process.hpp"
#include "leanat/result_store.hpp"
#include "leanat/storage_identity.hpp"
namespace leanat {
ProcessStore::ProcessStore(DomainId d, std::uint32_t s, std::size_t f, std::size_t w, std::size_t l)
    : domain_(d), store_(storage_detail::allocate_store_incarnation()), frames_limit_(f),
      waits_limit_(w), live_limit_(l) {
  (void)s;
}
Expected<ProcessStore::Frame *> ProcessStore::frame(Handle h) {
  auto i = frames_.find(h);
  if (i == frames_.end())
    return fail(ErrorCode::StaleHandle, "process handle");
  return &i->second;
}
Expected<void> ProcessStore::authorize(Handle h, const ExecutionContext &ctx) {
  auto f = frame(h);
  if (!f)
    return f.error();
  if (h.domain != ctx.domain || h.owner != ctx.owner)
    return fail(ErrorCode::WrongOwner, "process context");
  if (f.value()->info.instance_bound && f.value()->info.instance != ctx.instance)
    return fail(ErrorCode::WrongOwner, "process instance");
  f.value()->info.instance = ctx.instance;
  f.value()->info.instance_bound = true;
  return {};
}
Expected<Handle> ProcessStore::create(ProgramId p, std::uint64_t owner) {
  if (transaction_pending_)
    return fail(ErrorCode::NotReady, "process transaction pending");
  if (frames_.size() >= frames_limit_)
    return fail(ErrorCode::Capacity, "process frames full");
  if (generation_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "process generation exhausted");
  std::uint32_t slot = 0;
  for (;; ++slot) {
    bool used = false;
    for (auto &f : frames_)
      if (f.first.slot == slot)
        used = true;
    if (!used)
      break;
  }
  Handle h{HandleKind::Process, domain_, store_, slot, generation_++, owner};
  Frame f;
  f.handle = h;
  f.info.program = p;
  frames_.emplace(h, std::move(f));
  return h;
}
Expected<void> ProcessStore::begin(Handle h) {
  if (transaction_pending_)
    return fail(ErrorCode::NotReady, "process transaction pending");
  auto f = frame(h);
  if (!f)
    return f.error();
  if (f.value()->info.state != ProcessState::Runnable)
    return fail(ErrorCode::InvalidState, "process is not runnable");
  f.value()->info.state = ProcessState::Executing;
  return {};
}
Expected<SuspensionToken> ProcessStore::suspend(Handle h, const SingleWaitSpec &spec,
                                                ResumeFrame resume, ReadyKey now,
                                                std::optional<SingleWaitOutcome> latched) {
  if (transaction_pending_)
    return fail(ErrorCode::NotReady, "process transaction pending");
  auto f = frame(h);
  if (!f)
    return f.error();
  auto &v = *f.value();
  if (v.info.state != ProcessState::Executing)
    return fail(ErrorCode::InvalidState, "suspend requires executing frame");
  if (waits_.size() >= waits_limit_ || resume.live.size() > live_limit_)
    return fail(ErrorCode::Capacity, "wait or live value capacity");
  if (v.info.suspension_ordinal == UINT64_MAX || generation_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "suspension identity exhausted");
  SingleWaitSpec owned = spec;
  if (spec.kind == SingleWaitKind::After) {
    auto t = add_time(now.time, spec.after);
    if (!t)
      return t.error();
    owned.until = t.value();
  }
  if ((spec.kind == SingleWaitKind::After || spec.kind == SingleWaitKind::Until) &&
      !(now.time < owned.until))
    latched = SingleWaitOutcome{SingleWaitStatus::Ready, Value{}, now};
  // Edge waits explicitly ignore historical latch state.
  if (spec.kind == SingleWaitKind::Internal || spec.kind == SingleWaitKind::InputChanged)
    latched.reset();
  std::uint32_t slot = 0;
  for (;; ++slot) {
    bool used = false;
    for (auto &w : waits_)
      if (w.first.slot == slot)
        used = true;
    if (!used)
      break;
  }
  Handle wh{HandleKind::Wait, domain_, store_, slot, generation_++, h.owner};
  SuspensionToken token{h, wh, v.info.suspension_ordinal + 1};
  waits_.emplace(wh, Wait{wh, owned, token, std::move(latched)});
  v.resume = std::move(resume);
  v.info.suspension_ordinal = token.ordinal;
  v.info.suspension = token;
  v.info.state = waits_.at(wh).outcome ? ProcessState::ResumeQueued : ProcessState::Suspended;
  return token;
}
Expected<ResumeAction> ProcessStore::notify(const SuspensionToken &t, SingleWaitOutcome out) {
  if (transaction_pending_)
    return fail(ErrorCode::NotReady, "process transaction pending");
  auto f = frame(t.process);
  if (!f)
    return f.error();
  auto &v = *f.value();
  if (!v.info.suspension || !(*v.info.suspension == t))
    return fail(ErrorCode::StaleHandle, "stale suspension ordinal");
  if (v.info.state != ProcessState::Suspended)
    return fail(ErrorCode::Duplicate, "resume already claimed");
  auto i = waits_.find(t.wait);
  if (i == waits_.end())
    return fail(ErrorCode::StaleHandle, "wait handle");
  auto &spec = i->second.spec;
  if ((spec.kind == SingleWaitKind::After || spec.kind == SingleWaitKind::Until) &&
      out.status == SingleWaitStatus::Ready && out.source.time < spec.until)
    return fail(ErrorCode::NotReady, "timer before deadline");
  if ((spec.kind == SingleWaitKind::Internal || spec.kind == SingleWaitKind::InputChanged) &&
      out.source_sequence <= spec.sequence)
    return fail(ErrorCode::NotReady, "historical edge");
  i->second.outcome = std::move(out);
  v.info.state = ProcessState::ResumeQueued;
  return ResumeAction{t, v.resume, *i->second.outcome};
}
Expected<ResumeAction> ProcessStore::take_resume(const SuspensionToken &t) {
  if (transaction_pending_)
    return fail(ErrorCode::NotReady, "process transaction pending");
  auto f = frame(t.process);
  if (!f)
    return f.error();
  auto &v = *f.value();
  if (!v.info.suspension || !(*v.info.suspension == t))
    return fail(ErrorCode::StaleHandle, "stale suspension");
  if (v.info.state != ProcessState::ResumeQueued)
    return fail(ErrorCode::NotReady, "resume not queued");
  auto w = waits_.find(t.wait);
  if (w == waits_.end() || !w->second.outcome)
    return fail(ErrorCode::Integrity, "missing wait outcome");
  ResumeAction a{t, v.resume, *w->second.outcome};
  waits_.erase(w);
  v.resume = {};
  v.info.suspension.reset();
  v.info.state = ProcessState::Executing;
  return a;
}
Expected<SingleWaitOutcome> ProcessStore::read_wait_result(Handle h) const {
  auto i = waits_.find(h);
  if (i == waits_.end())
    return fail(ErrorCode::StaleHandle, "wait handle");
  if (!i->second.outcome)
    return fail(ErrorCode::NotReady, "wait pending");
  return *i->second.outcome;
}
Expected<void> ProcessStore::cancel(Handle h) {
  return complete(h);
}
Expected<void> ProcessStore::complete(Handle h) {
  if (transaction_pending_)
    return fail(ErrorCode::NotReady, "process transaction pending");
  auto f = frame(h);
  if (!f)
    return f.error();
  if (f.value()->info.suspension)
    waits_.erase(f.value()->info.suspension->wait);
  frames_.erase(h);
  return {};
}
Expected<ProcessSnapshot> ProcessStore::inspect(Handle h) const {
  auto i = frames_.find(h);
  if (i == frames_.end())
    return fail(ErrorCode::StaleHandle, "process handle");
  return i->second.info;
}
Expected<SuspensionToken> ProcessStore::suspension_for_wait(Handle h) const {
  auto w = waits_.find(h);
  if (w == waits_.end())
    return fail(ErrorCode::StaleHandle, "wait");
  auto f = frames_.find(w->second.token.process);
  if (f == frames_.end() || !f->second.info.suspension ||
      !(*f->second.info.suspension == w->second.token))
    return fail(ErrorCode::NotReady, "wait not suspended");
  return w->second.token;
}
Expected<SingleWaitSpec> ProcessStore::wait_spec(Handle h, EventTxn *txn) const {
  const ProcessStore *view = (txn && pending_txn_ == txn && pending_view_) ? pending_view_ : this;
  auto w = view->waits_.find(h);
  if (w == view->waits_.end())
    return fail(ErrorCode::StaleHandle, "wait");
  return w->second.spec;
}
class PreparedProcessMutation final : public PreparedParticipant {
  ProcessStore *target_;
  std::unique_ptr<ProcessStore> next_;
  ProcessStore *previous_view_;
  EventTxn *previous_txn_;
  bool active_{true};

public:
  PreparedProcessMutation(ProcessStore &t, std::unique_ptr<ProcessStore> n, EventTxn &txn)
      : target_(&t), next_(std::move(n)), previous_view_(t.pending_view_),
        previous_txn_(t.pending_txn_) {
    t.transaction_pending_ = true;
    t.pending_view_ = next_.get();
    t.pending_txn_ = &txn;
  }
  ~PreparedProcessMutation() override {
    discard();
  }
  Expected<void> validate_prepare() const override {
    if (!active_ || !target_->transaction_pending_)
      return fail(ErrorCode::InvalidState, "process preparation lost");
    return {};
  }
  std::size_t reserved_bytes() const noexcept override {
    std::size_t bytes = sizeof(*this);
    for (auto &f : next_->frames_) {
      bytes += sizeof(f);
      for (auto &v : f.second.resume.live)
        bytes += owned_value_bytes(v);
    }
    bytes += next_->waits_.size() * sizeof(ProcessStore::Wait);
    for (auto &w : next_->waits_)
      if (w.second.outcome)
        bytes += owned_value_bytes(w.second.outcome->value);
    return bytes;
  }
  Expected<void> validate() const override {
    if (!active_ || !target_->transaction_pending_)
      return fail(ErrorCode::InvalidState, "process preparation lost");
    if (target_->pending_view_ == next_.get())
      for (auto &w : next_->waits_) {
        auto f = next_->frames_.find(w.second.token.process);
        if (f == next_->frames_.end() || !f->second.info.suspension ||
            !(*f->second.info.suspension == w.second.token))
          return fail(ErrorCode::InvalidState, "registered wait lacks atomic Suspend");
      }
    return {};
  }
  void apply() noexcept override {
    target_->frames_.swap(next_->frames_);
    target_->waits_.swap(next_->waits_);
    target_->generation_ = std::max(target_->generation_, next_->generation_);
    if (target_->pending_view_ == next_.get()) {
      target_->transaction_pending_ = false;
      target_->pending_txn_ = nullptr;
      target_->pending_view_ = nullptr;
    }
    active_ = false;
  }
  void discard() noexcept override {
    if (active_) {
      target_->pending_view_ = previous_view_;
      target_->pending_txn_ = previous_txn_;
      target_->transaction_pending_ = previous_view_ != nullptr;
      active_ = false;
    }
  }
};
ProcessStore::StagingFootprint ProcessStore::staging_footprint() noexcept {
  return {sizeof(PreparedProcessMutation), sizeof(decltype(frames_)::value_type), sizeof(Wait)};
}
Expected<void> ProcessStore::stage_copy(std::unique_ptr<ProcessStore> next, EventTxn &txn) {
  generation_ = std::max(generation_, next->generation_);
  return txn.stage_participant(
      std::make_unique<PreparedProcessMutation>(*this, std::move(next), txn));
}
Expected<std::unique_ptr<ProcessStore>> ProcessStore::draft(EventTxn &txn) {
  if (transaction_pending_ && pending_txn_ != &txn)
    return fail(ErrorCode::NotReady, "another process transaction pending");
  auto next =
      std::unique_ptr<ProcessStore>(new ProcessStore(pending_view_ ? *pending_view_ : *this));
  next->transaction_pending_ = false;
  next->pending_view_ = nullptr;
  next->pending_txn_ = nullptr;
  return next;
}
Expected<Handle> ProcessStore::create(ProgramId p, std::uint64_t owner, EventTxn &txn) {
  if (txn.context().domain != domain_ || txn.context().owner != owner)
    return fail(ErrorCode::WrongOwner, "process spawn context");
  return create_controlled(p, owner, txn);
}
Expected<Handle> ProcessStore::create_controlled(ProgramId p, std::uint64_t owner, EventTxn &txn) {
  if (txn.context().domain != domain_)
    return fail(ErrorCode::WrongDomain, "controlled process spawn domain");
  if (txn.context().kind != ContextKind::Timed && txn.context().kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "controlled process spawn context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  d.value()->generation_ = generation_;
  auto h = d.value()->create(p, owner);
  generation_ = d.value()->generation_;
  if (!h)
    return h.error();
  d.value()->frame(h.value()).value()->info.instance = txn.context().instance;
  d.value()->frame(h.value()).value()->info.instance_bound = true;
  auto staged = stage_copy(std::move(d.value()), txn);
  if (!staged)
    return staged.error();
  return h;
}
Expected<SuspensionToken> ProcessStore::suspend(Handle h, const SingleWaitSpec &spec,
                                                ResumeFrame frame, ReadyKey now, EventTxn &txn,
                                                std::optional<SingleWaitOutcome> latched) {
  if (txn.context().domain != domain_ || txn.context().owner != h.owner)
    return fail(ErrorCode::WrongOwner, "process suspend context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  d.value()->generation_ = generation_;
  auto authorized = d.value()->authorize(h, txn.context());
  if (!authorized)
    return authorized.error();
  auto token = d.value()->suspend(h, spec, std::move(frame), now, std::move(latched));
  generation_ = d.value()->generation_;
  if (!token)
    return token.error();
  auto staged = stage_copy(std::move(d.value()), txn);
  if (!staged)
    return staged.error();
  return token;
}
Expected<void> ProcessStore::complete(Handle h, EventTxn &txn) {
  if (txn.context().domain != domain_ || txn.context().owner != h.owner)
    return fail(ErrorCode::WrongOwner, "process complete context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto authorized = d.value()->authorize(h, txn.context());
  if (!authorized)
    return authorized.error();
  auto completed = d.value()->complete(h);
  if (!completed)
    return completed.error();
  return stage_copy(std::move(d.value()), txn);
}
Expected<void> ProcessStore::cancel(Handle h, EventTxn &txn) {
  return complete(h, txn);
}
Expected<void> ProcessStore::cancel_controlled(Handle h, EventTxn &txn) {
  if (txn.context().domain != domain_ || h.domain != domain_)
    return fail(ErrorCode::WrongDomain, "controlled process cancel domain");
  if (txn.context().kind != ContextKind::Timed && txn.context().kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "controlled process cancel context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto authority = txn.context();
  authority.owner = h.owner;
  auto authorized = d.value()->authorize(h, authority);
  if (!authorized)
    return authorized.error();
  // Also remove a registered-but-not-yet-suspended wait belonging to this frame.
  for (auto it = d.value()->waits_.begin(); it != d.value()->waits_.end();) {
    if (it->second.token.process == h)
      it = d.value()->waits_.erase(it);
    else
      ++it;
  }
  auto completed = d.value()->complete(h);
  if (!completed)
    return completed.error();
  return stage_copy(std::move(d.value()), txn);
}
Expected<void> ProcessStore::cancel_wait_controlled(Handle wait, Handle process, EventTxn &txn) {
  if (txn.context().domain != domain_ || process.domain != domain_ || wait.domain != domain_)
    return fail(ErrorCode::WrongDomain, "controlled wait cancel domain");
  if (txn.context().kind != ContextKind::Timed && txn.context().kind != ContextKind::Process)
    return fail(ErrorCode::InvalidState, "controlled wait cancel context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto &view = *d.value();
  auto w = view.waits_.find(wait);
  if (w == view.waits_.end())
    return fail(ErrorCode::StaleHandle, "controlled wait cancel identity");
  if (w->second.token.process != process || wait.owner != process.owner)
    return fail(ErrorCode::WrongOwner, "controlled wait process binding");
  auto authority = txn.context();
  authority.owner = process.owner;
  auto authorized = view.authorize(process, authority);
  if (!authorized)
    return authorized.error();
  auto f = view.frame(process);
  const auto token = w->second.token;
  if (f.value()->info.suspension) {
    if (!(*f.value()->info.suspension == token))
      return fail(ErrorCode::StaleHandle, "controlled wait suspension binding");
    auto completed = view.complete(process);
    if (!completed)
      return completed.error();
  } else {
    if (f.value()->info.state != ProcessState::Executing)
      return fail(ErrorCode::InvalidState, "controlled wait is not registered or suspended");
    view.waits_.erase(wait);
  }
  return stage_copy(std::move(d.value()), txn);
}
Expected<void> ProcessStore::begin(Handle h, EventTxn &txn) {
  if (txn.context().domain != domain_ || txn.context().owner != h.owner)
    return fail(ErrorCode::WrongOwner, "process start context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto authorized = d.value()->authorize(h, txn.context());
  if (!authorized)
    return authorized.error();
  auto began = d.value()->begin(h);
  if (!began)
    return began.error();
  auto frame = d.value()->frame(h);
  frame.value()->info.instance = txn.context().instance;
  return stage_copy(std::move(d.value()), txn);
}
Expected<ResumeAction> ProcessStore::take_resume(const SuspensionToken &t, EventTxn &txn) {
  if (txn.context().domain != domain_ || txn.context().owner != t.process.owner)
    return fail(ErrorCode::WrongOwner, "process resume context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto authorized = d.value()->authorize(t.process, txn.context());
  if (!authorized)
    return authorized.error();
  auto resumed = d.value()->take_resume(t);
  if (!resumed)
    return resumed.error();
  auto staged = stage_copy(std::move(d.value()), txn);
  if (!staged)
    return staged.error();
  return resumed;
}
Expected<ResumeAction> ProcessStore::notify(const SuspensionToken &t, SingleWaitOutcome outcome,
                                            EventTxn &txn) {
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto authorized = d.value()->authorize(t.process, txn.context());
  if (!authorized)
    return authorized.error();
  auto action = d.value()->notify(t, std::move(outcome));
  if (!action)
    return action.error();
  auto staged = stage_copy(std::move(d.value()), txn);
  if (!staged)
    return staged.error();
  return action;
}
std::vector<SuspensionToken> ProcessStore::waiting_on(SingleWaitKind kind, Handle source) const {
  std::vector<SuspensionToken> tokens;
  for (const auto &entry : waits_) {
    const auto &wait = entry.second;
    auto frame = frames_.find(wait.token.process);
    if (wait.spec.kind == kind && wait.spec.source == source && !wait.outcome &&
        frame != frames_.end() && frame->second.info.state == ProcessState::Suspended)
      tokens.push_back(wait.token);
  }
  return tokens;
}
Expected<Handle> ProcessStore::register_wait(Handle process, const SingleWaitSpec &spec,
                                             EventTxn &txn) {
  if (txn.context().domain != domain_ || txn.context().owner != process.owner)
    return fail(ErrorCode::WrongOwner, "wait context");
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto &view = *d.value();
  auto authorized = view.authorize(process, txn.context());
  if (!authorized)
    return authorized.error();
  auto f = view.frame(process);
  if (!f)
    return f.error();
  if (f.value()->info.state != ProcessState::Executing)
    return fail(ErrorCode::InvalidState, "wait requires executing process");
  for (auto &w : view.waits_)
    if (w.second.token.process == process)
      return fail(ErrorCode::Duplicate, "process already has wait");
  if (view.waits_.size() >= waits_limit_)
    return fail(ErrorCode::Capacity, "wait capacity");
  if (generation_ == UINT64_MAX || f.value()->info.suspension_ordinal == UINT64_MAX)
    return fail(ErrorCode::Overflow, "wait identity exhausted");
  SingleWaitSpec owned = spec;
  if (spec.kind == SingleWaitKind::After) {
    auto time = add_time(txn.context().ready.time, spec.after);
    if (!time)
      return time.error();
    owned.until = time.value();
  }
  std::uint32_t slot = 0;
  for (;; ++slot) {
    bool used = false;
    for (auto &w : view.waits_)
      if (w.first.slot == slot)
        used = true;
    if (!used)
      break;
  }
  Handle h{HandleKind::Wait, domain_, store_, slot, generation_++, process.owner};
  view.generation_ = generation_;
  SuspensionToken token{process, h, f.value()->info.suspension_ordinal + 1};
  std::optional<SingleWaitOutcome> ready;
  if ((spec.kind == SingleWaitKind::After || spec.kind == SingleWaitKind::Until) &&
      !(txn.context().ready.time < owned.until))
    ready = SingleWaitOutcome{SingleWaitStatus::Ready, {}, txn.context().ready};
  view.waits_.emplace(h, Wait{h, owned, token, std::move(ready)});
  auto staged = stage_copy(std::move(d.value()), txn);
  if (!staged)
    return staged.error();
  return h;
}
Expected<SuspensionToken> ProcessStore::suspend_registered(Handle wait, ResumeFrame frame,
                                                           EventTxn &txn) {
  auto d = draft(txn);
  if (!d)
    return d.error();
  auto &view = *d.value();
  auto w = view.waits_.find(wait);
  if (w == view.waits_.end())
    return fail(ErrorCode::StaleHandle, "registered wait");
  if (wait.owner != txn.context().owner || wait.domain != txn.context().domain)
    return fail(ErrorCode::WrongOwner, "registered wait context");
  auto authorized = view.authorize(w->second.token.process, txn.context());
  if (!authorized)
    return authorized.error();
  auto f = view.frame(w->second.token.process);
  if (!f)
    return f.error();
  if (f.value()->info.state != ProcessState::Executing || f.value()->info.suspension)
    return fail(ErrorCode::InvalidState, "process already suspended");
  if (frame.live.size() > live_limit_)
    return fail(ErrorCode::Capacity, "live frame values");
  f.value()->resume = std::move(frame);
  f.value()->info.suspension = w->second.token;
  f.value()->info.suspension_ordinal = w->second.token.ordinal;
  f.value()->info.state = w->second.outcome ? ProcessState::ResumeQueued : ProcessState::Suspended;
  auto token = w->second.token;
  auto staged = stage_copy(std::move(d.value()), txn);
  if (!staged)
    return staged.error();
  return token;
}
} // namespace leanat
