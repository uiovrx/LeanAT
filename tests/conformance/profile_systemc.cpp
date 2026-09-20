#include "harness.hpp"
#include "profile_context.hpp"
#include "profile_runtime_vm.hpp"
#include <leanat/payload_helpers.hpp>
#include <leanat/runtime_services.hpp>
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
using namespace leanat;
using namespace leanat::systemc;
using namespace conformance;
using namespace conformance::profile;

struct ProfileMM : tlm::tlm_mm_interface {
  Recorder &records;
  unsigned frees{};
  explicit ProfileMM(Recorder &r) : records(r) {}
  void free(tlm::tlm_generic_payload *gp) override {
    ++frees;
    records.add("mm.free", Json::object({{"count", frees},
                                         {"refs", gp->get_ref_count()},
                                         {"time", sc_core::sc_time_stamp().value()}}));
  }
};
struct ProfileRunner;
struct ProfileHost : RuntimeHostAdapter {
  Recorder &records;
  std::vector<CallId> sent;
  ProfileHost(sc_core::sc_module_name n, const RuntimeConfig &c, Recorder &r)
      : RuntimeHostAdapter(n, c, TimeCodec{}, 1), records(r) {}
  Expected<WireReturn> transport(const SendIntent &intent) override {
    records.add("host.intent", intent);
    sent.push_back(intent.call_id);
    auto result = RuntimeHostAdapter::transport(intent);
    if (result)
      records.add("host.wire.return", result.value());
    else
      records.add("host.transport.error", result.error());
    return result;
  }
  void observe_milestone(const RuntimeMilestoneObservation &value) override {
    records.add("milestone.full", value);
  }
};
struct ProfileSocket : sc_core::sc_module {
  ProfileRunner &runner;
  RuntimeHostAdapter &host;
  ConnectionId connection;
  InstanceId source, sink;
  ProfileMM mm;
  unsigned char byte{7};
  tlm::tlm_generic_payload gp{&mm};
  tlm_utils::simple_initiator_socket<ProfileSocket> initiator;
  tlm_utils::simple_target_socket<ProfileSocket> target;
  TransportId transport{};
  Handle hop{}, transaction{};
  unsigned requests{}, releases{}, responses{}, acknowledgments{};
  bool owner_live{}, hold_response{};
  std::optional<SendIntent> active_send;
  ProfileSocket(sc_core::sc_module_name, ProfileRunner &, const RuntimeConnectionBinding &);
  tlm::tlm_sync_enum fw(tlm::tlm_generic_payload &, tlm::tlm_phase &, sc_core::sc_time &);
  tlm::tlm_sync_enum bw(tlm::tlm_generic_payload &, tlm::tlm_phase &, sc_core::sc_time &);
  void blocking(tlm::tlm_generic_payload &, sc_core::sc_time &);
  void initialize();
  void release_owner();
  void incoming(Duration delay = {});
  void outgoing();
  void send(PhaseId);
  void end_response();
};
struct ProfileRunner : sc_core::sc_module {
  ProfileContext &context;
  std::string scenario;
  Recorder records;
  RuntimeConfig config;
  std::vector<std::unique_ptr<ProfileSocket>> sockets;
  ProfileHost host;
  CoreRuntimeBackend backend;
  exec::Interpreter vm;
  tlm_utils::simple_initiator_socket_optional<ProfileRunner, 64> wide{"unsupported_width"};
  std::vector<std::string> branches;
  std::string failure;
  bool done{};
  std::uint64_t fuel_used{};
  unsigned immediate_calls{}, immediate_applied{};
  std::vector<CallId> received;
  std::optional<ResponseSnapshot> response;
  std::optional<ReadyKey> response_ready;
  SC_HAS_PROCESS(ProfileRunner);
  static RuntimeConfig limits(ProfileContext &c) {
    RuntimeConfig l;
    l.event_capacity = 1024;
    l.max_events_per_tick = 1000;
    return c.config(l);
  }
  ProfileRunner(sc_core::sc_module_name n, ProfileContext &c, std::string which)
      : sc_module(n), context(c), scenario(std::move(which)), config(limits(c)),
        host("host", config, records), backend(host.runtime, c.project()),
        vm(c.validated(), c.state_refs(), &backend) {
    require(!config.connection_bindings.empty(), "SystemC artifact has no binding");
    for (const auto &service : context.project().services)
      take(backend.register_core(service));
    take(backend.freeze());
    for (const auto &binding : config.connection_bindings)
      sockets.push_back(
          std::make_unique<ProfileSocket>(sc_core::sc_gen_unique_name("endpoint"), *this, binding));
    records.add("runtime.config", config);
    records.add("state.initial", context.state());
    host.runtime.set_ingress_policy([this](const WireCall &call) -> Expected<IngressPlan> {
      records.add("wire.ingress", call);
      IngressPlan plan;
      plan.reply.sync = Sync::Accepted;
      if (scenario == "C-T11") {
        struct Immediate : PreparedParticipant {
          unsigned &count;
          explicit Immediate(unsigned &c) : count(c) {}
          std::size_t reserved_bytes() const noexcept override {
            return sizeof(*this);
          }
          Expected<void> validate() const override {
            return {};
          }
          void apply() noexcept override {
            ++count;
          }
          void discard() noexcept override {}
        };
        ++immediate_calls;
        plan.complete_now = true;
        plan.reply.sync = Sync::Completed;
        plan.reply.response = ResponseSnapshot{ResponseStatus::Ok, {77}, false, {}};
        plan.prepared_service = std::make_unique<Immediate>(immediate_applied);
      }
      records.add("wire.ingress.return", plan.reply);
      return plan;
    });
    host.runtime.set_handler([this](const QueuedEvent &event, Runtime &) -> Expected<void> {
      records.add("dispatch", event);
      execute(event.event.key.instance, 0, {event.event.key.time, event.event.key.turn},
              event.event.key.connection, event.event.epoch);
      return {};
    });
    host.runtime.set_input_handler(
        [this](const WireCall &call, ReadyKey ready, Runtime &) -> Expected<void> {
          records.add("input.call", call);
          received.push_back(call.id);
          records.add("input.ready", ready);
          auto &socket = endpoint(call.connection);
          if (call.phase == begin_req) {
            ++socket.requests;
            execute(socket.sink, 0, ready, call.connection,
                    take(host.runtime.drains().epoch(socket.sink)));
            if (scenario == "C-T09")
              socket.send(begin_resp);
          } else if (call.phase == begin_resp) {
            if (scenario == "C-T09")
              require(call.request.data ==
                          Bytes{static_cast<unsigned char>(socket.connection.value + 76)},
                      "response crossed connection payload");
            response = ResponseSnapshot{call.request.status, call.request.data,
                                        call.request.dmi_hint, call.request.extensions};
            response_ready = ready;
            if (scenario == "C-T09")
              socket.send(end_resp);
          }
          return {};
        });
    host.runtime.set_milestone_handler(
        [this](const RuntimeMilestone &m, ReadyKey ready, Runtime &) -> Expected<void> {
          records.add("milestone.hop", m.hop);
          records.add("milestone.call", m.call_id);
          records.add("milestone.value", m.milestone);
          records.add("milestone.ready", ready);
          if (m.milestone.response) {
            response = m.milestone.response;
            response_ready = ready;
          }
          return {};
        });
    host.runtime.set_blocking_handler(
        [this](const BlockingRequest &request, Runtime &runtime) -> Expected<void> {
          records.add("blocking.request", request.request);
          records.add("blocking.arrival", request.arrival);
          execute(request.instance, 0, {request.arrival, 0}, request.connection, request.epoch);
          ResponseSnapshot result;
          result.status = ResponseStatus::Ok;
          result.data = {77};
          return runtime.complete_blocking(request.token, result);
        });
    take(host.runtime.start(context.manifest(config)));
    SC_THREAD(run);
  }
  ProfileSocket &endpoint(ConnectionId connection) {
    for (auto &s : sockets)
      if (s->connection == connection)
        return *s;
    throw std::runtime_error("unbound descriptor connection");
  }
  Tick now() const {
    return Tick{sc_core::sc_time_stamp().value()};
  }
  void settle() {
    for (unsigned i = 0; i < 32; ++i)
      sc_core::wait(sc_core::SC_ZERO_TIME);
    require(!host.runtime.stopped(), "profile Runtime stopped");
  }
  exec::SegmentResult execute(InstanceId side, unsigned local, ReadyKey ready,
                              ConnectionId connection, std::uint64_t epoch,
                              std::vector<Value> args = {}, std::uint64_t owner = 0) {
    auto id = context.handler(side.value, local).program_id;
    auto execution = context.execution(id, ready, connection, owner, epoch);
    require(execution.instance == side, "VM handler instance differs from descriptor");
    EventTxn txn({}, execution);
    const auto before = context.state();
    exec::FuelCounter fuel{10000};
    auto result = take(vm.execute_segment(id, execution, args, txn, fuel));
    require(result.kind == exec::SegmentResult::Kind::Returned,
            "profile business VM did not return");
    auto actions = take(host.runtime.commit_segment(txn));
    const auto &instance = context.instance(side.value);
    for (std::size_t i = 0; i < before.size(); ++i)
      if (i < instance.state_base || i >= instance.state_base + instance.state_count)
        require(before[i].value == context.state()[i].value, "VM wrote another instance state");
    records.add("vm.context", execution);
    records.add("vm.result", result);
    records.add("vm.commit", actions);
    records.add("vm.state", context.state());
    fuel_used += 10000 - fuel.remaining;
    records.add("vm.fuel.remaining", fuel.remaining);
    return result;
  }
  void schedule(InstanceId side, ConnectionId connection, Tick time, std::uint64_t epoch) {
    EventDraft event;
    auto ready = take(host.runtime.queue().successor(time));
    event.key = {ready.time, ready.turn, EventStage::Internal, side, connection, 0};
    event.epoch = epoch;
    records.add("schedule", event);
    take(host.runtime.schedule(event));
  }
  void inspect(ProfileSocket &s) {
    records.add("pin", Json::object({{"connection", observe(s.connection)},
                                     {"refs", s.gp.get_ref_count()},
                                     {"frees", s.mm.frees},
                                     {"byte", s.byte}}));
    records.add("ledger",
                take(take(host.runtime.protocol(scenario == "C-T12" || scenario == "C-T13" ||
                                                        scenario == "C-T14" || scenario == "C-T30"
                                                    ? s.source
                                                    : s.sink))
                         ->inspect(s.hop)));
    records.add("drain.outstanding", host.runtime.drains().outstanding());
    records.add("epoch", take(host.runtime.drains().epoch(s.sink)));
  }
  void lifecycle(unsigned stage);
  void run_case();
  void run() {
    try {
      run_case();
      records.add("state.final", context.state());
      records.add("stop", "Completed");
      done = true;
    } catch (const std::exception &e) {
      failure = e.what();
      std::cerr << "scenario failure: " << failure << '\n';
    }
    sc_core::sc_stop();
  }
};

ProfileSocket::ProfileSocket(sc_core::sc_module_name n, ProfileRunner &r,
                             const RuntimeConnectionBinding &b)
    : sc_module(n), runner(r), host(r.host), connection(b.connection), source(b.initiator),
      sink(b.target), mm(r.records), initiator("initiator"), target("target") {
  target.register_nb_transport_fw(this, &ProfileSocket::fw);
  initiator.register_nb_transport_bw(this, &ProfileSocket::bw);
  target.register_b_transport(this, &ProfileSocket::blocking);
  initiator.bind(target);
  const auto &binding = r.context.binding(connection.value);
  auto endpoint_width = [&](InstanceId side, unsigned local, exec::EndpointRole role) {
    for (const auto &e : r.context.component(side.value).endpoints)
      if (e.id == local) {
        require(e.role == role, "descriptor endpoint role");
        return e.bus_width;
      }
    throw std::runtime_error("descriptor endpoint absent");
  };
  require(endpoint_width(source, binding.source_endpoint.endpoint, exec::EndpointRole::Initiator) ==
                  initiator.get_bus_width() &&
              endpoint_width(sink, binding.sink_endpoint.endpoint, exec::EndpointRole::Target) ==
                  target.get_bus_width(),
          "actual socket width differs from descriptor");
  take(validate_native_profile({}, initiator, target));
  take(host.bind(
      connection,
      [this](const SendIntent &intent) -> Expected<tlm::tlm_generic_payload *> {
        active_send = intent;
        return &gp;
      },
      [this](auto &g, auto &p, auto &d) {
        const auto call = *active_send;
        auto projection = take(PayloadBridge{}.snapshot_call(g, call.phase));
        runner.records.add("native.call",
                           WireCall{call.call_id, connection, call.transport, call.flow, call.phase,
                                    runner.now(), Duration{d.value()}, projection});
        runner.records.add("native.outgoing",
                           Json::object({{"connection", observe(connection)},
                                         {"phase", unsigned(p)},
                                         {"delay", d.value()},
                                         {"time", sc_core::sc_time_stamp().value()},
                                         {"refs", g.get_ref_count()}}));
        auto result = (p == tlm::BEGIN_REQ || p == tlm::END_RESP)
                          ? initiator->nb_transport_fw(g, p, d)
                          : target->nb_transport_bw(g, p, d);
        runner.records.add("native.return", Json::object({{"sync", unsigned(result)},
                                                          {"phase", unsigned(p)},
                                                          {"delay", d.value()},
                                                          {"refs", g.get_ref_count()}}));
        return result;
      }));
}
void ProfileSocket::initialize() {
  require(!owner_live, "duplicate GP owner");
  gp.acquire();
  owner_live = true;
  byte = 7;
  gp.set_command(tlm::TLM_READ_COMMAND);
  gp.set_address(0x100000010ull);
  gp.set_data_ptr(&byte);
  gp.set_data_length(1);
  gp.set_streaming_width(1);
  gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
  runner.records.add("stimulus.gp", take(PayloadBridge{}.snapshot(gp)));
}
void ProfileSocket::release_owner() {
  require(owner_live, "GP owner released twice");
  owner_live = false;
  gp.release();
  runner.records.add("mm.release.owner",
                     Json::object({{"refs", gp.get_ref_count()}, {"frees", mm.frees}}));
}
tlm::tlm_sync_enum ProfileSocket::fw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                                     sc_core::sc_time &d) {
  if (runner.scenario == "C-T12" || runner.scenario == "C-T13" || runner.scenario == "C-T14" ||
      runner.scenario == "C-T30") {
    if (p == tlm::END_RESP) {
      ++acknowledgments;
      return tlm::TLM_ACCEPTED;
    }
    require(p == tlm::BEGIN_REQ, "external peer unexpected phase");
    runner.records.add("peer.request", take(PayloadBridge{}.snapshot(g)));
    runner.schedule(sink, connection, Tick{runner.now().value + 1},
                    take(host.runtime.drains().epoch(sink)));
    release_owner();
    require(mm.frees == 0, "native callback release destroyed call-pinned GP");
    if (runner.scenario == "C-T14")
      return tlm::TLM_ACCEPTED;
    g.set_response_status(tlm::TLM_OK_RESPONSE);
    g.get_data_ptr()[0] = 77;
    d = sc_core::sc_time::from_value(5);
    p = tlm::END_RESP;
    return tlm::TLM_COMPLETED;
  }
  auto result = host.receive(connection, Flow::Forward, g, p, d);
  if (!result) {
    runner.records.add("ingress.rejected", result.error());
    throw std::runtime_error(result.error().message);
  }
  return result.value();
}
tlm::tlm_sync_enum ProfileSocket::bw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                                     sc_core::sc_time &d) {
  if (runner.scenario == "C-T09" || runner.scenario == "C-T14")
    return take(host.receive(connection, Flow::Backward, g, p, d));
  if (p == tlm::END_REQ) {
    ++releases;
    if (runner.scenario == "C-T02") {
      p = tlm::END_RESP;
      d = sc_core::sc_time::from_value(999);
    }
    return tlm::TLM_ACCEPTED;
  }
  require(p == tlm::BEGIN_RESP, "external initiator unexpected response");
  ++responses;
  runner.records.add("peer.response", take(PayloadBridge{}.response(g)));
  return hold_response ? tlm::TLM_ACCEPTED : tlm::TLM_COMPLETED;
}
void ProfileSocket::blocking(tlm::tlm_generic_payload &g, sc_core::sc_time &d) {
  take(host.blocking(connection, g, d));
}
void ProfileSocket::incoming(Duration delay) {
  initialize();
  transport = TransportId{transport.value + 1};
  tlm::tlm_phase phase = tlm::BEGIN_REQ;
  auto d = sc_core::sc_time::from_value(delay.value);
  runner.records.add("stimulus.incoming",
                     Json::object({{"connection", observe(connection)},
                                   {"phase", unsigned(phase)},
                                   {"delay", d.value()},
                                   {"time", runner.now().value},
                                   {"payload", observe(take(PayloadBridge{}.snapshot(gp)))}}));
  require(initiator->nb_transport_fw(gp, phase, d) == tlm::TLM_ACCEPTED,
          "incoming request not accepted");
  hop = take(take(host.runtime.protocol(sink))->find_ledger(connection, transport));
  release_owner();
}
void ProfileSocket::outgoing() {
  initialize();
  transport = TransportId{connection.value};
  hop = take(take(host.runtime.protocol(source))->create_ledger(connection, transport));
  SendIntent send;
  send.connection = connection;
  send.transport = transport;
  send.call_id = take(host.runtime.allocate_call_id(CallOrigin::Outgoing));
  send.payload = take(PayloadBridge{}.snapshot(gp));
  send.not_before = runner.now();
  runner.records.add("intent", send);
  take(host.runtime.publish(send));
}
void ProfileSocket::send(PhaseId phase) {
  SendIntent send;
  send.connection = connection;
  send.transport = transport;
  send.txn = transaction;
  send.phase = phase;
  send.flow = phase == end_resp ? Flow::Forward : Flow::Backward;
  send.call_id = take(host.runtime.allocate_call_id(CallOrigin::Outgoing));
  send.not_before = Tick{runner.now().value + (runner.scenario == "C-T09" ? 1 : 0)};
  send.payload = take(PayloadBridge{}.snapshot(gp));
  if (phase == begin_resp) {
    send.payload.status = ResponseStatus::Ok;
    send.payload.data = {
        static_cast<unsigned char>(runner.scenario == "C-T09" ? connection.value + 76 : 77)};
  }
  runner.records.add("intent", send);
  take(host.runtime.publish(send));
}
void ProfileSocket::end_response() {
  tlm::tlm_phase phase = tlm::END_RESP;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  require(initiator->nb_transport_fw(gp, phase, delay) == tlm::TLM_ACCEPTED,
          "terminal acknowledgment rejected");
}

void ProfileRunner::lifecycle(unsigned stage) {
  records.add("stimulus.reset.stage", stage);
  auto &s = *sockets.front();
  s.hold_response = false;
  s.incoming();
  auto epoch = take(host.runtime.drains().epoch(s.sink));
  s.transaction = {HandleKind::Transaction, config.domain, 91, stage, stage + 1, 0};
  take(host.runtime.drains().register_responsibility(
      {s.transaction, s.sink, epoch, {}, {{s.hop, false, false, false, false}}, false, false}));
  take(host.runtime.track_cleanup(s.transaction, s.hop, false,
                                  take(PayloadBridge{}.snapshot(s.gp))));
  schedule(s.sink, s.connection, Tick{now().value + 2}, epoch);
  if (stage) {
    settle();
    s.send(end_req);
    settle();
    require(take(take(host.runtime.protocol(s.sink))->inspect(s.hop)).state ==
                WireState::RequestReleased,
            "service-stage ledger");
  }
  if (stage == 2) {
    s.hold_response = true;
    s.send(begin_resp);
    settle();
    require(take(take(host.runtime.protocol(s.sink))->inspect(s.hop)).state == WireState::Response,
            "response-stage ledger");
  }
  inspect(s);
  take(host.runtime.reset(s.sink, ResetPolicy::AbortLocalAndDrain));
  auto next_epoch = take(host.runtime.drains().epoch(s.sink));
  // Runtime owns reset admission; the host owns the descriptor-bound state cells.
  // Advance only lifecycle metadata here. The loaded reset handler writes the business value.
  const auto &bound = context.instance(s.sink.value);
  for (std::size_t i = bound.state_base; i < bound.state_base + bound.state_count; ++i)
    require(context.state()[i].epoch == epoch && context.state()[i].version < UINT64_MAX,
            "reset cell metadata precondition");
  for (std::size_t i = bound.state_base; i < bound.state_base + bound.state_count; ++i) {
    context.state()[i].epoch = next_epoch;
    ++context.state()[i].version;
  }
  records.add("host.state.epoch.transition", context.state());
  execute(s.sink, 1, {now(), 0}, s.connection, next_epoch);
  const auto expected_reset_state = context.state();
  const auto &reset_instance = context.instance(s.sink.value);
  require(reset_instance.state_count > 0, "reset fixture has no state");
  for (std::size_t i = reset_instance.state_base;
       i < reset_instance.state_base + reset_instance.state_count; ++i)
    require(expected_reset_state[i].value == Value{std::uint64_t{7}},
            "compiled reset handler did not restore initial7");
  require(s.mm.frees == stage, "reset released an open GP");
  settle();
  if (stage == 2) {
    require(s.mm.frees == stage, "response freed before ACK");
    s.end_response();
    settle();
  }
  require(s.mm.frees == stage + 1, "terminal MM count");
  require(s.releases == stage + 1 && s.responses == stage + 1,
          "duplicate or missing cleanup phase");
  require(host.runtime.drains().outstanding() == 0, "drain backlog after terminal");
  auto receipt = take(host.runtime.cancel_local(s.transaction, CancelReason::Reset)).receipt;
  require(receipt.has_value(), "durable reset receipt absent");
  auto drain = take(host.runtime.drains().inspect(*receipt));
  records.add("drain.receipt", *receipt);
  records.add("drain.state", drain.state);
  records.add("drain.reason", drain.reason);
  require(drain.state == DrainState::Complete, "drain did not complete");
  take(host.runtime.drains().release_receipt(*receipt));
  sc_core::wait(sc_core::sc_time::from_value(3));
  settle();
  const auto reset_state = context.state();
  records.add("state.after.stale", reset_state);
  for (std::size_t i = 0; i < reset_state.size(); ++i)
    require(reset_state[i].value == expected_reset_state[i].value,
            "stale epoch changed reset state");
  schedule(s.sink, s.connection, now(), next_epoch);
  settle();
  const auto &instance = context.instance(s.sink.value);
  require(instance.state_count > 0, "reset fixture missing descriptor state");
  for (std::size_t i = instance.state_base; i < instance.state_base + instance.state_count; ++i)
    require(context.state()[i].value == Value{std::uint64_t{99}},
            "new epoch VM did not write state");
  inspect(s);
  branches.push_back(stage == 0   ? "reset_request"
                     : stage == 1 ? "reset_service"
                                  : "reset_response");
}
void ProfileRunner::run_case() {
  auto &s = *sockets.front();
  if (scenario == "C-T15") {
    for (unsigned stage = 0; stage < 3; ++stage)
      lifecycle(stage);
    return;
  }
  if (scenario == "C-T11") {
    unsigned char byte = 7;
    tlm::tlm_generic_payload gp;
    gp.set_command(tlm::TLM_READ_COMMAND);
    gp.set_address(0x100000010ull);
    gp.set_data_ptr(&byte);
    gp.set_data_length(1);
    gp.set_streaming_width(1);
    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    records.add("stimulus.no_mm", take(PayloadBridge{}.snapshot(gp)));
    bool rejected = false;
    try {
      s.initiator->nb_transport_fw(gp, phase, delay);
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    require(rejected, "nb without MM accepted");
    require(s.requests == 0, "rejected nb executed business");
    require(immediate_calls == 0 && immediate_applied == 0,
            "noMM request reached immediate completion policy");
    s.initialize();
    phase = tlm::BEGIN_REQ;
    delay = sc_core::SC_ZERO_TIME;
    require(s.initiator->nb_transport_fw(s.gp, phase, delay) == tlm::TLM_COMPLETED,
            "same policy MM control did not complete immediately");
    require(immediate_calls == 1 && immediate_applied == 1 && s.byte == 77,
            "immediate completion positive control");
    records.add("immediate.gp", take(PayloadBridge{}.snapshot(s.gp)));
    s.release_owner();
    require(s.mm.frees == 1, "immediate MM control ownership");
    records.add("immediate.calls", immediate_calls);
    records.add("immediate.applied", immediate_applied);
    delay = sc_core::sc_time::from_value(3);
    records.add("stimulus.blocking",
                Json::object({{"delay", delay.value()},
                              {"payload", observe(take(PayloadBridge{}.snapshot(gp)))}}));
    s.initiator->b_transport(gp, delay);
    require(byte == 77 && delay == sc_core::SC_ZERO_TIME && now() == Tick{3},
            "blocking annotation/owned copyback");
    records.add("blocking.return", take(PayloadBridge{}.snapshot(gp)));
    branches = {"nb_no_mm_rejected", "blocking_no_mm_owned"};
    return;
  }
  if (scenario == "C-T28") {
    take(validate_native_profile({}, s.initiator, s.target));
    auto endian = validate_native_profile({NativeByteOrder::Big, 32}, s.initiator, s.target);
    auto width = validate_native_profile({}, wide, s.target);
    require(!endian && !width, "unsupported native configuration accepted");
    records.add("actual.byteorder", native_byte_order());
    records.add("endian.reject", endian.error());
    records.add("width.reject", width.error());
    execute(s.sink, 0, {now(), 0}, s.connection, 0);
    branches = {"actual_host_endian", "actual_socket_width_rejected",
                "declared_big_endian_rejected"};
    return;
  }
  if (scenario == "C-T02") {
    s.incoming(Duration{2});
    sc_core::wait(sc_core::sc_time::from_value(3));
    settle();
    require(s.requests == 1, "annotated request input not dispatched");
    s.send(end_req);
    settle();
    auto ledger = take(take(host.runtime.protocol(s.sink))->inspect(s.hop));
    records.add("accepted.ledger", ledger);
    require(ledger.state == WireState::RequestReleased, "Accepted returned phase affected ledger");
    require(host.runtime.queue().occupied() == 0,
            "Accepted returned delay scheduled spurious future milestone");
    s.send(begin_resp);
    settle();
    require(s.mm.frees == 1, "terminal pin leak");
    branches = {"real-gp-accepted-mutation", "native_accepted_phase_mutation_ignored",
                "native_accepted_delay_mutation_ignored", "annotated_input_milestone"};
    inspect(s);
    return;
  }
  if (scenario == "C-T09") {
    require(sockets.size() == 2, "multibinding artifact must have two connections");
    require(sockets[0]->source == sockets[1]->source && sockets[0]->sink != sockets[1]->sink,
            "multibinding identity layout");
    require(context.instance(sockets[0]->sink.value).definition ==
                context.instance(sockets[1]->sink.value).definition,
            "targets do not instantiate same descriptor component");
    for (auto &p : sockets)
      p->outgoing();
    settle();
    for (auto &p : sockets) {
      require(!take(host.runtime.protocol(p->source))->request_lane_free(p->connection),
              "connection request gate was not independently held");
      records.add("open.binding", take(take(host.runtime.protocol(p->source))->inspect(p->hop)));
    }
    sc_core::wait(sc_core::sc_time::from_value(2));
    settle();
    for (auto &p : sockets) {
      if (p->owner_live)
        p->release_owner();
      require(p->mm.frees == 1, "multibinding GP not released");
      require(p->byte == p->connection.value + 76, "connection GP payload isolation");
      const auto &instance = context.instance(p->sink.value);
      require(instance.state_count > 0, "target state absent");
      for (std::size_t i = instance.state_base; i < instance.state_base + instance.state_count; ++i)
        require(context.state()[i].value == Value{std::uint64_t{99}},
                "target instance state not independently executed");
      for (auto side : {p->source, p->sink}) {
        auto hop =
            take(take(host.runtime.protocol(side))->find_ledger(p->connection, p->transport));
        auto ledger = take(take(host.runtime.protocol(side))->inspect(hop));
        records.add("binding.ledger", ledger);
        require(ledger.state == WireState::Terminal && ledger.call_ordinal == 3,
                "binding not terminal with three local calls");
      }
    }
    require(host.sent == received && host.sent.size() == 6,
            "controlled native calls not paired across local sides");
    require(std::set<CallId>(host.sent.begin(), host.sent.end()).size() == 6,
            "CallId collided across bindings");
    records.add("calls.sent", host.sent);
    records.add("calls.received", received);
    branches = {"same_component_multiple_instances", "independent_connection_ledgers",
                "shared_domain_call_identity"};
    return;
  }
  if (scenario == "C-T14") {
    AdmissionStore admission(config.domain, s.source, {4, 4, 4, 4, 4});
    TransactOperation operation(host.runtime, admission);
    ExecutionContext execution;
    execution.kind = ContextKind::Process;
    execution.domain = config.domain;
    execution.instance = s.source;
    execution.connection = s.connection;
    execution.owner = 7;
    s.initialize();
    s.transport = TransportId{1};
    take(operation.start(execution, s.connection, s.transport,
                         take(PayloadBridge{}.snapshot(s.gp))));
    s.transaction = operation.transaction();
    s.hop = operation.hop();
    auto consumer = take(operation.subscribe_result(7));
    records.add("timeout.result", consumer.result);
    records.add("timeout.consumer", consumer.consumer);
    take(operation.advance(execution));
    settle();
    sc_core::wait(sc_core::sc_time::from_value(1));
    settle();
    execution.ready = {host.runtime.now(), 0};
    auto receipt = take(operation.cancel(execution, CancelReason::Timeout)).receipt;
    const auto local_timeout = take(host.runtime.results().read(consumer));
    records.add("timeout.local.before", local_timeout);
    require(take(operation.result()).local_cancel == CancelReason::Timeout &&
                !take(operation.result()).success(),
            "timeout operation did not publish local cancellation");
    require(receipt.has_value(), "timeout receipt absent");
    records.add("timeout.receipt", *receipt);
    require(s.mm.frees == 0, "timeout freed live GP");
    sc_core::wait(sc_core::sc_time::from_value(2));
    settle();
    s.byte = 88;
    s.gp.set_response_status(tlm::TLM_OK_RESPONSE);
    tlm::tlm_phase phase = tlm::BEGIN_RESP;
    auto delay = sc_core::sc_time::from_value(2);
    require(s.target->nb_transport_bw(s.gp, phase, delay) == tlm::TLM_ACCEPTED,
            "late response rejected");
    settle();
    sc_core::wait(sc_core::sc_time::from_value(3));
    settle();
    auto drain = take(host.runtime.drains().inspect(*receipt));
    records.add("timeout.final.reason", drain.reason);
    records.add("timeout.final.state", drain.state);
    require(drain.reason == CancelReason::Timeout && drain.state == DrainState::Complete,
            "late response changed timeout or failed drain");
    require(s.acknowledgments == 1 && s.mm.frees == 1, "late response not acknowledged safely");
    const auto after = take(host.runtime.results().read(consumer));
    records.add("timeout.local.after", after);
    require(after == local_timeout && !take(operation.result()).success(),
            "late success overwrote durable timeout");
    branches = {"timeout_remains_local", "late_response_wire_ack", "pin_retained_until_terminal"};
    inspect(s);
    execution.ready = {host.runtime.now(), 0};
    require(take(operation.reap(execution)), "timeout wire cleanup did not retire");
    take(host.runtime.results().release(consumer));
    take(host.runtime.drains().release_receipt(*receipt));
    records.add("timeout.results.remaining", host.runtime.results().occupied());
    require(host.runtime.results().occupied() == 0, "timeout leaked durable result ownership");
    return;
  }
  s.outgoing();
  settle();
  require(s.mm.frees == 1, "completed native return did not retire GP");
  require(!response, "response visible before effective return delay");
  s.byte = 99;
  records.add("external.buffer.reused", s.byte);
  sc_core::wait(sc_core::sc_time::from_value(6));
  settle();
  require(response && response->data == Bytes{77} && response_ready &&
              response_ready->time == Tick{5},
          "future response lost independent owning snapshot");
  records.add("response.owned", *response);
  records.add("response.ready", *response_ready);
  inspect(s);
  if (scenario == "C-T30") {
    auto handler = context.handler(s.sink.value, 2).program_id;
    (void)context.program(handler);
    TypeId bytes_type{};
    for (std::uint32_t i = 0; i < context.project().types.size(); ++i)
      if (context.project().types[i].kind == exec::TypeKind::Bytes)
        bytes_type = TypeId{i};
    auto reserved = take(host.runtime.results().reserve({s.hop, bytes_type, 4096, 7, 7}));
    records.add("result.handle", reserved.consumer.result);
    records.add("result.consumer", reserved.consumer.consumer);
    take(host.runtime.results().publish(reserved.reservation, Value{response->data},
                                        {s.hop, *response_ready, true}));
    auto result = execute(s.sink, 2, {now(), 0}, s.connection, 0,
                          {Value{reserved.consumer.result}, Value{reserved.consumer.consumer}}, 7);
    require(result.values == std::vector<Value>{Value{Bytes{77}}},
            "compiled durable consumer bytes");
    require(!host.runtime.results().read(reserved.consumer),
            "compiled ResultRelease did not release consumer");
    take(host.runtime.results().release_owner(reserved.reservation));
    branches = {"native_gp_freed_before_consume", "owned_result_published",
                "compiled_result_get_release"};
  } else
    branches = {scenario == "C-T12" ? "callee_releases_owner_in_callback"
                                    : "external_buffer_recycled",
                "call_pin_protects_return_snapshot", "future_owned_response"};
}
struct ProfilePumpRunner : sc_core::sc_module {
  ProfileContext &context;
  Recorder records;
  RuntimeConfig config;
  RuntimeHostAdapter host;
  RuntimeVmSession session;
  sc_core::sc_event observer_event;
  unsigned observed{};
  bool done{};
  std::string failure;
  static RuntimeConfig limits(ProfileContext &c) {
    RuntimeConfig l;
    l.max_events_per_tick = 16;
    l.max_events = 64;
    return c.config(l);
  }
  SC_HAS_PROCESS(ProfilePumpRunner);
  ProfilePumpRunner(sc_core::sc_module_name n, ProfileContext &c)
      : sc_module(n), context(c), config(limits(c)), host("host", config, TimeCodec{}, 1),
        session(c, host.runtime, records) {
    session.bind_core();
    session.freeze();
    take(host.runtime.start(c.manifest(config)));
    records.add("runtime.config", config);
    SC_METHOD(observer);
    sensitive << observer_event;
    SC_THREAD(run);
  }
  void observer() {
    ++observed;
    records.add("kernel.observer", Json::object({{"count", observed},
                                                 {"time", sc_core::sc_time_stamp().value()},
                                                 {"delta", sc_core::sc_delta_count()}}));
    if (!host.runtime.stopped())
      observer_event.notify(sc_core::SC_ZERO_TIME);
  }
  void run() {
    try {
      std::optional<std::uint32_t> selected;
      for (const auto &program : context.project().programs)
        if (program.context == ContextKind::Process) {
          require(!selected, "ambiguous ready-loop process");
          selected = program.id;
        }
      require(selected.has_value(), "ready-loop artifact has no process");
      auto id = *selected;
      session.schedule_process(id, {}, {Tick{0}, 0});
      sc_core::wait(sc_core::sc_time::from_value(1));
      require(host.runtime.stopped(), "ready-await loop did not stop at budget");
      require(host.runtime.stop_detail() == "events per tick exhausted" && !session.last_error(),
              "ready loop stopped for an unrelated failure");
      require(session.resumes() > 0 && observed > 1,
              "SystemC observer did not interleave with resumed VM");
      records.add("runtime.stop", host.runtime.stop_detail());
      records.add("vm.resumes", session.resumes());
      records.add("kernel.observed", observed);
      done = true;
    } catch (const std::exception &e) {
      failure = e.what();
    }
    sc_core::sc_stop();
  }
};
int sc_main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  try {
    auto context = take(ProfileContext::load(argv[2], argv[3], argv[4]));
    const std::set<std::string> supported = {"C-T02", "C-T09", "C-T11",         "C-T12", "C-T13",
                                             "C-T14", "C-T15", "C-T17-systemc", "C-T28", "C-T30"};
    require(supported.count(argv[5]), "unknown SystemC profile scenario");
    std::ofstream output(argv[1]);
    require(bool(output), "cannot write profile observations");
    if (std::string(argv[5]) == "C-T17-systemc") {
      sc_core::sc_report_handler::set_actions("LeanAT", sc_core::SC_ERROR, sc_core::SC_DO_NOTHING);
      ProfilePumpRunner runner("profile_pump", *context);
      sc_core::sc_start();
      require(runner.done,
              runner.failure.empty() ? "SystemC pump scenario did not finish" : runner.failure);
      output << context
                    ->record("C-T17", observe(runner.config),
                             Json::object({{"stop", runner.host.runtime.stop_detail()},
                                           {"fuel", runner.session.fuel_used()},
                                           {"events", runner.records.json()}}),
                             {"compiled-ready-wait-systemc-yield"})
                    .dump()
             << '\n';
    } else {
      ProfileRunner runner("profile_systemc", *context, argv[5]);
      sc_core::sc_start();
      if (!runner.done) {
        output << Json::object({{"assertionsPassed", false},
                                {"error", runner.failure},
                                {"events", runner.records.json()}})
                      .dump()
               << '\n';
        output.flush();
      }
      require(runner.done,
              runner.failure.empty() ? "SystemC scenario did not finish" : runner.failure);
      output << context
                    ->record(argv[5],
                             Json::object({{"scenario", argv[5]},
                                           {"backend", "real_systemc"},
                                           {"config", observe(runner.config)},
                                           {"readAddress", std::uint64_t{0x100000010ull}},
                                           {"initialBytes", observe(Bytes{7})}}),
                             Json::object({{"stop", "Completed"},
                                           {"fuel", runner.fuel_used},
                                           {"events", runner.records.json()}}),
                             runner.branches)
                    .dump()
             << '\n';
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
