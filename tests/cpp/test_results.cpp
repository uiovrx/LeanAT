#include "leanat/result_store.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  ResultStore s(1, 3, 2);
  const auto bytes = owned_value_bytes(Value(Bytes{1, 2}));
  Value empty_array(Value::Array(100000, Value(Bytes{})));
  LEANAT_CHECK(owned_value_bytes(empty_array) == SIZE_MAX);
  Bytes reserved_bytes;
  reserved_bytes.reserve(1024 * 1024);
  Value reserved_value(std::move(reserved_bytes));
  LEANAT_CHECK(owned_value_bytes(reserved_value) >= 1024 * 1024 + sizeof(Value));
  Value::Array spare_nodes;
  spare_nodes.reserve(1024);
  Value reserved_array(std::move(spare_nodes));
  LEANAT_CHECK(owned_value_bytes(reserved_array) > 1024 * sizeof(Value));
  ResultStore bounded(1, 1, 1);
  auto tiny = bounded.reserve({Handle{}, TypeId{}, 1});
  LEANAT_CHECK(tiny);
  LEANAT_CHECK(!bounded.publish(tiny.value().reservation, empty_array, {Handle{}, {}, true}));
  Handle source{HandleKind::Task, DomainId{}, 3, 1, 7, 9};
  auto r = s.reserve({source, TypeId{1}, bytes, 9, 4});
  LEANAT_CHECK(r);
  auto a = r.value().consumer;
  LEANAT_CHECK(!s.read(a));
  auto b = s.retain(a, 5);
  LEANAT_CHECK(b);
  LEANAT_CHECK(!s.publish(r.value().reservation, Value(Bytes{1}), {source, {Tick{20}, 0}, false}));
  LEANAT_CHECK(s.publish(r.value().reservation, Value(Bytes{1, 2}), {source, {Tick{20}, 0}, true}));
  auto pin = s.pin(a);
  LEANAT_CHECK(pin);
  auto full = s.snapshot();
  LEANAT_CHECK(full.result_slots.size() == 1 && full.consumer_slots.size() == 3 &&
               full.pin_count == 1 && full.pin_limit == 2);
  LEANAT_CHECK(full.result_slots[0].identity == a.result &&
               full.result_slots[0].value == std::optional<Value>{Value(Bytes{1, 2})});
  LEANAT_CHECK(full.consumer_slots[a.consumer.slot].identity == a.consumer &&
               full.consumer_slots[a.consumer.slot].active &&
               full.consumer_slots[a.consumer.slot].result == a.result);
  auto ownership = s.inspect_ownership(r.value().reservation);
  LEANAT_CHECK(ownership && ownership.value().consumer_count == 2 &&
               ownership.value().pin_count == 1 && ownership.value().published &&
               !ownership.value().owner_released && !ownership.value().publishing);
  auto foreign_owner = r.value().reservation;
  ++foreign_owner.result.owner;
  LEANAT_CHECK(!s.inspect_ownership(foreign_owner));
  LEANAT_CHECK(!s.take(a, 1));
  LEANAT_CHECK(s.take(a, bytes));
  LEANAT_CHECK(!s.release(a));
  LEANAT_CHECK(s.read(b.value()));
  auto sub = s.subscribe(r.value().reservation, 6);
  LEANAT_CHECK(sub && sub.value().latched_ready == std::optional<ReadyKey>{{Tick{20}, 0}});
  LEANAT_CHECK(s.release(b.value()));
  LEANAT_CHECK(s.release(sub.value().consumer));
  LEANAT_CHECK(s.release_owner(r.value().reservation));
  LEANAT_CHECK(s.occupied() == 1);
  ownership = s.inspect_ownership(r.value().reservation);
  LEANAT_CHECK(ownership && ownership.value().consumer_count == 0 &&
               ownership.value().pin_count == 1 && ownership.value().owner_released);
  LEANAT_CHECK(std::get<Bytes>(pin.value().value().data) == Bytes({1, 2}));
  pin = fail(ErrorCode::Cancelled, "drop pin");
  LEANAT_CHECK(s.occupied() == 0);
  LEANAT_CHECK(!s.inspect_ownership(r.value().reservation));
  auto after = s.snapshot();
  LEANAT_CHECK(!after.result_slots[0].alive && !after.result_slots[0].value &&
               after.pin_count == 0);
  LEANAT_CHECK(full.result_slots[0].value == std::optional<Value>{Value(Bytes{1, 2})});
  auto next = s.reserve({source, TypeId{}, bytes, 9, 4});
  LEANAT_CHECK(next);
  LEANAT_CHECK(!s.read(a));
  LEANAT_CHECK(s.release(next.value().consumer, ReleasePolicy::DropOnTerminal));
  auto dropped = s.inspect_ownership(next.value().reservation);
  LEANAT_CHECK(dropped && dropped.value().consumer_count == 0 && !dropped.value().published);
  auto inactive = s.read(next.value().consumer);
  LEANAT_CHECK(!inactive && inactive.error().code == ErrorCode::AlreadyReleased);
  LEANAT_CHECK(s.publish(next.value().reservation, Value(Bytes{3}), {source, {Tick{30}, 0}, true}));
  LEANAT_CHECK(!s.read(next.value().consumer));
  LEANAT_CHECK(s.release_owner(next.value().reservation));
  LEANAT_CHECK(s.occupied() == 0);
}
