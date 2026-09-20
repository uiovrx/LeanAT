#include "leanat/resource.hpp"
#include "test_support.hpp"
#include <type_traits>
using namespace leanat;
static_assert(!std::is_copy_constructible<Resource>::value, "resource capabilities must not clone");
int main() {
  auto m = Resource::pipeline(Duration{10}, Duration{2}, 2, 8);
  LEANAT_CHECK(m);
  auto r = std::move(m.value());
  ExecutionContext c;
  c.owner = 7;
  Handle owner{HandleKind::Transaction, DomainId{}, 1, 0, 1, 7};
  EventTxn t({}, c);
  std::vector<Grant> grants;
  for (auto expected : {0, 2, 10, 12}) {
    auto g = r.reserve_default(t, owner, Tick{});
    LEANAT_CHECK(g && g.value().start.value == std::uint64_t(expected));
    grants.push_back(g.value());
  }
  LEANAT_CHECK(t.commit());
  c.ready.time = Tick{3};
  EventTxn cancel({}, c);
  LEANAT_CHECK(r.cancel_pending(cancel, grants[0].ticket).value() ==
               ResourceCancelDisposition::CancellationDeferred);
  LEANAT_CHECK(cancel.commit());
  c.ready.time = Tick{10};
  EventTxn complete({}, c);
  LEANAT_CHECK(!r.complete(complete, grants[0].ticket, Tick{9}));
  LEANAT_CHECK(!r.complete(complete, grants[0].ticket, Tick{10}).value());
  LEANAT_CHECK(!r.complete(complete, grants[0].ticket, Tick{10}).value());
  LEANAT_CHECK(complete.commit());
  EventTxn inspect({}, c);
  auto snap = r.inspect(inspect);
  LEANAT_CHECK(snap && snap.value().reservations.size() == 3 &&
               snap.value().reservations[0].start.value == 2);
  auto zero = Resource::pipeline(Duration{}, Duration{}, 1, 2);
  c.ready.time = Tick{};
  EventTxn z({}, c);
  LEANAT_CHECK(zero.value().reserve_default(z, owner, Tick{}));
  LEANAT_CHECK(zero.value().reserve_default(z, owner, Tick{}));
  LEANAT_CHECK(!zero.value().reserve_default(z, owner, Tick{}));
  LEANAT_CHECK(z.discard());
  EventTxn overflow({}, c);
  LEANAT_CHECK(!zero.value().reserve(overflow, owner, Tick{UINT64_MAX}, Duration{1}));
  auto serial = Resource::serial(Duration{5}, 8);
  EventTxn st({}, c);
  LEANAT_CHECK(serial.value().reserve_default(st, owner, Tick{10}).value().start.value == 10);
  LEANAT_CHECK(serial.value().reserve_default(st, owner, Tick{11}).value().start.value == 15);
  auto another = Resource::serial(Duration{5}, 8);
  auto other = another.value().reserve_default(st, owner, Tick{10});
  LEANAT_CHECK(other);
  LEANAT_CHECK(!r.cancel_pending(st, other.value().ticket));
  ExecutionContext intruder = c;
  intruder.owner = 99;
  EventTxn wrong({}, intruder);
  LEANAT_CHECK(!another.value().cancel_pending(wrong, other.value().ticket));
  LEANAT_CHECK(!another.value().complete(st, other.value().ticket, Tick{15}));
}
