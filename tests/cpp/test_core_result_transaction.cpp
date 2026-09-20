#include "leanat/admission.hpp"
#include "leanat/runtime_services.hpp"
#include "test_support.hpp"
using namespace leanat;
struct ResultHost : RuntimeHost {
  Expected<WireReturn> transport(const SendIntent &) override {
    return fail(ErrorCode::ExternalFailure, "unused host");
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
void repeated_active_cancel() {
  ResultHost host;
  RuntimeConfig cfg;
  cfg.domain = DomainId{1};
  cfg.instance = InstanceId{1};
  cfg.connections = {ConnectionId{1}};
  cfg.descriptor_identity = "repeat-cancel";
  Runtime runtime(cfg, host);
  LEANAT_CHECK(
      runtime.start({cfg.domain, cfg.descriptor_identity, cfg.connections, true, true, {}, {}}));
  AdmissionStore admission(cfg.domain, cfg.instance, {2, 2, 2, 2, 2});
  AdmissionRequest request;
  request.connection = ConnectionId{1};
  request.transport = TransportId{1};
  request.owner = 7;
  auto admitted = admission.create_initiator(request);
  LEANAT_CHECK(admitted);
  auto txn = admitted.value().txn;
  auto hop = admitted.value().hop;
  LEANAT_CHECK(runtime.protocol().bind_ledger(hop, request.connection, request.transport, 1));
  WireCall call;
  call.id = CallId{1};
  call.connection = request.connection;
  call.transport = request.transport;
  call.phase = begin_req;
  auto ticket = runtime.protocol().begin_call(hop, call);
  LEANAT_CHECK(ticket);
  LEANAT_CHECK(runtime.protocol().end_call(ticket.value(), WireReturn{}));
  LEANAT_CHECK(runtime.protocol().inspect(hop).value().state == WireState::Request);
  LEANAT_CHECK(runtime.drains().register_responsibility(
      {txn, cfg.instance, 0, {}, {{hop, false, false, false, true}}, false, false}));
  exec::Project project;
  project.types = {
      {exec::TypeKind::Handle, static_cast<std::uint64_t>(HandleKind::Transaction), {}, {}},
      {exec::TypeKind::Bits, 64, {}, {}},
      {exec::TypeKind::Unit, 0, {}, {}}};
  exec::ServiceSignature signature;
  signature.op = exec::Op::CancelLocal;
  signature.input_types = {0, 1};
  signature.result_types = {2};
  signature.context_mask = 3;
  signature.effect_mask = exec::required_effect(signature.op);
  signature.provider_key = "leanat.core.transaction.cancel";
  signature.provider_version = "1";
  const std::string hash = "fd95191dfcd83497a02c4edcca1089d64b98ba014a288bf1800bc12df81a6956";
  for (std::size_t n = 0; n < 32; ++n)
    signature.abi_hash[n] =
        static_cast<std::uint8_t>(std::stoul(hash.substr(n * 2, 2), nullptr, 16));
  CoreRuntimeBackend backend(runtime, project);
  LEANAT_CHECK(backend.register_core(signature));
  LEANAT_CHECK(backend.freeze());
  ExecutionContext ctx;
  ctx.domain = cfg.domain;
  ctx.instance = cfg.instance;
  ctx.owner = 7;
  for (unsigned segment = 0; segment < 2; ++segment) {
    EventTxn tx({}, ctx);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      auto cancelled = backend.invoke(signature, {Value{txn}, Value{std::uint64_t{0}}}, ctx, tx);
      LEANAT_CHECK(cancelled && cancelled.value() == std::vector<Value>{Value{}});
    }
    LEANAT_CHECK(tx.commit());
    LEANAT_CHECK(runtime.drains().counter_snapshot().receipt_count == 0);
    LEANAT_CHECK(runtime.drains().counter_snapshot().next_receipt == 2);
    LEANAT_CHECK(runtime.drains().outstanding() == 1);
  }
  LEANAT_CHECK(runtime.drains().observe_wire(hop, true, false));
  LEANAT_CHECK(runtime.drains().consume_terminal(hop));
  EventTxn stale({}, ctx);
  auto cancelled = backend.invoke(signature, {Value{txn}, Value{std::uint64_t{0}}}, ctx, stale);
  LEANAT_CHECK(!cancelled && cancelled.error().code == ErrorCode::StaleHandle);
}
int main() {
  repeated_active_cancel();
  ResultHost host;
  RuntimeConfig cfg;
  cfg.domain = DomainId{1};
  cfg.instance = InstanceId{1};
  Runtime runtime(cfg, host);
  exec::Project project;
  project.types = {
      {exec::TypeKind::Handle, static_cast<std::uint64_t>(HandleKind::Result), {}, {}},
      {exec::TypeKind::Handle, static_cast<std::uint64_t>(HandleKind::Consumer), {}, {}},
      {exec::TypeKind::Bits, 64, {}, {}},
      {exec::TypeKind::Unit, 0, {}, {}}};
  auto signature = [](bool release) {
    exec::ServiceSignature s;
    s.id = release ? 1 : 0;
    s.op = release ? exec::Op::ResultRelease : exec::Op::ResultGet;
    s.input_types = {0, 1};
    s.result_types = {release ? 3u : 2u};
    s.context_mask = 3;
    s.effect_mask = exec::required_effect(s.op);
    s.provider_key = release ? "leanat.core.result.release" : "leanat.core.result.get";
    s.provider_version = "1";
    std::string hash = release ? "21c278f5ed404ee7e67528ae212eab663d65b69ea2c08440980a21a244d673e7"
                               : "f40fcd8a22c18448a9b8b9bef88c7eca9dc150e28444ab9e4b2465ee6de587d0";
    for (std::size_t n = 0; n < 32; ++n)
      s.abi_hash[n] = static_cast<std::uint8_t>(std::stoul(hash.substr(n * 2, 2), nullptr, 16));
    return s;
  };
  auto get = signature(false), release = signature(true);
  CoreRuntimeBackend backend(runtime, project);
  LEANAT_CHECK(backend.register_core(get));
  LEANAT_CHECK(backend.register_core(release));
  LEANAT_CHECK(backend.freeze());
  auto hop = runtime.protocol().create_ledger(ConnectionId{1}, TransportId{1}, 1, 7);
  LEANAT_CHECK(hop);
  auto stored = runtime.results().reserve({hop.value(), TypeId{2}, 64, 7, 7});
  LEANAT_CHECK(stored);
  LEANAT_CHECK(runtime.results().publish(stored.value().reservation, Value{std::uint64_t{42}},
                                         {hop.value(), {}, true}));
  ExecutionContext ctx;
  ctx.domain = cfg.domain;
  ctx.instance = cfg.instance;
  ctx.owner = 7;
  auto h = stored.value().consumer;
  {
    EventTxn tx({}, ctx);
    LEANAT_CHECK(backend.invoke(release, {Value{h.result}, Value{h.consumer}}, ctx, tx));
    LEANAT_CHECK(!backend.invoke(get, {Value{h.result}, Value{h.consumer}}, ctx, tx));
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(runtime.results().read(h));
  }
  {
    EventTxn tx({}, ctx);
    auto moved = runtime.results().prepare_transfer(tx, h, 7);
    LEANAT_CHECK(moved);
    LEANAT_CHECK(!backend.invoke(get, {Value{h.result}, Value{h.consumer}}, ctx, tx));
    auto value =
        backend.invoke(get, {Value{moved.value().result}, Value{moved.value().consumer}}, ctx, tx);
    LEANAT_CHECK(value && value.value() == std::vector<Value>{Value{std::uint64_t{42}}});
    LEANAT_CHECK(tx.commit());
    LEANAT_CHECK(!runtime.results().read(h));
    LEANAT_CHECK(runtime.results().read(moved.value()));
    LEANAT_CHECK(runtime.results().release(moved.value()));
  }
  LEANAT_CHECK(runtime.results().release_owner(stored.value().reservation));
  LEANAT_CHECK(runtime.results().occupied() == 0);
}
