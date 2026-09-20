#include <cassert>
#include <climits>
#include <iostream>
#include <leanat/systemc/adapter.hpp>
using namespace leanat;
using namespace leanat::systemc;
const tlm::tlm_phase &phase_a();
const tlm::tlm_phase &phase_b();
struct Manager : tlm::tlm_mm_interface {
  unsigned freed{};
  void free(tlm::tlm_generic_payload *) override {
    ++freed;
  }
};
int sc_main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "ba") {
    (void)phase_b();
    (void)phase_a();
  } else {
    (void)phase_a();
    (void)phase_b();
  }
  RegisteredPhaseCodec first, second;
  assert(first.register_phase("vendor.marker_a", PhaseId{101}, phase_a(), true));
  assert(first.register_phase("vendor.marker_b", PhaseId{102}, phase_b()));
  assert(second.register_phase("vendor.marker_b", PhaseId{102}, phase_b()));
  assert(second.register_phase("vendor.marker_a", PhaseId{101}, phase_a(), true));
  assert(first.decode(phase_a()).value() == PhaseId{101} &&
         second.decode(phase_a()).value() == PhaseId{101});
  assert(first.encode(PhaseId{102}).value() == phase_b());
  assert(first.key(PhaseId{101}).value() == "vendor.marker_a");
  assert(!first.register_phase("alias", PhaseId{103}, phase_a()));
  first.freeze();
  assert(!first.register_phase("late", PhaseId{104}, phase_b()));
  RuntimeDomain domain("domain", DomainId{1});
  NbBridge bridge(domain, InstanceId{1}, ConnectionId{1});
  assert(bridge.register_ignorable("vendor.marker_a", PhaseId{101}, phase_a()));
  Manager manager;
  tlm::tlm_generic_payload gp(&manager);
  gp.acquire();
  gp.set_data_length(UINT_MAX);
  gp.set_byte_enable_length(UINT_MAX);
  auto phase = phase_a();
  auto delay = sc_core::sc_time::from_value(7);
  unsigned forwarded = 0;
  auto result = bridge.receive(Flow::Forward, gp, phase, delay,
                               [&](const WireCall &) -> Expected<WireReturn> {
                                 ++forwarded;
                                 return WireReturn{};
                               });
  assert(result && result.value() == tlm::TLM_ACCEPTED && phase == phase_a() && delay.value() == 7);
  assert(forwarded == 0 && bridge.active() == 0 && bridge.ignored_calls() == 1 &&
         domain.pending() == 0 && gp.get_ref_count() == 1);
  assert(bridge.last_ignored() && bridge.last_ignored()->phase == PhaseId{101} &&
         bridge.last_ignored()->incoming_delay == Duration{7});
  phase = phase_b();
  auto mandatory = bridge.receive(Flow::Forward, gp, phase, delay,
                                  [&](const WireCall &) -> Expected<WireReturn> {
                                    ++forwarded;
                                    return WireReturn{};
                                  });
  assert(!mandatory && mandatory.error().code == ErrorCode::Unsupported && forwarded == 0);
  tlm::tlm_generic_payload no_mm;
  phase = phase_a();
  assert(!bridge.receive(Flow::Forward, no_mm, phase, delay,
                         [](const WireCall &) -> Expected<WireReturn> { return WireReturn{}; }));
  gp.release();
  assert(manager.freed == 1);
  std::cout << "{\"stable_phase_keys\":[\"vendor.marker_a\",\"vendor.marker_b\"],\"ignored\":1,"
               "\"forwarded\":0}\n";
  return 0;
}
