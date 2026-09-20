#include "leanat/process.hpp"
#include "leanat/task_pool.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  DomainId d{1};
  Handle scope{HandleKind::Scope, d, 1, 0, 1, 0}, waiter{HandleKind::Process, d, 1, 0, 1, 0};
  ResultStore results(8, 32, 8, d);
  TaskPoolDesc desc;
  desc.max_instances = 1;
  desc.task_capacity = 4;
  desc.result_capacity = 2;
  desc.overflow = TaskOverflow::AwaitSlot;
  desc.waiter_limit = 4;
  TaskPool pool(d, 3, desc, results);
  auto first = pool.try_spawn({}, scope);
  LEANAT_CHECK(first);
  auto wrong_kind = waiter;
  wrong_kind.kind = HandleKind::Event;
  LEANAT_CHECK(!pool.request_slot(wrong_kind, scope));
  auto forged = first.value();
  ++forged.owner;
  LEANAT_CHECK(!pool.cancel(forged, "forged", {}));
  auto consumer = pool.result_handle(first.value(), scope);
  LEANAT_CHECK(consumer);
  LEANAT_CHECK(!pool.try_spawn({}, scope));
  LEANAT_CHECK(!pool.request_slot(waiter, scope, ContextKind::Timed));
  auto ticket = pool.request_slot(waiter, scope);
  LEANAT_CHECK(ticket &&
               pool.ticket_info(ticket.value()).value().state == SpawnTicketState::Pending);
  LEANAT_CHECK(pool.complete(first.value(), {TaskOutcome::Kind::Success, Value{std::uint64_t{7}}},
                             {Tick{3}, 1}));
  LEANAT_CHECK(pool.active_frames() == 0 && pool.granted_reservations() == 1);
  auto forgedticket = ticket.value();
  ++forgedticket.owner;
  LEANAT_CHECK(!pool.cancel_ticket(forgedticket, waiter, scope));
  auto wrong = waiter;
  wrong.slot = 1;
  LEANAT_CHECK(!pool.spawn_reserved(ticket.value(), wrong, scope, {}));
  LEANAT_CHECK(pool.granted_reservations() == 1);
  auto second = pool.spawn_reserved(ticket.value(), waiter, scope, {});
  LEANAT_CHECK(second);
  LEANAT_CHECK(!pool.spawn_reserved(ticket.value(), waiter, scope, {}));
  LEANAT_CHECK(pool.release_ticket(ticket.value()));
  auto retained = results.retain(consumer.value(), 99);
  LEANAT_CHECK(retained);
  {
    auto pin = pool.pin_result(first.value(), consumer.value());
    LEANAT_CHECK(pin);
    LEANAT_CHECK(pool.release_result(first.value(), consumer.value()));
    LEANAT_CHECK(results.read(retained.value()));
    LEANAT_CHECK(results.release(retained.value()));
    LEANAT_CHECK(pool.state(first.value()));
  }
  auto next = pool.request_slot(waiter, scope);
  LEANAT_CHECK(next);
  LEANAT_CHECK(
      pool.complete(second.value(), {TaskOutcome::Kind::Error, Value{Bytes{1}}}, {Tick{4}, 0}));
  LEANAT_CHECK(pool.ticket_info(next.value()).value().state == SpawnTicketState::Granted);
  LEANAT_CHECK(pool.cancel_ticket(next.value(), waiter, scope));
  LEANAT_CHECK(pool.cancel_ticket(next.value(), waiter, scope));
  LEANAT_CHECK(pool.granted_reservations() == 0);
  LEANAT_CHECK(pool.release_ticket(next.value()));
  auto failed_ticket = pool.request_slot(waiter, scope);
  LEANAT_CHECK(failed_ticket);
  std::vector<Value> huge{Value{Bytes(5000)}};
  LEANAT_CHECK(!pool.spawn_reserved(failed_ticket.value(), waiter, scope, std::move(huge)));
  LEANAT_CHECK(pool.ticket_info(failed_ticket.value()).value().state == SpawnTicketState::Failed);
  LEANAT_CHECK(pool.granted_reservations() == 0);
  LEANAT_CHECK(pool.release_ticket(failed_ticket.value()));
  auto c2 = pool.result_handle(second.value(), scope);
  LEANAT_CHECK(pool.consume_result(second.value(), c2.value()));
  LEANAT_CHECK(!pool.release_result(second.value(), c2.value()));
  LEANAT_CHECK(!pool.try_spawn({}, scope, ContextKind::Transport));
  ResultStore qr(8, 16, 8, d);
  desc.overflow = TaskOverflow::Queue;
  desc.queue_depth = 1;
  desc.result_capacity = 3;
  TaskPool queue(d, 4, desc, qr);
  auto running = queue.submit_queued({}, scope),
       queued = queue.submit_queued({Value{Bytes{4}}}, scope);
  LEANAT_CHECK(running && queued);
  LEANAT_CHECK(queue.state(queued.value()).value() == TaskState::Queued);
  LEANAT_CHECK(!queue.submit_queued({}, scope));
  LEANAT_CHECK(queue.cancel(queued.value(), "cancel queued", {}));
  LEANAT_CHECK(queue.state(queued.value()).value() == TaskState::Cancelled);
  LEANAT_CHECK(queue.cancel(queued.value(), "again", {}));
  auto qc = queue.result_handle(queued.value(), scope);
  LEANAT_CHECK(queue.consume_result(queued.value(), qc.value()));
  LEANAT_CHECK(queue.active_frames() == 1);
  // A shared result store must actually reserve a result before granting.
  ResultStore tiny(1, 4, 2, d);
  TaskPool limited(d, 8, desc, tiny);
  auto only = limited.try_spawn({}, scope);
  LEANAT_CHECK(only);
  LEANAT_CHECK(limited.complete(only.value(), {TaskOutcome::Kind::Success, Value{}}, {}));
  LEANAT_CHECK(!limited.try_spawn({}, scope));
  CancelScopeStore scopes(d, 12, 4, 8, 16);
  auto owned_scope = scopes.create(scopes.root());
  LEANAT_CHECK(owned_scope);
  ResultStore scoped_results(4, 8, 4, d);
  desc.overflow = TaskOverflow::AwaitSlot;
  desc.waiter_limit = 2;
  TaskPool scoped_pool(d, 12, desc, scoped_results, &scopes);
  auto scoped_task = scoped_pool.try_spawn({}, owned_scope.value());
  LEANAT_CHECK(scoped_task);
  auto plan = scopes.cancel(owned_scope.value(), "cancel task and its own consumer");
  LEANAT_CHECK(plan && plan.value().actions.size() == 2);
  auto execute = [&](const ScopeCancelAction &a) {
    return scoped_pool.execute_cancel_action(a, ReadyKey{});
  };
  for (auto &action : plan.value().actions)
    LEANAT_CHECK(scopes.apply(owned_scope.value(), action.id, execute));
  LEANAT_CHECK(scopes.close(owned_scope.value()));
  LEANAT_CHECK(scoped_results.occupied() == 0);
  auto ticket_scope = scopes.create(scopes.root());
  LEANAT_CHECK(ticket_scope);
  auto reserved_ticket = scoped_pool.request_slot(waiter, ticket_scope.value());
  LEANAT_CHECK(reserved_ticket && scoped_pool.granted_reservations() == 1);
  auto ticket_plan = scopes.cancel(ticket_scope.value(), "cancel reservation");
  LEANAT_CHECK(ticket_plan && ticket_plan.value().actions.size() == 2);
  // Apply the consumer before the ticket to exercise independent stable actions.
  for (auto it = ticket_plan.value().actions.rbegin(); it != ticket_plan.value().actions.rend();
       ++it)
    LEANAT_CHECK(scopes.apply(ticket_scope.value(), it->id, execute));
  LEANAT_CHECK(scoped_pool.granted_reservations() == 0);
  LEANAT_CHECK(scoped_pool.release_ticket(reserved_ticket.value()));
  LEANAT_CHECK(scopes.close(ticket_scope.value()));
  LEANAT_CHECK(scoped_results.occupied() == 0);
  // E10: the parent holds the only frame while its child needs that same pool.
  ResultStore dependency_results(16, 32, 8, d);
  TaskPool recursive(d, 20, desc, dependency_results);
  ProcessStore executions(d, 20, 4, 4, 1024);
  auto parent_process = executions.create(ProgramId{1}, 1);
  LEANAT_CHECK(parent_process && executions.begin(parent_process.value()));
  auto parent_task = recursive.try_spawn({}, scope);
  LEANAT_CHECK(parent_task &&
               recursive.bind_execution(parent_task.value(), parent_process.value()));
  auto child_slot = recursive.request_slot(parent_process.value(), scope);
  LEANAT_CHECK(child_slot && recursive.ticket_info(child_slot.value()).value().state ==
                                 SpawnTicketState::Pending);
  auto deadlock = recursive.begin_slot_wait(child_slot.value(), parent_process.value(), scope);
  LEANAT_CHECK(!deadlock && deadlock.error().code == ErrorCode::InvalidState &&
               deadlock.error().message.find("TaskSlotDeadlock") == 0);
  LEANAT_CHECK(executions.inspect(parent_process.value()).value().state == ProcessState::Executing);
  LEANAT_CHECK(recursive.active_frames() == 1);
  LEANAT_CHECK(recursive.cancel_ticket(child_slot.value(), parent_process.value(), scope));

  // Cross-pool waiting has an escape path while the second holder can run.
  TaskPool left(d, 21, desc, dependency_results), right(d, 22, desc, dependency_results);
  TaskPoolDependencies dependencies(2);
  LEANAT_CHECK(dependencies.add(left) && dependencies.add(right));
  auto lp = executions.create(ProgramId{2}, 1), rp = executions.create(ProgramId{3}, 1);
  LEANAT_CHECK(lp && rp && executions.begin(lp.value()) && executions.begin(rp.value()));
  auto lt = left.try_spawn({}, scope), rt = right.try_spawn({}, scope);
  LEANAT_CHECK(lt && rt && left.bind_execution(lt.value(), lp.value()) &&
               right.bind_execution(rt.value(), rp.value()));
  auto to_right = right.request_slot(lp.value(), scope);
  LEANAT_CHECK(to_right && right.begin_slot_wait(to_right.value(), lp.value(), scope));
  SingleWaitSpec wait_spec;
  wait_spec.kind = SingleWaitKind::Internal;
  wait_spec.source = to_right.value();
  auto suspended_parent = executions.suspend(lp.value(), wait_spec, {}, {});
  LEANAT_CHECK(suspended_parent);
  auto to_left = left.request_slot(rp.value(), scope);
  LEANAT_CHECK(to_left);
  auto cycle = left.begin_slot_wait(to_left.value(), rp.value(), scope);
  LEANAT_CHECK(!cycle && cycle.error().message.find("TaskSlotDeadlock") == 0);
  LEANAT_CHECK(left.cancel_ticket(to_left.value(), rp.value(), scope));
  LEANAT_CHECK(right.complete(rt.value(), {TaskOutcome::Kind::Success, Value{}}, {}));
  LEANAT_CHECK(right.ticket_info(to_right.value()).value().state == SpawnTicketState::Granted);
  LEANAT_CHECK(executions.notify(suspended_parent.value(),
                                 {SingleWaitStatus::Ready, Value{to_right.value()}, {}, 1}));
  LEANAT_CHECK(executions.take_resume(suspended_parent.value()));
  auto child = right.spawn_reserved(to_right.value(), lp.value(), scope, {});
  LEANAT_CHECK(child && right.active_frames() == 1);
  LEANAT_CHECK(right.bind_execution(child.value(), rp.value()));
  ExecutionContext dependency_context;
  dependency_context.kind = ContextKind::Process;
  dependency_context.domain = d;
  dependency_context.owner = 1;
  EventTxn discarded_dependency({128, 16, 1024 * 1024, 16}, dependency_context);
  auto staged_right = right.prepare(discarded_dependency);
  LEANAT_CHECK(staged_right);
  auto abandoned_slot = staged_right.value()->request_slot(lp.value(), scope);
  LEANAT_CHECK(abandoned_slot &&
               staged_right.value()->begin_slot_wait(abandoned_slot.value(), lp.value(), scope));
  LEANAT_CHECK(discarded_dependency.discard());
  LEANAT_CHECK(!right.ticket_info(abandoned_slot.value()));
  auto remaining_escape = left.request_slot(rp.value(), scope);
  LEANAT_CHECK(remaining_escape &&
               left.begin_slot_wait(remaining_escape.value(), rp.value(), scope));
  LEANAT_CHECK(left.cancel_ticket(remaining_escape.value(), rp.value(), scope));
}
