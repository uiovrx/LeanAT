#include <cassert>
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
using namespace leanat;
using namespace leanat::systemc;
struct Manager : tlm::tlm_mm_interface {
  unsigned freed{};
  void free(tlm::tlm_generic_payload *) override {
    ++freed;
  }
};
struct Shared : sc_core::sc_module {
  static RuntimeConfig config() {
    RuntimeConfig c;
    c.domain = DomainId{9};
    c.instance = InstanceId{9};
    c.instances = {InstanceId{9}, InstanceId{10}};
    c.connections = {ConnectionId{9}};
    c.connection_bindings = {{ConnectionId{9}, InstanceId{9}, InstanceId{10}}};
    c.descriptor_identity = "shared";
    return c;
  }
  RuntimeHostAdapter host;
  tlm_utils::simple_target_socket<Shared> target;
  tlm_utils::simple_initiator_socket<Shared> initiator;
  Manager manager;
  tlm::tlm_generic_payload gp{&manager};
  unsigned char data{4};
  std::vector<CallId> sent, received;
  SC_HAS_PROCESS(Shared);
  Shared(sc_core::sc_module_name n)
      : sc_module(n), host("pump", config()), target("target"), initiator("initiator") {
    target.register_nb_transport_fw(this, &Shared::fw);
    initiator.register_nb_transport_bw(this, &Shared::bw);
    initiator.bind(target);
    gp.acquire();
    gp.set_command(tlm::TLM_READ_COMMAND);
    gp.set_data_ptr(&data);
    gp.set_data_length(1);
    gp.set_streaming_width(1);
    assert(host.bind(
        ConnectionId{9},
        [this](const SendIntent &) -> Expected<tlm::tlm_generic_payload *> { return &gp; },
        [this](auto &g, auto &p, auto &d) {
          if (p == tlm::BEGIN_REQ) {
            auto r = initiator->nb_transport_fw(g, p, d);
            gp.release();
            return r;
          }
          if (p == tlm::END_RESP)
            return initiator->nb_transport_fw(g, p, d);
          return target->nb_transport_bw(g, p, d);
        }));
    auto c = config();
    HostBindingManifest manifest{c.domain, c.descriptor_identity, c.connections,        true,
                                 true,     c.instances,           c.connection_bindings};
    assert(host.runtime.start(manifest));
    host.runtime.set_input_handler(
        [this](const WireCall &call, ReadyKey ready, Runtime &r) -> Expected<void> {
          received.push_back(call.id);
          if (call.phase == end_resp)
            return {};
          auto id = r.allocate_call_id(CallOrigin::Outgoing);
          if (!id)
            return id.error();
          sent.push_back(id.value());
          SendIntent output;
          output.connection = call.connection;
          output.transport = call.transport;
          output.call_id = id.value();
          output.payload = call.request;
          output.not_before = Tick{ready.time.value + 1};
          if (call.phase == begin_req) {
            output.flow = Flow::Backward;
            output.phase = begin_resp;
            output.payload.status = ResponseStatus::Ok;
            output.payload.data = {77};
          } else {
            assert(call.phase == begin_resp && call.request.data == Bytes{77});
            output.flow = Flow::Forward;
            output.phase = end_resp;
          }
          return r.publish(output);
        });
    auto engine = host.runtime.protocol(InstanceId{9});
    assert(engine);
    assert(engine.value()->create_ledger(ConnectionId{9}, TransportId{1}));
    auto id = host.runtime.allocate_call_id(CallOrigin::Outgoing);
    assert(id);
    sent.push_back(id.value());
    SendIntent initial;
    initial.connection = ConnectionId{9};
    initial.transport = TransportId{1};
    initial.call_id = id.value();
    initial.payload = PayloadBridge{}.snapshot(gp).value();
    assert(host.runtime.publish(initial));
    SC_THREAD(check);
  }
  tlm::tlm_sync_enum fw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p, sc_core::sc_time &d) {
    auto r = host.receive(ConnectionId{9}, Flow::Forward, g, p, d);
    assert(r);
    return r.value();
  }
  tlm::tlm_sync_enum bw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p, sc_core::sc_time &d) {
    auto r = host.receive(ConnectionId{9}, Flow::Backward, g, p, d);
    assert(r);
    return r.value();
  }
  void check() {
    sc_core::wait(sc_core::sc_time::from_value(5));
    assert(!host.runtime.stopped());
    assert(manager.freed == 1 && sent == received && sent.size() == 3);
    for (auto side : {InstanceId{9}, InstanceId{10}}) {
      auto engine = host.runtime.protocol(side);
      assert(engine);
      auto ledger = engine.value()->find_ledger(ConnectionId{9}, TransportId{1});
      assert(ledger);
      auto snapshot = engine.value()->inspect(ledger.value());
      assert(snapshot && snapshot.value().state == WireState::Terminal &&
             snapshot.value().call_ordinal == 3);
    }
    sc_core::sc_stop();
  }
};
int sc_main(int, char **) {
  Shared test("shared");
  sc_core::sc_start();
  return 0;
}
