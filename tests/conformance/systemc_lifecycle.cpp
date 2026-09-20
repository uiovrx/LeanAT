#include "harness.hpp"
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
using namespace leanat;
using namespace leanat::systemc;
using namespace conformance;
struct LifecycleMM : tlm::tlm_mm_interface {
  unsigned frees{};
  void free(tlm::tlm_generic_payload *) override {
    ++frees;
  }
};
struct ResetPeer : sc_core::sc_module {
  static RuntimeConfig config(unsigned domain) {
    RuntimeConfig c;
    c.domain = DomainId{domain};
    c.instance = InstanceId{0};
    c.connections = {ConnectionId{1}};
    c.descriptor_identity = "reset-lifecycle";
    c.drain_capacity = 4;
    return c;
  }
  RuntimeHostAdapter host;
  tlm_utils::simple_initiator_socket<ResetPeer> initiator;
  tlm_utils::simple_target_socket<ResetPeer> target;
  LifecycleMM mm;
  tlm::tlm_generic_payload gp{&mm};
  unsigned char byte{7};
  DomainId domain;
  Handle hop{}, transaction{};
  unsigned requests{}, releases{}, responses{}, stale_writes{}, current_writes{};
  bool hold_response{};
  ResetPeer(sc_core::sc_module_name name, unsigned id)
      : sc_module(name), host("host", config(id)), initiator("initiator"), target("target"),
        domain{id} {
    target.register_nb_transport_fw(this, &ResetPeer::forward);
    initiator.register_nb_transport_bw(this, &ResetPeer::backward);
    initiator.bind(target);
    take(host.bind(
        ConnectionId{1},
        [this](const SendIntent &i) { return host.resolve_transport(i.transport); },
        [this](auto &g, auto &p, auto &d) { return target->nb_transport_bw(g, p, d); }));
    take(host.runtime.start({domain, "reset-lifecycle", {ConnectionId{1}}, true, true}));
    host.runtime.set_input_handler(
        [this](const WireCall &c, ReadyKey, Runtime &) -> Expected<void> {
          if (c.phase == begin_req)
            ++requests;
          return {};
        });
    host.runtime.set_handler([this](const QueuedEvent &e, Runtime &) -> Expected<void> {
      if (e.event.epoch == 0)
        ++stale_writes;
      else
        ++current_writes;
      return {};
    });
  }
  tlm::tlm_sync_enum forward(tlm::tlm_generic_payload &g, tlm::tlm_phase &p, sc_core::sc_time &d) {
    return take(host.receive(ConnectionId{1}, Flow::Forward, g, p, d));
  }
  tlm::tlm_sync_enum backward(tlm::tlm_generic_payload &, tlm::tlm_phase &p, sc_core::sc_time &) {
    if (p == tlm::END_REQ) {
      ++releases;
      return tlm::TLM_ACCEPTED;
    }
    require(p == tlm::BEGIN_RESP, "unexpected cleanup phase");
    ++responses;
    return hold_response ? tlm::TLM_ACCEPTED : tlm::TLM_COMPLETED;
  }
  void settle() {
    for (unsigned n = 0; n < 20; ++n)
      sc_core::wait(sc_core::SC_ZERO_TIME);
    require(!host.runtime.stopped(), "Runtime stopped");
  }
  Tick now() {
    return Tick{sc_core::sc_time_stamp().value()};
  }
  void begin() {
    gp.acquire();
    gp.set_command(tlm::TLM_READ_COMMAND);
    gp.set_data_ptr(&byte);
    gp.set_data_length(1);
    gp.set_streaming_width(1);
    gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    tlm::tlm_phase p = tlm::BEGIN_REQ;
    sc_core::sc_time d = sc_core::SC_ZERO_TIME;
    require(initiator->nb_transport_fw(gp, p, d) == tlm::TLM_ACCEPTED, "request not admitted");
    hop = take(host.runtime.protocol().find_ledger(ConnectionId{1}, TransportId{1}));
    transaction = {HandleKind::Transaction, domain, 91, 0, 1, 0};
    take(host.runtime.drains().register_responsibility(
        {transaction, InstanceId{0}, 0, {}, {{hop, false, false, false, false}}, false, false}));
    take(host.runtime.track_cleanup(transaction, hop, false, take(PayloadBridge{}.snapshot(gp))));
    gp.release();
    require(mm.frees == 0, "GP freed while request open");
    EventDraft old;
    old.key = {Tick{now().value + 2}, 0, EventStage::Internal, InstanceId{0}, {}, 0};
    old.epoch = 0;
    take(host.runtime.schedule(old));
  }
  void send(PhaseId phase) {
    auto payload = take(PayloadBridge{}.snapshot(gp));
    if (phase == begin_resp)
      payload.status = ResponseStatus::Ok;
    SendIntent i;
    i.connection = ConnectionId{1};
    i.txn = transaction;
    i.transport = TransportId{1};
    i.flow = Flow::Backward;
    i.phase = phase;
    i.not_before = now();
    i.payload = payload;
    i.call_id = take(host.runtime.allocate_call_id(CallOrigin::Outgoing));
    take(host.runtime.publish(i));
    settle();
  }
  void finish_peer() {
    tlm::tlm_phase p = tlm::END_RESP;
    sc_core::sc_time d = sc_core::SC_ZERO_TIME;
    require(initiator->nb_transport_fw(gp, p, d) == tlm::TLM_ACCEPTED, "END_RESP not accepted");
    settle();
  }
  void verify_finish() {
    settle();
    require(mm.frees == 1, "MM must free exactly once at terminal");
    require(take(host.runtime.protocol().inspect(hop)).state == WireState::Terminal,
            "wire not terminal");
    require(host.runtime.drains().outstanding() == 0, "completed wire left drain backlog");
    auto receipt = take(host.runtime.cancel_local(transaction, CancelReason::Reset)).receipt;
    require(receipt.has_value(), "missing durable drain receipt");
    require(take(host.runtime.drains().inspect(*receipt)).state == DrainState::Complete,
            "drain not complete");
    take(host.runtime.drains().release_receipt(*receipt));
    sc_core::wait(sc_core::sc_time::from_value(3));
    settle();
    require(stale_writes == 0, "old epoch wrote new state");
    EventDraft fresh;
    fresh.key = {now(), 0, EventStage::Internal, InstanceId{0}, {}, 0};
    fresh.epoch = take(host.runtime.drains().epoch(InstanceId{0}));
    take(host.runtime.schedule(fresh));
    settle();
    require(current_writes == 1, "new epoch business did not execute");
  }
  void scenario(unsigned stage) {
    begin();
    if (stage) {
      settle();
      require(requests == 1, "service input missing");
      send(end_req);
      require(take(host.runtime.protocol().inspect(hop)).state == WireState::RequestReleased,
              "not servicing");
    }
    if (stage == 2) {
      hold_response = true;
      send(begin_resp);
      require(take(host.runtime.protocol().inspect(hop)).state == WireState::Response,
              "response not pending");
    }
    take(host.runtime.reset(InstanceId{0}, ResetPolicy::AbortLocalAndDrain));
    require(mm.frees == 0, "reset freed open GP");
    settle();
    if (stage == 2) {
      require(mm.frees == 0, "reset freed response before END_RESP");
      finish_peer();
    }
    require(responses == 1 && releases == 1, "duplicate/missing cleanup phases");
    verify_finish();
  }
  void repeated() {
    hold_response = true;
    begin();
    settle();
    std::optional<Handle> first_receipt;
    for (unsigned n = 0; n < 8; ++n) {
      take(host.runtime.reset(InstanceId{0}, ResetPolicy::AbortLocalAndDrain));
      settle();
      require(host.runtime.drains().outstanding() == 1 && mm.frees == 0,
              "reset lost or released pending responsibility");
      auto disposition = take(host.runtime.cancel_local(transaction, CancelReason::Reset));
      require(disposition.receipt.has_value(), "reset failed to preserve receipt");
      if (first_receipt)
        require(*first_receipt == *disposition.receipt,
                "repeated reset allocated a duplicate receipt");
      else
        first_receipt = disposition.receipt;
      require(take(host.runtime.drains().inspect(*first_receipt)).state == DrainState::Pending,
              "unfinished responsibility falsely reported complete");
      require(host.runtime.drains().stop_report().size() == 1 &&
                  host.runtime.queue().occupied() <= 1,
              "reset accumulated hidden work");
    }
    require(take(host.runtime.drains().epoch(InstanceId{0})) == 8, "reset epoch mismatch");
    require(responses == 1 && releases == 1, "repeated reset duplicated cleanup");
    finish_peer();
    verify_finish();
  }
};

struct BindingComponent : sc_core::sc_module {
  RuntimeHostAdapter &host;
  ConnectionId connection;
  InstanceId initiator_side, target_side;
  tlm_utils::simple_initiator_socket<BindingComponent> initiator;
  tlm_utils::simple_target_socket<BindingComponent> target;
  LifecycleMM mm;
  tlm::tlm_generic_payload gp{&mm};
  unsigned char byte{};
  std::vector<CallId> sent, received;
  BindingComponent(sc_core::sc_module_name name, RuntimeHostAdapter &h, unsigned c, unsigned side)
      : sc_module(name), host(h), connection{c}, initiator_side{side}, target_side{side + 1},
        initiator("initiator"), target("target") {
    target.register_nb_transport_fw(this, &BindingComponent::fw);
    initiator.register_nb_transport_bw(this, &BindingComponent::bw);
    initiator.bind(target);
    take(host.bind(
        connection,
        [this](const SendIntent &) -> Expected<tlm::tlm_generic_payload *> { return &gp; },
        [this](auto &g, auto &p, auto &d) {
          if (p == tlm::BEGIN_REQ) {
            auto ret = initiator->nb_transport_fw(g, p, d);
            gp.release();
            return ret;
          }
          if (p == tlm::END_RESP)
            return initiator->nb_transport_fw(g, p, d);
          return target->nb_transport_bw(g, p, d);
        }));
  }
  tlm::tlm_sync_enum fw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p, sc_core::sc_time &d) {
    return take(host.receive(connection, Flow::Forward, g, p, d));
  }
  tlm::tlm_sync_enum bw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p, sc_core::sc_time &d) {
    return take(host.receive(connection, Flow::Backward, g, p, d));
  }
  void begin() {
    gp.acquire();
    gp.set_command(tlm::TLM_READ_COMMAND);
    gp.set_data_ptr(&byte);
    gp.set_data_length(1);
    gp.set_streaming_width(1);
    gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    take(take(host.runtime.protocol(initiator_side))
             ->create_ledger(connection, TransportId{connection.value}));
    SendIntent i;
    i.connection = connection;
    i.transport = TransportId{connection.value};
    i.call_id = take(host.runtime.allocate_call_id(CallOrigin::Outgoing));
    sent.push_back(i.call_id);
    i.payload = take(PayloadBridge{}.snapshot(gp));
    i.not_before = Tick{sc_core::sc_time_stamp().value()};
    take(host.runtime.publish(i));
  }
  Expected<void> input(const WireCall &c, ReadyKey ready) {
    received.push_back(c.id);
    if (c.phase == end_resp)
      return {};
    SendIntent i;
    i.connection = connection;
    i.transport = c.transport;
    i.call_id = take(host.runtime.allocate_call_id(CallOrigin::Outgoing));
    sent.push_back(i.call_id);
    i.payload = c.request;
    i.not_before = Tick{ready.time.value + 1};
    if (c.phase == begin_req) {
      i.flow = Flow::Backward;
      i.phase = begin_resp;
      i.payload.status = ResponseStatus::Ok;
      i.payload.data = {static_cast<unsigned char>(connection.value + 40)};
    } else {
      require(c.phase == begin_resp &&
                  c.request.data == Bytes{static_cast<unsigned char>(connection.value + 40)},
              "cross binding data/phase");
      i.flow = Flow::Forward;
      i.phase = end_resp;
    }
    return host.runtime.publish(i);
  }
  void verify() {
    require(mm.frees == 1 && byte == connection.value + 40, "instance MM/data isolation");
    require(sent == received && sent.size() == 3, "call pairing not shared across sides");
    for (auto side : {initiator_side, target_side}) {
      auto engine = take(host.runtime.protocol(side));
      auto hop = take(engine->find_ledger(connection, TransportId{connection.value}));
      auto ledger = take(engine->inspect(hop));
      require(ledger.state == WireState::Terminal && ledger.call_ordinal == 3,
              "per-side ledger did not complete");
    }
  }
};
struct MultiBinding : sc_core::sc_module {
  static RuntimeConfig config() {
    RuntimeConfig c;
    c.domain = DomainId{40};
    c.instance = InstanceId{0};
    c.instances = {InstanceId{0}, InstanceId{1}, InstanceId{2}, InstanceId{3}};
    c.connections = {ConnectionId{1}, ConnectionId{2}};
    c.connection_bindings = {{ConnectionId{1}, InstanceId{0}, InstanceId{1}},
                             {ConnectionId{2}, InstanceId{2}, InstanceId{3}}};
    c.descriptor_identity = "multi-lifecycle";
    return c;
  }
  RuntimeHostAdapter host;
  BindingComponent first, second;
  MultiBinding(sc_core::sc_module_name name)
      : sc_module(name), host("host", config()), first("first", host, 1, 0),
        second("second", host, 2, 2) {
    auto c = config();
    take(host.runtime.start({c.domain, c.descriptor_identity, c.connections, true, true,
                             c.instances, c.connection_bindings}));
    host.runtime.set_input_handler([this](const WireCall &c, ReadyKey key, Runtime &) {
      return (c.connection == ConnectionId{1} ? first : second).input(c, key);
    });
  }
  void scenario() {
    first.begin();
    second.begin();
    for (unsigned n = 0; n < 20; ++n)
      sc_core::wait(sc_core::SC_ZERO_TIME);
    require(!take(host.runtime.protocol(InstanceId{0}))->request_lane_free(ConnectionId{1}) &&
                !take(host.runtime.protocol(InstanceId{2}))->request_lane_free(ConnectionId{2}),
            "independent requests not simultaneously open");
    require(first.mm.frees == 0 && second.mm.frees == 0, "GP freed while requests active");
    sc_core::wait(sc_core::sc_time::from_value(4));
    for (unsigned n = 0; n < 20; ++n)
      sc_core::wait(sc_core::SC_ZERO_TIME);
    require(!host.runtime.stopped(), "multi Runtime stopped");
    first.verify();
    second.verify();
    for (auto a : first.sent)
      for (auto b : second.sent)
        require(a != b, "cross binding CallId collision");
  }
};
struct LifecycleRunner : sc_core::sc_module {
  Report &report;
  ResetPeer request, service, response, repeated;
  MultiBinding multi;
  SC_HAS_PROCESS(LifecycleRunner);
  LifecycleRunner(sc_core::sc_module_name name, Report &r)
      : sc_module(name), report(r), request("request", 31), service("service", 32),
        response("response", 33), repeated("repeated", 34), multi("multi") {
    SC_THREAD(run);
  }
  void run() {
    report.run("C-T15",
               "real bound socket/MM target reset in Request, RequestReleased service, Response "
               "awaiting END_RESP; queued epoch0 business",
               "one release+response, no early MM free, terminal frees1, stale writes0, new epoch "
               "writes1 per stage",
               [&] {
                 request.scenario(0);
                 service.scenario(1);
                 response.scenario(2);
                 return "3 stages: cleanup END_REQ1 BEGIN_RESP1 each; MM frees1 only terminal; old "
                        "writes0/new writes1 each";
               });
    report.run("E-T37",
               "eight AbortLocalAndDrain resets while one real GP response remains unacknowledged",
               "epochs1..8; backlog remains1, cleanup phases once, no free until ACK, final "
               "backlog0/free1",
               [&] {
                 repeated.repeated();
                 return "8 resets, epoch8, backlog1 until END_RESP, END_REQ1 BEGIN_RESP1, final "
                        "backlog0/MM frees1, old writes0/new writes1";
               });
    report.run("C-T09",
               "two instances of one socket component; two connections/four local protocol sides; "
               "simultaneous READs",
               "both request gates occupied independently, no cross data or CallId collision, each "
               "side terminal3, MM frees1 each",
               [&] {
                 multi.scenario();
                 return "two simultaneous open request lanes; READ41/42 isolated; six distinct "
                        "paired calls; four terminal ledgers; MM frees1 each";
               });
    sc_core::sc_stop();
  }
};
int sc_main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  Report report(argv[1]);
  LifecycleRunner runner("lifecycle", report);
  sc_core::sc_start();
  return report.failures ? 1 : 0;
}
