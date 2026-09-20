#include "leanat/process.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  ProcessStore store(DomainId{1}, 1, 1, 1, 4);
  auto h = store.create(ProgramId{1}, 7);
  LEANAT_CHECK(h);
  LEANAT_CHECK(!store.create(ProgramId{1}, 7));
  LEANAT_CHECK(store.begin(h.value()));
  SingleWaitSpec spec;
  spec.until = Tick{2};
  ResumeFrame frame{BlockId{3}, {Value{std::uint64_t{9}}, Value{Bytes{1, 2}}}};
  auto a = store.suspend(h.value(), spec, frame, ReadyKey{Tick{1}, 0});
  LEANAT_CHECK(a);
  LEANAT_CHECK(!store.notify(a.value(), {SingleWaitStatus::Ready, {}, ReadyKey{Tick{1}, 0}}));
  LEANAT_CHECK(store.notify(
      a.value(), {SingleWaitStatus::Ready, Value{std::uint64_t{5}}, ReadyKey{Tick{2}, 0}}));
  LEANAT_CHECK(!store.notify(a.value(), {}));
  auto resumed = store.take_resume(a.value());
  LEANAT_CHECK(resumed && resumed.value().frame.live == frame.live);
  LEANAT_CHECK(!store.read_wait_result(a.value().wait));
  auto b = store.suspend(h.value(), spec, frame, ReadyKey{Tick{1}, 0});
  LEANAT_CHECK(b);
  LEANAT_CHECK(!store.notify(a.value(), {}));
  LEANAT_CHECK(store.inspect(h.value()).value().state == ProcessState::Suspended);
  LEANAT_CHECK(store.cancel(h.value()));
  auto reused = store.create(ProgramId{1}, 7);
  LEANAT_CHECK(reused && reused.value().slot == h.value().slot &&
               reused.value().generation != h.value().generation);
  LEANAT_CHECK(!store.notify(b.value(), {}));
  LEANAT_CHECK(store.begin(reused.value()));
  ExecutionContext ctx;
  ctx.domain = DomainId{1};
  ctx.owner = 7;
  ctx.epoch = 0;
  ProcessStore rollback_store(DomainId{1}, 8, 1, 1, 1);
  Handle escaped;
  {
    EventTxn txn({}, ctx);
    auto prepared = rollback_store.create(ProgramId{1}, 7, txn);
    LEANAT_CHECK(prepared);
    escaped = prepared.value();
    LEANAT_CHECK(txn.discard());
  }
  auto fresh = rollback_store.create(ProgramId{1}, 7);
  LEANAT_CHECK(fresh && fresh.value() != escaped && !rollback_store.inspect(escaped));
  {
    EventTxn txn({}, ctx);
    auto t = store.suspend(reused.value(), spec, frame, ReadyKey{Tick{1}, 0}, txn);
    LEANAT_CHECK(t);
    LEANAT_CHECK(store.inspect(reused.value()).value().state == ProcessState::Executing);
    LEANAT_CHECK(txn.discard());
    LEANAT_CHECK(store.wait_count() == 0);
  }
  {
    EventTxn txn({}, ctx);
    auto t = store.suspend(reused.value(), spec, frame, ReadyKey{Tick{3}, 0}, txn);
    LEANAT_CHECK(t);
    LEANAT_CHECK(txn.commit());
    LEANAT_CHECK(store.inspect(reused.value()).value().state == ProcessState::ResumeQueued);
    LEANAT_CHECK(store.take_resume(t.value()));
  }
  SingleWaitSpec edge;
  edge.kind = SingleWaitKind::Internal;
  edge.sequence = 8;
  auto t = store.suspend(reused.value(), edge, {}, ReadyKey{}, SingleWaitOutcome{});
  LEANAT_CHECK(t);
  LEANAT_CHECK(store.inspect(reused.value()).value().state == ProcessState::Suspended);
  LEANAT_CHECK(!store.notify(t.value(), {SingleWaitStatus::Ready, {}, ReadyKey{}, 8}));
  LEANAT_CHECK(store.notify(t.value(), {SingleWaitStatus::Ready, {}, ReadyKey{}, 9}));
}
