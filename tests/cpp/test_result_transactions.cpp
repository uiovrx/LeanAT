#include "leanat/result_store.hpp"
#include "test_support.hpp"
using namespace leanat;
struct BudgetParticipant : PreparedParticipant {
  int &value;
  std::size_t bytes;
  bool dropped{};
  BudgetParticipant(int &v, std::size_t n) : value(v), bytes(n) {}
  std::size_t reserved_bytes() const noexcept override {
    return bytes;
  }
  Expected<void> validate() const override {
    return {};
  }
  void apply() noexcept override {
    ++value;
  }
  void discard() noexcept override {
    dropped = true;
  }
};
struct OrderedParticipant : PreparedParticipant {
  std::vector<int> &order;
  int id;
  OrderedParticipant(std::vector<int> &o, int n) : order(o), id(n) {}
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this);
  }
  Expected<void> validate() const override {
    return {};
  }
  void apply() noexcept override {}
  void discard() noexcept override {
    order.push_back(id);
  }
};
int main() {
  ExecutionContext ctx;
  ctx.ready = {Tick{9}, 3};
  ResultStore store(2, 8, 2);
  SegmentBudget budget;
  budget.bytes = 128 * 1024;
  EventTxn segment(budget, ctx);
  auto r = store.prepare_reserve(segment, {Handle{}});
  LEANAT_CHECK(r && !store.alive(r.value().reservation));
  LEANAT_CHECK(!store.prepare_read(segment, r.value().consumer));
  LEANAT_CHECK(store.prepare_publish_in(segment, r.value().reservation, Value(Bytes{1, 2}),
                                        {Handle{}, {Tick{9}, 3}, true}));
  LEANAT_CHECK(store.prepare_read(segment, r.value().consumer));
  LEANAT_CHECK(store.prepare_ready(segment, r.value().consumer).value() == ReadyKey({Tick{9}, 3}));
  auto retained = store.prepare_retain(segment, r.value().consumer, 7);
  LEANAT_CHECK(retained);
  LEANAT_CHECK(store.prepare_release(segment, r.value().consumer));
  LEANAT_CHECK(store.prepare_release_owner(segment, r.value().reservation));
  LEANAT_CHECK(segment.commit());
  LEANAT_CHECK(store.read(retained.value()));
  LEANAT_CHECK(!store.read(r.value().consumer));
  EventTxn abandon(budget, ctx);
  auto provisional = store.prepare_reserve(abandon, {Handle{}});
  LEANAT_CHECK(provisional && abandon.discard());
  auto real = store.reserve({Handle{}});
  LEANAT_CHECK(real && real.value().reservation.result != provisional.value().reservation.result);
  SegmentBudget small;
  small.bytes = 1;
  EventTxn limited(small, ctx);
  auto before = limited.remaining_bytes();
  LEANAT_CHECK(!store.prepare_retain(limited, retained.value(), 8));
  LEANAT_CHECK(limited.remaining_bytes() == before);
  LEANAT_CHECK(store.read(retained.value()));
  int count = 0;
  LEANAT_CHECK(!limited.stage_participant(std::make_unique<BudgetParticipant>(count, 2)));
  LEANAT_CHECK(count == 0 && limited.remaining_bytes() == 1);
  VersionedCell data{Value(Bytes(50000, 1))};
  EventTxn repeated({}, ctx);
  LEANAT_CHECK(repeated.buffer(data, Value(Bytes(50000, 2))));
  auto remaining = repeated.remaining_bytes();
  LEANAT_CHECK(repeated.buffer(data, Value(Bytes(50000, 3))));
  LEANAT_CHECK(repeated.remaining_bytes() == remaining);
  LEANAT_CHECK(repeated.commit());
  EventTxn rollback(budget, ctx);
  auto cp = rollback.checkpoint();
  auto first = store.prepare_retain(rollback, retained.value(), 9);
  LEANAT_CHECK(first);
  auto after = rollback.remaining_bytes();
  LEANAT_CHECK(after < budget.bytes);
  LEANAT_CHECK(rollback.rollback(std::move(cp.value())));
  LEANAT_CHECK(rollback.remaining_bytes() == budget.bytes);
  LEANAT_CHECK(!store.read(first.value()));
  std::vector<int> order;
  order.reserve(3);
  EventTxn chained({}, ctx);
  LEANAT_CHECK(chained.stage_participant(std::make_unique<OrderedParticipant>(order, 1)));
  auto checkpoint = chained.checkpoint();
  LEANAT_CHECK(chained.stage_participant(std::make_unique<OrderedParticipant>(order, 2)));
  LEANAT_CHECK(chained.stage_participant(std::make_unique<OrderedParticipant>(order, 3)));
  LEANAT_CHECK(chained.rollback(std::move(checkpoint.value())));
  LEANAT_CHECK(order == std::vector<int>({3, 2}));
  LEANAT_CHECK(chained.discard());
  LEANAT_CHECK(order == std::vector<int>({3, 2, 1}));
  EventTxn growth({}, ctx);
  auto changing = std::make_unique<BudgetParticipant>(count, 1);
  auto raw = changing.get();
  LEANAT_CHECK(growth.stage_participant(std::move(changing)));
  raw->bytes = 2;
  LEANAT_CHECK(!growth.commit() && count == 0);
  LEANAT_CHECK(growth.discard());
  EventTxn accounted({}, ctx);
  auto provider = std::make_unique<BudgetParticipant>(count, 1);
  auto current = provider.get();
  LEANAT_CHECK(accounted.stage_participant(std::move(provider)));
  auto save_budget = accounted.checkpoint();
  auto free_before = accounted.remaining_bytes();
  LEANAT_CHECK(accounted.reserve_participant_growth(*current, 128));
  current->bytes = 128;
  LEANAT_CHECK(accounted.remaining_bytes() == free_before - 127);
  current->bytes = 1; // Provider restores its own state before rolling back the segment savepoint.
  LEANAT_CHECK(accounted.rollback(std::move(save_budget.value())));
  LEANAT_CHECK(accounted.remaining_bytes() == free_before);
  LEANAT_CHECK(accounted.reserve_participant_growth(*current, 64));
  current->bytes = 64;
  LEANAT_CHECK(accounted.commit() && count == 1);
  SegmentBudget action_budget;
  action_budget.bytes = sizeof(SendIntent) + 32;
  EventTxn action_tx(action_budget, ctx);
  SendIntent oversized;
  std::string name;
  name.reserve(1024 * 1024);
  name = "x";
  oversized.payload.extensions.emplace(std::move(name), Bytes{});
  LEANAT_CHECK(bounded_payload_bytes(oversized.payload) > 1024 * 1024);
  LEANAT_CHECK(!action_tx.stage_action(std::move(oversized)));
  LEANAT_CHECK(action_tx.remaining_bytes() == action_budget.bytes);
  LEANAT_CHECK(action_tx.commit());
}
