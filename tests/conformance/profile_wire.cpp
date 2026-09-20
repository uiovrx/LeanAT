#include "profile_wire.hpp"
#include "harness.hpp"
#include <fstream>
#include <iostream>

namespace conformance::profile {
namespace {
struct WireHost final : RuntimeHost {
  Runtime *runtime{};
  Recorder records;
  std::uint64_t calls{}, inputs{};
  bool loopback{true}, updated_response{};
  Duration request_delay{100};
  std::vector<std::pair<PhaseId, Tick>> input_order;
  Expected<WireReturn> transport(const SendIntent &intent) override {
    ++calls;
    WireCall call{intent.call_id,
                  intent.connection,
                  intent.transport,
                  intent.flow,
                  intent.phase,
                  runtime->now(),
                  intent.phase == end_req ? request_delay : Duration{},
                  intent.payload};
    records.add("outboundCall", call);
    auto started = runtime->start_outbound(intent, call);
    if (!started)
      return started.error();
    if (!loopback) {
      WireReturn reply;
      if (updated_response && intent.phase == begin_resp) {
        reply.sync = Sync::Updated;
        reply.phase = end_resp;
        reply.outgoing_delay = Duration{5};
      }
      records.add("externalPeerReturn", reply);
      return reply;
    }
    records.add("receiverCall", call);
    auto returned = runtime->ingress(call);
    if (returned)
      records.add("receiverAndCallerReturn", returned.value());
    else
      records.add("returnError", returned.error());
    return returned;
  }
  void arm(std::optional<WakePoint> wake) override {
    records.add("arm",
                wake ? Json::object({{"time", observe(wake->time)}, {"turn", observe(wake->turn)}})
                     : Json());
  }
  void publish_output(PortId p, const Value &v) override {
    records.add("output", Json::object({{"port", observe(p)}, {"value", observe(v)}}));
  }
  void emit_trace(const TraceEvent &v) override {
    records.add("trace", v);
  }
  void observe_milestone(const RuntimeMilestoneObservation &v) override {
    records.add("milestone", v);
  }
  void attach(Runtime &r) {
    runtime = &r;
    r.set_input_handler([this](const WireCall &c, ReadyKey ready, Runtime &) -> Expected<void> {
      ++inputs;
      input_order.push_back({c.phase, ready.time});
      records.add("input", Json::object({{"call", observe(c)}, {"ready", observe(ready)}}));
      return {};
    });
  }
  void drain(Tick until) {
    unsigned budget = 0;
    while (runtime->next_wakeup() && !(until < runtime->next_wakeup()->time)) {
      require(++budget <= 128, "bounded wire host pump did not finish");
      take(runtime->pump_batch(runtime->next_wakeup()->time, 16));
    }
    require(!runtime->stopped(), "wire host runtime stopped");
  }
};

Json runtime_terminal_branch(ProfileContext &context) {
  auto config = context.config();
  require(!config.connection_bindings.empty(), "terminal runtime requires descriptor binding");
  auto binding = config.connection_bindings.front();
  WireHost host;
  host.loopback = false;
  host.updated_response = true;
  host.request_delay = Duration{};
  Runtime runtime(config, host);
  host.attach(runtime);
  take(runtime.start(context.manifest(config)));
  auto engine = take(runtime.protocol(binding.target));
  auto hop = take(engine->create_ledger(binding.connection, TransportId{1}));
  PayloadSnapshot payload;
  payload.command = Command::Read;
  payload.data = {0, 0};
  payload.streaming_width = 2;
  WireCall call{take(runtime.allocate_call_id(CallOrigin::ExternalIngress)),
                binding.connection,
                TransportId{1},
                Flow::Forward,
                begin_req,
                Tick{0},
                {},
                payload};
  host.records.add("externalCall", call);
  host.records.add("externalReturn", take(runtime.ingress(call)));
  host.drain(Tick{0});
  std::uint64_t terminal_consumptions = 0;
  runtime.set_milestone_handler(
      [&](const RuntimeMilestone &m, ReadyKey ready, Runtime &) -> Expected<void> {
        host.records.add("businessMilestone", Json::object({{"callId", observe(m.call_id)},
                                                            {"hop", observe(m.hop)},
                                                            {"milestone", observe(m.milestone)},
                                                            {"ready", observe(ready)}}));
        if (m.milestone.kind == MilestoneKind::Terminal) {
          ++terminal_consumptions;
          require(ready.time == Tick{25}, "business terminal consumed early");
        }
        return {};
      });
  for (auto phase : {end_req, begin_resp}) {
    SendIntent intent;
    intent.call_id = take(runtime.allocate_call_id(CallOrigin::Outgoing));
    intent.connection = binding.connection;
    intent.transport = TransportId{1};
    intent.phase = phase;
    intent.flow = Flow::Backward;
    intent.not_before = phase == begin_resp ? Tick{20} : Tick{0};
    intent.payload = payload;
    if (phase == begin_resp) {
      intent.payload.status = ResponseStatus::Ok;
      intent.payload.data = {4, 9};
    }
    host.records.add("publishedIntent", intent);
    take(runtime.publish(intent));
    host.drain(intent.not_before);
  }
  for (auto time : {Tick{20}, Tick{24}}) {
    take(runtime.pump_batch(time, 16));
    auto before = runtime.milestone(hop, MilestoneKind::Terminal);
    require(!before && before.error().code == ErrorCode::NotReady, "terminal readable before25");
    host.records.add("earlyTerminalRead",
                     Json::object({{"time", observe(time)}, {"error", observe(before.error())}}));
    require(terminal_consumptions == 0, "terminal consumer ran before25");
  }
  host.drain(Tick{25});
  auto terminal = take(runtime.milestone(hop, MilestoneKind::Terminal));
  host.records.add("terminalAt25", terminal.milestone);
  host.records.add("terminalConsumptions", terminal_consumptions);
  host.records.add("finalLedger", take(engine->inspect(hop)));
  require(terminal.milestone.time == Tick{25} && terminal_consumptions == 1,
          "terminal not consumed exactly once at25");
  return host.records.json();
}

Json runtime_wire_branch(ProfileContext &context, bool ignored) {
  auto config = context.config();
  require(!config.connection_bindings.empty(), "wire runtime requires descriptor binding");
  auto binding = config.connection_bindings.front();
  WireHost host;
  Runtime runtime(config, host);
  host.attach(runtime);
  take(runtime.start(context.manifest(config)));
  host.records.add("runtimeConfig", config);
  auto sender = take(runtime.protocol(binding.initiator));
  auto receiver = take(runtime.protocol(binding.target));
  PayloadSnapshot payload;
  payload.command = Command::Read;
  payload.data = {0, 0};
  payload.streaming_width = 2;
  std::vector<Handle> ledgers;
  if (ignored) {
    take(receiver->allow_ignorable(PhaseId{99}));
    auto h = take(receiver->create_ledger(binding.connection, TransportId{1}));
    ledgers.push_back(h);
    WireCall call{take(runtime.allocate_call_id(CallOrigin::ExternalIngress)),
                  binding.connection,
                  TransportId{1},
                  Flow::Forward,
                  PhaseId{99},
                  Tick{0},
                  Duration{7},
                  payload};
    host.records.add("externalCall", call);
    auto returned = take(runtime.ingress(call));
    host.records.add("externalReturn", returned);
    host.drain(Tick{7});
    require(returned.sync == Sync::Accepted && host.calls == 0 && host.inputs == 0,
            "ignorable ingress forwarded or reached business handler");
    require(take(receiver->inspect(h)).state == WireState::Idle, "ignored ingress changed ledger");
  } else {
    for (unsigned transport = 1; transport <= 2; ++transport) {
      ledgers.push_back(take(sender->create_ledger(binding.connection, TransportId{transport})));
      ledgers.push_back(take(receiver->create_ledger(binding.connection, TransportId{transport})));
    }
    auto publish = [&](std::uint64_t transport, PhaseId phase, Tick time) {
      SendIntent intent;
      intent.call_id = take(runtime.allocate_call_id(CallOrigin::Outgoing));
      intent.connection = binding.connection;
      intent.transport = TransportId{transport};
      intent.phase = phase;
      intent.flow = phase == end_req ? Flow::Backward : Flow::Forward;
      intent.payload = payload;
      intent.not_before = time;
      host.records.add("publishedIntent", intent);
      take(runtime.publish(intent));
      host.drain(time);
    };
    publish(1, begin_req, Tick{0});
    WireCall delayed{take(runtime.allocate_call_id(CallOrigin::ExternalIngress)),
                     binding.connection,
                     TransportId{1},
                     Flow::Backward,
                     end_req,
                     Tick{0},
                     Duration{100},
                     payload};
    host.records.add("externalDelayedCall", delayed);
    auto targetTicket = take(receiver->begin_call(delayed));
    host.records.add("targetDelayedCallTicket", targetTicket.handle);
    WireReturn accepted;
    host.records.add("targetDelayedReturn", accepted);
    host.records.add("targetDelayedExchange", take(receiver->end_call(targetTicket, accepted)));
    host.records.add("initiatorDelayedReturn", take(runtime.ingress(delayed)));
    host.drain(Tick{0});
    publish(2, begin_req, Tick{1});
    require(host.inputs == 2, "future release input ran before second request input");
    host.drain(Tick{100});
    require(host.calls == 2 && host.inputs == 3, "missing wire or business observation");
    require(host.input_order == std::vector<std::pair<PhaseId, Tick>>{{begin_req, Tick{0}},
                                                                      {begin_req, Tick{1}},
                                                                      {end_req, Tick{100}}},
            "business input order ignored effective time");
  }
  for (auto h : ledgers) {
    host.records.add("ledgerHandle", h);
    host.records.add("finalLedger", take(runtime.inspect_hop(h)));
  }
  host.records.add("hostCalls", host.calls);
  host.records.add("businessInputs", host.inputs);
  return host.records.json();
}

struct WireFixture {
  RuntimeConfig config;
  RuntimeConnectionBinding binding;
  ProtocolEngine engine;
  Recorder records;
  Recorder inputs;
  std::uint64_t sequence{};
  PayloadSnapshot payload;

  static RuntimeConnectionBinding selected(const RuntimeConfig &config) {
    require(!config.connection_bindings.empty(), "wire corpus requires descriptor binding");
    auto binding = config.connection_bindings.front();
    require(std::find(config.connections.begin(), config.connections.end(), binding.connection) !=
                config.connections.end(),
            "descriptor binding missing runtime connection");
    return binding;
  }
  explicit WireFixture(ProfileContext &context)
      : config(context.config()), binding(selected(config)),
        engine(config.domain, binding.initiator, 32, 32, 64) {
    payload.command = Command::Read;
    payload.data = {0, 0};
    payload.streaming_width = 2;
    records.add("descriptorTopology",
                Json::object({{"domain", observe(config.domain.value)},
                              {"initiator", observe(binding.initiator.value)},
                              {"target", observe(binding.target.value)},
                              {"connection", observe(binding.connection.value)}}));
    inputs.add("configuration", Json::object({{"domain", observe(config.domain.value)},
                                              {"instance", observe(binding.initiator.value)},
                                              {"target", observe(binding.target.value)},
                                              {"connection", observe(binding.connection.value)},
                                              {"ledgerCapacity", observe(std::uint64_t{32})},
                                              {"ticketCapacity", observe(std::uint64_t{32})},
                                              {"callsPerLedger", observe(std::uint64_t{64})}}));
  }
  Handle ledger(std::uint64_t transport = 1) {
    auto h = take(engine.create_ledger(binding.connection, TransportId{transport}, 1, 0));
    records.add("createdLedgerHandle", h);
    records.add("createdLedger", take(engine.inspect(h)));
    return h;
  }
  Expected<ExchangeResult> exchange(Handle h, PhaseId phase, Sync sync,
                                    std::optional<PhaseId> returned = {}, Tick now = {},
                                    Duration input = {}, Duration output = {},
                                    bool ignored_mutation = false) {
    auto before = take(engine.inspect(h));
    records.add("beforeCall", before);
    WireCall c{CallId{++sequence},
               before.identity.connection,
               before.identity.transport,
               phase == end_req || phase == begin_resp ? Flow::Backward : Flow::Forward,
               phase,
               now,
               input,
               payload};
    if (phase == begin_resp) {
      c.request.status = ResponseStatus::Ok;
      c.request.data = {4, 9};
    }
    records.add("wireCall", c);
    inputs.add("wireCall", c);
    auto ticket = engine.begin_call(h, c);
    if (!ticket) {
      records.add("callError", ticket.error());
      records.add("afterCallError", take(engine.inspect(h)));
      return ticket.error();
    }
    records.add("callTicket", ticket.value().handle);
    records.add("afterCall", take(engine.inspect(h)));
    records.add("afterCallRequestLaneFree", engine.request_lane_free(binding.connection));
    records.add("afterCallResponseLaneFree", engine.response_lane_free(binding.connection));
    WireReturn r{sync, returned, output, {}};
    if (sync == Sync::Completed || (sync == Sync::Updated && returned == begin_resp) ||
        phase == begin_resp)
      r.response = ResponseSnapshot{ResponseStatus::Ok, {4, 9}, false, {}};
    if (ignored_mutation)
      r.response = ResponseSnapshot{
          ResponseStatus::GenericError, {255, 7, 8}, true, {{"ignored-return", {1, 2, 3}}}};
    records.add("wireReturn", r);
    inputs.add("wireReturn", r);
    auto result = engine.end_call(ticket.value(), r);
    if (result) {
      records.add("exchange", result.value());
      records.add("needsAck", result.value().needs_ack);
    } else {
      records.add("returnError", result.error());
      auto failure = take(engine.inspect_failure(ticket.value()));
      records.add("retainedFailedCall", failure.call);
      if (failure.returned)
        records.add("retainedFailedReturn", *failure.returned);
      records.add("retainedFailure", failure.error);
    }
    records.add("afterReturn", take(engine.inspect(h)));
    records.add("requestLaneFree", engine.request_lane_free(binding.connection));
    records.add("responseLaneFree", engine.response_lane_free(binding.connection));
    return result;
  }
  ExchangeResult call(Handle h, PhaseId phase, Sync sync, std::optional<PhaseId> returned = {},
                      Tick now = {}, Duration input = {}, Duration output = {}) {
    return take(exchange(h, phase, sync, returned, now, input, output));
  }
};
} // namespace

Json run_profile_wire(ProfileContext &context, const std::string &id) {
  WireFixture f(context);
  if (id == "C-T01") {
    auto h = f.ledger();
    auto e = f.call(h, begin_req, Sync::Completed, {}, Tick{100}, Duration{10}, Duration{25});
    require(e.wire_terminal, "not terminal");
    for (const auto &m : e.milestones)
      require(m.time == Tick{125}, "return delay double-added");
  } else if (id == "C-T02") {
    auto h = f.ledger();
    auto e = take(f.exchange(h, begin_req, Sync::Accepted, PhaseId{999}, {}, Duration{7},
                             Duration{99}, true));
    require(e.next_state == WireState::Request && e.milestones.empty(), "accepted changed state");
    require(take(f.engine.inspect(h)).last_timing == Tick{7}, "accepted used return delay");
  } else if (id == "C-T03") {
    auto h = f.ledger();
    auto e = f.call(h, begin_req, Sync::Updated, begin_resp, {}, {}, Duration{12});
    require(e.needs_ack && !e.wire_terminal, "shortcut ack state");
    require(e.milestones.size() == 2, "shortcut milestone count");
    require(e.milestones[0].kind == MilestoneKind::RequestReleased && e.milestones[0].implicit,
            "request release not implicit");
    require(take(f.engine.inspect(h)).call_ordinal == 1, "fabricated wire call");
  } else if (id == "C-T04") {
    auto h = f.ledger();
    auto e = f.call(h, begin_req, Sync::Completed);
    require(e.wire_terminal && !e.needs_ack, "completed requires extra ack");
    bool data = false;
    for (const auto &m : e.milestones)
      if (m.response)
        data = m.response->data == Bytes({4, 9});
    require(data, "missing owned response");
    require(take(f.engine.inspect(h)).call_ordinal == 1, "extra call");
  } else if (id == "C-T05") {
    auto h = f.ledger();
    f.call(h, begin_req, Sync::Accepted);
    f.call(h, end_req, Sync::Accepted);
    auto e = f.call(h, begin_resp, Sync::Updated, end_resp, Tick{20}, {}, Duration{5});
    unsigned terminals = 0;
    for (const auto &m : e.milestones)
      if (m.kind == MilestoneKind::Terminal) {
        ++terminals;
        require(m.time == Tick{25}, "early terminal");
      }
    require(terminals == 1 && e.wire_terminal, "terminal count");
    f.records.add("runtimeTerminalConsumption", runtime_terminal_branch(context));
  } else if (id == "C-T06") {
    auto h = f.ledger();
    f.call(h, begin_req, Sync::Updated, begin_resp);
    auto e = f.call(h, end_resp, Sync::Accepted, {}, Tick{3}, Duration{2}, Duration{99});
    require(e.wire_terminal && !e.needs_ack, "END_RESP Accepted not terminal");
    require(e.milestones.back().time == Tick{5}, "terminal annotation");
  } else if (id == "C-T07") {
    for (auto sync : {Sync::Updated, Sync::Completed}) {
      WireFixture fresh(context);
      auto h = fresh.ledger();
      fresh.call(h, begin_req, Sync::Accepted);
      auto result = fresh.exchange(h, end_req, sync, begin_resp);
      require(!result && result.error().code == ErrorCode::ProtocolViolation,
              "illegal END_REQ return accepted");
      require(take(fresh.engine.inspect(h)).faulted, "failure not retained");
      f.records.add("independentIllegalReturnCase", fresh.records.json());
      f.inputs.add("independentIllegalReturnCase", fresh.inputs.json());
    }
  } else if (id == "C-T08") {
    auto a = f.ledger(1);
    f.call(a, begin_req, Sync::Accepted);
    auto released = f.call(a, end_req, Sync::Accepted, {}, {}, Duration{100});
    auto b = f.ledger(2);
    auto accepted = f.call(b, begin_req, Sync::Accepted, {}, Tick{1});
    require(accepted.next_state == WireState::Request, "wire lane still busy");
    require(released.milestones[0].time == Tick{100}, "effective release lost");
    f.records.add("runtimeEffectiveOrder", runtime_wire_branch(context, false));
  } else if (id == "C-T25") {
    take(f.engine.allow_ignorable(PhaseId{99}));
    f.records.add("registeredIgnorablePhase", std::uint64_t{99});
    f.inputs.add("registeredIgnorablePhase", std::uint64_t{99});
    auto h = f.ledger();
    auto e = f.call(h, PhaseId{99}, Sync::Accepted);
    require(e.ignored && e.milestones.empty() && e.next_state == WireState::Idle,
            "ignorable phase changed state");
    f.records.add("runtimeIgnoredNoForward", runtime_wire_branch(context, true));
  } else {
    throw std::runtime_error("unsupported profile wire case: " + id);
  }
  auto observations = Json::object({{"records", f.records.json()},
                                    {"stop", Json("ScenarioCompleted")},
                                    {"fuel", observe(std::uint64_t{0})}});
  return context.record(id, f.inputs.json(), observations,
                        {id == "C-T02" ? "native-accepted-ignored-fields" : "native-wire"},
                        id == "C-T02" ? "Partial" : "Complete");
}
} // namespace conformance::profile

int main(int argc, char **argv) {
  if (argc != 5 && argc != 6)
    return 2;
  try {
    conformance::require(argc == 5 || std::string(argv[5]) == "wire",
                         "profile wire catalog scenario must be wire");
    auto context =
        conformance::take(conformance::profile::ProfileContext::load(argv[2], argv[3], argv[4]));
    std::ofstream output(argv[1]);
    conformance::require(bool(output), "profile wire output unavailable");
    for (const std::string id :
         {"C-T01", "C-T02", "C-T03", "C-T04", "C-T05", "C-T06", "C-T07", "C-T08", "C-T25"})
      output << conformance::profile::run_profile_wire(*context, id).dump() << '\n';
    conformance::require(bool(output), "profile wire output failed");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
