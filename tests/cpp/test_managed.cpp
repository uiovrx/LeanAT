#include "leanat/managed.hpp"
#include "leanat/memory.hpp"
#include "leanat/resource.hpp"
#include "leanat/runtime.hpp"
#include "test_support.hpp"
using namespace leanat;
struct ManagedHost final : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return fail(ErrorCode::Unsupported, "fixture has no transport");
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
static void staged_savepoints() {
  ResultStore results(16, 32, 16, DomainId{1});
  ManagedLimits limits;
  limits.leases = 2;
  limits.operations = 2;
  ManagedAccessManager manager(results, DomainId{1}, true, limits);
  ManagedRegionDesc region;
  region.id = RegionId{1};
  region.end = 3;
  region.backing = std::make_shared<Bytes>(4, 0);
  region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{1}; };
  LEANAT_CHECK(manager.add_region(region));
  ManagedRequest request;
  request.region = region.id;
  request.end = 3;
  request.domain = DomainId{1};
  request.owner = 9;
  ExecutionContext context;
  context.domain = request.domain;
  context.owner = request.owner;
  SegmentBudget budget;
  budget.bytes = 4 * 1024 * 1024;
  EventTxn transaction(budget, context);
  auto first = manager.prepare_request(transaction, request);
  LEANAT_CHECK(first);
  auto savepoint = transaction.checkpoint();
  LEANAT_CHECK(savepoint);
  auto second = manager.prepare_request(transaction, request);
  LEANAT_CHECK(second);
  LEANAT_CHECK(transaction.rollback(std::move(savepoint.value())));
  auto retry = manager.prepare_request(transaction, request);
  LEANAT_CHECK(retry && retry.value().slot == second.value().slot);
  LEANAT_CHECK(retry.value().generation > second.value().generation);
  auto before_failure = transaction.remaining_bytes();
  auto failure_checkpoint = transaction.checkpoint();
  LEANAT_CHECK(failure_checkpoint);
  LEANAT_CHECK(!manager.prepare_request(transaction, request));
  LEANAT_CHECK(transaction.remaining_bytes() == before_failure);
  LEANAT_CHECK(transaction.validate_effects_since(failure_checkpoint.value(), false, false, false));
  LEANAT_CHECK(transaction.commit());
  LEANAT_CHECK(!manager.release_lease(second.value()));
  LEANAT_CHECK(manager.release_lease(retry.value()));
  // A discarded lease must never become a different live lease later.
  EventTxn abandoned(budget, context);
  auto provisional = manager.prepare_request(abandoned, request);
  LEANAT_CHECK(provisional && abandoned.discard());
  auto replacement = manager.request(request);
  LEANAT_CHECK(replacement && replacement.value().slot == provisional.value().slot);
  LEANAT_CHECK(replacement.value().generation > provisional.value().generation);
  LEANAT_CHECK(!manager.release_lease(provisional.value()));
  OwnedAccessRequest write;
  write.lease = first.value();
  write.command = Command::Write;
  write.count = 1;
  write.input = {7};
  EventTxn accesses(budget, context);
  auto a = manager.prepare_begin(accesses, write);
  LEANAT_CHECK(a);
  auto access_savepoint = accesses.checkpoint();
  LEANAT_CHECK(access_savepoint);
  write.address = 1;
  write.input = {8};
  auto b = manager.prepare_begin(accesses, write);
  LEANAT_CHECK(b);
  LEANAT_CHECK(accesses.rollback(std::move(access_savepoint.value())));
  auto b_retry = manager.prepare_begin(accesses, write);
  LEANAT_CHECK(b_retry && b_retry.value().slot == b.value().slot);
  LEANAT_CHECK(b_retry.value().generation > b.value().generation);
  LEANAT_CHECK(accesses.commit());
  LEANAT_CHECK(!manager.result_handle(b.value()));
  LEANAT_CHECK(results.occupied() == 2);
  LEANAT_CHECK(manager.advance(Tick{2}));
  LEANAT_CHECK((*region.backing)[0] == 7 && (*region.backing)[1] == 8);
  // Independently committed operations produce the same bytes and completion keys.
  ResultStore independent_results(16, 32, 16, DomainId{1});
  ManagedAccessManager independent(independent_results, DomainId{1}, true, limits);
  auto independent_region = region;
  independent_region.backing = std::make_shared<Bytes>(4, 0);
  LEANAT_CHECK(independent.add_region(independent_region));
  write.lease = independent.request(request).value();
  write.address = 0;
  write.input = {7};
  auto ia = independent.begin(write);
  write.address = 1;
  write.input = {8};
  auto ib = independent.begin(write);
  LEANAT_CHECK(ia && ib && independent.advance(Tick{2}));
  LEANAT_CHECK(*region.backing == *independent_region.backing);
  LEANAT_CHECK(manager.completion(a.value()).value().ready ==
               independent.completion(ia.value()).value().ready);
  LEANAT_CHECK(manager.completion(b_retry.value()).value().ready ==
               independent.completion(ib.value()).value().ready);
  // Discarding an operation retains its consumed generation as well.
  ManagedAccessManager discarded_manager(independent_results, DomainId{1}, true, limits);
  LEANAT_CHECK(discarded_manager.add_region(independent_region));
  write.lease = discarded_manager.request(request).value();
  EventTxn dropped(budget, context);
  auto dropped_access = discarded_manager.prepare_begin(dropped, write);
  LEANAT_CHECK(dropped_access && dropped.discard());
  auto fresh_access = discarded_manager.begin(write);
  LEANAT_CHECK(fresh_access && fresh_access.value().slot == dropped_access.value().slot);
  LEANAT_CHECK(fresh_access.value().generation > dropped_access.value().generation);
  LEANAT_CHECK(!discarded_manager.result_handle(dropped_access.value()));
}
static void private_scheduler_ignores_full_event_queue() {
  EventQueue unrelated_queue(1, DomainId{1});
  LEANAT_CHECK(unrelated_queue.enqueue(EventDraft{}));
  LEANAT_CHECK(!unrelated_queue.enqueue(EventDraft{}));
  ResultStore results(2, 2, 2, DomainId{1});
  ManagedAccessManager manager(results, DomainId{1}, true);
  ManagedRegionDesc region;
  region.id = RegionId{1};
  region.end = 3;
  region.backing = std::make_shared<Bytes>(4, 0);
  region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{1}; };
  LEANAT_CHECK(manager.add_region(region));
  ManagedRequest request;
  request.region = region.id;
  request.end = 3;
  request.domain = DomainId{1};
  request.owner = 9;
  auto lease = manager.request(request);
  LEANAT_CHECK(lease);
  OwnedAccessRequest access;
  access.lease = lease.value();
  access.count = 1;
  auto pending = manager.begin(access);
  LEANAT_CHECK(pending);
  LEANAT_CHECK(manager.advance(Tick{1}));
  LEANAT_CHECK(manager.completion(pending.value()));
  LEANAT_CHECK(!unrelated_queue.enqueue(EventDraft{}));
}

int main() {
  private_scheduler_ignores_full_event_queue();
  staged_savepoints();
  ResultStore results(16, 32, 16, DomainId{1});
  ManagedAccessManager manager(results, DomainId{1}, true);
  auto old = std::make_shared<Bytes>(8, 0);
  std::weak_ptr<Bytes> weak = old;
  ManagedRegionDesc region;
  region.id = RegionId{1};
  region.end = 7;
  region.backing = old;
  region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{2}; };
  LEANAT_CHECK(manager.add_region(region));
  ManagedRequest request;
  request.region = region.id;
  request.end = 7;
  request.domain = DomainId{1};
  request.owner = 9;
  auto foreign_connection = request;
  foreign_connection.connection = ConnectionId{17};
  LEANAT_CHECK(!manager.request(foreign_connection));
  auto lease = manager.request(request);
  LEANAT_CHECK(lease);
  OwnedAccessRequest write;
  write.lease = lease.value();
  write.command = Command::Write;
  write.address = 1;
  write.count = 2;
  write.input = {4, 5};
  write.arrival = Tick{10};
  auto future = manager.begin(write);
  LEANAT_CHECK(future);
  write.arrival = Tick{1};
  write.input = {2, 3};
  auto early = manager.begin(write);
  LEANAT_CHECK(early);
  auto rh = manager.result_handle(early.value()).value();
  LEANAT_CHECK(!manager.pin_result(early.value(), rh.consumer));
  LEANAT_CHECK(manager.advance(Tick{1}));
  LEANAT_CHECK((*old)[1] == 0);
  LEANAT_CHECK(manager.backing_pins() == 1);
  LEANAT_CHECK(manager.advance(Tick{3}));
  LEANAT_CHECK((*old)[1] == 2);
  LEANAT_CHECK(manager.completion(early.value()).value().ready.time == Tick{3});
  LEANAT_CHECK(manager.pin_result(early.value(), rh.consumer));
  LEANAT_CHECK(manager.release_result(early.value(), rh.consumer));
  LEANAT_CHECK(!manager.release_result(early.value(), rh.consumer));
  LEANAT_CHECK(manager.invalidate(region.id, 0, 7, Tick{8}));
  LEANAT_CHECK(manager.advance(Tick{7}));
  write.arrival = Tick{7};
  write.input = {8, 9};
  auto admitted = manager.begin(write);
  LEANAT_CHECK(admitted);
  LEANAT_CHECK(manager.advance(Tick{7}));
  LEANAT_CHECK(manager.advance(Tick{8}));
  LEANAT_CHECK(!manager.begin(write));
  LEANAT_CHECK(manager.completion(future.value()).value().failure == AccessFailure::Invalidated);
  auto replacement = std::make_shared<Bytes>(8, 0);
  LEANAT_CHECK(manager.replace_backing(region.id, replacement, 2));
  region.backing.reset();
  old.reset();
  LEANAT_CHECK(!weak.expired());
  LEANAT_CHECK(manager.advance(Tick{9}));
  LEANAT_CHECK(weak.expired());
  LEANAT_CHECK((*replacement)[1] == 0);
  auto fresh = manager.request(request);
  LEANAT_CHECK(fresh);
  write.lease = fresh.value();
  write.arrival = Tick{10};
  auto cancelled = manager.begin(write);
  LEANAT_CHECK(cancelled);
  LEANAT_CHECK(manager.advance(Tick{10}));
  LEANAT_CHECK(manager.reset());
  LEANAT_CHECK(manager.completion(cancelled.value()).value().failure == AccessFailure::Reset);
  LEANAT_CHECK(manager.backing_pins() == 1);
  LEANAT_CHECK(manager.advance(Tick{12}));
  LEANAT_CHECK(manager.backing_pins() == 0);
  LEANAT_CHECK((*replacement)[1] == 0);
  auto denied = request;
  denied.domain = DomainId{2};
  LEANAT_CHECK(!manager.request(denied));
  LEANAT_CHECK(!manager.standard_dmi_allowed(region.id));
  LEANAT_CHECK(manager.release_lease(fresh.value()));
  LEANAT_CHECK(!manager.release_lease(fresh.value()));
  auto newer = manager.request(request);
  LEANAT_CHECK(newer);
  write.lease = newer.value();
  write.arrival = Tick{12};
  write.byte_enable = true;
  LEANAT_CHECK(!manager.begin(write));
  // AT and managed use the same Memory and M37 reservation cell.
  auto memory_result = Memory::make(8);
  LEANAT_CHECK(memory_result);
  auto memory = std::make_shared<Memory>(std::move(memory_result.value()));
  auto resource_result =
      Resource::make({ResourceKind::Serial, 1, Duration{2}, Duration{}, 8}, DomainId{1}, 77);
  LEANAT_CHECK(resource_result);
  auto resource = std::move(resource_result.value());
  ManagedAccessManager integrated(results, DomainId{1}, true, {}, 26);
  LEANAT_CHECK(integrated.bind_shared_resource(resource));
  region.backing.reset();
  region.version = 1;
  LEANAT_CHECK(integrated.add_memory_region(region, memory));
  auto shared_lease = integrated.request(request).value();
  ExecutionContext context;
  context.domain = DomainId{1};
  context.owner = 9;
  context.ready = {Tick{0}, 0};
  EventTxn at(SegmentBudget{}, context);
  Handle at_owner{HandleKind::Transaction, DomainId{1}, 1, 1, 1, 9};
  auto at_grant = resource.reserve(at, at_owner, Tick{0}, Duration{3});
  LEANAT_CHECK(at_grant);
  LEANAT_CHECK(at.commit());
  write.lease = shared_lease;
  write.byte_enable = false;
  write.arrival = Tick{1};
  write.input = {11, 12};
  auto shared_op = integrated.begin(write);
  LEANAT_CHECK(shared_op);
  LEANAT_CHECK(integrated.advance(Tick{4}));
  LEANAT_CHECK(memory->data()[1] == 0);
  LEANAT_CHECK(integrated.advance(Tick{5}));
  LEANAT_CHECK(memory->data()[1] == 11);
  LEANAT_CHECK(integrated.completion(shared_op.value()).value().ready.time == Tick{5});
  context.ready = {Tick{5}, 0};
  EventTxn inspect(SegmentBudget{}, context);
  auto memory_read = memory->read_bytes(inspect, 1, 2);
  LEANAT_CHECK(memory_read && memory_read.value() == Bytes({11, 12}));
  LEANAT_CHECK(resource.complete(inspect, at_grant.value().ticket, Tick{5}));
  LEANAT_CHECK(inspect.commit());
  OwnedAccessRequest read;
  read.lease = shared_lease;
  read.command = Command::Read;
  read.address = 1;
  read.count = 2;
  read.arrival = Tick{5};
  auto reading = integrated.begin(read);
  LEANAT_CHECK(reading);
  LEANAT_CHECK(integrated.advance(Tick{7}));
  auto read_result = integrated.result_handle(reading.value()).value();
  auto extra = results.retain(read_result, 10);
  LEANAT_CHECK(extra);
  auto pinned = integrated.pin_result(reading.value(), read_result.consumer);
  LEANAT_CHECK(pinned);
  LEANAT_CHECK(integrated.release_result(reading.value(), read_result.consumer));
  context.ready = {Tick{7}, 0};
  EventTxn clear(SegmentBudget{}, context);
  LEANAT_CHECK(memory->reset(clear));
  LEANAT_CHECK(clear.commit());
  LEANAT_CHECK(std::get<Bytes>(std::get<Value::Array>(pinned.value().value().data)[2].data) ==
               Bytes({11, 12}));
  auto retained = results.read(extra.value());
  LEANAT_CHECK(retained);
  LEANAT_CHECK(std::get<Bytes>(std::get<Value::Array>(retained.value().data)[2].data) ==
               Bytes({11, 12}));
  LEANAT_CHECK(results.release(extra.value()));
  write.arrival = Tick{8};
  write.lease = shared_lease;
  auto resource_cancelled = integrated.begin(write);
  LEANAT_CHECK(resource_cancelled);
  LEANAT_CHECK(integrated.advance(Tick{8}));
  context.ready = {Tick{8}, 0};
  EventTxn cancel_ticket(SegmentBudget{}, context);
  auto outstanding = resource.inspect(cancel_ticket);
  LEANAT_CHECK(outstanding && outstanding.value().reservations.size() == 1);
  LEANAT_CHECK(
      resource.cancel_pending(cancel_ticket, outstanding.value().reservations.front().ticket));
  LEANAT_CHECK(cancel_ticket.commit());
  LEANAT_CHECK(integrated.advance(Tick{10}));
  LEANAT_CHECK(integrated.completion(resource_cancelled.value()).value().failure ==
               AccessFailure::Cancelled);
  LEANAT_CHECK(memory->data()[1] == 0);
  LEANAT_CHECK(integrated.backing_pins() == 0);
  // Fault injection: an outstanding result-publication preparation must block the
  // whole finish segment, including the M37 ticket and Memory write.
  write.arrival = Tick{11};
  auto atomic_finish = integrated.begin(write);
  LEANAT_CHECK(atomic_finish);
  LEANAT_CHECK(integrated.advance(Tick{11}));
  auto atomic_result = integrated.result_handle(atomic_finish.value()).value();
  auto conflict =
      results.prepare_publish(ResultOwnerHandle{atomic_result.result}, Value{std::uint64_t{0}},
                              PublicationContext{atomic_finish.value(), {Tick{13}, 0}, true});
  LEANAT_CHECK(conflict);
  LEANAT_CHECK(!integrated.advance(Tick{13}));
  LEANAT_CHECK(memory->data()[1] == 0);
  LEANAT_CHECK(!integrated.completion(atomic_finish.value()));
  conflict.value()->discard();
  conflict.value().reset();
  LEANAT_CHECK(integrated.advance(Tick{13}));
  LEANAT_CHECK(memory->data()[1] == 11);
  LEANAT_CHECK(integrated.completion(atomic_finish.value()).value().disposition ==
               CommitDisposition::WriteCommitted);
  ManagedLimits tiny;
  tiny.operations = 1;
  tiny.leases = 1;
  tiny.input_bytes = 2;
  ManagedAccessManager bounded(results, DomainId{1}, true, tiny, 27);
  region.backing = std::make_shared<Bytes>(8, 0);
  region.service = [](Command, std::size_t) -> Expected<Duration> {
    return fail(ErrorCode::Capacity, "injected service capacity");
  };
  LEANAT_CHECK(bounded.add_region(region));
  auto limited = bounded.request(request);
  LEANAT_CHECK(limited);
  LEANAT_CHECK(!bounded.request(request));
  read.lease = limited.value();
  read.arrival = Tick{0};
  auto service_error = bounded.begin(read);
  LEANAT_CHECK(service_error);
  LEANAT_CHECK(!bounded.begin(read));
  LEANAT_CHECK(bounded.advance(Tick{0}));
  LEANAT_CHECK(bounded.completion(service_error.value()).value().failure ==
               AccessFailure::ServiceFailure);
  LEANAT_CHECK(bounded.completion(service_error.value()).value().disposition ==
               CommitDisposition::NotCommitted);
  LEANAT_CHECK(bounded.release_lease(limited.value()));
  auto reissued = bounded.request(request);
  LEANAT_CHECK(reissued);
  LEANAT_CHECK(reissued.value().generation != limited.value().generation);
  LEANAT_CHECK(!bounded.begin(read));
  ManagedHost host;
  RuntimeConfig config;
  config.domain = DomainId{1};
  config.descriptor_identity = "managed-fixture";
  Runtime runtime(config, host);
  HostBindingManifest manifest;
  manifest.domain = DomainId{1};
  manifest.descriptor_identity = "managed-fixture";
  manifest.clock_grid_valid = true;
  manifest.capabilities_valid = true;
  LEANAT_CHECK(runtime.start(manifest));
  ManagedAccessManager pumped(runtime.results(), DomainId{1}, true);
  region.service = [](Command, std::size_t) -> Expected<Duration> { return Duration{0}; };
  LEANAT_CHECK(pumped.add_region(region));
  write.lease = pumped.request(request).value();
  write.arrival = Tick{2};
  write.byte_enable = false;
  auto pump_op = pumped.begin(write);
  LEANAT_CHECK(pump_op);
  unsigned dispatches = 0;
  runtime.set_handler([&](const QueuedEvent &event, Runtime &rt) -> Expected<void> {
    ++dispatches;
    auto step = pumped.advance_one({event.event.key.time, event.event.key.turn});
    if (!step) {
      return step;
    }
    auto next = pumped.next_ready();
    if (next) {
      auto key = rt.queue().successor(next->time);
      if (!key) {
        return key.error();
      }
      EventDraft follow;
      follow.key.time = key.value().time;
      follow.key.turn = key.value().turn;
      auto queued = rt.schedule(follow);
      if (!queued) {
        return queued.error();
      }
    }
    return {};
  });
  EventDraft wake;
  wake.key.time = Tick{2};
  LEANAT_CHECK(runtime.schedule(wake));
  for (unsigned i = 0; i < 8 && runtime.next_wakeup(); ++i) {
    LEANAT_CHECK(runtime.pump_batch(Tick{2}, 8));
  }
  LEANAT_CHECK(dispatches == 2);
  LEANAT_CHECK(pumped.completion(pump_op.value()).value().ready == ReadyKey({Tick{2}, 1}));
  return 0;
}
