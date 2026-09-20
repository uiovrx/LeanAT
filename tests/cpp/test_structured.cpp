#include "leanat/structured.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  DomainId d{1};
  Handle scope{HandleKind::Scope, d, 1, 0, 1, 0};
  ResultStore results(16, 32, 16, d);
  TaskPoolDesc desc;
  desc.max_instances = 3;
  desc.task_capacity = 8;
  desc.result_capacity = 8;
  TaskPool pool(d, 3, desc, results);
  auto group = make_task_group(pool, scope, 1), other = make_task_group(pool, scope, 2);
  LEANAT_CHECK(group && other);
  auto copied = group.value();
  auto a = group_try_spawn(group.value(), {});
  LEANAT_CHECK(a && copied.membership->size() == 1);
  auto snap = snapshot_idle(group.value());
  auto b = group_try_spawn(group.value(), {}), c = group_try_spawn(other.value(), {});
  LEANAT_CHECK(b && c);
  LEANAT_CHECK(!idle_ready(pool, snap.value()).value());
  LEANAT_CHECK(pool.complete(a.value(), {TaskOutcome::Kind::Error, Value{}}, {}));
  LEANAT_CHECK(idle_ready(pool, snap.value()).value());
  LEANAT_CHECK(cancel_children(group.value(), "group", {}));
  LEANAT_CHECK(pool.state(c.value()).value() == TaskState::Runnable);
  LEANAT_CHECK(!snapshot_idle(group.value(), ContextKind::Timed));
  auto children = fan_out(other.value(), {{}, {}, {}}, {}, ContextKind::Process);
  LEANAT_CHECK(!children);
  LEANAT_CHECK(pool.active_frames() == 1);
  LEANAT_CHECK(pool.cancel(c.value(), "done", {}));
  auto cc = pool.result_handle(c.value(), scope);
  LEANAT_CHECK(pool.release_result(c.value(), cc.value()));
  auto good = fan_out(other.value(), {{}, {}}, {});
  LEANAT_CHECK(good);
  LEANAT_CHECK(!fan_in(pool, good.value()));
  for (auto child : good.value().children)
    LEANAT_CHECK(pool.complete(
        child.task, {TaskOutcome::Kind::Success, Value{std::uint64_t{child.task.slot}}}, {}));
  auto values = fan_in(pool, good.value());
  LEANAT_CHECK(values && values.value().size() == 2 && good.value().children.empty());
  LEANAT_CHECK(transaction_deadline(Tick{10}, Duration{0}).value() == Tick{10});
  LEANAT_CHECK(!transaction_deadline(Tick{UINT64_MAX}, Duration{1}));
  RetryPolicy policy;
  policy.max_retries = 3;
  policy.deadline = Tick{100};
  policy.attempt_timeout = Duration{20};
  policy.backoff = Duration{5};
  RetryController retries(
      policy, {RetrySafetyKind::Idempotent, "verified operation contract", {}, {}, true},
      [](const RetrySafety &s) -> Expected<void> {
        if (s.kind != RetrySafetyKind::Idempotent || s.contract != "verified operation contract")
          return fail(ErrorCode::InvalidArgument, "contract absent from trusted test descriptor");
        return {};
      });
  auto at = retries.next(Tick{10}, false);
  LEANAT_CHECK(at && at.value() && at.value()->deadline == Tick{30});
  LEANAT_CHECK(!retries.next(Tick{11}, true));
  Handle receipt{HandleKind::Drain, d, 7, 0, 1, 0};
  LEANAT_CHECK(retries.record({TimeoutState::TimedOut, {}, receipt, true}));
  auto bt = retries.next(Tick{30}, true);
  LEANAT_CHECK(bt && bt.value()->start == Tick{35} && bt.value()->deadline == Tick{55});
  LEANAT_CHECK(retries.record({TimeoutState::Success, Value{}, {}, true}));
  LEANAT_CHECK(!retries.record({TimeoutState::Success, Value{}, {}, true}));
  LEANAT_CHECK(retries.outstanding_drains().size() == 1);
  LEANAT_CHECK(!retries.next(Tick{100}, true).value());
  RetryController unsafe(policy, {RetrySafetyKind::ProvenUnsent, "", {}, {}, false});
  LEANAT_CHECK(unsafe.next(Tick{1}, false));
  LEANAT_CHECK(unsafe.record({TimeoutState::TimedOut, {}, {}, true}));
  LEANAT_CHECK(!unsafe.next(Tick{2}, true));
  AdmissionStore admission(d, InstanceId{1}, {8, 8, 2, 8, 8});
  Handle txn{HandleKind::Transaction, d, 1, 0, 1, 1};
  int drains = 0;
  TimeoutOperation immediate(admission, txn, ConnectionId{1}, Tick{5},
                             [&]() -> Expected<std::optional<Handle>> {
                               ++drains;
                               return std::optional<Handle>{receipt};
                             });
  LEANAT_CHECK(immediate.start({Tick{5}, 0}));
  LEANAT_CHECK(immediate.result().state == TimeoutState::TimedOut && drains == 0);
  AdmissionRequest req;
  req.connection = ConnectionId{2};
  req.transport = TransportId{1};
  req.owner = 1;
  auto admitted = admission.admit(req, true);
  LEANAT_CHECK(admitted);
  int sends = 0;
  TimeoutOperation op(admission, admitted.value().txn, ConnectionId{2}, Tick{20},
                      [&]() -> Expected<std::optional<Handle>> {
                        ++drains;
                        return std::optional<Handle>{receipt};
                      });
  LEANAT_CHECK(op.start({Tick{1}, 0}));
  auto sent = op.advance_gate({Tick{2}, 0}, true, [&](RequestPermit) -> Expected<void> {
    ++sends;
    return op.record_response(Value{Bytes{3}}, {Tick{20}, 0});
  });
  LEANAT_CHECK(sent && sent.value() && sends == 1);
  LEANAT_CHECK(op.record_timeout({Tick{20}, 0}));
  LEANAT_CHECK(op.record_response(Value{Bytes{3}}, {Tick{20}, 0}));
  LEANAT_CHECK(!op.resolve(false));
  LEANAT_CHECK(op.resolve(true).value().state == TimeoutState::FinishingResponse);
  LEANAT_CHECK(op.finish_response(Value{Bytes{4}}));
  LEANAT_CHECK(op.result().state == TimeoutState::Success && drains == 0);
  LEANAT_CHECK(!op.record_timeout({Tick{30}, 0}));
  req.connection = ConnectionId{3};
  req.transport = TransportId{2};
  auto late = admission.admit(req, true);
  LEANAT_CHECK(late);
  TimeoutOperation lateop(admission, late.value().txn, ConnectionId{3}, Tick{40},
                          [&]() -> Expected<std::optional<Handle>> {
                            ++drains;
                            return std::optional<Handle>{receipt};
                          });
  LEANAT_CHECK(lateop.start({Tick{30}, 0}));
  LEANAT_CHECK(
      lateop.advance_gate({Tick{31}, 0}, true, [](RequestPermit) -> Expected<void> { return {}; }));
  LEANAT_CHECK(lateop.record_timeout({Tick{40}, 0}));
  LEANAT_CHECK(lateop.record_response(Value{}, {Tick{40}, 1}));
  LEANAT_CHECK(lateop.resolve(true).value().state == TimeoutState::TimedOut && drains == 1);
  LEANAT_CHECK(!lateop.record_response(Value{}, {Tick{41}, 0}));
  req.connection = ConnectionId{4};
  req.transport = TransportId{3};
  auto blocked = admission.admit(req, true);
  LEANAT_CHECK(blocked);
  admission.update_request_lane(ConnectionId{4}, false, {Tick{1}, 0});
  TimeoutOperation gatewait(admission, blocked.value().txn, ConnectionId{4}, Tick{10}, {});
  LEANAT_CHECK(gatewait.start({Tick{2}, 0}));
  LEANAT_CHECK(
      !gatewait.advance_gate({Tick{3}, 0}, true, [](RequestPermit) -> Expected<void> { return {}; })
           .value());
  admission.update_request_lane(ConnectionId{4}, true, {Tick{10}, 0});
  LEANAT_CHECK(!gatewait
                    .advance_gate({Tick{10}, 0}, true,
                                  [&](RequestPermit) -> Expected<void> {
                                    ++sends;
                                    return {};
                                  })
                    .value());
  LEANAT_CHECK(sends == 1 && gatewait.result().state == TimeoutState::TimedOut);
  // Ticket capacity is reusable on both response completion and publication failure.
  AdmissionStore small(d, InstanceId{7}, {2, 2, 1, 2, 1});
  AdmissionRequest smallreq;
  smallreq.connection = ConnectionId{9};
  smallreq.transport = TransportId{9};
  auto smalltxn = small.admit(smallreq, true);
  LEANAT_CHECK(smalltxn);
  for (unsigned i = 0; i < 3; ++i) {
    small.update_request_lane(ConnectionId{9}, true, {Tick{1}, i});
    TimeoutOperation cycle(small, smalltxn.value().txn, ConnectionId{9}, Tick{50}, {});
    LEANAT_CHECK(cycle.start({Tick{1}, i}));
    LEANAT_CHECK(cycle.advance_gate({Tick{2}, i}, true, [&](RequestPermit) -> Expected<void> {
      return cycle.record_response(Value{}, {Tick{3}, i});
    }));
    LEANAT_CHECK(cycle.resolve(true));
    LEANAT_CHECK(cycle.finish_response(Value{}));
  }
  small.update_request_lane(ConnectionId{9}, true, {Tick{4}, 0});
  TimeoutOperation failedpublish(small, smalltxn.value().txn, ConnectionId{9}, Tick{50}, {});
  LEANAT_CHECK(failedpublish.start({Tick{4}, 0}));
  LEANAT_CHECK(!failedpublish.advance_gate({Tick{5}, 0}, true, [](RequestPermit) -> Expected<void> {
    return fail(ErrorCode::ExternalFailure, "publication rejected before wire call");
  }));
  TimeoutOperation afterfailure(small, smalltxn.value().txn, ConnectionId{9}, Tick{50}, {});
  LEANAT_CHECK(afterfailure.start({Tick{6}, 0}));
  LEANAT_CHECK(afterfailure.record_timeout({Tick{50}, 0}));
  LEANAT_CHECK(afterfailure.resolve(true).value().state == TimeoutState::TimedOut);
}
