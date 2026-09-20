#include "harness.hpp"
#include <leanat/crossbar.hpp>
#include <leanat/managed.hpp>
#include <leanat/memory.hpp>
#include <leanat/result_store.hpp>
#include <leanat/systemc/adapter.hpp>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
using namespace leanat;
using namespace leanat::systemc;
using namespace conformance;
struct MM : tlm::tlm_mm_interface {
  unsigned frees{};
  void free(tlm::tlm_generic_payload *) override {
    ++frees;
  }
};
static void setup(tlm::tlm_generic_payload &g, unsigned char *data, unsigned size,
                  bool write = false) {
  g.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
  g.set_address(0);
  g.set_data_ptr(data);
  g.set_data_length(size);
  g.set_streaming_width(size ? size : 1);
  g.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
  g.set_dmi_allowed(false);
}
struct Scenarios : sc_core::sc_module {
  Report &report;
  RuntimeDomain domain;
  NbBridge receiver;
  BlockingBridge blocking;
  Memory memory;
  tlm_utils::simple_initiator_socket<Scenarios> initiator;
  tlm_utils::simple_target_socket<Scenarios> target;
  SC_HAS_PROCESS(Scenarios);
  Scenarios(sc_core::sc_module_name name, Report &r)
      : sc_module(name), report(r), domain("domain", DomainId{1}),
        receiver(domain, InstanceId{1}, ConnectionId{1}), blocking(domain),
        memory(take(Memory::make(8))), initiator("initiator"), target("target") {
    target.register_nb_transport_fw(this, &Scenarios::nb);
    target.register_b_transport(this, &Scenarios::bt);
    target.register_transport_dbg(this, &Scenarios::debug);
    initiator.register_nb_transport_bw(this, &Scenarios::bw);
    initiator.bind(target);
    take(domain.start());
    SC_THREAD(run);
  }
  Expected<ResponseSnapshot> service(const PayloadSnapshot &p) {
    ExecutionContext c;
    c.domain = DomainId{1};
    c.ready.time = take(domain.time.to_tick(sc_core::sc_time_stamp()));
    EventTxn tx({}, c);
    auto v = memory.transfer(tx, p);
    if (!v)
      return v.error();
    auto committed = tx.commit();
    if (!committed)
      return committed.error();
    return ResponseSnapshot{v.value().status, v.value().data, false, {}};
  }
  tlm::tlm_sync_enum nb(tlm::tlm_generic_payload &g, tlm::tlm_phase &p, sc_core::sc_time &delay) {
    return take(receiver.receive(Flow::Forward, g, p, delay,
                                 [&](const WireCall &call) -> Expected<WireReturn> {
                                   auto s = service(call.request);
                                   if (!s)
                                     return s.error();
                                   return WireReturn{Sync::Completed, {}, {}, std::move(s.value())};
                                 }));
  }
  void bt(tlm::tlm_generic_payload &g, sc_core::sc_time &delay) {
    take(blocking.transport(g, delay, Duration{3},
                            [&](const PayloadSnapshot &p) { return service(p); }));
  }
  unsigned debug(tlm::tlm_generic_payload &g) {
    return take(BlockingBridge::debug(g, 8, [&](std::uint64_t a, std::uint8_t &b, bool write) {
      if (a >= memory.size())
        return false;
      ExecutionContext c;
      c.kind = ContextKind::Debug;
      EventTxn tx({}, c);
      auto v = memory.debug_transfer(tx, write ? Command::Write : Command::Read, a, Bytes{b});
      if (!v)
        return false;
      if (!write)
        b = v.value().data[0];
      return bool(tx.commit());
    }));
  }
  tlm::tlm_sync_enum bw(tlm::tlm_generic_payload &, tlm::tlm_phase &, sc_core::sc_time &) {
    return tlm::TLM_ACCEPTED;
  }
  void run() {
    report.run(
        "C-T02", "real GP WRITE[7,8]; ACCEPTED return phase overwritten END_RESP and delay99",
        "caller restores input BEGIN_REQ/delay2; request bytes unchanged", [&] {
          MM mm;
          tlm::tlm_generic_payload gp(&mm);
          unsigned char bytes[] = {7, 8};
          setup(gp, bytes, 2, true);
          gp.acquire();
          NbBridge bridge(domain, InstanceId{2}, ConnectionId{2});
          tlm::tlm_phase p = tlm::BEGIN_REQ;
          auto delay = sc_core::sc_time::from_value(2);
          auto ret =
              take(bridge.exchange(Flow::Forward, gp, p, delay, [](auto &, auto &ph, auto &d) {
                ph = tlm::END_RESP;
                d = sc_core::sc_time::from_value(99);
                return tlm::TLM_ACCEPTED;
              }));
          require(ret.sync == Sync::Accepted && p == tlm::BEGIN_REQ && delay.value() == 2 &&
                      bytes[0] == 7 && bytes[1] == 8,
                  "ACCEPTED corrupted request/return view");
          gp.release();
          return "BEGIN_REQ, delay2, WRITE[7,8] preserved after native ACCEPTED";
        });
    report.run("C-T11", "no-MM nb pin, then no-MM b_transport through real bound sockets",
               "nb rejected; blocking private managed bridge succeeds/copies READ", [&] {
                 unsigned char bytes[] = {0, 0};
                 tlm::tlm_generic_payload gp;
                 setup(gp, bytes, 2);
                 require(!PayloadBridge{}.pin_nb(gp), "noMM nb accepted");
                 sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
                 initiator->b_transport(gp, delay);
                 require(gp.is_response_ok() && !gp.has_mm(), "blocking bridge/MM failed");
                 return "nb pin rejected; blocking READ OK; caller still noMM";
               });
    report.run("C-T12", "native callback releases original GP owner before returning COMPLETED",
               "bridge pin protects snapshot; MM free after snapshot capture", [&] {
                 MM mm;
                 tlm::tlm_generic_payload gp(&mm);
                 unsigned char data[] = {11, 12};
                 setup(gp, data, 2);
                 gp.acquire();
                 NbBridge bridge(domain, InstanceId{3}, ConnectionId{3});
                 tlm::tlm_phase p = tlm::BEGIN_REQ;
                 sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
                 bool protected_inside = false;
                 auto ret = take(
                     bridge.exchange(Flow::Forward, gp, p, delay, [&](auto &g, auto &, auto &) {
                       g.release();
                       protected_inside = mm.frees == 0;
                       g.set_response_status(tlm::TLM_OK_RESPONSE);
                       return tlm::TLM_COMPLETED;
                     }));
                 require(protected_inside && mm.frees == 1 && ret.response &&
                             ret.response->data == Bytes({11, 12}),
                         "pin lifetime/snapshot failed");
                 return "callback release did not free while pinned; MM frees1 after capture; "
                        "owned[11,12]";
               });
    report.run(
        "C-T13", "GP references external vector[21,22]; COMPLETED then vector cleared/shrunk",
        "future response owns bytes independent of reclaimed external buffer", [&] {
          MM mm;
          tlm::tlm_generic_payload gp(&mm);
          Bytes input{21, 22};
          setup(gp, input.data(), 2);
          gp.acquire();
          NbBridge bridge(domain, InstanceId{4}, ConnectionId{4});
          tlm::tlm_phase p = tlm::BEGIN_REQ;
          sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
          auto ret = take(bridge.exchange(Flow::Forward, gp, p, delay, [](auto &g, auto &, auto &) {
            g.set_response_status(tlm::TLM_OK_RESPONSE);
            return tlm::TLM_COMPLETED;
          }));
          gp.release();
          input.clear();
          input.shrink_to_fit();
          require(mm.frees == 1 && ret.response && ret.response->data == Bytes({21, 22}),
                  "borrowed response escaped");
          return "MM frees1, external vector released, future response[21,22] intact";
        });
    report.run("C-T23", "real sockets nb WRITE[5,6], noMM blocking READ, debug WRITE9, nb READ",
               "one Memory backing; copyback[5,6] then[9,6]; own MM released once", [&] {
                 MM mm;
                 tlm::tlm_generic_payload gp(&mm);
                 unsigned char input[] = {5, 6};
                 setup(gp, input, 2, true);
                 gp.acquire();
                 tlm::tlm_phase p = tlm::BEGIN_REQ;
                 sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
                 require(initiator->nb_transport_fw(gp, p, delay) == tlm::TLM_COMPLETED,
                         "nb write incomplete");
                 require(input[0] == 5 && input[1] == 6, "WRITE copyback corrupt");
                 gp.release();
                 require(mm.frees == 1, "nb pin leak");
                 unsigned char read[] = {0, 0};
                 tlm::tlm_generic_payload plain;
                 setup(plain, read, 2);
                 initiator->b_transport(plain, delay);
                 require(read[0] == 5 && read[1] == 6, "blocking state mismatch");
                 unsigned char poke[] = {9};
                 setup(plain, poke, 1, true);
                 require(initiator->transport_dbg(plain) == 1, "debug count");
                 MM mm2;
                 tlm::tlm_generic_payload last(&mm2);
                 setup(last, read, 2);
                 last.acquire();
                 p = tlm::BEGIN_REQ;
                 require(initiator->nb_transport_fw(last, p, delay) == tlm::TLM_COMPLETED &&
                             read[0] == 9 && read[1] == 6,
                         "nb read did not see debug backing");
                 last.release();
                 require(mm2.frees == 1, "read pin leaked");
                 return "nb/b/debug share bytes; READ[5,6] then[9,6], both MM frees1";
               });
    report.run(
        "C-T30", "COMPLETED GP READ[31,32], publish result, GP reclaimed, consume later",
        "durable ResultStore owned bytes with no GP access", [&] {
          MM mm;
          tlm::tlm_generic_payload gp(&mm);
          unsigned char data[] = {31, 32};
          setup(gp, data, 2);
          gp.acquire();
          NbBridge bridge(domain, InstanceId{5}, ConnectionId{5});
          tlm::tlm_phase p = tlm::BEGIN_REQ;
          sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
          auto ret = take(bridge.exchange(Flow::Forward, gp, p, delay, [](auto &g, auto &, auto &) {
            g.set_response_status(tlm::TLM_OK_RESPONSE);
            return tlm::TLM_COMPLETED;
          }));
          ResultStore store;
          Handle source{HandleKind::Transaction, {}, 1, 0, 1, 7};
          auto reserved = take(store.reserve({source, TypeId{2}, 64, 7, 7}));
          take(store.publish(reserved.reservation, Value{ret.response->data}, {source, {}, true}));
          gp.release();
          std::fill(std::begin(data), std::end(data), 0);
          require(mm.frees == 1, "GP not reclaimed");
          require(take(store.read(reserved.consumer)) == Value{Bytes{31, 32}},
                  "durable result corrupt");
          take(store.release(reserved.consumer));
          return "GP MM frees1; native bytes zeroed; result[31,32] preserved";
        });
    report.run("E-T26", "raw region base100,size4,READ query101",
               "real pointer and inclusive[100,103]", [&] {
                 auto bytes = std::make_shared<Bytes>(Bytes{1, 2, 3, 4});
                 RawDmiHost raw(bytes, 100, TimeCodec{}, Duration{2}, Duration{3});
                 tlm::tlm_generic_payload gp;
                 gp.set_command(tlm::TLM_READ_COMMAND);
                 gp.set_address(101);
                 tlm::tlm_dmi dmi;
                 require(take(raw.get(gp, dmi, [](auto, auto) {})), "raw denied");
                 require(dmi.get_dmi_ptr() == bytes->data() && dmi.get_start_address() == 100 &&
                             dmi.get_end_address() == 103,
                         "raw descriptor mismatch");
                 require(dmi.get_dmi_ptr()[gp.get_address() - dmi.get_start_address()] == 2,
                         "raw pointer offset mismatch");
                 return "pointer==backing.data, inclusive[100,103], query101 reads2";
               });
    report.run("E-T29", "raw grant then replace; invalidation callback attempts synchronous get",
               "callback sees old backing and re-get rejected before replace", [&] {
                 auto old = std::make_shared<Bytes>(Bytes{4, 5});
                 RawDmiHost raw(old, 0, TimeCodec{}, Duration{}, Duration{});
                 tlm::tlm_generic_payload gp;
                 gp.set_command(tlm::TLM_READ_COMMAND);
                 gp.set_address(0);
                 tlm::tlm_dmi dmi;
                 bool called = false, rejected = false, old_seen = false;
                 require(take(raw.get(gp, dmi,
                                      [&](auto lo, auto hi) {
                                        called = lo == 0 && hi == 1;
                                        old_seen = dmi.get_dmi_ptr()[0] == 4;
                                        tlm::tlm_dmi nested;
                                        rejected = !raw.get(gp, nested, [](auto, auto) {});
                                      })),
                         "initial grant failed");
                 take(raw.replace(std::make_shared<Bytes>(Bytes{8, 9})));
                 require(called && rejected && old_seen && raw.generation() == 2,
                         "invalidation order/reentry mismatch");
                 return "callback oldbyte4 before replacement; re-get rejected; generation2";
               });
    report.run(
        "E-T13",
        "native forward BEGIN_REQ callback synchronously calls reverse BEGIN_RESP on same GP",
        "nested call rejected immediately; inner callback never invoked or queued", [&] {
          MM mm;
          tlm::tlm_generic_payload gp(&mm);
          unsigned char bytes[] = {1};
          setup(gp, bytes, 1);
          gp.acquire();
          NbBridge bridge(domain, InstanceId{9}, ConnectionId{9});
          tlm::tlm_phase phase = tlm::BEGIN_REQ;
          sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
          bool rejected = false, inner_called = false;
          auto result =
              take(bridge.exchange(Flow::Forward, gp, phase, delay, [&](auto &g, auto &, auto &) {
                tlm::tlm_phase response = tlm::BEGIN_RESP;
                sc_core::sc_time d = sc_core::SC_ZERO_TIME;
                g.set_response_status(tlm::TLM_OK_RESPONSE);
                auto nested =
                    bridge.exchange(Flow::Backward, g, response, d, [&](auto &, auto &, auto &) {
                      inner_called = true;
                      return tlm::TLM_ACCEPTED;
                    });
                rejected = !nested;
                return tlm::TLM_ACCEPTED;
              }));
          require(rejected && !inner_called && result.sync == Sync::Accepted,
                  "nested callback queued or executed");
          gp.release();
          return "reverse call rejected synchronously before native dispatch; outer ACCEPTED "
                 "returned; inner callback count0";
        });
    report.run("E-T20",
               "custom peer changes READ data pointer to null with nonzero length before COMPLETED",
               "monitor rejects invalid response without dereferencing null; arbitrary dangling "
               "external pointers remain external contract",
               [&] {
                 unsigned char bytes[] = {1};
                 tlm::tlm_generic_payload gp;
                 setup(gp, bytes, 1);
                 gp.set_data_ptr(nullptr);
                 gp.set_response_status(tlm::TLM_OK_RESPONSE);
                 auto snapshot = PayloadBridge{}.response(gp);
                 require(!snapshot, "null nonempty response accepted");
                 return "null data/nonzero length rejected by PayloadBridge monitor; no claim to "
                        "detect arbitrary nonnull dangling pointers";
               });
    report.run("E-T25",
               "phase bijection but vendor rule changes request lane cardinality and actions",
               "adapter semantic validation rejects phase-only renaming", [] {
                 AdapterContract left, right;
                 left.phases = {PhaseId{1}};
                 right.phases = {PhaseId{9}};
                 AdapterRule base;
                 base.phase = PhaseId{1};
                 base.guard = "true";
                 left.rules = {base};
                 base.phase = PhaseId{9};
                 right.rules = {base};
                 take(CheckedAdapter::validate(left, right, {{PhaseId{1}, PhaseId{9}}}));
                 right.rules[0].request_lanes = 2;
                 require(!CheckedAdapter::validate(left, right, {{PhaseId{1}, PhaseId{9}}}),
                         "lane mismatch accepted");
                 right.rules[0].request_lanes = 0;
                 right.rules[0].actions = 1;
                 require(!CheckedAdapter::validate(left, right, {{PhaseId{1}, PhaseId{9}}}),
                         "action mismatch accepted");
                 return "identity semantic control passes; lane2 and changed lifecycle action each "
                        "rejected";
               });
    report.run("E-T33",
               "raw downstream[0,255], two aliases upstream1000/2000 mapping downstream64..127; "
               "invalidation80..95",
               "clipped translated pointers and two synchronous upstream invalidations", [] {
                 auto bytes = std::make_shared<Bytes>(256, 3);
                 RawDmiHost raw(bytes, 0, TimeCodec{}, Duration{1}, Duration{2});
                 CrossbarConfig cfg;
                 cfg.upstreams = {ConnectionId{1}, ConnectionId{3}};
                 cfg.downstreams = {ConnectionId{2}};
                 cfg.regions = {{0x1000, 64, 64, ConnectionId{2}},
                                {0x2000, 64, 64, ConnectionId{2}}};
                 Crossbar bus(cfg, {}, 1);
                 tlm::tlm_generic_payload gp;
                 gp.set_command(tlm::TLM_READ_COMMAND);
                 gp.set_address(80);
                 tlm::tlm_dmi dmi;
                 std::vector<RouteInvalidation> notifications;
                 require(take(raw.get(gp, dmi,
                                      [&](auto start, auto end) {
                                        notifications =
                                            take(bus.invalidate(ConnectionId{2}, start, end, 4));
                                      })),
                         "raw grant denied");
                 RouteDmiGrant grant{dmi.get_dmi_ptr(),     dmi.get_start_address(),
                                     dmi.get_end_address(), 3,
                                     Duration{1},           Duration{2}};
                 for (unsigned n = 0; n < 2; ++n) {
                   auto base = n ? 0x2000u : 0x1000u;
                   auto route = *bus.decode(base + 16);
                   auto translated = take(bus.translate_dmi(
                       route, grant, {ConnectionId{n ? 3u : 1u}, base + 16, Command::Read, 3}));
                   require(translated && translated->pointer == bytes->data() + 64 &&
                               translated->start == base && translated->end == base + 63,
                           "clipping or pointer offset wrong");
                 }
                 take(raw.invalidate(80, 95));
                 require(notifications.size() == 2, "fanout count");
                 require(notifications[0].start == 0x1000 && notifications[0].end == 0x103f &&
                             notifications[1].start == 0x2000 && notifications[1].end == 0x203f,
                         "invalidation address translation");
                 return "pointers backing+64, ranges1000..103F/2000..203F; whole downstream grant "
                        "invalidated; synchronous clipped invalidations1000..103F and2000..203F";
               });
    report.run("E-T14",
               "registered ignorable custom phase and unknown mandatory phase on sparse MM GP",
               "ignorable ACCEPTED unchanged without dispatch/ledger; mandatory rejects", [&] {
                 struct Marker : tlm::tlm_phase {
                   Marker() : tlm::tlm_phase(typeid(Marker), "conformance.ignore") {}
                 };
                 struct Mandatory : tlm::tlm_phase {
                   Mandatory() : tlm::tlm_phase(typeid(Mandatory), "conformance.mandatory") {}
                 };
                 Marker marker;
                 Mandatory mandatory;
                 NbBridge bridge(domain, InstanceId{10}, ConnectionId{10});
                 take(bridge.register_ignorable("conformance.ignore", PhaseId{101}, marker));
                 MM mm;
                 tlm::tlm_generic_payload gp(&mm);
                 gp.acquire();
                 gp.set_data_length(UINT_MAX);
                 unsigned called = 0;
                 tlm::tlm_phase p = marker;
                 auto delay = sc_core::sc_time::from_value(7);
                 auto ret = take(bridge.receive(Flow::Forward, gp, p, delay,
                                                [&](const WireCall &) -> Expected<WireReturn> {
                                                  ++called;
                                                  return WireReturn{};
                                                }));
                 require(ret == tlm::TLM_ACCEPTED && p == marker && delay.value() == 7 &&
                             called == 0 && bridge.active() == 0 && bridge.ignored_calls() == 1,
                         "ignorable changed state");
                 p = mandatory;
                 require(!bridge.receive(Flow::Forward, gp, p, delay,
                                         [&](const WireCall &) -> Expected<WireReturn> {
                                           ++called;
                                           return WireReturn{};
                                         }) &&
                             called == 0,
                         "mandatory forwarded");
                 gp.release();
                 require(mm.frees == 1, "ignored marker pin leaked");
                 return "marker Accepted phase/delay7 unchanged; zero forwarded,zero ledger; "
                        "mandatory rejected; MM frees1";
               });
    report.run(
        "E-T28",
        "raw READ latency2 per byte, four bytes; managed service=3+count and two count4 reads at0",
        "raw client charge8; managed service charged7 once per request and serialized "
        "completion7/14",
        [] {
          auto backing = std::make_shared<Bytes>(8, 3);
          RawDmiHost raw(backing, 0, TimeCodec{}, Duration{2}, Duration{4});
          tlm::tlm_generic_payload gp;
          gp.set_command(tlm::TLM_READ_COMMAND);
          gp.set_address(0);
          tlm::tlm_dmi dmi;
          require(take(raw.get(gp, dmi, [](auto, auto) {})), "raw denied");
          require(dmi.get_read_latency().value() == 2 && dmi.get_read_latency().value() * 4 == 8,
                  "raw per-byte latency");
          ResultStore results;
          ManagedAccessManager manager(results, {}, true);
          unsigned service_calls = 0;
          ManagedRegionDesc region;
          region.id = RegionId{1};
          region.end = 7;
          region.backing = backing;
          region.service = [&](Command, std::size_t count) -> Expected<Duration> {
            ++service_calls;
            return Duration{3 + count};
          };
          take(manager.add_region(region));
          ManagedRequest request;
          request.region = RegionId{1};
          request.end = 7;
          auto lease = take(manager.request(request));
          auto first = take(manager.begin({lease, Command::Read, 0, 4, {}, Tick{}}));
          auto second = take(manager.begin({lease, Command::Read, 0, 4, {}, Tick{}}));
          take(manager.advance(Tick{6}));
          require(!manager.completion(first), "managed completed early");
          take(manager.advance(Tick{7}));
          require(take(manager.completion(first)).ready.time == Tick{7} &&
                      !manager.completion(second),
                  "managed service incorrectly charged");
          take(manager.advance(Tick{14}));
          require(take(manager.completion(second)).ready.time == Tick{14} && service_calls == 2,
                  "managed requests not serialized/service called per byte");
          return "raw per-byte2 => four-byte client8; managed callback called2 times; request "
                 "completions7 and14";
        });
    sc_core::sc_stop();
  }
};
int sc_main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  Report report(argv[1]);
  Scenarios tests("conformance", report);
  sc_core::sc_start();
  return report.failures ? 1 : 0;
}
