#include "leanat/event_queue.hpp"
#include "leanat/result_store.hpp"
#include "test_support.hpp"
using namespace leanat;

int main() {
  EventQueue first(2, DomainId{55});
  EventQueue second(2, DomainId{55});
  EventDraft event;
  event.owner = 9;
  auto a = first.enqueue(event);
  auto b = second.enqueue(event);
  LEANAT_CHECK(a && b);
  // Independent stores must never accept one another's consumer capabilities,
  // even when the domain, owner and slot allocation order coincide.
  LEANAT_CHECK(!second.cancel(a.value()));
  LEANAT_CHECK(second.can_cancel(b.value()).value());

  ResultStore left(2, 4, 2, DomainId{55});
  ResultStore right(2, 4, 2, DomainId{55});
  ResultCreate create;
  create.producer_owner = 9;
  create.consumer_owner = 9;
  auto x = left.reserve(create);
  auto y = right.reserve(create);
  LEANAT_CHECK(x && y);
  LEANAT_CHECK(right.publish(y.value().reservation, Value{std::uint64_t{22}},
                            PublicationContext{create.source, {}, true}));
  LEANAT_CHECK(!right.read(x.value().consumer));
  LEANAT_CHECK(std::get<std::uint64_t>(right.read(y.value().consumer).value().data) == 22);
}
