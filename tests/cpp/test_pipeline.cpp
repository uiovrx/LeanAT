#include "leanat/pipeline.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  EventQueue queue(8);
  auto m = Pipeline::make(Duration{10}, Duration{2}, 2, queue, 8);
  LEANAT_CHECK(m);
  auto p = std::move(m.value());
  Handle owner{HandleKind::Transaction, DomainId{}, 1, 0, 1, 9};
  ExecutionContext c;
  c.owner = 9;
  EventTxn t({}, c);
  std::vector<PipelineTicket> tickets;
  for (auto start : {0, 2, 10, 12}) {
    auto ticket = p.submit(t, owner, Tick{});
    LEANAT_CHECK(ticket && ticket.value().grant.start.value == std::uint64_t(start));
    tickets.push_back(ticket.value());
  }
  LEANAT_CHECK(t.commit());
  LEANAT_CHECK(queue.occupied() == 4);
  c.ready.time = Tick{10};
  EventTxn ready({}, c);
  LEANAT_CHECK(!p.on_ready(ready, tickets[0], tickets[0].ready_event));
  auto ready_batch = queue.pop_batch(Tick{10}, 8);
  LEANAT_CHECK(ready_batch && ready_batch.value());
  LEANAT_CHECK(p.on_ready(ready, tickets[0], tickets[0].ready_event).value());
  LEANAT_CHECK(!p.on_ready(ready, tickets[0], tickets[0].ready_event).value());
  LEANAT_CHECK(ready.commit());
  EventQueue full(1);
  auto small = Pipeline::make(Duration{}, Duration{}, 1, full, 3);
  c.ready.time = Tick{};
  EventTxn first({}, c);
  auto one = small.value().submit(first, owner, Tick{});
  LEANAT_CHECK(one && first.commit());
  EventTxn fail({}, c);
  LEANAT_CHECK(!small.value().submit(fail, owner, Tick{}));
  LEANAT_CHECK(small.value().inspect(fail).value().reservations.size() == 1);
  LEANAT_CHECK(fail.commit());
  auto batch = full.pop_batch(Tick{}, 10);
  LEANAT_CHECK(batch && batch.value()->ready.turn == 1);
  EventQueue cancelled_queue(2);
  auto cancellable = Pipeline::make(Duration{5}, Duration{}, 1, cancelled_queue, 2);
  EventTxn pending({}, c);
  auto pending_ticket = cancellable.value().submit(pending, owner, Tick{});
  LEANAT_CHECK(pending_ticket);
  LEANAT_CHECK(cancellable.value().cancel_pending(pending, pending_ticket.value()).value() ==
               ResourceCancelDisposition::CancelledUnpublished);
  LEANAT_CHECK(cancellable.value().inspect(pending).value().reservations.empty());
  LEANAT_CHECK(pending.commit());
  EventQueue reset_queue(2);
  auto reset_pipeline = Pipeline::make(Duration{5}, Duration{}, 1, reset_queue, 2);
  EventTxn before_reset({}, c);
  auto old = reset_pipeline.value().submit(before_reset, owner, Tick{});
  LEANAT_CHECK(old && before_reset.commit());
  c.ready.time = Tick{5};
  c.epoch = 1;
  LEANAT_CHECK(reset_queue.pop_batch(Tick{5}, 2));
  EventTxn cleanup({}, c);
  auto business = reset_pipeline.value().on_ready(cleanup, old.value(), old.value().ready_event);
  LEANAT_CHECK(business && !business.value());
  LEANAT_CHECK(reset_pipeline.value().inspect(cleanup).value().reservations.empty());
  LEANAT_CHECK(cleanup.commit(1));
}
