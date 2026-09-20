#include "../support/event_queue_test_access.hpp"
#include "leanat/event_queue.hpp"
#include "test_support.hpp"
using namespace leanat;
static void finish(EventQueue &queue, ClosedBatchSlice batch) {
  for (const auto &member : batch.members)
    LEANAT_CHECK(queue.ack_executed(batch.id, member.token, ExecutionDisposition::Committed));
  LEANAT_CHECK(queue.resolver_step(batch.id, 1, 0));
  LEANAT_CHECK(queue.finish_batch(batch.id));
}
int main() {
  EventQueue q(4);
  EventDraft a{{Tick{5}, 0, EventStage::Output}, Value(std::uint64_t{1})};
  auto x = q.enqueue(a);
  LEANAT_CHECK(x);
  a.key.stage = EventStage::Reset;
  auto y = q.enqueue(a);
  LEANAT_CHECK(y);
  auto b = q.pop_batch(Tick{5}, 1);
  LEANAT_CHECK(b && b.value() && b.value()->members[0].token == y.value());
  auto id = b.value()->id;
  LEANAT_CHECK(!q.enqueue(a));
  a.key.turn = 1;
  auto z = q.enqueue(a);
  LEANAT_CHECK(z);
  LEANAT_CHECK(!q.resolver_step(id, 1, 2));
  LEANAT_CHECK(!q.ack_executed(id, x.value(), ExecutionDisposition::Committed));
  LEANAT_CHECK(q.ack_executed(id, y.value(), ExecutionDisposition::Committed));
  auto c = q.pop_batch(Tick{5}, 1);
  LEANAT_CHECK(c.value()->cursor == 1 && c.value()->members[0].token == x.value());
  LEANAT_CHECK(q.cancel(x.value()).value());
  LEANAT_CHECK(!q.cancel(x.value()).value());
  LEANAT_CHECK(q.ack_executed(id, x.value(), ExecutionDisposition::Cancelled));
  LEANAT_CHECK(!q.finish_batch(id));
  LEANAT_CHECK(!q.resolver_step(id, 1, 2).value());
  LEANAT_CHECK(q.next_wakeup());
  LEANAT_CHECK(q.resolver_step(id, 1, 2).value());
  LEANAT_CHECK(q.finish_batch(id));
  LEANAT_CHECK(q.occupied() == 1);
  LEANAT_CHECK(!elapsed(Tick{0}, Tick{1}));
  LEANAT_CHECK(!add_time(Tick{UINT64_MAX}, Duration{1}));
  EventQueue small(1);
  {
    auto r = small.prepare(a);
    LEANAT_CHECK(r);
    LEANAT_CHECK(!small.prepare(a));
  }
  LEANAT_CHECK(small.occupied() == 0);
  // Slot reuse must not affect queue-owned order.
  EventQueue fifo(3);
  auto p = fifo.enqueue(EventDraft{{Tick{1}, 0}, Value{}});
  auto future = fifo.enqueue(EventDraft{{Tick{2}, 0}, Value{}});
  auto first = fifo.pop_batch(Tick{1}, 1).value().value();
  LEANAT_CHECK(fifo.ack_executed(first.id, p.value(), ExecutionDisposition::Committed));
  LEANAT_CHECK(fifo.resolver_step(first.id, 1, 0));
  LEANAT_CHECK(fifo.finish_batch(first.id));
  auto newer = fifo.enqueue(EventDraft{{Tick{2}, 0}, Value{}});
  auto batch = fifo.pop_batch(Tick{2}, 2).value().value();
  LEANAT_CHECK(batch.members[0].token == future.value());
  LEANAT_CHECK(batch.members[1].token == newer.value());
  LEANAT_CHECK(!fifo.cancel(p.value()));
  using Access = testing::EventQueueTestAccess;
  // Position immediately before the true UInt64 boundary; do not replace queue logic.
  EventQueue generations(1);
  LEANAT_CHECK(Access::seed_free_generation(generations, 0, UINT64_MAX - 1));
  auto last_generation = generations.enqueue(EventDraft{{Tick{1}, 0}, Value{}});
  LEANAT_CHECK(last_generation && last_generation.value().generation == UINT64_MAX);
  auto last_batch = generations.pop_batch(Tick{1}, 1);
  LEANAT_CHECK(last_batch && last_batch.value());
  LEANAT_CHECK(!Access::seed_free_generation(generations, 0, 0));
  finish(generations, std::move(*last_batch.value()));
  auto sequence_before = Access::next_sequence(generations);
  auto retired = generations.enqueue(EventDraft{{Tick{2}, 0}, Value{}});
  LEANAT_CHECK(!retired && retired.error().code == ErrorCode::Capacity);
  LEANAT_CHECK(Access::retired(generations, 0));
  LEANAT_CHECK(Access::next_sequence(generations) == sequence_before);
  auto stale = generations.cancel(last_generation.value());
  LEANAT_CHECK(!stale && stale.error().code == ErrorCode::StaleHandle);
  LEANAT_CHECK(generations.collect_stale(1) == 0);
  LEANAT_CHECK(!generations.enqueue(EventDraft{{Tick{3}, 0}, Value{}}));
  LEANAT_CHECK(Access::retired(generations, 0));
  LEANAT_CHECK(!Access::seed_free_generation(generations, 0, 1));

  EventQueue sequences(2);
  LEANAT_CHECK(Access::seed_next_sequence(sequences, UINT64_MAX - 1));
  auto last_sequence = sequences.enqueue(EventDraft{{Tick{1}, 0}, Value{}});
  LEANAT_CHECK(last_sequence);
  LEANAT_CHECK(Access::next_sequence(sequences) == UINT64_MAX);
  auto overflow = sequences.enqueue(EventDraft{{Tick{2}, 0}, Value{}});
  LEANAT_CHECK(!overflow && overflow.error().code == ErrorCode::Overflow);
  LEANAT_CHECK(sequences.occupied() == 1 && Access::next_sequence(sequences) == UINT64_MAX);
  auto sequence_batch = sequences.pop_batch(Tick{1}, 1).value().value();
  LEANAT_CHECK(sequence_batch.members[0].event.key.sequence == UINT64_MAX - 1);
  finish(sequences, std::move(sequence_batch));
  LEANAT_CHECK(!sequences.enqueue(EventDraft{{Tick{3}, 0}, Value{}}));
  LEANAT_CHECK(!Access::seed_next_sequence(sequences, 1));

  EventQueue batches(1);
  LEANAT_CHECK(Access::seed_next_batch(batches, UINT64_MAX - 1));
  LEANAT_CHECK(batches.enqueue(EventDraft{{Tick{1}, 0}, Value{}}));
  auto final_batch = batches.pop_batch(Tick{1}, 1).value().value();
  LEANAT_CHECK(final_batch.id.value == UINT64_MAX - 1);
  finish(batches, std::move(final_batch));
  auto retained_event = batches.enqueue(EventDraft{{Tick{2}, 0}, Value{}});
  LEANAT_CHECK(retained_event);
  auto batch_overflow = batches.pop_batch(Tick{2}, 1);
  LEANAT_CHECK(!batch_overflow && batch_overflow.error().code == ErrorCode::Overflow);
  LEANAT_CHECK(Access::next_batch(batches) == UINT64_MAX && batches.occupied() == 1);
  LEANAT_CHECK(batches.can_cancel(retained_event.value()).value());
  LEANAT_CHECK(batches.next_wakeup()->time == Tick{2});

  EventQueue turns(1);
  LEANAT_CHECK(turns.enqueue(EventDraft{{Tick{9}, UINT64_MAX}, Value{}}));
  auto turn_batch = turns.pop_batch(Tick{9}, 1).value().value();
  auto turn_overflow = turns.successor(Tick{9});
  LEANAT_CHECK(!turn_overflow && turn_overflow.error().code == ErrorCode::Overflow);
  LEANAT_CHECK(turns.successor(Tick{10}).value() == ReadyKey({Tick{10}, 0}));
  finish(turns, std::move(turn_batch));
  auto time_overflow = add_time(Tick{UINT64_MAX}, Duration{1});
  LEANAT_CHECK(!time_overflow && time_overflow.error().code == ErrorCode::Overflow);
  LEANAT_CHECK(add_time(Tick{UINT64_MAX - 1}, Duration{1}).value() == Tick{UINT64_MAX});
  LEANAT_CHECK(!checked_mul(UINT64_MAX, 2));

  EventQueue values(1, {}, 1, sizeof(Value) + 8);
  auto initial_sequence = Access::next_sequence(values);
  auto too_large = values.enqueue(EventDraft{{}, Value(Bytes(9))});
  LEANAT_CHECK(!too_large && too_large.error().code == ErrorCode::Capacity);
  Value nested;
  for (std::size_t depth = 0; depth < value_max_depth; ++depth) {
    Value::Array parent;
    parent.push_back(std::move(nested));
    nested = Value(std::move(parent));
  }
  LEANAT_CHECK(bounded_value_bytes(nested) == SIZE_MAX);
  LEANAT_CHECK(!values.enqueue(EventDraft{{}, std::move(nested)}));
  LEANAT_CHECK(values.occupied() == 0 && Access::next_sequence(values) == initial_sequence);
}
