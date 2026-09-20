#pragma once
inline std::string conformance_latched_sources() {
  for (bool task : {false, true}) {
    ResultStore results(16, 32, 16);
    Handle scope{HandleKind::Scope, {}, 1, 0, 1, 0};
    ResultHandle source_result;
    Handle source;
    TaskPoolDesc desc;
    desc.max_instances = 1;
    TaskPool tasks({}, 1, desc, results);
    if (task) {
      source = take(tasks.try_spawn({}, scope));
      source_result = take(tasks.result_handle(source, scope));
      take(tasks.complete(source, {TaskOutcome::Kind::Success, Value{Bytes{6, 7}}}, {Tick{2}, 0}));
    } else {
      WireFixture wire;
      auto hop = wire.ledger();
      auto completed = wire.call(hop, begin_req, Sync::Completed, {}, Tick{2});
      source = hop;
      auto stored = take(results.reserve({source, TypeId{}, 64, 0, 0}));
      source_result = stored.consumer;
      Value bytes;
      for (auto &m : completed.milestones)
        if (m.response)
          bytes = Value{m.response->data};
      take(results.publish(stored.reservation, bytes, {source, {Tick{2}, 0}, true}));
      take(results.release_owner(stored.reservation));
    }
    WaitGroupStore waits({}, 2, 1, 1, 512, results);
    WaitCreate create;
    create.branch_count = 1;
    create.owner_scope = scope;
    create.owner_process = {HandleKind::Process, {}, 3, 0, 1, 0};
    auto group = take(waits.create(create));
    WaitBranch branch;
    branch.source = source;
    branch.source_result = source_result;
    take(waits.arm(group, branch, BatchId{1}, {Tick{3}, 0}));
    take(waits.commit(group));
    auto ready = take(waits.resolve_closed_batch(BatchId{1}, true));
    require(ready.size() == 1 && ready[0].ready.time == Tick{2}, "precompleted source lost wakeup");
    take(waits.release(group, take(waits.result_handle(group, scope)), scope));
    if (task)
      take(tasks.release_result(source, source_result));
    else
      take(results.release(source_result));
    require(results.occupied() == 0, "late wait source result leak");
  }
  return "COMPLETED response and completed task independently published at2; each arm at3 resolved "
         "immediately with original ready2; result stores empty";
}
inline std::string conformance_all_matrix() {
  ResultStore results(16, 32, 32);
  WaitGroupStore waits({}, 1, 2, 4, 2048, results);
  Handle scope{HandleKind::Scope, {}, 2, 0, 1, 0}, process{HandleKind::Process, {}, 3, 0, 1, 0};
  Handle source{HandleKind::Task, {}, 4, 0, 1, 0};
  auto stored = take(results.reserve({source, TypeId{}, 256, 0, 0}));
  Value cancelled = encode_task_outcome({TaskOutcome::Kind::Cancelled, Value{Bytes{9}}});
  take(results.publish(stored.reservation, cancelled, {source, {Tick{2}, 0}, true}));
  take(results.release_owner(stored.reservation));
  WaitCreate c;
  c.mode = WaitMode::All;
  c.branch_count = 3;
  c.owner_scope = scope;
  c.owner_process = process;
  auto h = take(waits.create(c));
  for (unsigned i = 0; i < 3; ++i) {
    WaitBranch b;
    b.ordinal = i;
    b.source = source;
    if (i != 1)
      b.source_result = stored.consumer;
    take(waits.arm(h, b, BatchId{1}, {Tick{3}, 0}));
  }
  take(waits.commit(h));
  require(take(waits.resolve_closed_batch(BatchId{1}, true)).empty(),
          "collectAll failed before last branch terminal");
  Value error = encode_task_outcome({TaskOutcome::Kind::Error, Value{Bytes{8}}});
  take(waits.record({h, 1, source, {Tick{4}, 0}, BatchId{2}, error, true}));
  take(waits.record({h, 1, source, {Tick{4}, 0}, BatchId{2}, error, true}));
  auto resolved = take(waits.resolve_closed_batch(BatchId{2}, true));
  require(resolved.size() == 1, "All did not resolve once");
  auto layout = std::get<Value::Array>(resolved[0].outcome.data);
  require(std::get<std::uint64_t>(layout[0].data) == 0, "collectAll layout tag");
  auto values = std::get<Value::Array>(layout[1].data);
  require(values == Value::Array({cancelled, error, cancelled}),
          "All declaration order/duplicate source data lost");
  take(results.release(stored.consumer));
  auto own = take(waits.result_handle(h, scope));
  {
    auto pinned = take(waits.result(h, own));
    take(waits.release(h, own, scope));
    require(pinned.value() == resolved[0].outcome, "aggregate pin invalidated on group release");
  }
  require(results.occupied() == 0, "All consumer/pin leak");
  conformance_zero_time(true);
  return "collectAll waits for Error+Cancelled including duplicate source independent "
         "observations; ordered[Cancelled9,Error8,Cancelled9]; duplicate notification harmless; "
         "result survives group release; empty All fuel stops at4";
}
