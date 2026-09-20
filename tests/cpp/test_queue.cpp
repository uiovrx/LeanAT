#include "leanat/queue.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  BoundedQueue q(2);
  ExecutionContext c;
  EventTxn t({}, c);
  LEANAT_CHECK(q.try_push(t, Value(std::uint64_t(1))).value());
  LEANAT_CHECK(q.try_push(t, Value(std::uint64_t(2))).value());
  LEANAT_CHECK(!q.try_push(t, Value(std::uint64_t(3))).value());
  LEANAT_CHECK(q.size(t).value() == 2);
  LEANAT_CHECK(std::get<std::uint64_t>(q.try_pop(t).value()->value.data) == 1);
  LEANAT_CHECK(t.discard());
  EventTxn empty({}, c);
  LEANAT_CHECK(q.size(empty).value() == 0);
  Handle scope{HandleKind::Scope, DomainId{}, 1, 0, 1, 1},
      drain{HandleKind::Drain, DomainId{}, 1, 0, 1, 2};
  q.set_owner_validator([](Handle h) -> Expected<void> {
    if (!h.generation)
      return fail(ErrorCode::StaleHandle, "stale");
    return {};
  });
  EventTxn push({}, c);
  LEANAT_CHECK(q.try_push(push, Value(std::uint64_t(1)), scope).value());
  LEANAT_CHECK(q.try_push(push, Value(std::uint64_t(2)), scope).value());
  LEANAT_CHECK(q.mark_published(push, 2));
  LEANAT_CHECK(push.commit());
  EventTxn clean({}, c);
  LEANAT_CHECK(!q.transfer_drain_owned(clean, scope, drain, 0));
  LEANAT_CHECK(q.remove_unpublished_owned(clean, scope).value().entry_ids.size() == 1);
  LEANAT_CHECK(q.transfer_drain_owned(clean, scope, drain, 1).value().entry_ids.size() == 1);
  LEANAT_CHECK(q.size(clean).value() == 1);
  auto pop = q.try_pop(clean);
  LEANAT_CHECK(pop && pop.value()->owner == drain);
  LEANAT_CHECK(clean.commit());
  ResultStore results(4, 16, 4);
  c.owner = 12;
  Handle source{HandleKind::Task, DomainId{}, 1, 1, 1, 12};
  auto result = results.reserve({source, TypeId{1}, 256, 12, 12});
  LEANAT_CHECK(result);
  LEANAT_CHECK(
      results.publish(result.value().reservation, Value(std::uint64_t(42)), {source, {}, true}));
  BoundedQueue owned(1), other(1);
  owned.set_consumer_store(results, 99);
  other.set_consumer_store(results, 100);
  EventTxn transfer({}, c);
  LEANAT_CHECK(owned.try_push_owned(transfer, result.value().consumer).value());
  LEANAT_CHECK(!other.try_push_owned(transfer, result.value().consumer));
  LEANAT_CHECK(other.size(transfer).value() == 0);
  LEANAT_CHECK(transfer.discard());
  LEANAT_CHECK(results.read(result.value().consumer));
  EventTxn committed({}, c);
  LEANAT_CHECK(owned.try_push_owned(committed, result.value().consumer).value());
  LEANAT_CHECK(committed.commit());
  LEANAT_CHECK(!results.read(result.value().consumer));
  c.owner = 123;
  EventTxn take({}, c);
  auto item = owned.try_pop(take);
  LEANAT_CHECK(item && item.value()->consumer);
  LEANAT_CHECK(take.commit());
  LEANAT_CHECK(results.read(*item.value()->consumer));
  LEANAT_CHECK(results.release(*item.value()->consumer));
}
