#include "leanat/memory.hpp"
#include "leanat/event_txn.hpp"
#include "test_support.hpp"
using namespace leanat;

namespace {
struct LateConflict final : PreparedParticipant {
  bool &valid;
  bool &applied;
  bool &discarded;
  LateConflict(bool &v, bool &a, bool &d) : valid(v), applied(a), discarded(d) {}
  std::size_t reserved_bytes() const noexcept override { return sizeof(*this); }
  Expected<void> validate() const override {
    if (!valid) return fail(ErrorCode::InvalidState, "provider changed before commit");
    return {};
  }
  void apply() noexcept override { applied = true; }
  void discard() noexcept override { discarded = true; }
};
}

int main() {
  auto created = Memory::make(8, Bytes{0, 1, 2, 3, 4, 5, 6, 7});
  LEANAT_CHECK(created);
  auto memory = std::move(created.value());
  const auto *stable = memory.data();
  EventQueue queue(2, DomainId{7});
  ExecutionContext context;
  context.domain = DomainId{7};
  context.instance = InstanceId{1};
  context.owner = 10;
  VersionedCell state{Value{std::uint64_t{0}}, 0, 0};
  bool valid = true, applied = false, discarded = false;

  {
    EventTxn txn(SegmentBudget{}, context);
    LEANAT_CHECK(memory.write_bytes(txn, 1, Bytes{99, 98}));
    LEANAT_CHECK(txn.buffer(state, Value{std::uint64_t{42}}));
    EventDraft event;
    event.key.time = Tick{5};
    event.owner = context.owner;
    LEANAT_CHECK(txn.stage_event(queue, event));
    LEANAT_CHECK(txn.stage_participant(std::make_unique<LateConflict>(valid, applied, discarded)));
    valid = false;
    LEANAT_CHECK(!txn.commit());
    LEANAT_CHECK(memory.data()[1] == 1);
    LEANAT_CHECK(std::get<std::uint64_t>(state.value.data) == 0);
    LEANAT_CHECK(!queue.next_wakeup());
    LEANAT_CHECK(!applied);
  }
  LEANAT_CHECK(discarded && queue.occupied() == 0);

  {
    EventTxn txn(SegmentBudget{}, context);
    LEANAT_CHECK(memory.write_bytes(txn, 1, Bytes{99, 98}));
    LEANAT_CHECK(txn.buffer(state, Value{std::uint64_t{42}}));
    EventDraft event;
    event.key.time = Tick{5};
    event.owner = context.owner;
    LEANAT_CHECK(txn.stage_event(queue, event));
    LEANAT_CHECK(txn.commit());
  }
  LEANAT_CHECK(memory.data() == stable && memory.data()[1] == 99);
  LEANAT_CHECK(std::get<std::uint64_t>(state.value.data) == 42);
  LEANAT_CHECK(queue.next_wakeup() && queue.next_wakeup()->time == Tick{5});

  {
    EventTxn txn(SegmentBudget{}, context);
    LEANAT_CHECK(memory.reset(txn));
    LEANAT_CHECK(txn.commit());
  }
  LEANAT_CHECK(memory.data() == stable && memory.data()[1] == 1);
}
