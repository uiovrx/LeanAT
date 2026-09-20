#include "leanat/cancel_scope.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  DomainId d{1};
  CancelScopeStore scopes(d, 2, 12, 4, 32);
  auto root = scopes.root();
  auto forgedroot = root;
  ++forgedroot.owner;
  LEANAT_CHECK(!scopes.create(forgedroot));
  LEANAT_CHECK(!scopes.cancel(forgedroot, "forged"));
  auto a = scopes.create(root), b = scopes.create(root);
  LEANAT_CHECK(a && b);
  auto child = scopes.create(a.value());
  LEANAT_CHECK(child);
  LEANAT_CHECK(!scopes.transfer(a.value(), root, child.value()));
  Handle task{HandleKind::Task, d, 4, 0, 1, 0};
  LEANAT_CHECK(scopes.attach({task, OwnedKind::Task, false}, child.value()));
  LEANAT_CHECK(!scopes.attach({task, OwnedKind::Task, false}, b.value()));
  LEANAT_CHECK(!scopes.transfer(task, b.value(), a.value()));
  LEANAT_CHECK(scopes.transfer(task, child.value(), b.value()));
  LEANAT_CHECK(!scopes.close(b.value()));
  auto observation = scopes.observe_cancel(a.value(), b.value(), b.value());
  LEANAT_CHECK(observation);
  LEANAT_CHECK(!scopes.transfer(b.value(), root, child.value()));
  LEANAT_CHECK(!scopes.observe_cancel(a.value(), child.value(), b.value()));
  auto plan = scopes.cancel(b.value(), "timeout");
  LEANAT_CHECK(plan && plan.value().actions.size() == 1);
  LEANAT_CHECK(!scopes.close(b.value()));
  LEANAT_CHECK(!scopes.transfer(task, b.value(), root));
  auto repeat = scopes.cancel(b.value(), "other");
  LEANAT_CHECK(repeat && repeat.value().reason == "timeout" &&
               repeat.value().actions[0].id == plan.value().actions[0].id);
  int calls = 0;
  auto exec = [&](const ScopeCancelAction &) -> Expected<void> {
    ++calls;
    return {};
  };
  auto id = plan.value().actions[0].id;
  LEANAT_CHECK(scopes.apply(b.value(), id, exec));
  LEANAT_CHECK(scopes.apply(b.value(), id, exec));
  LEANAT_CHECK(calls == 1);
  LEANAT_CHECK(scopes.state(b.value()).value() == ScopeState::Cancelled);
  LEANAT_CHECK(!scopes.close(b.value()));
  LEANAT_CHECK(scopes.release_observer(observation.value()));
  LEANAT_CHECK(scopes.close(b.value()));
  LEANAT_CHECK(!scopes.state(b.value()));
  auto newb = scopes.create(root);
  LEANAT_CHECK(newb && newb.value() != b.value());
  auto parentplan = scopes.cancel(a.value(), "parent");
  LEANAT_CHECK(parentplan);
  LEANAT_CHECK(scopes.state(child.value()).value() == ScopeState::Cancelled);
  LEANAT_CHECK(!scopes.create(child.value()));
  LEANAT_CHECK(scopes.close(child.value()));
  for (auto action : parentplan.value().actions)
    LEANAT_CHECK(scopes.apply(a.value(), action.id, exec));
  LEANAT_CHECK(scopes.close(a.value()));
  CancelScopeStore limited(d, 9, 5, 1, 1);
  auto s = limited.create(limited.root());
  LEANAT_CHECK(s);
  auto full = limited.create(s.value());
  LEANAT_CHECK(full);
  LEANAT_CHECK(!limited.attach({task, OwnedKind::Task, false}, s.value()));
  LEANAT_CHECK(!limited.close(s.value()));
}
