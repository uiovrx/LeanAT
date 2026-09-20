#include "harness.hpp"
#include <leanat/systemc/adapter.hpp>
using namespace leanat;
using namespace leanat::systemc;
using namespace conformance;
const tlm::tlm_phase &conformance_phase_a();
const tlm::tlm_phase &conformance_phase_b();
struct ProfileChecks : sc_core::sc_module {
  tlm::tlm_initiator_socket<32> source32;
  tlm::tlm_target_socket<32> target32;
  tlm::tlm_initiator_socket<64> source64;
  tlm::tlm_target_socket<64> target64;
  ProfileChecks(sc_core::sc_module_name name)
      : sc_module(name), source32("s32"), target32("t32"), source64("s64"), target64("t64") {}
};
int sc_main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  Report report(argv[1]);
  std::string order = argv[2];
  report.run("E-T16", "two translation units initialize actual native phases in order " + order,
             "stable keys/semantic IDs unchanged despite native registration order", [&] {
               if (order == "ba") {
                 conformance_phase_b();
                 conformance_phase_a();
               } else {
                 conformance_phase_a();
                 conformance_phase_b();
               }
               RegisteredPhaseCodec codec;
               take(codec.register_phase("vendor.a", PhaseId{101}, conformance_phase_a()));
               take(codec.register_phase("vendor.b", PhaseId{102}, conformance_phase_b()));
               require(take(codec.decode(conformance_phase_a())) == PhaseId{101} &&
                           take(codec.decode(conformance_phase_b())) == PhaseId{102},
                       "stable mapping wrong");
               require(take(codec.key(PhaseId{101})) == "vendor.a" &&
                           take(codec.encode(PhaseId{102})) == conformance_phase_b(),
                       "stable key encode wrong");
               return "order=" + order +
                      ", nativeA=" + std::to_string(unsigned(conformance_phase_a())) +
                      ", nativeB=" + std::to_string(unsigned(conformance_phase_b())) +
                      ", stableA=101/vendor.a,stableB=102/vendor.b";
             });
  report.run(
      "C-T28",
      "actual host byte-order probe, real32/64-bit TLM socket declarations; required Little/32 "
      "native profile",
      "supported Little32 accepted; Big/Unknown/zero/64/mixed width rejected before elaboration",
      [] {
        ProfileChecks sockets("profile");
        auto supported = validate_native_profile({}, sockets.source32, sockets.target32);
        require(bool(supported) == (native_byte_order() == NativeByteOrder::Little),
                "actual host probe ignored");
        for (auto profile :
             {NativeProfile{NativeByteOrder::Big, 32}, NativeProfile{NativeByteOrder::Unknown, 32},
              NativeProfile{NativeByteOrder::Little, 64},
              NativeProfile{NativeByteOrder::Little, 0}}) {
          auto rejected = validate_native_profile(profile, sockets.source32, sockets.target32);
          require(!rejected && rejected.error().code == ErrorCode::Unsupported,
                  "unsupported native profile admitted");
        }
        require(!validate_native_profile({}, sockets.source32, sockets.target64) &&
                    !validate_native_profile({}, sockets.source64, sockets.target64),
                "actual socket width mismatch ignored");
        require(sc_core::sc_time_stamp() == sc_core::SC_ZERO_TIME,
                "profile check ran after kernel start");
        return std::string("actualHost=") +
               (native_byte_order() == NativeByteOrder::Little ? "Little" : "Unsupported") +
               "; actual32/32 validated; Big/Unknown/0/64/mixed32/64 rejected pre-elaboration; no "
               "big-endian conversion claimed";
      });
  return report.failures ? 1 : 0;
}
