#include "leanat/drain.hpp"
#include "test_support.hpp"
using namespace leanat;
Handle txn(unsigned n) {
  return Handle{HandleKind::Transaction, DomainId{1}, 1, n, 1, 7};
}
Handle hop(unsigned n) {
  auto h = txn(n);
  h.kind = HandleKind::Hop;
  return h;
}
void chained_rollback_identities() {
  DrainStore store(DomainId{1}, 8, 2, 1);
  LEANAT_CHECK(store.add_instance(InstanceId{1}));
  for (unsigned n = 1; n <= 2; ++n)
    LEANAT_CHECK(store.register_responsibility(
        {txn(n), InstanceId{1}, 0, {}, {{hop(n), false, false, false, true}}, false, false}));
  EventTxn segment({}, {});
  auto a = store.prepare(segment);
  LEANAT_CHECK(a);
  LEANAT_CHECK(segment.stage_participant(std::move(a.value())));
  auto checkpoint = segment.checkpoint();
  LEANAT_CHECK(checkpoint);
  auto b = store.prepare(segment);
  LEANAT_CHECK(b);
  auto discarded_receipt = b.value()->view().cancel_local(txn(1), CancelReason::User);
  auto discarded_reset = b.value()->view().reset(InstanceId{1}, ResetPolicy::AbortLocalAndDrain);
  LEANAT_CHECK(discarded_receipt && discarded_receipt.value().receipt && discarded_reset);
  LEANAT_CHECK(segment.stage_participant(std::move(b.value())));
  LEANAT_CHECK(segment.rollback(std::move(checkpoint.value())));
  auto c = store.prepare(segment);
  LEANAT_CHECK(c);
  LEANAT_CHECK(!c.value()->view().is_cancelled(txn(1)));
  LEANAT_CHECK(c.value()->view().epoch(InstanceId{1}).value() == 0);
  auto receipt = c.value()->view().cancel_local(txn(1), CancelReason::Timeout);
  auto reset = c.value()->view().reset(InstanceId{1}, ResetPolicy::AbortLocalAndDrain);
  LEANAT_CHECK(receipt && receipt.value().receipt && reset);
  LEANAT_CHECK(*receipt.value().receipt != *discarded_receipt.value().receipt);
  LEANAT_CHECK(receipt.value().receipt->generation > discarded_receipt.value().receipt->generation);
  LEANAT_CHECK(reset.value().reset_id > discarded_reset.value().reset_id);
  LEANAT_CHECK(segment.stage_participant(std::move(c.value())));
  LEANAT_CHECK(segment.commit());
  LEANAT_CHECK(!store.inspect(*discarded_receipt.value().receipt));
  LEANAT_CHECK(store.inspect(*receipt.value().receipt).value().reason == CancelReason::Timeout);
  LEANAT_CHECK(store.epoch(InstanceId{1}).value() == 1);
}

void prepared_snapshot_budget() {
  DrainStore store(DomainId{1}, 8, 64, 1);
  LEANAT_CHECK(store.add_instance(InstanceId{1}));
  for (unsigned n = 1; n <= 4; ++n)
    LEANAT_CHECK(store.register_responsibility(
        {txn(n), InstanceId{1}, 0, {}, {{hop(n), false, false, false, false}}, false, false}));
  auto probe = store.prepare();
  LEANAT_CHECK(probe);
  const auto baseline = probe.value()->reserved_bytes();
  probe.value()->discard();
  SegmentBudget budget;
  budget.bytes = 2 * baseline + 4 * sizeof(Handle) - 1;
  EventTxn segment(budget, {});
  auto a = store.prepare(segment);
  LEANAT_CHECK(a);
  LEANAT_CHECK(segment.stage_participant(std::move(a.value())));
  auto checkpoint = segment.checkpoint();
  LEANAT_CHECK(checkpoint);
  auto b = store.prepare(segment);
  LEANAT_CHECK(b);
  LEANAT_CHECK(b.value()->view().reset(InstanceId{1}, ResetPolicy::DrainThenReset));
  LEANAT_CHECK(b.value()->reserved_bytes() >= baseline + 4 * sizeof(Handle));
  auto rejected = segment.stage_participant(std::move(b.value()));
  LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::Capacity);
  LEANAT_CHECK(segment.rollback(std::move(checkpoint.value())));
  LEANAT_CHECK(segment.commit());
  LEANAT_CHECK(store.admits_root(InstanceId{1}));
  LEANAT_CHECK(store.outstanding() == 4);

  auto roomy = store.prepare();
  LEANAT_CHECK(roomy);
  Responsibility spare{txn(5), InstanceId{1}, 0, {}, {}, false, false};
  spare.hops.reserve(64);
  spare.hops.push_back({hop(5), false, false, false, false});
  const auto capacity = spare.hops.capacity();
  LEANAT_CHECK(roomy.value()->view().register_responsibility(std::move(spare)));
  LEANAT_CHECK(roomy.value()->reserved_bytes() >= baseline + capacity * sizeof(DrainHop));
  budget.bytes = baseline + capacity * sizeof(DrainHop) - 1;
  EventTxn short_segment(budget, {});
  rejected = short_segment.stage_participant(std::move(roomy.value()));
  LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::Capacity);
  LEANAT_CHECK(store.outstanding() == 4);
  LEANAT_CHECK(store.prepare());
}

int main() {
  chained_rollback_identities();
  prepared_snapshot_budget();
  {
    DrainStore store(DomainId{1}, 1, 1, 1);
    LEANAT_CHECK(store.add_instance(InstanceId{1}));
    LEANAT_CHECK(store.register_responsibility({txn(30), InstanceId{1}, 0, {}, {}, false, false}));
    LEANAT_CHECK(store.reset(InstanceId{1}, ResetPolicy::DrainThenReset));
    LEANAT_CHECK(store.cancel_local(txn(30), CancelReason::User));
    LEANAT_CHECK(store.retire_responsibility(txn(30)));
    LEANAT_CHECK(store.counter_snapshot().responsibilities == 1);
    LEANAT_CHECK(store.advance_reset(InstanceId{1}).value().state == ResetState::Applied);
    LEANAT_CHECK(store.counter_snapshot().responsibilities == 0);
    LEANAT_CHECK(store.register_responsibility({txn(31), InstanceId{1}, 1, {}, {}, false, false}));
  }
  DrainStore d(DomainId{1}, 8, 2, 1);
  LEANAT_CHECK(d.add_instance(InstanceId{1}));
  Responsibility r{txn(1), InstanceId{1}, 0, {}, {{hop(1), false, false, false, true}},
                   false,  false};
  LEANAT_CHECK(d.register_responsibility(r));
  auto c = d.cancel_local(txn(1), CancelReason::Timeout);
  LEANAT_CHECK(c && c.value().receipt);
  auto again = d.cancel_local(txn(1), CancelReason::User);
  LEANAT_CHECK(again && again.value().receipt == c.value().receipt);
  LEANAT_CHECK(d.inspect(*c.value().receipt).value().reason == CancelReason::Timeout);
  LEANAT_CHECK(d.release_receipt(*c.value().receipt));
  LEANAT_CHECK(d.outstanding() == 1);
  LEANAT_CHECK(!d.inspect(*c.value().receipt));
  auto released_repeat = d.cancel_local(txn(1), CancelReason::User);
  LEANAT_CHECK(released_repeat && !released_repeat.value().local_only &&
               !released_repeat.value().receipt);
  LEANAT_CHECK(!d.release_receipt(*c.value().receipt));
  LEANAT_CHECK(d.observe_wire(hop(1), true, false));
  LEANAT_CHECK(d.outstanding() == 1);
  LEANAT_CHECK(d.consume_terminal(hop(1)));
  LEANAT_CHECK(d.outstanding() == 0);
  Responsibility root{txn(2), InstanceId{1}, 0, {}, {}, false, false};
  LEANAT_CHECK(d.register_responsibility(root));
  auto reset = d.reset(InstanceId{1}, ResetPolicy::DrainThenReset);
  LEANAT_CHECK(reset && reset.value().state == ResetState::Draining);
  LEANAT_CHECK(!d.admits_root(InstanceId{1}));
  LEANAT_CHECK(d.reset(InstanceId{1}, ResetPolicy::DrainThenReset).value().reset_id ==
               reset.value().reset_id);
  auto unrelated = root;
  unrelated.transaction = txn(3);
  LEANAT_CHECK(!d.register_responsibility(unrelated));
  auto child = root;
  child.transaction = txn(4);
  LEANAT_CHECK(d.register_responsibility(child, txn(2)));
  LEANAT_CHECK(d.finish_local(txn(2)));
  LEANAT_CHECK(d.advance_reset(InstanceId{1}).value().state == ResetState::Draining);
  LEANAT_CHECK(d.finish_local(txn(4)));
  LEANAT_CHECK(d.advance_reset(InstanceId{1}).value().state == ResetState::Applied);
  LEANAT_CHECK(d.epoch(InstanceId{1}).value() == 1);
  LEANAT_CHECK(d.admits_root(InstanceId{1}));
  auto newroot = root;
  newroot.transaction = txn(5);
  newroot.epoch = 1;
  newroot.hops = {{hop(5), false, false, false, false}};
  LEANAT_CHECK(d.register_responsibility(newroot));
  LEANAT_CHECK(d.reset(InstanceId{1}, ResetPolicy::DrainThenReset));
  auto abort = d.reset(InstanceId{1}, ResetPolicy::AbortLocalAndDrain);
  LEANAT_CHECK(abort && abort.value().state == ResetState::Applied);
  LEANAT_CHECK(d.epoch(InstanceId{1}).value() == 2);
  LEANAT_CHECK(d.outstanding() == 1);
  LEANAT_CHECK(d.stop_report().size() == 1);
  auto prepared = d.prepare();
  LEANAT_CHECK(prepared);
  auto pending = newroot;
  pending.transaction = txn(6);
  pending.epoch = 2;
  LEANAT_CHECK(prepared.value()->view().register_responsibility(pending));
  LEANAT_CHECK(d.outstanding() == 1);
  prepared.value()->discard();
  LEANAT_CHECK(d.outstanding() == 1);
  auto committed = d.prepare();
  LEANAT_CHECK(committed);
  LEANAT_CHECK(committed.value()->view().register_responsibility(pending));
  ExecutionContext context;
  EventTxn segment({}, context);
  LEANAT_CHECK(segment.stage_participant(std::move(committed.value())));
  LEANAT_CHECK(segment.commit());
  LEANAT_CHECK(d.outstanding() == 2);
}
