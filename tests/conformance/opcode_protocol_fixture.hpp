#pragma once
#include "../../tools/cpp/opcode_fixture.hpp"
#include <leanat/descriptor.hpp>
#include <leanat/protocol_services.hpp>

namespace leanat::opcode_test {
namespace protocol_fixture_detail {
template <class T> T need(Expected<T> value) {
  if (!value)
    throw std::runtime_error(value.error().message);
  return std::move(value.value());
}
inline void need(Expected<void> value) {
  if (!value)
    throw std::runtime_error(value.error().message);
}
inline const Value *env(const FixtureConfig &c, const std::string &key) {
  auto i = c.environment.find(key);
  return i == c.environment.end() ? nullptr : &i->second;
}
inline std::uint64_t number(const FixtureConfig &c, const std::string &key,
                            std::uint64_t fallback) {
  auto v = env(c, key);
  if (!v)
    return fallback;
  auto n = std::get_if<std::uint64_t>(&v->data);
  if (!n)
    throw std::runtime_error("protocol fixture U64 environment: " + key);
  return *n;
}
inline bool boolean(const FixtureConfig &c, const std::string &key, bool fallback) {
  auto v = env(c, key);
  if (!v)
    return fallback;
  auto b = std::get_if<bool>(&v->data);
  if (!b)
    throw std::runtime_error("protocol fixture Bool environment: " + key);
  return *b;
}
inline Value remap_capability(const Value &input, const Value *baseline, Handle actual) {
  auto supplied = std::get_if<Handle>(&input.data);
  auto logical = baseline ? std::get_if<Handle>(&baseline->data) : nullptr;
  if (!supplied || !logical)
    throw std::runtime_error("protocol fixture requires typed capability baseline");
  auto translate = [](std::uint64_t value, std::uint64_t base, std::uint64_t target,
                      std::uint64_t limit) -> std::uint64_t {
    if (value >= base) {
      auto delta = value - base;
      if (delta > limit - target)
        throw std::runtime_error("capability displacement overflow");
      return target + delta;
    }
    auto delta = base - value;
    if (delta > target)
      throw std::runtime_error("capability displacement underflow");
    return target - delta;
  };
  actual.kind = supplied->kind;
  actual.domain = supplied->domain;
  actual.owner = supplied->owner;
  actual.store = static_cast<std::uint32_t>(
      translate(supplied->store, logical->store, actual.store, UINT32_MAX));
  actual.slot =
      static_cast<std::uint32_t>(translate(supplied->slot, logical->slot, actual.slot, UINT32_MAX));
  actual.generation =
      translate(supplied->generation, logical->generation, actual.generation, UINT64_MAX);
  return Value{actual};
}
template <class T> Json queried(Expected<T> value) {
  return value ? Json::object({{"value", observe(value.value())}})
               : Json::object({{"error", observe(value.error())}});
}
inline void protocol_abi(const exec::ServiceSignature &s, const exec::Project &p) {
  using exec::Op;
  const auto label =
      std::string("leanat.protocol.v1:") + (s.op == Op::NewTransaction ? "newTransaction"
                                            : s.op == Op::StagePhase   ? "stagePhase"
                                                                       : "ackResponse");
  if (s.provider_key != "leanat.protocol" || s.provider_version != "1" ||
      s.abi_hash != exec::sha256(Bytes(label.begin(), label.end())) || s.context_mask != 3 ||
      s.effect_mask != exec::required_effect(s.op) || s.extra_fuel != 0)
    throw std::runtime_error("protocol fixture canonical provider metadata mismatch");
  auto type = [&](std::uint32_t id) -> const exec::Type & {
    if (id >= p.types.size())
      throw std::runtime_error("protocol fixture type index");
    return p.types[id];
  };
  auto u64 = [&](std::uint32_t id) {
    auto &t = type(id);
    return t.kind == exec::TypeKind::Bits && t.bound == 64;
  };
  auto txn = [&](std::uint32_t id) {
    auto &t = type(id);
    return t.kind == exec::TypeKind::Handle &&
           t.bound == static_cast<std::uint64_t>(HandleKind::Transaction);
  };
  const auto arity = s.op == Op::NewTransaction ? 6u : s.op == Op::StagePhase ? 4u : 3u;
  if (s.input_types.size() != arity || s.result_types.size() != 1)
    throw std::runtime_error("protocol fixture canonical provider arity mismatch");
  bool valid = true;
  if (s.op == Op::NewTransaction) {
    for (unsigned i = 0; i < 5; ++i)
      valid = valid && u64(s.input_types[i]);
    valid = valid && type(s.input_types[5]).kind == exec::TypeKind::Bytes && txn(s.result_types[0]);
  } else {
    valid = txn(s.input_types[0]) && type(s.result_types[0]).kind == exec::TypeKind::Unit;
    for (unsigned i = 1; i < arity; ++i)
      valid = valid && u64(s.input_types[i]);
  }
  if (!valid)
    throw std::runtime_error("protocol fixture canonical provider types mismatch");
}

struct State {
  Runtime &runtime;
  ExecutionContext context;
  std::shared_ptr<AdmissionStore> admission;
  std::optional<AdmissionDisposition> seeded;
  std::optional<ReservedResult> result;
  std::optional<Handle> result_source;
  VersionedCell *ack{};
  ConnectionId connection;
  conformance::profile::Recorder setup;
  State(Runtime &r, const ExecutionContext &c, ConnectionId link)
      : runtime(r), context(c), admission(std::make_shared<AdmissionStore>(
                                    c.domain, c.instance, AdmissionLimits{16, 16, 4, 16, 16})),
        connection(link) {}
  std::vector<Handle> hops() const {
    std::vector<Handle> out;
    if (seeded)
      out.push_back(seeded->hop);
    for (const auto &r : runtime.drains().stop_report())
      for (const auto &h : r.hops)
        if (std::find(out.begin(), out.end(), h.hop) == out.end())
          out.push_back(h.hop);
    return out;
  }
  Json identities() const {
    std::vector<Json> out;
    auto named = [&](const std::string &name, Handle h) {
      out.push_back(Json::object({{"name", name}, {"identity", observe(h)}}));
    };
    std::size_t ordinal = 0;
    for (auto h : hops()) {
      auto a = admission->inspect(h);
      if (a) {
        named("transaction." + std::to_string(ordinal), a.value().txn);
        named("hop." + std::to_string(ordinal++), h);
      } else if (seeded && seeded->hop == h) {
        named("transaction." + std::to_string(ordinal), seeded->txn);
        named("hop." + std::to_string(ordinal++), h);
      }
    }
    if (result) {
      named("result", result->consumer.result);
      named("consumer", result->consumer.consumer);
    }
    if (result_source)
      named("resultSource", *result_source);
    ordinal = 0;
    for (const auto &gate : admission->snapshot().gates)
      named("requestGate." + std::to_string(ordinal++), gate.handle);
    return Json::array(out);
  }
  Json snapshot() const {
    auto counters = runtime.counter_snapshot();
    auto drain_counters = runtime.drains().counter_snapshot();
    auto protocol = need(runtime.protocol(context.instance));
    auto protocol_counters = protocol->counter_snapshot();
    auto admission_state = admission->snapshot();
    std::vector<Json> gates, responses, lanes;
    for (const auto &g : admission_state.gates)
      gates.push_back(Json::object({{"handle", observe(g.handle)},
                                    {"transaction", observe(g.txn)},
                                    {"connection", observe(g.connection)},
                                    {"state", observe(g.state)},
                                    {"ready", observe(g.ready)},
                                    {"sequence", observe(g.sequence)}}));
    for (const auto &r : admission_state.responses)
      responses.push_back(Json::object({{"hop", observe(r.hop)},
                                        {"ready", observe(r.ready)},
                                        {"response", observe(r.response)},
                                        {"permit", observe(r.permit)},
                                        {"sent", r.sent}}));
    for (const auto &lane : admission_state.wire_busy)
      lanes.push_back(Json::object({{"connection", observe(lane.first)}, {"busy", lane.second}}));
    std::vector<Json> ledger_rows, drain_rows;
    for (auto h : hops()) {
      auto a = admission->inspect(h);
      Json admission_value;
      if (a) {
        const auto &v = a.value();
        admission_value = Json::object(
            {{"txn", observe(v.txn)},
             {"hop", observe(v.hop)},
             {"connection", observe(v.connection)},
             {"transport", observe(v.transport)},
             {"transportGeneration", observe(v.transport_generation)},
             {"inTime", observe(v.in_time)},
             {"request", observe(v.owned_request)},
             {"wireTerminal", v.wire_terminal},
             {"semanticTerminal", v.semantic_terminal},
             {"pending", v.pending},
             {"servicing", v.servicing},
             {"sequence", observe(v.sequence)},
             {"resetDeferred", v.reset_deferred},
             {"route", Json::object({{"upstream", observe(v.route.upstream)},
                                     {"downstream", observe(v.route.downstream)},
                                     {"originalAddress", observe(v.route.original_address)},
                                     {"localAddress", observe(v.route.local_address)}})}});
      } else
        admission_value = Json::object({{"error", observe(a.error())}});
      ledger_rows.push_back(Json::object(
          {{"admission", admission_value}, {"wire", queried(runtime.inspect_hop(h))}}));
    }
    for (const auto &r : runtime.drains().stop_report()) {
      std::vector<Json> hs;
      for (const auto &h : r.hops)
        hs.push_back(Json::object({{"hop", observe(h.hop)},
                                   {"wireTerminal", h.wire_terminal},
                                   {"timingConsumed", h.timing_consumed},
                                   {"cleanupReturned", h.cleanup_returned},
                                   {"callPin", h.call_pin}}));
      drain_rows.push_back(Json::object({{"transaction", observe(r.transaction)},
                                         {"instance", observe(r.instance)},
                                         {"epoch", observe(r.epoch)},
                                         {"parent", observe(r.parent)},
                                         {"hops", Json::array(hs)},
                                         {"localFinished", r.local_finished},
                                         {"cancelled", r.cancelled}}));
    }
    Json result_value;
    if (result) {
      auto ownership = runtime.results().inspect_ownership(result->reservation);
      Json counts = ownership
                        ? Json::object({{"consumers", observe(ownership.value().consumer_count)},
                                        {"pins", observe(ownership.value().pin_count)},
                                        {"published", ownership.value().published},
                                        {"ownerReleased", ownership.value().owner_released},
                                        {"publishing", ownership.value().publishing}})
                        : Json::object({{"error", observe(ownership.error())}});
      result_value =
          Json::object({{"read", queried(runtime.results().read(result->consumer))},
                        {"ready", queried(runtime.results().published_ready(result->consumer))},
                        {"ownership", counts},
                        {"alive", runtime.results().alive(result->reservation)}});
    }
    auto wake = runtime.next_wakeup();
    auto results = runtime.results().snapshot();
    std::vector<Json> result_slots, consumer_slots;
    for (const auto &s : results.result_slots)
      result_slots.push_back(Json::object(
          {{"identity", observe(s.identity)},
           {"alive", s.alive},
           {"producerAlive", s.producer_alive},
           {"publishing", s.publishing},
           {"value", observe(s.value)},
           {"ready", observe(s.ready)},
           {"consumerCount", observe(s.consumer_count)},
           {"pinCount", observe(s.pin_count)},
           {"create", Json::object({{"source", observe(s.create.source)},
                                    {"type", observe(s.create.type)},
                                    {"maxBytes", observe(s.create.max_bytes)},
                                    {"producerOwner", observe(s.create.producer_owner)},
                                    {"consumerOwner", observe(s.create.consumer_owner)}})}}));
    for (const auto &c : results.consumer_slots)
      consumer_slots.push_back(Json::object({{"identity", observe(c.identity)},
                                             {"result", observe(c.result)},
                                             {"active", c.active}}));
    return Json::object(
        {{"drainCounters",
          Json::object({{"nextReceipt", observe(drain_counters.next_receipt)},
                        {"nextReset", observe(drain_counters.next_reset)},
                        {"receiptCount", observe(drain_counters.receipt_count)},
                        {"capacity", observe(drain_counters.capacity)},
                        {"hopLimit", observe(drain_counters.hop_limit)},
                        {"responsibilities", observe(drain_counters.responsibilities)}})},
         {"protocolCounters",
          Json::object({{"nextGeneration", observe(protocol_counters.next_generation)},
                        {"ledgers", observe(protocol_counters.ledgers)},
                        {"pendingCalls", observe(protocol_counters.pending_calls)},
                        {"preparedBindings", observe(protocol_counters.prepared_bindings)},
                        {"preparedRetirements", observe(protocol_counters.prepared_retirements)},
                        {"ledgerCapacity", observe(protocol_counters.ledger_capacity)},
                        {"ticketCapacity", observe(protocol_counters.ticket_capacity)},
                        {"callsPerLedger", observe(protocol_counters.calls_per_ledger)},
                        {"requestLaneFree", protocol->request_lane_free(connection)},
                        {"responseLaneFree", protocol->response_lane_free(connection)}})},
         {"runtimeCounters",
          Json::object({{"nextCall", observe(counters.next_call)},
                        {"wakeGeneration", observe(counters.wake_generation)},
                        {"allocatedCalls", observe(counters.allocated_calls)},
                        {"intentCapacity", observe(counters.intent_capacity)},
                        {"callCapacity", observe(counters.call_capacity)},
                        {"ledgerCapacity", observe(counters.ledger_capacity)},
                        {"drainCapacity", observe(counters.drain_capacity)},
                        {"preparedIntents", observe(counters.prepared_intents)},
                        {"committedIntents", observe(counters.committed_intents)}})},
         {"admissionStore",
          Json::object(
              {{"nextGeneration", observe(admission_state.next_generation)},
               {"nextSequence", observe(admission_state.next_sequence)},
               {"revision", observe(admission_state.revision)},
               {"prepared", admission_state.prepared},
               {"activeServices", observe(admission_state.active_services)},
               {"reservedResponses", observe(admission_state.reserved_responses)},
               {"responseOrder", observe(admission_state.order)},
               {"limits",
                Json::object({{"transactions", observe(admission_state.limits.transactions)},
                              {"hops", observe(admission_state.limits.hops)},
                              {"services", observe(admission_state.limits.services)},
                              {"responses", observe(admission_state.limits.responses)},
                              {"gates", observe(admission_state.limits.request_gate_tickets)}})},
               {"gates", Json::array(gates)},
               {"responses", Json::array(responses)},
               {"wireBusy", Json::array(lanes)}})},
         {"setup", setup.json()},
         {"ledgers", Json::array(ledger_rows)},
         {"drains", Json::array(drain_rows)},
         {"drainOutstanding", observe(runtime.drains().outstanding())},
         {"activeServices", observe(admission->active_services())},
         {"intents", observe(runtime.pending_intents())},
         {"result", result_value},
         {"resultSlots", observe(runtime.results().occupied())},
         {"resultStore",
          Json::object({{"results", Json::array(result_slots)},
                        {"consumers", Json::array(consumer_slots)},
                        {"nextResultGeneration", observe(results.next_result_generation)},
                        {"nextConsumerGeneration", observe(results.next_consumer_generation)},
                        {"pinCount", observe(results.pin_count)},
                        {"pinLimit", observe(results.pin_limit)}})},
         {"ack", ack ? observe(*ack) : Json()},
         {"stopped", runtime.stopped()},
         {"stopDetail", runtime.stop_detail()},
         {"wake", wake
                      ? Json::object({{"time", observe(wake->time)}, {"turn", observe(wake->turn)}})
                      : Json()}});
  }
};
} // namespace protocol_fixture_detail

struct OwnedDrainSeed {
  std::shared_ptr<protocol_fixture_detail::State> state;
  Handle transaction, hop, receipt;
};

// Reusable setup performs a real wire exchange before acquiring an owned cancellation receipt.
inline Expected<OwnedDrainSeed>
seed_owned_protocol_drain(Runtime &runtime, const ExecutionContext &context,
                          ConnectionId connection, TransportId transport,
                          const PayloadSnapshot &payload,
                          CancelReason reason = CancelReason::User) {
  using namespace protocol_fixture_detail;
  try {
    auto state = std::make_shared<State>(runtime, context, connection);
    AdmissionRequest request;
    request.connection = connection;
    request.transport = transport;
    request.owner = context.owner;
    request.in_time = context.ready.time;
    request.request = payload;
    request.route.original_address = payload.address;
    request.route.local_address = payload.address;
    state->seeded = need(state->admission->create_initiator(request));
    auto &engine = *need(runtime.protocol(context.instance));
    need(engine.bind_ledger(state->seeded->hop, connection, transport, 1));
    need(runtime.drains().register_responsibility(
        {state->seeded->txn,
         context.instance,
         context.epoch,
         {},
         {{state->seeded->hop, false, false, false, false}},
         false,
         false}));
    need(runtime.track_cleanup(state->seeded->txn, state->seeded->hop, true, payload,
                               state->admission.get()));
    auto id = need(runtime.allocate_call_id(CallOrigin::ExternalIngress));
    WireCall call{id,        connection,         transport, Flow::Forward,
                  begin_req, context.ready.time, {},        payload};
    WireReturn reply{Sync::Updated,
                     begin_resp,
                     {},
                     ResponseSnapshot{ResponseStatus::Ok, payload.data, false, {}}};
    state->setup.add("call", call);
    auto ticket = need(engine.begin_call(state->seeded->hop, call));
    state->setup.add("return", reply);
    auto exchange = need(engine.end_call(ticket, reply));
    state->setup.add("exchange", exchange);
    need(runtime.drains().observe_wire(state->seeded->hop, exchange.wire_terminal, false));
    need(runtime.release_call_id(id));
    state->ack = need(engine.ack_cell(state->seeded->hop, context.epoch));
    auto cancelled = need(runtime.cancel_local(state->seeded->txn, reason));
    if (cancelled.local_only || !cancelled.receipt)
      return fail(ErrorCode::ProtocolViolation, "wire-started cancellation did not own a receipt");
    return OwnedDrainSeed{state, state->seeded->txn, state->seeded->hop, *cancelled.receipt};
  } catch (const std::exception &error) {
    return fail(ErrorCode::InvalidArgument, error.what());
  }
}
inline Expected<ProviderFixture> make_protocol_fixture(Runtime &runtime,
                                                       CoreRuntimeBackend &backend,
                                                       const exec::Project &project,
                                                       const FixtureConfig &config) {
  using namespace protocol_fixture_detail;
  using exec::Op;
  try {
    bool found = false;
    std::vector<Op> operations;
    std::optional<std::uint32_t> read_type;
    for (const auto &s : project.services) {
      if (s.op == Op::NewTransaction || s.op == Op::StagePhase || s.op == Op::AckResponse ||
          s.op == Op::CancelLocal || s.op == Op::ResultGet || s.op == Op::ResultRelease)
        if (std::find(operations.begin(), operations.end(), s.op) == operations.end())
          operations.push_back(s.op);
      if (s.op == Op::ResultGet && !s.result_types.empty())
        read_type = s.result_types[0];
    }
    if (operations.empty())
      return fail(ErrorCode::Unsupported, "no protocol/result fixture operation");
    if (operations.size() != 1 && !env(config, "operation"))
      return fail(ErrorCode::InvalidArgument,
                  "multi-service fixture requires explicit operation tag");
    const auto selected = static_cast<Op>(
        number(config, "operation", static_cast<std::uint64_t>(operations.front())));
    if (std::find(operations.begin(), operations.end(), selected) == operations.end())
      return fail(ErrorCode::InvalidArgument, "fixture operation absent from actual services");
    const bool stage = selected == Op::StagePhase, ack = selected == Op::AckResponse,
               cancel = selected == Op::CancelLocal;
    const bool result_ops = selected == Op::ResultGet || selected == Op::ResultRelease;
    auto connection = number(config, "connection", config.context.connection.value);
    if (connection > UINT32_MAX)
      return fail(ErrorCode::InvalidArgument, "protocol fixture connection overflow");
    auto state = std::make_shared<State>(runtime, config.context,
                                         ConnectionId{static_cast<std::uint32_t>(connection)});
    ProviderFixture fixture;
    fixture.inputs = config.inputs;
    if (stage || ack || cancel) {
      if (fixture.inputs.empty())
        return fail(ErrorCode::InvalidArgument, "seeded protocol fixture requires txn input slot");
      AdmissionRequest request;
      request.connection = state->connection;
      request.transport = TransportId{number(config, "transport", 1)};
      request.owner = config.context.owner;
      request.in_time = config.context.ready.time;
      request.request.command = Command::Read;
      request.request.address = number(config, "address", 0);
      request.request.data = Bytes{4, 9};
      if (auto bytes = env(config, "requestBytes")) {
        auto data = std::get_if<Bytes>(&bytes->data);
        if (!data)
          return fail(ErrorCode::TypeMismatch, "requestBytes must be Bytes");
        request.request.data = *data;
      }
      request.request.streaming_width = std::max<std::size_t>(1, request.request.data.size());
      request.route.original_address = request.request.address;
      request.route.local_address = request.request.address;
      state->seeded = need(state->admission->create_initiator(request));
      auto &engine = *need(runtime.protocol(config.context.instance));
      need(engine.bind_ledger(state->seeded->hop, request.connection, request.transport, 1));
      need(runtime.drains().register_responsibility(
          {state->seeded->txn,
           config.context.instance,
           config.context.epoch,
           {},
           {{state->seeded->hop, false, false, false, false}},
           false,
           false}));
      need(runtime.track_cleanup(state->seeded->txn, state->seeded->hop, true, request.request,
                                 state->admission.get()));
      fixture.inputs[0] = remap_capability(fixture.inputs[0], env(config, "transactionIdentity"),
                                           state->seeded->txn);
      auto initial = number(config, "protocolState", ack ? 1 : 0);
      if (initial > 2)
        return fail(ErrorCode::InvalidArgument, "protocolState must be Idle/Response/Terminal");
      if (initial) {
        auto id = need(runtime.allocate_call_id(CallOrigin::ExternalIngress));
        WireCall call{id,
                      request.connection,
                      request.transport,
                      Flow::Forward,
                      begin_req,
                      config.context.ready.time,
                      {},
                      request.request};
        WireReturn reply{initial == 1 ? Sync::Updated : Sync::Completed,
                         initial == 1 ? std::optional<PhaseId>{begin_resp} : std::nullopt,
                         {},
                         ResponseSnapshot{ResponseStatus::Ok, request.request.data, false, {}}};
        state->setup.add("call", call);
        auto ticket = need(engine.begin_call(state->seeded->hop, call));
        state->setup.add("return", reply);
        auto exchange = need(engine.end_call(ticket, reply));
        state->setup.add("exchange", exchange);
        need(runtime.drains().observe_wire(state->seeded->hop, exchange.wire_terminal, false));
        if (exchange.wire_terminal)
          need(runtime.drains().consume_terminal(state->seeded->hop));
        need(runtime.release_call_id(id));
      }
      if (initial == 1)
        state->ack = need(engine.ack_cell(state->seeded->hop, config.context.epoch));
    }
    if (result_ops) {
      if (fixture.inputs.size() < 2 || project.programs.empty())
        return fail(ErrorCode::InvalidArgument,
                    "result fixture needs two handle input slots and program");
      auto type = number(config, "resultType", read_type.value_or(UINT32_MAX));
      if (type >= project.types.size())
        return fail(ErrorCode::InvalidArgument, "result fixture requires concrete resultType");
      state->result_source = need(
          runtime.processes().create(ProgramId{project.programs.front().id}, config.context.owner));
      state->result = need(runtime.results().reserve(
          {*state->result_source, TypeId{static_cast<std::uint32_t>(type)}, 4096,
           config.context.owner, config.context.owner}));
      if (boolean(config, "resultPublished", true)) {
        auto value = env(config, "resultValue");
        need(runtime.results().publish(state->result->reservation,
                                       value ? *value : Value{Bytes{4, 9}},
                                       {*state->result_source, config.context.ready, true}));
      }
      if (boolean(config, "resultReleased", false))
        need(runtime.results().release(state->result->consumer));
      fixture.inputs[0] = remap_capability(fixture.inputs[0], env(config, "resultIdentity"),
                                           state->result->consumer.result);
      fixture.inputs[1] = remap_capability(fixture.inputs[1], env(config, "consumerIdentity"),
                                           state->result->consumer.consumer);
    }
    for (const auto &s : project.services) {
      if (s.op == Op::NewTransaction || s.op == Op::StagePhase || s.op == Op::AckResponse) {
        protocol_abi(s, project);
        need(register_protocol_service(backend, runtime, *state->admission, s, {4096, {}}));
        found = true;
      } else if (s.op == Op::CancelLocal || s.op == Op::ResultGet || s.op == Op::ResultRelease) {
        need(backend.register_core(s, project));
        found = true;
      }
    }
    if (!found)
      return fail(ErrorCode::Unsupported, "no protocol/result opcode provider in project");
    fixture.lifetime.push_back(state);
    fixture.snapshot = [state] { return state->snapshot(); };
    fixture.identities = [state] { return state->identities(); };
    return fixture;
  } catch (const std::exception &error) {
    return fail(ErrorCode::InvalidArgument, error.what());
  }
}
} // namespace leanat::opcode_test
