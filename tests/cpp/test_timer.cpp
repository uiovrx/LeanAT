#include "leanat/runtime.hpp"
#include "leanat/timer.hpp"
#include "test_support.hpp"
using namespace leanat;
ExecutionContext context(Tick time = {}) {
  ExecutionContext c;
  c.owner = 7;
  c.epoch = 1;
  c.ready = {time, 0};
  return c;
}
void finish(EventQueue &q, const ClosedBatchSlice &b) {
  for (auto &e : b.members)
    LEANAT_CHECK(q.ack_executed(b.id, e.token,
                                e.cancelled ? ExecutionDisposition::Cancelled
                                            : ExecutionDisposition::Committed));
  LEANAT_CHECK(q.resolver_step(b.id, 1, 0).value());
  LEANAT_CHECK(q.finish_batch(b.id));
}
int main() {
  EventQueue q(2);
  Timer timer({Tick{5}, Duration{}}, 7, 1);
  EventToken old;
  {
    EventTxn tx({}, context());
    old = timer.arm(tx, q).value();
    LEANAT_CHECK(tx.commit());
  }
  {
    EventTxn tx({}, context());
    auto newer = timer.arm(tx, q);
    LEANAT_CHECK(newer);
    LEANAT_CHECK(tx.discard());
  }
  {
    EventTxn tx({}, context());
    LEANAT_CHECK(timer.active(tx).value().value() == old);
  }
  auto occupied = q.enqueue(EventDraft{EventKey{Tick{9}}, Value{}, 99, 1});
  LEANAT_CHECK(occupied);
  {
    EventTxn tx({}, context());
    LEANAT_CHECK(!timer.arm(tx, q));
    LEANAT_CHECK(timer.active(tx).value().value() == old);
    LEANAT_CHECK(tx.commit());
  }
  auto batch = q.pop_batch(Tick{5}, 8).value().value();
  {
    EventTxn tx({}, context(Tick{5}));
    LEANAT_CHECK(timer.on_timer(tx, q, old).value());
    LEANAT_CHECK(!timer.on_timer(tx, q, old).value());
    LEANAT_CHECK(tx.commit());
  }
  {
    EventTxn tx({}, context(Tick{5}));
    LEANAT_CHECK(!timer.cancel(tx, q, old).value());
  }
  finish(q, batch);
  {
    EventTxn tx({}, context(Tick{5}));
    LEANAT_CHECK(!timer.cancel(tx, q, old).value());
  }
  q.collect_stale(2);
  LEANAT_CHECK(q.enqueue(EventDraft{EventKey{Tick{10}}, Value{}, 99, 1}));
  {
    EventTxn tx({}, context(Tick{5}));
    LEANAT_CHECK(!timer.cancel(tx, q, old));
  }
  EventQueue pq(4);
  Timer periodic({{}, Duration{3}}, 7, 1);
  EventToken first;
  {
    EventTxn tx({}, context());
    first = periodic.arm(tx, pq).value();
    LEANAT_CHECK(tx.commit());
  }
  auto b = pq.pop_batch(Tick{3}, 4).value().value();
  {
    EventTxn tx({}, context(Tick{3}));
    LEANAT_CHECK(periodic.on_timer(tx, pq, first).value());
    LEANAT_CHECK(tx.commit());
  }
  finish(pq, b);
  LEANAT_CHECK(pq.next_wakeup()->time == Tick{6});
  {
    EventTxn tx({}, context(Tick{4}));
    LEANAT_CHECK(!periodic.cancel(tx, pq, first).value());
    LEANAT_CHECK(tx.commit());
  }
  auto second_batch = pq.pop_batch(Tick{6}, 4).value().value();
  {
    EventTxn tx({}, context(Tick{6}));
    LEANAT_CHECK(periodic.on_timer(tx, pq, second_batch.members[0].token).value());
    LEANAT_CHECK(tx.commit());
  }
  finish(pq, second_batch);
  {
    EventTxn tx({}, context(Tick{7}));
    LEANAT_CHECK(!periodic.cancel(tx, pq, first));
    LEANAT_CHECK(periodic.cancel_active(tx, pq).value());
    LEANAT_CHECK(tx.commit());
  }
  Interrupt irq(7, 1, InstanceId{}, PortId{2});
  EventQueue iq(4);
  EventToken clear;
  {
    EventTxn tx({}, context());
    LEANAT_CHECK(irq.set_level(tx, iq, true));
    LEANAT_CHECK(!irq.pulse(tx, iq, Duration{2}));
    LEANAT_CHECK(tx.discard());
  }
  LEANAT_CHECK(!irq.committed_level());
  {
    EventTxn tx({}, context());
    clear = irq.pulse(tx, iq, Duration{2}).value();
    LEANAT_CHECK(!irq.pulse(tx, iq, Duration{2}));
    LEANAT_CHECK(tx.discard());
  }
  LEANAT_CHECK(iq.occupied() == 0 && !irq.committed_level());
  {
    EventTxn tx({}, context());
    clear = irq.pulse(tx, iq, Duration{2}).value();
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(irq.committed_level());
  {
    EventTxn tx({}, context(Tick{1}));
    LEANAT_CHECK(irq.reset(tx, iq));
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(!irq.committed_level());
  {
    EventTxn tx({}, context(Tick{1}));
    auto newclear = irq.pulse(tx, iq, Duration{3});
    LEANAT_CHECK(newclear);
    LEANAT_CHECK(tx.commit());
  }
  auto cb = iq.pop_batch(Tick{2}, 4).value().value();
  {
    EventTxn tx({}, context(Tick{2}));
    LEANAT_CHECK(!irq.on_clear(tx, iq, clear).value());
    LEANAT_CHECK(tx.commit());
  }
  finish(iq, cb);
  LEANAT_CHECK(irq.committed_level());
  cb = iq.pop_batch(Tick{4}, 4).value().value();
  {
    EventTxn tx({}, context(Tick{4}));
    LEANAT_CHECK(irq.on_clear(tx, iq, cb.members[0].token).value());
    LEANAT_CHECK(tx.commit());
  }
  finish(iq, cb);
  LEANAT_CHECK(!irq.committed_level());
  {
    EventTxn tx({}, context(Tick{UINT64_MAX}));
    LEANAT_CHECK(!irq.pulse(tx, iq, Duration{1}));
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(!irq.committed_level());
  EventQueue full(1);
  LEANAT_CHECK(full.enqueue(EventDraft{EventKey{Tick{9}}, Value{}, 99, 1}));
  {
    EventTxn tx({}, context());
    LEANAT_CHECK(!irq.pulse(tx, full, Duration{1}));
    LEANAT_CHECK(tx.commit());
  }
  LEANAT_CHECK(!irq.committed_level());
  EventQueue oq(2);
  Timer overflow({{}, Duration{3}}, 7, 1);
  EventToken last;
  {
    EventTxn tx({}, context(Tick{UINT64_MAX - 3}));
    last = overflow.arm(tx, oq).value();
    LEANAT_CHECK(tx.commit());
  }
  auto last_batch = oq.pop_batch(Tick{UINT64_MAX}, 2).value().value();
  {
    EventTxn tx({}, context(Tick{UINT64_MAX}));
    auto result = overflow.on_timer(tx, oq, last);
    LEANAT_CHECK(!result && result.error().code == ErrorCode::Overflow);
    LEANAT_CHECK(tx.discard());
  }
  LEANAT_CHECK(oq.occupied() == 1);
  LEANAT_CHECK(oq.ack_executed(last_batch.id, last, ExecutionDisposition::Failed));
  LEANAT_CHECK(oq.resolver_step(last_batch.id, 1, 0));
  LEANAT_CHECK(oq.finish_batch(last_batch.id));
  {
    auto foreign = context();
    foreign.domain = DomainId{99};
    EventTxn tx({}, foreign);
    LEANAT_CHECK(!irq.set_level(tx, iq, true));
  }
  struct OutputHost : RuntimeHost {
    Expected<WireReturn> transport(const SendIntent &) override {
      return fail(ErrorCode::Unsupported, "no transport");
    }
    void arm(std::optional<WakePoint>) override {}
    void publish_output(PortId, const Value &) override {}
    void emit_trace(const TraceEvent &) override {}
  } host;
  RuntimeConfig config;
  config.descriptor_identity = "timer";
  Runtime runtime(config, host);
  LEANAT_CHECK(runtime.start(HostBindingManifest{DomainId{}, "timer", {}, true, true}));
  ExecutionContext rc;
  rc.kind = ContextKind::Process;
  rc.owner = 7;
  Timer resets({{}, Duration{2}}, 7, 0);
  Interrupt output(7, 0, InstanceId{}, PortId{1});
  std::vector<bool> levels;
  output.bind_output([&](PortId, bool level) { levels.push_back(level); });
  {
    EventTxn tx({}, rc);
    LEANAT_CHECK(resets.arm(tx, runtime.queue()));
    LEANAT_CHECK(output.pulse(tx, runtime.queue(), Duration{5}));
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  LEANAT_CHECK(output.committed_level());
  LEANAT_CHECK(runtime.reset(InstanceId{}, ResetPolicy::AbortLocalAndDrain));
  rc.epoch = 1;
  {
    EventTxn tx({}, rc);
    LEANAT_CHECK(resets.reset_epoch(tx, runtime.queue()));
    LEANAT_CHECK(output.reset_epoch(tx, runtime.queue()));
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  LEANAT_CHECK(!output.committed_level());
  EventToken reserved_clear;
  {
    EventTxn tx({}, rc);
    LEANAT_CHECK(resets.arm(tx, runtime.queue()));
    reserved_clear = output.pulse(tx, runtime.queue(), Duration{3}).value();
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  runtime.set_handler([&](const QueuedEvent &e, Runtime &r) -> Expected<void> {
    if (e.token == reserved_clear) {
      auto cleared = output.dispatch_clear(r.queue(), e.token);
      if (!cleared)
        return cleared.error();
      LEANAT_CHECK(cleared.value());
    }
    return {};
  });
  for (unsigned n = 0; n < 16 && runtime.next_wakeup() && runtime.next_wakeup()->time.value <= 3;
       ++n)
    LEANAT_CHECK(runtime.pump_batch(Tick{3}, 64));
  LEANAT_CHECK(!output.committed_level() &&
               levels == std::vector<bool>({true, false, true, false}));
  LEANAT_CHECK(!output.dispatch_clear(runtime.queue(), reserved_clear).value());
}
