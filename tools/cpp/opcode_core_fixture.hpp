#pragma once
#include "../../tests/support/event_queue_test_access.hpp"
#include "opcode_fixture.hpp"

namespace leanat::opcode_test {
inline Expected<ProviderFixture> bind_core_fixture(Runtime &runtime, CoreRuntimeBackend &backend,
                                                   const exec::Project &project,
                                                   const FixtureConfig &config) {
  struct Owned {
    std::optional<Handle> process, event;
    bool retired{};
    std::uint64_t completed_ordinal{};
    ReadyKey completed_ready{};
    ProgramId program{};
    std::vector<Value> completed_values;
    std::map<Handle, Handle> remap;
  };
  auto owned = std::make_shared<Owned>();
  auto env = [&](const char *name) -> const Value * {
    auto found = config.environment.find(name);
    return found == config.environment.end() ? nullptr : &found->second;
  };
  // Establish an actual completed dispatch frontier, rather than manufacturing
  // successor keys in the fixture or interpreting the expected observation.
  if (!runtime.queue().next_wakeup()) {
    EventDraft marker;
    marker.key = {config.context.ready.time, config.context.ready.turn, EventStage::Internal,
                  config.context.instance,   config.context.connection, 0};
    marker.owner = config.context.owner;
    marker.epoch = config.context.epoch;
    auto token = runtime.queue().enqueue(marker);
    if (!token)
      return token.error();
    auto batch = runtime.queue().pop_batch(config.context.ready.time, 1);
    if (!batch)
      return batch.error();
    if (!batch.value())
      return fail(ErrorCode::InvalidState, "core fixture dispatch frontier");
    auto ack = runtime.queue().ack_executed(batch.value()->id, token.value(),
                                            ExecutionDisposition::Committed);
    if (!ack)
      return ack.error();
    auto resolve = runtime.queue().resolver_step(batch.value()->id, 1, 0);
    if (!resolve)
      return resolve.error();
    auto finish = runtime.queue().finish_batch(batch.value()->id);
    if (!finish)
      return finish.error();
  }
  if (auto logical = env("runtime.process.identity")) {
    auto identity = std::get_if<Handle>(&logical->data);
    auto programValue = env("runtime.process.program");
    auto program = programValue ? std::get_if<std::uint64_t>(&programValue->data) : nullptr;
    if (!identity || identity->kind != HandleKind::Process || !program || *program > UINT32_MAX)
      return fail(ErrorCode::TypeMismatch, "core fixture process setup");
    EventTxn seed({}, config.context);
    auto process =
        runtime.processes().create(ProgramId{std::uint32_t(*program)}, identity->owner, seed);
    if (!process)
      return process.error();
    auto begun = runtime.processes().begin(process.value(), seed);
    if (!begun)
      return begun.error();
    auto commit = seed.commit(config.context.epoch);
    if (!commit)
      return commit.error();
    auto bound = backend.bind_current_process(process.value());
    if (!bound)
      return bound.error();
    owned->process = process.value();
    owned->program = ProgramId{std::uint32_t(*program)};
    owned->remap.emplace(*identity, process.value());
  }
  if (auto logical = env("storage.event.identity")) {
    auto identity = std::get_if<Handle>(&logical->data);
    auto record = env("storage.event.record");
    auto fields = record ? std::get_if<Value::Array>(&record->data) : nullptr;
    if (!identity || identity->kind != HandleKind::Event || !fields || fields->size() != 6)
      return fail(ErrorCode::TypeMismatch, "core fixture event setup");
    std::uint64_t values[5]{};
    for (std::size_t n = 0; n < 5; ++n) {
      auto number = std::get_if<std::uint64_t>(&(*fields)[n].data);
      if (!number)
        return fail(ErrorCode::TypeMismatch, "core fixture event key");
      values[n] = *number;
    }
    if (values[2] > unsigned(EventStage::Output) || values[3] > UINT32_MAX ||
        values[4] > UINT32_MAX)
      return fail(ErrorCode::Overflow, "core fixture event key width");
    EventDraft event{{Tick{values[0]}, values[1], EventStage(values[2]),
                      InstanceId{std::uint32_t(values[3])}, ConnectionId{std::uint32_t(values[4])},
                      0},
                     (*fields)[5],
                     identity->owner,
                     config.context.epoch};
    auto token = runtime.queue().enqueue(event);
    if (!token)
      return token.error();
    owned->event = token.value();
    owned->remap.emplace(*identity, token.value());
  }
  for (const auto &signature : project.services) {
    auto registered = backend.register_core(signature, project);
    if (!registered)
      return registered.error();
  }
  std::function<Value(const Value &)> remap = [&](const Value &value) -> Value {
    if (auto handle = std::get_if<Handle>(&value.data)) {
      auto found = owned->remap.find(*handle);
      if (found != owned->remap.end())
        return Value{found->second};
      // Negative source inputs may vary a seeded capability's generation or
      // owner. Preserve that variation relative to its actual allocation;
      // passing the logical store number through could accidentally alias it.
      for (const auto &entry : owned->remap) {
        const auto &logical = entry.first;
        if (handle->kind == logical.kind && handle->domain == logical.domain &&
            handle->store == logical.store && handle->slot == logical.slot) {
          Handle actual = entry.second;
          actual.owner = handle->owner;
          if (handle->generation >= logical.generation) {
            auto delta = handle->generation - logical.generation;
            actual.generation =
                delta <= UINT64_MAX - actual.generation ? actual.generation + delta : 0;
          } else {
            auto delta = logical.generation - handle->generation;
            actual.generation = delta <= actual.generation ? actual.generation - delta : 0;
          }
          return Value{actual};
        }
      }
    }
    if (auto array = std::get_if<Value::Array>(&value.data)) {
      Value::Array result;
      for (const auto &item : *array)
        result.push_back(remap(item));
      return Value{std::move(result)};
    }
    return value;
  };
  ProviderFixture fixture;
  for (const auto &input : config.inputs)
    fixture.inputs.push_back(remap(input));
  fixture.lifetime.push_back(owned);
  fixture.after_segment = [&runtime, owned, ready = config.context.ready](
                              const exec::SegmentResult &segment) -> Expected<void> {
    if (owned->process && segment.kind == exec::SegmentResult::Kind::Returned) {
      auto frame = runtime.processes().inspect(*owned->process);
      if (!frame)
        return frame.error();
      auto completed = runtime.processes().complete(*owned->process);
      if (!completed)
        return completed.error();
      owned->retired = true;
      owned->completed_ordinal = frame.value().suspension_ordinal;
      owned->completed_ready = ready;
      owned->completed_values = segment.values;
    }
    return {};
  };
  fixture.identities = [owned]() {
    return Json::object({{"process", owned->process ? observe(*owned->process) : Json{}},
                         {"event", owned->event ? observe(*owned->event) : Json{}}});
  };
  fixture.snapshot = [&runtime, owned]() {
    auto queue = testing::EventQueueTestAccess::snapshot(runtime.queue());
    auto process_counters = runtime.processes().counter_snapshot();
    std::vector<Json> events, slots;
    for (const auto &slot : queue.slots) {
      slots.push_back(
          Json::object({{"token", observe(slot.queued.token)}, {"state", Json(slot.state)}}));
      if (slot.state == "queued" || slot.state == "active")
        events.push_back(Json::object({{"token", observe(slot.queued.token)},
                                       {"key", observe(slot.queued.event.key)},
                                       {"value", observe(slot.queued.event.value)},
                                       {"owner", Json(slot.queued.event.owner)},
                                       {"epoch", Json(slot.queued.event.epoch)},
                                       {"cancelled", Json(slot.queued.cancelled)},
                                       {"state", Json(slot.state)}}));
    }
    Json process, wait;
    if (owned->process) {
      auto state = runtime.processes().inspect(*owned->process);
      if (!state)
        process = Json::object({{"identity", observe(*owned->process)},
                                {"retired", Json(owned->retired)},
                                {"program", Json(owned->program.value)},
                                {"ordinal", Json(owned->completed_ordinal)},
                                {"completedReady", observe(owned->completed_ready)},
                                {"completedValues", observe(owned->completed_values)},
                                {"error", observe(state.error())}});
      else
        process = Json::object(
            {{"identity", observe(*owned->process)},
             {"program", Json(state.value().program.value)},
             {"instance", Json(state.value().instance.value)},
             {"state", Json(unsigned(state.value().state))},
             {"ordinal", Json(state.value().suspension_ordinal)},
             {"wait",
              state.value().suspension ? observe(state.value().suspension->wait) : Json{}}});
      if (state && state.value().suspension) {
        auto token = *state.value().suspension;
        auto spec = runtime.processes().wait_spec(token.wait);
        auto outcome = runtime.processes().read_wait_result(token.wait);
        if (spec)
          wait = Json::object({{"identity", observe(token.wait)},
                               {"process", observe(token.process)},
                               {"kind", Json(unsigned(spec.value().kind))},
                               {"deadline", Json(spec.value().until.value)},
                               {"ordinal", Json(token.ordinal)},
                               {"outcome", outcome ? observe(outcome.value().value) : Json{}},
                               {"ready", outcome ? observe(outcome.value().source) : Json{}}});
      }
    }
    return Json::object({{"events", Json::array(events)},
                         {"eventCapacity", Json(queue.slots.size())},
                         {"eventSlots", Json::array(slots)},
                         {"processCounters",
                          Json::object({{"domain", Json(process_counters.domain.value)},
                                        {"store", Json(process_counters.store)},
                                        {"nextGeneration", Json(process_counters.next_generation)},
                                        {"frameCapacity", Json(process_counters.frame_capacity)},
                                        {"waitCapacity", Json(process_counters.wait_capacity)},
                                        {"liveCapacity", Json(process_counters.live_capacity)},
                                        {"frames", Json(process_counters.frames)},
                                        {"waits", Json(process_counters.waits)}})},
                         {"process", process},
                         {"wait", wait},
                         {"frames", Json(runtime.processes().frame_count())},
                         {"waits", Json(runtime.processes().wait_count())},
                         {"frontier", queue.frontier ? observe(*queue.frontier) : Json{}},
                         {"nextSequence", Json(queue.next_sequence)},
                         {"nextBatch", Json(queue.next_batch)}});
  };
  return fixture;
}
} // namespace leanat::opcode_test
