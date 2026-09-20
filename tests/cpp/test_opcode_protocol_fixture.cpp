#include "../../tests/conformance/opcode_protocol_fixture.hpp"
#include "test_support.hpp"
using namespace leanat;
using namespace leanat::opcode_test;
struct FixtureHost final : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return WireReturn{};
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
int main() {
  FixtureHost host;
  RuntimeConfig config;
  config.domain = DomainId{1};
  config.instance = InstanceId{1};
  config.connections = {ConnectionId{1}};
  config.descriptor_identity = "protocol-fixture";
  Runtime runtime(config, host);
  LEANAT_CHECK(
      runtime.start({config.domain, config.descriptor_identity, config.connections, true, true}));
  AdmissionStore admission(config.domain, config.instance, {8, 8, 1, 1, 8});
  AdmissionRequest request;
  request.connection = ConnectionId{1};
  request.owner = 9;
  request.transport = TransportId{1};
  LEANAT_CHECK(admission.create_initiator(request));
  request.transport = TransportId{2};
  auto live = admission.create_initiator(request);
  LEANAT_CHECK(live && live.value().txn.generation == 3);
  CoreRuntimeBackend backend(runtime);
  exec::ServiceSignature phase;
  phase.id = 1;
  phase.op = exec::Op::StagePhase;
  phase.input_types = {0, 0, 0, 0};
  phase.result_types = {0};
  phase.context_mask = 3;
  phase.effect_mask = exec::required_effect(phase.op);
  phase.provider_key = "leanat.protocol";
  phase.provider_version = "1";
  LEANAT_CHECK(register_protocol_service(backend, runtime, admission, phase, {4096, {}}));
  LEANAT_CHECK(backend.freeze());
  ExecutionContext context;
  context.domain = config.domain;
  context.instance = config.instance;
  context.connection = ConnectionId{1};
  context.owner = 9;
  Handle logical{HandleKind::Transaction, config.domain, 12, 0, 1, 9};
  Value baseline{logical};
  for (bool wrong_slot : {false, true}) {
    auto forged = logical;
    if (wrong_slot)
      forged.slot = 1;
    else
      forged.generation = 3; // The unconverted forged number equals the actual live generation.
    auto mapped =
        protocol_fixture_detail::remap_capability(Value{forged}, &baseline, live.value().txn);
    auto capability = std::get<Handle>(mapped.data);
    LEANAT_CHECK(capability != live.value().txn);
    EventTxn tx(SegmentBudget{}, context);
    auto invoked = backend.invoke(
        phase, {mapped, Value{std::uint64_t{1}}, Value{std::uint64_t{1}}, Value{std::uint64_t{0}}},
        context, tx);
    LEANAT_CHECK(!invoked && invoked.error().code == ErrorCode::StaleHandle);
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(runtime.pending_intents().empty());
  }
  LEANAT_CHECK(
      runtime.protocol().bind_ledger(live.value().hop, request.connection, request.transport));
  {
    EventTxn tx(SegmentBudget{}, context);
    auto point = tx.checkpoint();
    LEANAT_CHECK(point);
    LEANAT_CHECK(admission.stage_retire_unstarted(tx, live.value().hop, runtime.protocol()));
    LEANAT_CHECK(admission.lookup(live.value().txn, request.connection));
    LEANAT_CHECK(runtime.protocol().inspect(live.value().hop));
    LEANAT_CHECK(tx.rollback(std::move(point.value())));
    LEANAT_CHECK(admission.lookup(live.value().txn, request.connection));
    LEANAT_CHECK(runtime.protocol().inspect(live.value().hop));
    LEANAT_CHECK(admission.stage_retire_unstarted(tx, live.value().hop, runtime.protocol()));
    LEANAT_CHECK(runtime.commit_segment(tx));
    LEANAT_CHECK(!admission.lookup(live.value().txn, request.connection));
    LEANAT_CHECK(!runtime.protocol().inspect(live.value().hop));
  }
  {
    EventTxn tx(SegmentBudget{}, context);
    auto update = admission.prepare(tx);
    LEANAT_CHECK(update);
    request.transport = TransportId{3};
    auto newly_created = update.value()->draft().create_initiator(request);
    LEANAT_CHECK(newly_created);
    LEANAT_CHECK(tx.stage_participant(std::move(update.value())));
    LEANAT_CHECK(runtime.protocol().prepare_bind_ledger(tx, newly_created.value().hop,
                                                        request.connection, request.transport));
    LEANAT_CHECK(
        admission.stage_retire_unstarted(tx, newly_created.value().hop, runtime.protocol()));
    LEANAT_CHECK(runtime.commit_segment(tx));
    LEANAT_CHECK(!admission.lookup(newly_created.value().txn, request.connection));
    LEANAT_CHECK(!runtime.protocol().inspect(newly_created.value().hop));
  }
  {
    Runtime owned_runtime(config, host);
    LEANAT_CHECK(owned_runtime.start(
        {config.domain, config.descriptor_identity, config.connections, true, true}));
    PayloadSnapshot payload;
    payload.command = Command::Read;
    payload.data = {4, 9};
    payload.streaming_width = 2;
    auto owned = seed_owned_protocol_drain(owned_runtime, context, ConnectionId{1}, TransportId{10},
                                           payload);
    LEANAT_CHECK(owned);
    auto status = owned_runtime.drains().inspect(owned.value().receipt);
    LEANAT_CHECK(status && status.value().state == DrainState::Pending);
    LEANAT_CHECK(status.value().hops.size() == 1);
    LEANAT_CHECK(status.value().hops.front().hop == owned.value().hop);
    auto wire = owned_runtime.protocol().inspect(owned.value().hop);
    LEANAT_CHECK(wire && wire.value().state == WireState::Response);
    LEANAT_CHECK(owned_runtime.drains().release_receipt(owned.value().receipt));
    LEANAT_CHECK(!owned_runtime.drains().inspect(owned.value().receipt));
    LEANAT_CHECK(owned_runtime.drains().outstanding() == 1);
    auto cleanup = owned_runtime.pending_intents();
    LEANAT_CHECK(cleanup.size() == 1 && cleanup.front().phase == end_resp);
    LEANAT_CHECK(cleanup.front().txn == owned.value().transaction);
    LEANAT_CHECK(cleanup.front().payload.data == payload.data);
    LEANAT_CHECK(owned_runtime.counter_snapshot().next_call == 3);
  }
  return 0;
}
