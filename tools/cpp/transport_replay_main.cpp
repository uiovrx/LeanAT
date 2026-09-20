#include <array>
#include <fstream>
#include <iostream>
#include <leanat/runtime.hpp>
#include <sstream>
#include <stdexcept>
#ifdef LEANAT_SYSTEMC_TRANSPORT
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#endif
using namespace leanat;
namespace {
template <class T> T checked(Expected<T> v) {
  if (!v)
    throw std::runtime_error(v.error().message);
  return std::move(v.value());
}
void checked(Expected<void> v) {
  if (!v)
    throw std::runtime_error(v.error().message);
}
struct Callback {
  uint64_t time, connection, phase, delay;
  Bytes data;
};
struct Feedback {
  unsigned connection, phase, sync;
  uint64_t delay;
};
struct Script {
  bool divergent_outbound{};
  std::vector<Feedback> feedback;
  std::array<PayloadSnapshot, 2> requests;
  std::vector<Callback> callbacks;
};
Bytes bytes(std::istream &in) {
  unsigned n;
  if (!(in >> n) || n > 64)
    throw std::runtime_error("bounded bytes required");
  Bytes out;
  for (unsigned j = 0; j < n; ++j) {
    unsigned b;
    if (!(in >> b) || b > 255)
      throw std::runtime_error("invalid byte");
    out.push_back(uint8_t(b));
  }
  return out;
}
Script read(const char *path) {
  std::ifstream in(path);
  in.seekg(0, std::ios::end);
  auto size = in.tellg();
  if (size < 0 || size > 8192)
    throw std::runtime_error("script byte bound exceeded");
  in.seekg(0);
  std::string magic;
  in >> magic;
  if (magic != "LEANAT_TRANSPORT_V1")
    throw std::runtime_error("unsupported script schema");
  Script s;
  for (auto &p : s.requests) {
    p.command = Command::Read;
    p.address = 0;
    p.data = bytes(in);
    p.byte_enable = bytes(in);
    p.streaming_width = p.data.size();
    if (p.data.size() != 4 || p.byte_enable.size() != 2)
      throw std::runtime_error("only bounded masked four-byte reads supported");
  }
  unsigned n;
  if (!(in >> n) || n != 4)
    throw std::runtime_error("four callbacks required");
  for (unsigned j = 0; j < n; ++j) {
    Callback c;
    if (!(in >> c.time >> c.connection >> c.phase >> c.delay) || c.time > 100 || c.connection < 1 ||
        c.connection > 2 || (c.phase != 2 && c.phase != 3) || c.delay > 10)
      throw std::runtime_error("invalid callback");
    c.data = bytes(in);
    if (c.data.size() != 4)
      throw std::runtime_error("full callback bytes required");
    s.callbacks.push_back(c);
  }
  for (unsigned j = 0; j < 4; ++j) {
    Feedback f;
    if (!(in >> f.connection >> f.phase >> f.sync >> f.delay) || f.connection < 1 ||
        f.connection > 2 || (f.phase != 1 && f.phase != 4) || (f.sync != 0 && f.sync != 2) ||
        f.delay > 10)
      throw std::runtime_error("invalid feedback");
    s.feedback.push_back(f);
  }
  if (in >> magic)
    throw std::runtime_error("trailing script data");
  return s;
}
std::string array(const Bytes &b) {
  std::ostringstream o;
  o << '[';
  for (size_t j = 0; j < b.size(); ++j) {
    if (j)
      o << ',';
    o << unsigned(b[j]);
  }
  o << ']';
  return o.str();
}
std::string payload(const PayloadSnapshot &p) {
  if (!p.extensions.empty())
    throw std::runtime_error("unexpected actual payload extension outside capture scope");
  std::ostringstream o;
  o << "{\"command\":\""
    << (p.command == Command::Read    ? "Read"
        : p.command == Command::Write ? "Write"
                                      : "Ignore")
    << "\",\"address\":\"" << p.address << "\",\"data\":" << array(p.data)
    << ",\"byteEnable\":" << array(p.byte_enable) << ",\"streamingWidth\":\"" << p.streaming_width
    << "\",\"status\":" << unsigned(p.status) << ",\"dmiHint\":" << (p.dmi_hint ? "true" : "false")
    << ",\"extensions\":{}}";
  return o.str();
}
struct Observations {
  std::vector<std::string> records, semantic, milestones;
  std::array<PayloadSnapshot, 2> current;
  std::array<PayloadSnapshot, 2> owned_requests;
  const Script &script;
  size_t returned{};
  Feedback feedback(unsigned conn, unsigned phase, const PayloadSnapshot &p, uint64_t time) {
    auto expected = script.requests.at(conn - 1);
    if (phase == 4) {
      for (const auto &c : script.callbacks)
        if (c.connection == conn && c.phase == 3)
          expected.data = c.data;
      expected.status = ResponseStatus::Ok;
    }
    if (time != (phase == 1 ? 1 : 7) || p.data != expected.data ||
        p.byte_enable != expected.byte_enable || p.address != expected.address ||
        p.command != expected.command || p.streaming_width != expected.streaming_width ||
        p.status != expected.status || p.dmi_hint || !p.extensions.empty())
      throw std::runtime_error("outgoing full payload/time diverged before replay feedback: time=" +
                               std::to_string(time) + " conn=" + std::to_string(conn) +
                               " phase=" + std::to_string(phase) + " actual=" + payload(p) +
                               " expected=" + payload(expected));
    if (returned >= script.feedback.size())
      throw std::runtime_error("unexpected outgoing call");
    auto f = script.feedback[returned++];
    if (f.connection != conn || f.phase != phase)
      throw std::runtime_error("outgoing call diverged before replay feedback expected=" +
                               std::to_string(f.connection) + "/" + std::to_string(f.phase) +
                               " actual=" + std::to_string(conn) + "/" + std::to_string(phase));
    return f;
  }
  explicit Observations(const Script &s)
      : current(s.requests), owned_requests(s.requests), script(s) {}
  void milestone(const RuntimeMilestoneObservation &m) {
    const auto &k = m.key;
    const auto &id = m.identity;
    const auto &event = m.event;
    const auto &v = m.causal.milestone;
    if (v.response && !v.response->extensions.empty())
      throw std::runtime_error("unexpected actual milestone extension outside capture scope");
    std::ostringstream o;
    o << "{\"kind\":\""
      << (v.kind == MilestoneKind::RequestReleased ? "requestReleased"
          : v.kind == MilestoneKind::ResponseReady ? "responseReady"
                                                   : "terminal")
      << "\",\"key\":{\"time\":\"" << k.time.value << "\",\"turn\":\"" << k.turn
      << "\",\"stage\":" << unsigned(k.stage) << ",\"instance\":" << k.instance.value
      << ",\"connection\":" << k.connection.value << ",\"sequence\":\"" << k.sequence
      << "\"},\"domain\":" << id.domain.value << ",\"instance\":" << id.local_side.value
      << ",\"connection\":" << id.connection.value << ",\"transport\":\"" << id.transport.value
      << "\",\"transportGeneration\":\"" << id.transport_generation << "\",\"callId\":\""
      << m.causal.call_id.value << "\",\"milestoneTime\":\"" << v.time.value
      << "\",\"implicit\":" << (v.implicit ? "true" : "false")
      << ",\"event\":{\"kind\":" << unsigned(event.kind) << ",\"domain\":" << event.domain.value
      << ",\"store\":" << event.store << ",\"slot\":" << event.slot << ",\"generation\":\""
      << event.generation << "\",\"owner\":\"" << event.owner << "\"},\"response\":";
    if (v.response)
      o << "{\"status\":" << unsigned(v.response->status) << ",\"data\":" << array(v.response->data)
        << ",\"dmiHint\":" << (v.response->dmi_hint ? "true" : "false") << ",\"extensions\":{}}";
    else
      o << "null";
    o << '}';
    milestones.push_back(o.str());
  }
  void call(const char *kind, uint64_t time, unsigned conn, unsigned phase, uint64_t delay,
            const PayloadSnapshot &p) {
    std::ostringstream o;
    o << "{\"observedOrdinal\":\"" << records.size() << "\",\"callOrdinal\":\""
      << records.size() / 2 + 1
      << "\",\"domain\":1,\"instance\":1,\"localSide\":\"initiator\",\"kind\":\"" << kind
      << "\",\"time\":\"" << time << "\",\"connection\":" << conn << ",\"transport\":\"" << conn
      << "\",\"phase\":" << phase << ",\"flow\":\"" << (phase == 1 || phase == 4 ? "fw" : "bw")
      << "\",\"delay\":\"" << delay << "\",\"payload\":" << payload(p) << '}';
    records.push_back(o.str());
  }
  void ret(const char *kind, unsigned conn, unsigned phase, Sync sync, uint64_t delay) {
    std::ostringstream o;
    o << "{\"observedOrdinal\":\"" << records.size() << "\",\"callOrdinal\":\""
      << records.size() / 2 + 1
      << "\",\"domain\":1,\"instance\":1,\"localSide\":\"initiator\",\"kind\":\"" << kind
      << "\",\"connection\":" << conn << ",\"callPhase\":" << phase
      << ",\"sync\":" << unsigned(sync) << ",\"phase\":null,\"delay\":\"" << delay
      << "\",\"response\":null}";
    records.push_back(o.str());
  }
  void attach(Runtime &r) {
    r.set_input_handler([this](const WireCall &c, ReadyKey key, Runtime &r) -> Expected<void> {
      std::ostringstream o;
      o << "{\"time\":\"" << key.time.value << "\",\"turn\":\"" << key.turn
        << "\",\"connection\":" << c.connection.value << ",\"phase\":" << c.phase.value
        << ",\"payload\":" << payload(c.request) << '}';
      semantic.push_back(o.str());
      if (c.phase == begin_resp) {
        SendIntent i;
        i.connection = c.connection;
        i.transport = c.transport;
        i.call_id = checked(r.allocate_call_id(CallOrigin::Outgoing));
        i.phase = end_resp;
        // The model owns request metadata; response bytes come from the actual delivered callback.
        i.payload = owned_requests.at(c.connection.value - 1);
        i.payload.data = c.request.data;
        i.payload.status = c.request.status;
        i.payload.dmi_hint = c.request.dmi_hint;
        i.payload.extensions = c.request.extensions;
        i.not_before = Tick{key.time.value + 1};
        return r.publish(i);
      }
      return {};
    });
    for (unsigned j = 0; j < 2; ++j) {
      checked(r.protocol().create_ledger(ConnectionId{j + 1}, TransportId{j + 1}));
      SendIntent i;
      i.connection = ConnectionId{j + 1};
      i.transport = TransportId{j + 1};
      i.call_id = checked(r.allocate_call_id(CallOrigin::Outgoing));
      i.payload = current[j];
      if (script.divergent_outbound && j == 0)
        i.payload.data[0] ^= 1;
      i.not_before = Tick{1};
      checked(r.publish(i));
    }
  }
  void result(Runtime &r) {
    if (r.stopped() || r.next_wakeup() || records.size() != 16 || semantic.size() != 4 ||
        milestones.size() != 6)
      throw std::runtime_error("incomplete transport execution");
    for (unsigned j = 1; j <= 2; ++j) {
      auto h = checked(r.protocol().find_ledger(ConnectionId{j}, TransportId{j}));
      if (checked(r.protocol().inspect(h)).state != WireState::Terminal)
        throw std::runtime_error("nonterminal ledger");
    }
    auto list = [](const auto &v) {
      std::string s = "[";
      for (size_t j = 0; j < v.size(); ++j) {
        if (j)
          s += ",";
        s += v[j];
      }
      return s + "]";
    };
    std::cout << "{\"schema\":\"leanat.transport-run.v1\",\"complete\":true,\"stop\":\"Quiescent\","
                 "\"records\":"
              << list(records) << ",\"semantic\":" << list(semantic)
              << ",\"milestones\":" << list(milestones) << ",\"terminalTransports\":[\"1\",\"2\"]}"
              << std::endl;
  }
};
RuntimeConfig config() {
  RuntimeConfig c;
  c.domain = DomainId{1};
  c.instance = InstanceId{1};
  c.connections = {ConnectionId{1}, ConnectionId{2}};
  c.descriptor_identity = "base-transport-replay-v1";
  return c;
}
HostBindingManifest manifest() {
  auto c = config();
  return {c.domain, c.descriptor_identity, c.connections, true, true};
}
#ifndef LEANAT_SYSTEMC_TRANSPORT
struct Host : RuntimeHost {
  Runtime *r{};
  Observations &o;
  explicit Host(Observations &o) : o(o) {}
  Expected<WireReturn> transport(const SendIntent &i) override {
    WireCall c{i.call_id, i.connection, i.transport, i.flow, i.phase, r->now(), {}, i.payload};
    auto x = r->start_outbound(i, c);
    if (!x)
      return x.error();
    o.call("outgoing-call", c.call_time.value, c.connection.value, c.phase.value,
           c.incoming_delay.value, c.request);
    Feedback f;
    try {
      f = o.feedback(c.connection.value, c.phase.value, c.request, c.call_time.value);
    } catch (const std::exception &e) {
      std::cerr << e.what() << std::endl;
      std::cerr << "feedbackConsumed=" << o.returned << std::endl;
      throw;
    }
    WireReturn ret;
    ret.sync = Sync(f.sync);
    ret.outgoing_delay = Duration{f.delay};
    o.ret("outgoing-return", i.connection.value, i.phase.value, ret.sync, f.delay);
    return ret;
  }
  void arm(std::optional<WakePoint>) override {}
  void publish_output(PortId, const Value &) override {}
  void emit_trace(const TraceEvent &) override {}
  void observe_milestone(const RuntimeMilestoneObservation &m) override {
    o.milestone(m);
  }
};
void execute(Script &s) {
  Observations o(s);
  Host h(o);
  Runtime r(config(), h);
  h.r = &r;
  checked(r.start(manifest()));
  o.attach(r);
  auto drain = [&](uint64_t until, bool inclusive) {
    unsigned budget = 0;
    while (r.next_wakeup() && (r.next_wakeup()->time.value < until ||
                               (inclusive && r.next_wakeup()->time.value == until))) {
      if (++budget > 128)
        throw std::runtime_error("pump bound exceeded");
      checked(r.pump_batch(r.next_wakeup()->time, 64));
    }
  };
  for (const auto &c : s.callbacks) {
    drain(c.time, false);
    auto &p = o.current[c.connection - 1];
    p.data = c.data;
    if (c.phase == 3)
      p.status = ResponseStatus::Ok;
    auto id = checked(r.allocate_call_id(CallOrigin::ExternalIngress));
    WireCall call{id,
                  ConnectionId{uint32_t(c.connection)},
                  TransportId{c.connection},
                  Flow::Backward,
                  PhaseId{uint32_t(c.phase)},
                  Tick{c.time},
                  Duration{c.delay},
                  p};
    o.call("callback-call", c.time, c.connection, c.phase, c.delay, p);
    call.request = PayloadSnapshot{};
    if (c.phase == 3) {
      call.request.status = ResponseStatus::Ok;
      call.request.data = c.data;
    }
    auto ret = checked(r.ingress(call));
    o.ret("callback-return", c.connection, c.phase, ret.sync, ret.outgoing_delay.value);
  }
  drain(100, true);
  o.result(r);
}
#else
using namespace leanat::systemc;
struct ObservingAdapter : RuntimeHostAdapter {
  Observations &observations;
  ObservingAdapter(sc_core::sc_module_name n, Observations &o)
      : RuntimeHostAdapter(n, config()), observations(o) {}
  void observe_milestone(const RuntimeMilestoneObservation &m) override {
    observations.milestone(m);
  }
};
struct Manager : tlm::tlm_mm_interface {
  void free(tlm::tlm_generic_payload *) override {}
};
struct Harness : sc_core::sc_module {
  Script &script;
  Observations o;
  ObservingAdapter host;
  Manager manager;
  std::array<tlm::tlm_generic_payload, 2> gp;
  tlm_utils::simple_initiator_socket_tagged<Harness> i1, i2;
  tlm_utils::simple_target_socket_tagged<Harness> t1, t2;
  SC_HAS_PROCESS(Harness);
  Harness(sc_core::sc_module_name n, Script &s)
      : sc_module(n), script(s), o(s), host("pump", o), i1("i1"), i2("i2"), t1("t1"), t2("t2") {
    i1.bind(t1);
    i2.bind(t2);
    i1.register_nb_transport_bw(this, &Harness::bw, 1);
    i2.register_nb_transport_bw(this, &Harness::bw, 2);
    t1.register_nb_transport_fw(this, &Harness::fw, 1);
    t2.register_nb_transport_fw(this, &Harness::fw, 2);
    for (unsigned j = 0; j < 2; ++j) {
      auto &g = gp[j];
      auto &p = o.current[j];
      g.set_mm(&manager);
      g.acquire();
      g.set_command(tlm::TLM_READ_COMMAND);
      g.set_address(0);
      g.set_data_ptr(p.data.data());
      g.set_data_length(4);
      g.set_streaming_width(4);
      g.set_byte_enable_ptr(p.byte_enable.data());
      g.set_byte_enable_length(2);
      g.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
      g.set_dmi_allowed(false);
      checked(host.bind(
          ConnectionId{j + 1},
          [this, j](const SendIntent &intent) -> Expected<tlm::tlm_generic_payload *> {
            auto &g = gp[j];
            const auto &p = intent.payload;
            if (p.data.size() != 4 || p.byte_enable.size() != 2 || !p.extensions.empty())
              return fail(ErrorCode::InvalidArgument, "unsupported outgoing snapshot");
            std::copy(p.data.begin(), p.data.end(), g.get_data_ptr());
            std::copy(p.byte_enable.begin(), p.byte_enable.end(), g.get_byte_enable_ptr());
            g.set_address(p.address);
            g.set_streaming_width(unsigned(p.streaming_width));
            g.set_command(p.command == Command::Read    ? tlm::TLM_READ_COMMAND
                          : p.command == Command::Write ? tlm::TLM_WRITE_COMMAND
                                                        : tlm::TLM_IGNORE_COMMAND);
            g.set_response_status(p.status == ResponseStatus::Ok ? tlm::TLM_OK_RESPONSE
                                                                 : tlm::TLM_INCOMPLETE_RESPONSE);
            g.set_dmi_allowed(p.dmi_hint);
            return &g;
          },
          [this, j](auto &g, auto &p, auto &d) {
            return j == 0 ? i1->nb_transport_fw(g, p, d) : i2->nb_transport_fw(g, p, d);
          }));
    }
    checked(host.runtime.start(manifest()));
    o.attach(host.runtime);
    SC_THREAD(run);
  }
  ~Harness() {
    for (auto &g : gp)
      g.release();
  }
  tlm::tlm_sync_enum fw(int id, tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                        sc_core::sc_time &d) {
    auto phase = p == tlm::BEGIN_REQ ? 1 : 4;
    o.call("outgoing-call", sc_core::sc_time_stamp().value(), id, phase, d.value(),
           checked(PayloadBridge{}.snapshot(g)));
    auto f = o.feedback(id, phase, checked(PayloadBridge{}.snapshot(g)),
                        sc_core::sc_time_stamp().value());
    d = sc_core::sc_time::from_value(f.delay);
    auto sync = f.sync == 0 ? tlm::TLM_ACCEPTED : tlm::TLM_COMPLETED;
    o.ret("outgoing-return", id, phase, Sync(f.sync), f.delay);
    return sync;
  }
  tlm::tlm_sync_enum bw(int id, tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                        sc_core::sc_time &d) {
    auto phase = p == tlm::END_REQ ? 2 : 3;
    o.call("callback-call", sc_core::sc_time_stamp().value(), id, phase, d.value(),
           checked(PayloadBridge{}.snapshot(g)));
    auto ret = checked(host.receive(ConnectionId{uint32_t(id)}, Flow::Backward, g, p, d));
    o.ret("callback-return", id, phase,
          ret == tlm::TLM_ACCEPTED  ? Sync::Accepted
          : ret == tlm::TLM_UPDATED ? Sync::Updated
                                    : Sync::Completed,
          d.value());
    return ret;
  }
  void run() {
    for (const auto &c : script.callbacks) {
      auto now = sc_core::sc_time_stamp().value();
      if (c.time < now)
        throw std::runtime_error("callback time decreased");
      if (c.time > now)
        sc_core::wait(sc_core::sc_time::from_value(c.time - now));
      auto &g = gp[c.connection - 1];
      std::copy(c.data.begin(), c.data.end(), g.get_data_ptr());
      if (c.phase == 3)
        g.set_response_status(tlm::TLM_OK_RESPONSE);
      tlm::tlm_phase phase = c.phase == 2 ? tlm::END_REQ : tlm::BEGIN_RESP;
      auto delay = sc_core::sc_time::from_value(c.delay);
      if (c.connection == 1)
        t1->nb_transport_bw(g, phase, delay);
      else
        t2->nb_transport_bw(g, phase, delay);
    }
    sc_core::wait(sc_core::sc_time::from_value(20));
    o.result(host.runtime);
    sc_core::sc_stop();
  }
};
void execute(Script &s) {
  sc_core::sc_report_handler::set_actions(sc_core::SC_INFO, sc_core::SC_DO_NOTHING);
  Harness h("transport", s);
  sc_core::sc_start();
}
#endif
} // namespace
#ifdef LEANAT_SYSTEMC_TRANSPORT
int sc_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
  try {
    if (argc != 2 && !(argc == 3 && std::string(argv[2]) == "--divergent-outbound"))
      throw std::runtime_error("usage: leanat_transport_replay bounded-script.txt");
    auto s = read(argv[1]);
    s.divergent_outbound = argc == 3;
    execute(s);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 2;
  }
}
