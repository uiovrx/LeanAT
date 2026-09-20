#include "leanat/wait_group.hpp"
#include "test_support.hpp"
using namespace leanat;
int main() {
  DomainId d{1};
  ResultStore results(32, 64, 32, d);
  WaitGroupStore waits(d, 9, 4, 4, 512, results);
  Handle process{HandleKind::Process, d, 1, 0, 1, 1}, scope{HandleKind::Scope, d, 1, 0, 1, 0},
      source{HandleKind::Event, d, 2, 0, 1, 0};
  int resumed = 0, unregistered = 0, transferred = 0;
  WaitCreate c;
  c.owner_process = process;
  c.owner_scope = scope;
  c.branch_count = 2;
  c.resume = [&](const Value &, ReadyKey) -> Expected<void> {
    ++resumed;
    return {};
  };
  auto h = waits.create(c);
  LEANAT_CHECK(h);
  auto forged = h.value();
  ++forged.owner;
  LEANAT_CHECK(!waits.commit(forged));
  LEANAT_CHECK(!waits.create(c));
  WaitBranch a;
  a.ordinal = 0;
  a.priority = 10;
  a.source = source;
  a.unregister = [&] { ++unregistered; };
  WaitBranch b = a;
  b.ordinal = 1;
  b.priority = 0;
  b.transfer_winner = [&]() -> Expected<void> {
    ++transferred;
    return {};
  };
  LEANAT_CHECK(waits.arm(h.value(), a, BatchId{1}, {Tick{5}, 0}));
  LEANAT_CHECK(waits.arm(h.value(), b, BatchId{1}, {Tick{5}, 0}));
  LEANAT_CHECK(waits.commit(h.value()));
  LEANAT_CHECK(waits.record(
      {h.value(), 0, source, {Tick{5}, 0}, BatchId{1}, Value{std::uint64_t{11}}, false}));
  LEANAT_CHECK(waits.record(
      {h.value(), 1, source, {Tick{5}, 0}, BatchId{1}, Value{std::uint64_t{22}}, false}));
  LEANAT_CHECK(!waits.resolve_closed_batch(BatchId{1}, false));
  auto resolved = waits.resolve_closed_batch(BatchId{1}, true);
  LEANAT_CHECK(resolved && resolved.value().size() == 1 && resumed == 1 && transferred == 1 &&
               unregistered == 1);
  auto array = std::get<Value::Array>(resolved.value()[0].outcome.data);
  LEANAT_CHECK(std::get<std::uint64_t>(array[0].data) == 1);
  LEANAT_CHECK(waits.resolve_closed_batch(BatchId{1}, true).value().empty());
  LEANAT_CHECK(!waits.record({h.value(), 0, source, {Tick{5}, 1}, BatchId{2}, Value{}, false}));
  auto consumer = waits.result_handle(h.value(), scope);
  LEANAT_CHECK(consumer);
  auto retained = waits.retain_wait_result(h.value(), consumer.value(), 77);
  LEANAT_CHECK(retained);
  auto wrong = scope;
  wrong.slot = 8;
  LEANAT_CHECK(!waits.release(h.value(), consumer.value(), wrong));
  LEANAT_CHECK(waits.release(h.value(), consumer.value(), scope));
  LEANAT_CHECK(results.read(retained.value()));
  LEANAT_CHECK(results.release(retained.value()));
  // ReadyKey dominates priority, preserving an earlier turn's timeout.
  c.resume = {};
  auto second = waits.create(c);
  LEANAT_CHECK(second);
  LEANAT_CHECK(waits.arm(second.value(), a, BatchId{2}, {Tick{5}, 1}));
  LEANAT_CHECK(waits.arm(second.value(), b, BatchId{2}, {Tick{5}, 1}));
  LEANAT_CHECK(waits.commit(second.value()));
  LEANAT_CHECK(waits.record({second.value(), 0, source, {Tick{5}, 0}, BatchId{2}, Value{}, false}));
  LEANAT_CHECK(waits.record({second.value(), 1, source, {Tick{5}, 1}, BatchId{2}, Value{}, false}));
  auto early = waits.resolve_closed_batch(BatchId{2}, true);
  LEANAT_CHECK(early);
  LEANAT_CHECK(
      std::get<std::uint64_t>(std::get<Value::Array>(early.value()[0].outcome.data)[0].data) == 0);
  auto sc = waits.result_handle(second.value(), scope);
  LEANAT_CHECK(waits.release(second.value(), sc.value(), scope));
  // FailFast explicitly keeps unfinished observations absent.
  c.mode = WaitMode::All;
  c.failure_policy = WaitFailurePolicy::FailFast;
  auto all = waits.create(c);
  LEANAT_CHECK(all);
  LEANAT_CHECK(waits.arm(all.value(), a, BatchId{3}, {}));
  LEANAT_CHECK(waits.arm(all.value(), b, BatchId{3}, {}));
  LEANAT_CHECK(waits.commit(all.value()));
  LEANAT_CHECK(
      waits.record({all.value(), 1, source, {Tick{6}, 0}, BatchId{3}, Value{Bytes{9}}, true}));
  auto failed = waits.resolve_closed_batch(BatchId{3}, true);
  LEANAT_CHECK(failed && failed.value().size() == 1);
  auto layout = std::get<Value::Array>(failed.value()[0].outcome.data);
  LEANAT_CHECK(std::get<std::uint64_t>(layout[0].data) == 1);
  auto obs = std::get<Value::Array>(layout[3].data);
  LEANAT_CHECK(!std::get<bool>(std::get<Value::Array>(obs[0].data)[0].data));
  auto ac = waits.result_handle(all.value(), scope);
  LEANAT_CHECK(waits.release(all.value(), ac.value(), scope));
  c.branch_count = 0;
  auto empty = waits.create(c);
  LEANAT_CHECK(empty && waits.commit(empty.value()));
  LEANAT_CHECK(waits.resolve_closed_batch(BatchId{4}, true).value().size() == 1);
  auto ec = waits.result_handle(empty.value(), scope);
  LEANAT_CHECK(waits.release(empty.value(), ec.value(), scope));
  // FailFast remains failure when every branch completed in the same batch.
  c.branch_count = 2;
  auto simultaneous = waits.create(c);
  LEANAT_CHECK(simultaneous);
  LEANAT_CHECK(waits.arm(simultaneous.value(), a, BatchId{5}, {}));
  LEANAT_CHECK(waits.arm(simultaneous.value(), b, BatchId{5}, {}));
  LEANAT_CHECK(waits.commit(simultaneous.value()));
  LEANAT_CHECK(
      waits.record({simultaneous.value(), 0, source, {Tick{7}, 0}, BatchId{5}, Value{}, false}));
  LEANAT_CHECK(
      waits.record({simultaneous.value(), 1, source, {Tick{7}, 0}, BatchId{5}, Value{}, true}));
  auto simultaneous_result = waits.resolve_closed_batch(BatchId{5}, true);
  LEANAT_CHECK(simultaneous_result &&
               std::get<std::uint64_t>(
                   std::get<Value::Array>(simultaneous_result.value()[0].outcome.data)[0].data) ==
                   1);
  auto simc = waits.result_handle(simultaneous.value(), scope);
  LEANAT_CHECK(waits.release(simultaneous.value(), simc.value(), scope));
  // Atomic prepare rollback on duplicate arm releases reservation and subscription.
  c.mode = WaitMode::Any;
  c.branch_count = 2;
  auto bad = waits.create(c);
  LEANAT_CHECK(bad);
  LEANAT_CHECK(waits.arm(bad.value(), a, BatchId{5}, {}));
  LEANAT_CHECK(!waits.arm(bad.value(), a, BatchId{5}, {}));
  LEANAT_CHECK(!waits.commit(bad.value()));
  LEANAT_CHECK(results.occupied() == 0);
  // A failed resume enqueue preserves the chosen result and can retry delivery.
  int attempts = 0;
  c.branch_count = 1;
  c.resume = [&](const Value &, ReadyKey) -> Expected<void> {
    if (++attempts == 1)
      return fail(ErrorCode::Capacity, "resume queue temporarily full");
    return {};
  };
  auto retry = waits.create(c);
  LEANAT_CHECK(retry);
  LEANAT_CHECK(waits.arm(retry.value(), a, BatchId{6}, {}));
  LEANAT_CHECK(waits.commit(retry.value()));
  LEANAT_CHECK(waits.record(
      {retry.value(), 0, source, {Tick{9}, 0}, BatchId{6}, Value{std::uint64_t{5}}, false}));
  LEANAT_CHECK(!waits.resolve_closed_batch(BatchId{6}, true));
  LEANAT_CHECK(!waits.create(c));
  LEANAT_CHECK(waits.resolve_closed_batch(BatchId{7}, true).value().size() == 1 && attempts == 2);
  auto retry_consumer = waits.result_handle(retry.value(), scope);
  LEANAT_CHECK(waits.release(retry.value(), retry_consumer.value(), scope));
  // Transactional views cannot run a native unregister hook before commit.
  c.resume = {};
  int native_calls = 0;
  auto native = waits.create(c);
  LEANAT_CHECK(native);
  auto native_branch = a;
  native_branch.unregister = [&] { ++native_calls; };
  LEANAT_CHECK(waits.arm(native.value(), native_branch, BatchId{8}, {}));
  LEANAT_CHECK(waits.commit(native.value()));
  auto native_consumer = waits.result_handle(native.value(), scope);
  ExecutionContext ctx;
  ctx.domain = d;
  EventTxn cancelled_prepare({32, 32, 1024 * 1024, 32}, ctx);
  auto prepared = waits.prepare(cancelled_prepare);
  LEANAT_CHECK(prepared);
  LEANAT_CHECK(!prepared.value()->release(native.value(), native_consumer.value(), scope));
  LEANAT_CHECK(native_calls == 0);
  LEANAT_CHECK(cancelled_prepare.discard());
  LEANAT_CHECK(waits.release(native.value(), native_consumer.value(), scope) && native_calls == 1);
  auto throwing = waits.create(c);
  LEANAT_CHECK(throwing);
  bool throw_once = true;
  auto throwing_branch = a;
  throwing_branch.unregister = [&] { if(throw_once){throw_once=false;throw std::runtime_error("cleanup failure");} };
  LEANAT_CHECK(waits.arm(throwing.value(), throwing_branch, BatchId{8}, {}));
  LEANAT_CHECK(waits.commit(throwing.value()));
  auto throwing_consumer = waits.result_handle(throwing.value(), scope);
  auto cleanup_failure = waits.release(throwing.value(), throwing_consumer.value(), scope);
  LEANAT_CHECK(!cleanup_failure && cleanup_failure.error().code == ErrorCode::ExternalFailure);
  LEANAT_CHECK(waits.release(throwing.value(), throwing_consumer.value(), scope));
  auto other_source = source;
  ++other_source.generation;
  auto foreign_result = results.reserve({other_source, {}, 512, 1, 1});
  LEANAT_CHECK(foreign_result);
  auto mismatch = waits.create(c);
  LEANAT_CHECK(mismatch);
  auto mismatched_branch = a;
  mismatched_branch.source_result = foreign_result.value().consumer;
  LEANAT_CHECK(!waits.arm(mismatch.value(), mismatched_branch, BatchId{9}, {}));
  LEANAT_CHECK(results.release(foreign_result.value().consumer));
  LEANAT_CHECK(results.release_owner(foreign_result.value().reservation));
  WaitGroupStore another(d, 9, 1, 1, 512, results);
  auto foreign_wait = another.create(c);
  LEANAT_CHECK(foreign_wait);
  LEANAT_CHECK(!waits.commit(foreign_wait.value()));
  auto fc = another.result_handle(foreign_wait.value(), scope);
  LEANAT_CHECK(another.release(foreign_wait.value(), fc.value(), scope));
  LEANAT_CHECK(results.occupied() == 0);
  auto typed = results.reserve({source, TypeId{42}, 512, 1, 1});
  LEANAT_CHECK(typed);
  auto typed_group = waits.create(c);
  LEANAT_CHECK(typed_group);
  WaitBranch typed_branch;
  typed_branch.source = source;
  typed_branch.source_result = typed.value().consumer;
  LEANAT_CHECK(waits.arm(typed_group.value(), typed_branch, BatchId{10}, {}));
  LEANAT_CHECK(waits.snapshot().value().front().branches.front().type == TypeId{42});
  LEANAT_CHECK(waits.commit(typed_group.value()));
  LEANAT_CHECK(results.publish(typed.value().reservation, Value{std::uint64_t{7}},
                               {source, {}, true}));
  LEANAT_CHECK(waits.refresh_sources(BatchId{10}, {}));
  LEANAT_CHECK(waits.resolve_closed_batch(BatchId{10}, true));
  auto typed_snapshot = waits.snapshot().value().front();
  LEANAT_CHECK(!typed_snapshot.branches.front().consumer &&
               typed_snapshot.branches.front().type == TypeId{42});
  auto typed_consumer = waits.result_handle(typed_group.value(), scope);
  LEANAT_CHECK(waits.release(typed_group.value(), typed_consumer.value(), scope));
  LEANAT_CHECK(results.release(typed.value().consumer));
  LEANAT_CHECK(results.release_owner(typed.value().reservation));
  LEANAT_CHECK(results.occupied() == 0);
}
