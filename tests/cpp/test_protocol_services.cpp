#include "leanat/protocol_services.hpp"
#include "test_support.hpp"
using namespace leanat;
struct Host final : RuntimeHost {
  Runtime *runtime{};
  std::size_t calls{};
  std::optional<WakePoint> wake;
  Expected<WireReturn> transport(const SendIntent &i) override {
    ++calls;
    WireCall c;
    c.id = i.call_id;
    c.connection = i.connection;
    c.transport = i.transport;
    c.flow = i.flow;
    c.phase = i.phase;
    c.call_time = runtime->now();
    c.request = i.payload;
    auto started = runtime->start_outbound(i, c);
    if (!started)
      return started.error();
    WireReturn r;
    r.sync = Sync::Completed;
    r.response = ResponseSnapshot{ResponseStatus::Ok, i.payload.data, false, {}};
    return r;
  }
  void arm(std::optional<WakePoint> w) override {
    wake = w;
  }
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
};
static exec::ServiceSignature signature(unsigned id, exec::Op op, std::size_t inputs) {
  exec::ServiceSignature s;
  s.id = id;
  s.op = op;
  s.input_types.resize(inputs);
  s.result_types = {0};
  s.context_mask = 3;
  s.effect_mask = exec::required_effect(op);
  s.provider_key = "leanat.protocol";
  s.provider_version = "1";
  return s;
}
int main() {
  Host host;
  RuntimeConfig config;
  config.domain = DomainId{1};
  config.instance = InstanceId{1};
  config.drain_capacity = 4;
  config.connections = {ConnectionId{1}};
  config.descriptor_identity = "protocol-services";
  Runtime runtime(config, host);
  host.runtime = &runtime;
  LEANAT_CHECK(
      runtime.start({config.domain, config.descriptor_identity, config.connections, true, true}));
  AdmissionStore admission(DomainId{1}, InstanceId{1}, {4, 4, 1, 1, 4});
  CoreRuntimeBackend backend(runtime);
  auto create = signature(101, exec::Op::NewTransaction, 6),
       phase = signature(102, exec::Op::StagePhase, 4);
  LEANAT_CHECK(register_protocol_service(backend, runtime, admission, create, {32, {}}));
  LEANAT_CHECK(register_protocol_service(backend, runtime, admission, phase, {32, {}}));
  LEANAT_CHECK(backend.freeze());
  ExecutionContext context;
  context.domain = DomainId{1};
  context.instance = InstanceId{1};
  context.connection = ConnectionId{1};
  context.owner = 7;
  context.ready = {Tick{10}, 0};
  auto start = [&](EventTxn &tx, unsigned transport) -> Expected<Handle> {
    auto result =
        backend.invoke(create,
                       {Value{std::uint64_t{1}}, Value{std::uint64_t{transport}},
                        Value{std::uint64_t{1}}, Value{static_cast<std::uint64_t>(Command::Read)},
                        Value{std::uint64_t{0x100}}, Value{Bytes{4, 5}}},
                       context, tx);
    if (!result)
      return result.error();
    return std::get<Handle>(result.value()[0].data);
  };
  Handle stale;
  {
    EventTxn tx(SegmentBudget{}, context);
    auto created = start(tx, 1);
    LEANAT_CHECK(created);
    stale = created.value();
    LEANAT_CHECK(!admission.lookup(stale, ConnectionId{1}));
    auto admission_update = dynamic_cast<AdmissionUpdate *>(tx.participant(&admission));
    LEANAT_CHECK(admission_update);
    auto staged_hop = admission_update->draft().lookup(stale, ConnectionId{1});
    LEANAT_CHECK(staged_hop);
    auto staged_ledger = runtime.protocol().prepared_inspect(tx, staged_hop.value().hop);
    LEANAT_CHECK(staged_ledger && staged_ledger.value().state == WireState::Idle &&
                 staged_ledger.value().call_ordinal == 0);
    EventTxn foreign(SegmentBudget{}, context);
    auto forbidden = runtime.protocol().prepared_inspect(foreign, staged_hop.value().hop);
    LEANAT_CHECK(!forbidden && forbidden.error().code == ErrorCode::WrongOwner);
    LEANAT_CHECK(foreign.discard());
    auto before_cancel = tx.checkpoint();
    LEANAT_CHECK(before_cancel);
    auto cancelled_new = runtime.stage_cancel_local(tx, stale, CancelReason::User);
    LEANAT_CHECK(cancelled_new && cancelled_new.value().local_only &&
                 !cancelled_new.value().receipt);
    LEANAT_CHECK(tx.rollback(std::move(before_cancel.value())));
    auto staged = backend.invoke(phase,
                                 {Value{stale}, Value{std::uint64_t{1}},
                                  Value{std::uint64_t{begin_req.value}}, Value{std::uint64_t{10}}},
                                 context, tx);
    LEANAT_CHECK(staged);
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(!runtime.protocol().prepared_inspect(tx, staged_hop.value().hop));
    LEANAT_CHECK(!admission.lookup(stale, ConnectionId{1}));
    LEANAT_CHECK(runtime.drains().outstanding() == 0 && host.calls == 0);
  }
  Handle txn;
  {
    EventTxn tx(SegmentBudget{}, context);
    auto created = start(tx, 1);
    LEANAT_CHECK(created && created.value() != stale);
    txn = created.value();
    auto before_send = tx.checkpoint();
    LEANAT_CHECK(before_send);
    auto staged = backend.invoke(phase,
                                 {Value{txn}, Value{std::uint64_t{1}},
                                  Value{std::uint64_t{begin_req.value}}, Value{std::uint64_t{10}}},
                                 context, tx);
    LEANAT_CHECK(staged);
    LEANAT_CHECK(tx.rollback(std::move(before_send.value())));
    staged = backend.invoke(phase,
                            {Value{txn}, Value{std::uint64_t{1}},
                             Value{std::uint64_t{begin_req.value}}, Value{std::uint64_t{10}}},
                            context, tx);
    LEANAT_CHECK(staged);
    auto committed = runtime.commit_segment(tx);
    if (!committed)
      std::cerr << committed.error().message << '\n';
    LEANAT_CHECK(committed);
  }
  for (unsigned i = 0; i < 10 && host.calls == 0; ++i) {
    auto pump = runtime.pump_batch(Tick{10}, 100);
    LEANAT_CHECK(pump);
  }
  LEANAT_CHECK(host.calls == 1 && !runtime.stopped());
  auto hop = admission.lookup(txn, ConnectionId{1});
  LEANAT_CHECK(hop);
  LEANAT_CHECK(runtime.protocol().inspect(hop.value().hop).value().state == WireState::Terminal);
  Handle cancelled_txn;
  {
    EventTxn tx(SegmentBudget{}, context);
    auto created = start(tx, 2);
    LEANAT_CHECK(created);
    cancelled_txn = created.value();
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  auto request = [&](EventTxn &tx) {
    return backend.invoke(phase,
                          {Value{cancelled_txn}, Value{std::uint64_t{1}},
                           Value{std::uint64_t{begin_req.value}}, Value{std::uint64_t{10}}},
                          context, tx);
  };
  {
    EventTxn tx(SegmentBudget{}, context);
    LEANAT_CHECK(runtime.stage_cancel_local(tx, cancelled_txn, CancelReason::User));
    auto rejected = request(tx);
    LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::StaleHandle);
    LEANAT_CHECK(tx.discard());
    LEANAT_CHECK(!runtime.drains().is_cancelled(cancelled_txn));
  }
  {
    EventTxn tx(SegmentBudget{}, context);
    LEANAT_CHECK(request(tx));
    LEANAT_CHECK(tx.discard());
  }
  {
    EventTxn tx(SegmentBudget{}, context);
    LEANAT_CHECK(runtime.stage_cancel_local(tx, cancelled_txn, CancelReason::User));
    LEANAT_CHECK(runtime.commit_segment(tx));
  }
  {
    EventTxn tx(SegmentBudget{}, context);
    auto rejected = request(tx);
    LEANAT_CHECK(!rejected && rejected.error().code == ErrorCode::StaleHandle);
    LEANAT_CHECK(tx.discard());
  }
  LEANAT_CHECK(host.calls == 1 && runtime.pending_intents().empty());
  for (unsigned n = 0; n < 6; ++n) {
    EventTxn tx(SegmentBudget{}, context);
    auto created = start(tx, 100 + n);
    LEANAT_CHECK(created);
    auto staged_admission = dynamic_cast<AdmissionUpdate *>(tx.participant(&admission));
    LEANAT_CHECK(staged_admission);
    auto created_hop = staged_admission->draft().lookup(created.value(), ConnectionId{1});
    LEANAT_CHECK(created_hop);
    auto cancelled = runtime.stage_cancel_local(tx, created.value(), CancelReason::User);
    LEANAT_CHECK(cancelled && cancelled.value().local_only && !cancelled.value().receipt);
    LEANAT_CHECK(runtime.commit_segment(tx));
    LEANAT_CHECK(!admission.lookup(created.value(), ConnectionId{1}));
    LEANAT_CHECK(!runtime.protocol().inspect(created_hop.value().hop));
  }
  {
    Host bounded_host;
    auto bounded_config = config;
    bounded_config.drain_capacity = 1;
    Runtime bounded_runtime(bounded_config, bounded_host);
    bounded_host.runtime = &bounded_runtime;
    LEANAT_CHECK(bounded_runtime.start({bounded_config.domain, bounded_config.descriptor_identity,
                                        bounded_config.connections, true, true}));
    AdmissionStore bounded_admission(config.domain, config.instance, {2, 2, 1, 1, 2});
    CoreRuntimeBackend bounded_backend(bounded_runtime);
    LEANAT_CHECK(register_protocol_service(bounded_backend, bounded_runtime, bounded_admission,
                                           create, {32, {}}));
    LEANAT_CHECK(register_protocol_service(bounded_backend, bounded_runtime, bounded_admission,
                                           phase, {32, {}}));
    LEANAT_CHECK(bounded_backend.freeze());
    for (unsigned n = 0; n < 2; ++n) {
      EventTxn tx(SegmentBudget{}, context);
      auto created = bounded_backend.invoke(create,
                                            {Value{std::uint64_t{1}}, Value{std::uint64_t{10 + n}},
                                             Value{std::uint64_t{1}}, Value{std::uint64_t{0}},
                                             Value{std::uint64_t{0}}, Value{Bytes{4, 9}}},
                                            context, tx);
      if (!n) {
        LEANAT_CHECK(created);
        LEANAT_CHECK(bounded_runtime.commit_segment(tx));
      } else {
        LEANAT_CHECK(!created && created.error().code == ErrorCode::Capacity);
        LEANAT_CHECK(tx.discard());
      }
    }
    LEANAT_CHECK(bounded_admission.snapshot().next_generation == 5);
    LEANAT_CHECK(bounded_runtime.protocol().counter_snapshot().ledgers == 1);
    LEANAT_CHECK(bounded_runtime.drains().counter_snapshot().responsibilities == 1);
    auto transaction = bounded_runtime.drains().stop_report().front().transaction;
    EventTxn future(SegmentBudget{}, context);
    LEANAT_CHECK(bounded_backend.invoke(phase,
                                        {Value{transaction}, Value{std::uint64_t{1}},
                                         Value{std::uint64_t{1}}, Value{std::uint64_t{100}}},
                                        context, future));
    auto cancellation = bounded_runtime.stage_cancel_local(future, transaction, CancelReason::User);
    LEANAT_CHECK(cancellation && cancellation.value().local_only);
    LEANAT_CHECK(bounded_runtime.commit_segment(future));
    LEANAT_CHECK(bounded_host.calls == 0);
    LEANAT_CHECK(bounded_runtime.pending_intents().size() == 1);
    LEANAT_CHECK(bounded_admission.lookup(transaction, ConnectionId{1}));
    LEANAT_CHECK(bounded_admission.snapshot().gates.size() == 1);
    auto responsibilities = bounded_runtime.drains().stop_report();
    LEANAT_CHECK(responsibilities.empty());
    LEANAT_CHECK(bounded_runtime.drains().is_cancelled(transaction));
    LEANAT_CHECK(bounded_runtime.drains().counter_snapshot().responsibilities == 1);
  }
  return 0;
}
