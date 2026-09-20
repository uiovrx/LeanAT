#include <cassert>
#include <leanat/systemc/adapter.hpp>
#include <leanat/systemc/typed_adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
using namespace leanat;
using namespace leanat::systemc;
struct MM : tlm::tlm_mm_interface {
  int frees{};
  void free(tlm::tlm_generic_payload *) override {
    ++frees;
  }
};
struct VendorTypes {
  using tlm_payload_type = tlm::tlm_generic_payload;
  using tlm_phase_type = tlm::tlm_phase;
};
struct TestService : PreparedParticipant {
  int &applied;
  explicit TestService(int &a) : applied(a) {}
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this);
  }
  Expected<void> validate() const override {
    return {};
  }
  void apply() noexcept override {
    ++applied;
  }
  void discard() noexcept override {}
};
struct Test : sc_core::sc_module {
  RuntimeDomain domain;
  NbBridge target_bridge;
  BlockingBridge blocking;
  static CheckedAdapter mapping() {
    AdapterContract l, r;
    std::map<PhaseId, PhaseId> map;
    for (unsigned i = 1; i <= 4; ++i) {
      l.phases.push_back(PhaseId{i});
      r.phases.push_back(PhaseId{i + 10});
      map.emplace(PhaseId{i}, PhaseId{i + 10});
      AdapterRule a;
      a.phase = PhaseId{i};
      a.guard = "true";
      l.rules.push_back(a);
      a.phase = PhaseId{i + 10};
      r.rules.push_back(a);
    }
    auto good = CheckedAdapter::validate(l, r, map);
    assert(good);
    r.rules[0].request_lanes = 2;
    assert(!CheckedAdapter::validate(l, r, map));
    return good.value();
  }
  TypedSocketAdapter<tlm::tlm_base_protocol_types, VendorTypes> vendor;
  tlm_utils::simple_target_socket<Test, 32, VendorTypes> target;
  tlm_utils::simple_initiator_socket<Test> initiator;
  static RuntimeConfig config() {
    RuntimeConfig c;
    c.domain = DomainId{2};
    c.instance = InstanceId{2};
    c.descriptor_identity = "test";
    c.connections = {ConnectionId{2}};
    return c;
  }
  RuntimeHostAdapter host;
  sc_core::sc_signal<bool> signal;
  BoolSignalInput signal_input;
  MM hostmm;
  tlm::tlm_generic_payload hostgp;
  unsigned char hostdata[1]{};
  MM asyncmm;
  tlm::tlm_generic_payload asyncgp{&asyncmm};
  unsigned char asyncdata[1]{};
  MM inboundmm;
  tlm::tlm_generic_payload inboundgp{&inboundmm};
  unsigned char inbounddata[1]{42};
  unsigned char inboundmask[1]{0};
  unsigned actual_response{};
  TransportId inboundid{};
  int pumped{}, sent{};
  Bytes storage = Bytes(16);
  int calls{};
  MM mm;
  tlm::tlm_generic_payload gp;
  unsigned char data[4]{};
  SC_HAS_PROCESS(Test);
  Test(sc_core::sc_module_name n)
      : sc_module(n), domain("domain", DomainId{1}),
        target_bridge(domain, InstanceId{1}, ConnectionId{1}), blocking(domain),
        vendor("vendor", mapping(),
               {{PhaseId{11}, tlm::BEGIN_REQ},
                {PhaseId{12}, tlm::END_REQ},
                {PhaseId{13}, tlm::BEGIN_RESP},
                {PhaseId{14}, tlm::END_RESP}}),
        target("target"), initiator("initiator"), host("c4pump", config(), TimeCodec{}, 1),
        signal("signal"), signal_input("signal_input", host.runtime, InstanceId{2}, PortId{1}),
        hostgp(&hostmm), gp(&mm) {
    target.register_nb_transport_fw(this, &Test::fw);
    target.register_b_transport(this, &Test::bt);
    initiator.register_nb_transport_bw(this, &Test::bw);
    initiator.bind(vendor.left);
    vendor.right.bind(target);
    gp.acquire();
    gp.set_command(tlm::TLM_READ_COMMAND);
    gp.set_data_ptr(data);
    gp.set_data_length(4);
    gp.set_streaming_width(4);
    gp.set_address(0);
    signal_input.input(signal);
    host.output = [this](PortId, const Value &v) { signal.write(std::get<bool>(v.data)); };
    hostgp.acquire();
    hostgp.set_command(tlm::TLM_READ_COMMAND);
    hostgp.set_data_ptr(hostdata);
    hostgp.set_data_length(1);
    hostgp.set_streaming_width(1);
    asyncgp.acquire();
    asyncgp.set_command(tlm::TLM_READ_COMMAND);
    asyncgp.set_data_ptr(asyncdata);
    asyncgp.set_data_length(1);
    asyncgp.set_streaming_width(1);
    assert(host.bind(
        ConnectionId{2},
        [this](const SendIntent &i) -> Expected<tlm::tlm_generic_payload *> {
          if (i.transport == TransportId{1})
            return &hostgp;
          if (i.transport == TransportId{10})
            return &asyncgp;
          return host.resolve_transport(i.transport);
        },
        [this](auto &g, auto &phase, auto &d) {
          ++sent;
          if (&g == &asyncgp) {
            if (phase == tlm::END_RESP)
              return tlm::TLM_ACCEPTED;
            assert(phase == tlm::BEGIN_REQ);
            auto time = Tick{sc_core::sc_time_stamp().value() + 1};
            assert(domain.schedule(time, [this] {
              asyncdata[0] = 88;
              asyncgp.set_response_status(tlm::TLM_OK_RESPONSE);
              tlm::tlm_phase phase = tlm::BEGIN_RESP;
              auto delay = sc_core::sc_time::from_value(1);
              auto result = host.receive(ConnectionId{2}, Flow::Backward, asyncgp, phase, delay);
              assert(result && result.value() == tlm::TLM_ACCEPTED);
            }));
            asyncgp.release();
            return tlm::TLM_ACCEPTED;
          }
          g.set_response_status(tlm::TLM_OK_RESPONSE);
          if (phase == tlm::BEGIN_RESP)
            assert(g.get_data_ptr()[0] == 42);
          else
            hostdata[0] = 55;
          d = sc_core::sc_time::from_value(2);
          return tlm::TLM_COMPLETED;
        }));
    assert(host.runtime.start({DomainId{2}, "test", {ConnectionId{2}}, true, true}));
    host.runtime.set_handler([this](const QueuedEvent &, Runtime &) -> Expected<void> {
      ++pumped;
      return {};
    });
    host.runtime.set_milestone_handler(
        [this](const RuntimeMilestone &m, ReadyKey, Runtime &) -> Expected<void> {
          if (m.milestone.response && !m.milestone.response->data.empty() &&
              m.milestone.response->data[0] == 42)
            actual_response = 42;
          return {};
        });
    host.runtime.set_input_handler(
        [this](const WireCall &call, ReadyKey ready, Runtime &runtime) -> Expected<void> {
          if (call.phase == begin_req)
            inboundid = call.transport;
          auto id = runtime.allocate_call_id(CallOrigin::Outgoing);
          if (!id)
            return id.error();
          SendIntent response;
          response.connection = call.connection;
          response.transport = call.transport;
          response.call_id = id.value();
          response.flow = call.phase == begin_resp ? Flow::Forward : Flow::Backward;
          response.phase = call.phase == begin_resp ? end_resp : begin_resp;
          response.not_before = ready.time;
          response.payload = call.request;
          response.payload.status = ResponseStatus::Ok;
          if (call.phase != begin_resp)
            response.payload.data = {66};
          return runtime.publish(response);
        });
    EventDraft ev;
    ev.key.time = Tick{3};
    assert(host.runtime.schedule(ev));
    assert(host.runtime.schedule(ev));
    auto ledger = host.runtime.protocol().create_ledger(ConnectionId{2}, TransportId{1});
    assert(ledger);
    auto id = host.runtime.allocate_call_id(CallOrigin::Outgoing);
    assert(id);
    SendIntent intent;
    intent.call_id = id.value();
    intent.connection = ConnectionId{2};
    intent.transport = TransportId{1};
    intent.not_before = Tick{4};
    intent.payload = PayloadBridge{}.snapshot(hostgp).value();
    assert(host.runtime.publish(intent));
    auto asyncledger = host.runtime.protocol().create_ledger(ConnectionId{2}, TransportId{10});
    assert(asyncledger);
    auto asyncid = host.runtime.allocate_call_id(CallOrigin::Outgoing);
    assert(asyncid);
    SendIntent asynchronous;
    asynchronous.call_id = asyncid.value();
    asynchronous.connection = ConnectionId{2};
    asynchronous.transport = TransportId{10};
    asynchronous.not_before = Tick{1};
    asynchronous.payload = PayloadBridge{}.snapshot(asyncgp).value();
    assert(host.runtime.publish(asynchronous));
    SC_THREAD(run);
    assert(domain.start());
  }
  tlm::tlm_sync_enum fw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p, sc_core::sc_time &d) {
    auto r = target_bridge.receive(
        Flow::Forward, g, p, d, [&](const WireCall &c) -> Expected<WireReturn> {
          ++calls;
          if (c.phase == begin_req) {
            auto due = add_time(c.call_time, c.incoming_delay);
            assert(due);
            assert(domain.schedule(due.value(), [this] {
              tlm::tlm_phase p = tlm::BEGIN_RESP;
              sc_core::sc_time d = sc_core::sc_time::from_value(2);
              gp.set_response_status(tlm::TLM_OK_RESPONSE);
              data[0] = 42;
              auto r = target_bridge.exchange(Flow::Backward, gp, p, d,
                                              [this](auto &g, auto &ph, auto &dt) {
                                                return target->nb_transport_bw(g, ph, dt);
                                              });
              assert(r && r.value().sync == Sync::Accepted);
            }));
          }
          return WireReturn{};
        });
    assert(r);
    return r.value();
  }
  tlm::tlm_sync_enum bw(tlm::tlm_generic_payload &, tlm::tlm_phase &p, sc_core::sc_time &d) {
    assert(p == tlm::BEGIN_RESP);
    assert(data[0] == 42);
    assert(domain.schedule(Tick{sc_core::sc_time_stamp().value() + d.value()}, [this] {
      tlm::tlm_phase p = tlm::END_RESP;
      auto d = sc_core::SC_ZERO_TIME;
      assert(initiator->nb_transport_fw(gp, p, d) == tlm::TLM_ACCEPTED);
      assert(target_bridge.active() == 0);
      gp.release();
    }));
    return tlm::TLM_ACCEPTED;
  }
  void bt(tlm::tlm_generic_payload &g, sc_core::sc_time &d) {
    auto r = blocking.transport(g, d, Duration{3},
                                [this](const PayloadSnapshot &p) -> Expected<ResponseSnapshot> {
                                  ResponseSnapshot r{ResponseStatus::Ok, p.data, false, {}};
                                  if (p.command == Command::Read)
                                    r.data[0] = storage[0];
                                  else
                                    storage[0] = p.data[0];
                                  return r;
                                });
    assert(r);
  }
  void run() {
    tlm::tlm_phase p = tlm::BEGIN_REQ;
    auto d = sc_core::sc_time::from_value(5);
    assert(initiator->nb_transport_fw(gp, p, d) == tlm::TLM_ACCEPTED);
    assert(d.value() == 5);
    sc_core::wait(sc_core::sc_time::from_value(10));
    assert(mm.frees == 1 && calls == 2);
    assert(pumped == 2 && sent == 3 && hostdata[0] == 55 && !host.runtime.stopped());
    assert(asyncmm.frees == 1 && !host.resolve_transport(TransportId{10}));
    hostgp.release();
    assert(hostmm.frees == 1);
    inboundgp.acquire();
    inboundgp.set_command(tlm::TLM_READ_COMMAND);
    inboundgp.set_data_ptr(inbounddata);
    inboundgp.set_data_length(1);
    inboundgp.set_streaming_width(1);
    inboundgp.set_byte_enable_ptr(inboundmask);
    inboundgp.set_byte_enable_length(1);
    tlm::tlm_phase incoming = tlm::BEGIN_REQ;
    auto incoming_delay = sc_core::SC_ZERO_TIME;
    auto received =
        host.receive(ConnectionId{2}, Flow::Forward, inboundgp, incoming, incoming_delay);
    assert(received && received.value() == tlm::TLM_ACCEPTED);
    inboundgp.release();
    tlm::tlm_generic_payload b;
    unsigned char v = 91;
    b.set_command(tlm::TLM_WRITE_COMMAND);
    b.set_data_ptr(&v);
    b.set_data_length(1);
    b.set_streaming_width(1);
    host.publish_output(PortId{1}, Value{false});
    host.publish_output(PortId{1}, Value{false});
    host.publish_output(PortId{1}, Value{true});
    for (int delta = 0; delta < 8; ++delta)
      sc_core::wait(sc_core::SC_ZERO_TIME);
    assert(inboundmm.frees == 1);
    inbounddata[0] = 99; // Future C4 response milestone must retain the captured 42.
    host.publish_output(PortId{1}, Value{false});
    auto bd = sc_core::sc_time::from_value(2);
    auto before = sc_core::sc_time_stamp();
    initiator->b_transport(b, bd);
    assert(sc_core::sc_time_stamp().value() == before.value() + 5 && bd == sc_core::SC_ZERO_TIME &&
           storage[0] == 91);
    assert(pumped == 4);
    assert(sent == 4 && inboundmm.frees == 1 && !host.resolve_transport(inboundid));
    assert(actual_response == 42);
    {
      NativeGuard guard;
      auto denied = blocking.transport(b, bd, Duration{}, [](auto &) -> Expected<ResponseSnapshot> {
        return fail(ErrorCode::InvalidState, "should not run");
      });
      assert(!denied);
    }
    auto dbg = BlockingBridge::debug(b, 1, [this](auto a, auto &v, bool write) {
      if (a >= storage.size())
        return false;
      if (write)
        storage[a] = v;
      else
        v = storage[a];
      return true;
    });
    assert(dbg && dbg.value() == 1);
    sc_core::sc_stop();
  }
};
int sc_main(int, char **) {
  bool duplicate_native_rejected = false;
  try {
    TypedSocketAdapter<> duplicate_native("duplicate_native", Test::mapping(),
                                          {{PhaseId{11}, tlm::BEGIN_REQ},
                                           {PhaseId{12}, tlm::BEGIN_REQ},
                                           {PhaseId{13}, tlm::BEGIN_RESP},
                                           {PhaseId{14}, tlm::END_RESP}});
  } catch (const std::invalid_argument &) {
    duplicate_native_rejected = true;
  }
  assert(duplicate_native_rejected);
  RuntimeConfig committing = Test::config();
  committing.domain = DomainId{5};
  RuntimeHostAdapter bad_copyback("bad_copyback", committing);
  assert(bad_copyback.runtime.start({DomainId{5}, "test", {ConnectionId{2}}, true, true}));
  int applied = 0;
  bad_copyback.runtime.set_ingress_policy([&](const WireCall &) -> Expected<IngressPlan> {
    IngressPlan p;
    p.complete_now = true;
    p.prepared_service = std::make_unique<TestService>(applied);
    p.reply.sync = Sync::Completed;
    p.reply.response = ResponseSnapshot{ResponseStatus::Ok, {1}, false, {{"unregistered", {1}}}};
    return p;
  });
  MM copymm;
  tlm::tlm_generic_payload copygp(&copymm);
  unsigned char copydata = 0;
  copygp.acquire();
  copygp.set_command(tlm::TLM_READ_COMMAND);
  copygp.set_data_ptr(&copydata);
  copygp.set_data_length(1);
  copygp.set_streaming_width(1);
  tlm::tlm_phase copyphase = tlm::BEGIN_REQ;
  auto copydelay = sc_core::SC_ZERO_TIME;
  auto copy_result =
      bad_copyback.receive(ConnectionId{2}, Flow::Forward, copygp, copyphase, copydelay);
  assert(!copy_result && bad_copyback.runtime.stopped() && applied == 1 && copydata == 0);
  copygp.release();
  assert(copymm.frees == 1);
  RuntimeConfig small = Test::config();
  small.domain = DomainId{3};
  small.event_capacity = 1;
  small.call_capacity = 1;
  RuntimeHostAdapter failed("failed_ingress", small);
  assert(failed.runtime.start({DomainId{3}, "test", {ConnectionId{2}}, true, true}));
  MM failuremm;
  tlm::tlm_generic_payload failuregp(&failuremm);
  failuregp.acquire();
  for (int retry = 0; retry < 2; ++retry) {
    tlm::tlm_phase phase = tlm::BEGIN_REQ;
    auto delay = sc_core::SC_ZERO_TIME;
    auto error = failed.receive(ConnectionId{2}, Flow::Forward, failuregp, phase, delay);
    assert(!error && error.error().code == ErrorCode::Capacity);
    assert(failuregp.get_ref_count() == 1);
  }
  failuregp.release();
  assert(failuremm.frees == 1);
  PayloadBridge payload;
  tlm::tlm_generic_payload unowned;
  assert(!payload.pin_nb(unowned));
  unowned.set_data_length(UINT_MAX);
  unowned.set_byte_enable_length(UINT_MAX);
  assert(payload.snapshot_call(unowned, end_req));
  assert(payload.snapshot_call(unowned, end_resp));
  PayloadSnapshot specification;
  specification.command = Command::Read;
  specification.data = {1, 2, 3};
  auto managed = payload.make_managed(specification);
  assert(managed && managed.value().get().has_mm());
  auto snapshot = payload.snapshot(managed.value().get());
  managed.value().get().get_data_ptr()[0] = 9;
  assert(snapshot && snapshot.value().data[0] == 1);
  RuntimeDomain stack_domain("stack_domain", DomainId{4});
  NbBridge stack_bridge(stack_domain, InstanceId{4}, ConnectionId{4});
  PayloadSnapshot empty;
  auto first = payload.make_managed(empty);
  auto second = payload.make_managed(empty);
  tlm::tlm_phase stack_phase = tlm::BEGIN_REQ;
  auto stack_delay = sc_core::SC_ZERO_TIME;
  auto accepted = [](const WireCall &) -> Expected<WireReturn> { return WireReturn{}; };
  assert(
      stack_bridge.receive(Flow::Forward, first.value().get(), stack_phase, stack_delay, accepted));
  first.value().get().set_response_status(tlm::TLM_OK_RESPONSE);
  stack_phase = tlm::BEGIN_RESP;
  assert(stack_bridge.receive(Flow::Backward, first.value().get(), stack_phase, stack_delay,
                              accepted));
  stack_phase = tlm::END_RESP;
  assert(stack_bridge.receive(Flow::Forward, first.value().get(), stack_phase, stack_delay,
                              [&](const WireCall &outer) -> Expected<WireReturn> {
                                auto identity = outer.id;
                                tlm::tlm_phase inner_phase = tlm::BEGIN_REQ;
                                auto inner_delay = sc_core::SC_ZERO_TIME;
                                auto inner = stack_bridge.receive(
                                    Flow::Forward, second.value().get(), inner_phase, inner_delay,
                                    [](const WireCall &) -> Expected<WireReturn> {
                                      WireReturn ret;
                                      ret.sync = Sync::Completed;
                                      ret.response =
                                          ResponseSnapshot{ResponseStatus::Ok, {}, false, {}};
                                      return ret;
                                    });
                                assert(inner);
                                assert(outer.id == identity && outer.phase == end_resp);
                                return WireReturn{};
                              }));
  assert(stack_bridge.active() == 0);
  TimeCodec c(10);
  assert(!c.to_tick(sc_core::sc_time::from_value(11)));
  assert(!c.effective_time(sc_core::sc_time::from_value(3), sc_core::sc_time::from_value(7)));
  assert(!c.from_tick(Tick{UINT64_MAX}));
  assert(c.effective_time(sc_core::sc_time::from_value(1000), sc_core::sc_time::from_value(250))
             .value()
             .value == 125);
  auto backing = std::make_shared<Bytes>(4, 7);
  RawDmiHost raw(backing, 100, TimeCodec{}, Duration{1}, Duration{2});
  tlm::tlm_generic_payload query;
  query.set_address(101);
  query.set_command(tlm::TLM_READ_COMMAND);
  tlm::tlm_dmi grant;
  int invalidations = 0;
  assert(raw.get(query, grant, [&](auto s, auto e) {
    assert(s == 100 && e == 103);
    ++invalidations;
    tlm::tlm_dmi nested;
    assert(!raw.get(query, nested, [](auto, auto) {}));
  }));
  assert(grant.get_dmi_ptr() == backing->data());
  assert(raw.replace(std::make_shared<Bytes>(8)));
  assert(invalidations == 1 && raw.generation() == 2);
  query.set_address(999);
  assert(!raw.get(query, grant, [](auto, auto) {}).value() && grant.get_dmi_ptr() == nullptr);
  Test t("test");
  sc_core::sc_start();
  return 0;
}
