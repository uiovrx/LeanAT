#include <cassert>
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
using namespace leanat;
using namespace leanat::systemc;
struct MM : tlm::tlm_mm_interface {
  unsigned freed{};
  void free(tlm::tlm_generic_payload *) override {
    ++freed;
  }
};
struct Test : sc_core::sc_module {
  static RuntimeConfig config(unsigned id) {
    RuntimeConfig c;
    c.domain = DomainId{id};
    c.instance = InstanceId{id};
    c.connections = {ConnectionId{id}};
    c.descriptor_identity = "host-lifetime";
    return c;
  }
  MM mm;
  unsigned char byte{7};
  tlm::tlm_generic_payload gp{&mm};
  RuntimeHostAdapter fault, business;
  tlm_utils::simple_target_socket<Test> target;
  tlm_utils::simple_initiator_socket<Test> initiator;
  bool serviced{}, blocked_done{};
  SC_HAS_PROCESS(Test);
  Test(sc_core::sc_module_name n)
      : sc_module(n), fault("fault", config(21)), business("business", config(22)),
        target("target"), initiator("initiator") {
    target.register_b_transport(this, &Test::blocking);
    initiator.bind(target);
    for (auto *host : {&fault, &business}) {
      auto c = config(host == &fault ? 21 : 22);
      assert(host->runtime.start({c.domain, c.descriptor_identity, c.connections, true, true}));
    }
    assert(fault.bind(
        ConnectionId{21},
        [this](const SendIntent &) -> Expected<tlm::tlm_generic_payload *> { return &gp; },
        [](auto &, auto &p, auto &) {
          assert(p == tlm::END_REQ);
          return tlm::TLM_COMPLETED;
        }));
    fault.runtime.set_input_handler(
        [](const WireCall &call, ReadyKey ready, Runtime &r) -> Expected<void> {
          auto id = r.allocate_call_id(CallOrigin::Outgoing);
          if (!id)
            return id.error();
          SendIntent send;
          send.call_id = id.value();
          send.connection = call.connection;
          send.transport = call.transport;
          send.flow = Flow::Backward;
          send.phase = end_req;
          send.not_before = ready.time;
          send.payload = call.request;
          return r.publish(send);
        });
    business.runtime.set_blocking_handler(
        [this](const BlockingRequest &r, Runtime &runtime) -> Expected<void> {
          assert(sc_core::sc_time_stamp().value() == 3);
          assert(r.arrival == Tick{3});
          assert(r.request.data == Bytes{11});
          serviced = true;
          ResponseSnapshot response;
          response.status = ResponseStatus::Ok;
          response.data = {44};
          return runtime.complete_blocking(r.token, response);
        });
    gp.acquire();
    gp.set_command(tlm::TLM_READ_COMMAND);
    gp.set_data_ptr(&byte);
    gp.set_data_length(1);
    gp.set_streaming_width(1);
    SC_THREAD(run);
    SC_THREAD(check);
  }
  void blocking(tlm::tlm_generic_payload &g, sc_core::sc_time &delay) {
    assert(business.blocking(ConnectionId{22}, g, delay));
  }
  void run() {
    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    sc_core::sc_time zero = sc_core::SC_ZERO_TIME;
    auto received = fault.receive(ConnectionId{21}, Flow::Forward, gp, phase, zero);
    assert(received && received.value() == tlm::TLM_ACCEPTED);
    gp.release();
    assert(mm.freed == 0);
    unsigned char data = 11;
    tlm::tlm_generic_payload request;
    request.set_command(tlm::TLM_READ_COMMAND);
    request.set_data_ptr(&data);
    request.set_data_length(1);
    request.set_streaming_width(1);
    auto delay = sc_core::sc_time::from_value(3);
    initiator->b_transport(request, delay);
    assert(serviced && data == 44 && delay == sc_core::SC_ZERO_TIME &&
           sc_core::sc_time_stamp().value() == 3);
    blocked_done = true;
  }
  void check() {
    sc_core::wait(sc_core::sc_time::from_value(5));
    assert(fault.runtime.stopped());
    assert(mm.freed == 0 && gp.get_ref_count() == 1);
    assert(fault.resolve_transport(TransportId{1}));
    assert(blocked_done && !business.runtime.stopped());
    sc_core::sc_stop();
  }
};
int sc_main(int, char **) {
  // This fixture deliberately faults one Runtime and inspects its retained ownership.
  sc_core::sc_report_handler::set_actions("LeanAT", sc_core::SC_ERROR, sc_core::SC_DO_NOTHING);
  Test test("test");
  sc_core::sc_start();
  return 0;
}
