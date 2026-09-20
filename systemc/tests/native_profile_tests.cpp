#include <cassert>
#include <leanat/systemc/adapter.hpp>
using namespace leanat;
using namespace leanat::systemc;
struct ProfileFixture : sc_core::sc_module {
  tlm::tlm_initiator_socket<32> source32;
  tlm::tlm_target_socket<32> target32;
  tlm::tlm_initiator_socket<64> source64;
  tlm::tlm_target_socket<64> target64;
  ProfileFixture(sc_core::sc_module_name n)
      : sc_module(n), source32("source32"), target32("target32"), source64("source64"),
        target64("target64") {
    auto supported = validate_native_profile({}, source32, target32);
    if (native_byte_order() == NativeByteOrder::Little)
      assert(supported);
    else
      assert(!supported && supported.error().code == ErrorCode::Unsupported);
    for (auto profile :
         {NativeProfile{NativeByteOrder::Big, 32}, NativeProfile{NativeByteOrder::Unknown, 32},
          NativeProfile{NativeByteOrder::Little, 64}, NativeProfile{NativeByteOrder::Little, 0}}) {
      auto rejected = validate_native_profile(profile, source32, target32);
      assert(!rejected && rejected.error().code == ErrorCode::Unsupported);
    }
    // Real native socket declarations supply widths, not caller-asserted capability flags.
    auto mixed = validate_native_profile({}, source32, target64);
    auto wide = validate_native_profile({}, source64, target64);
    assert(!mixed && mixed.error().code == ErrorCode::Unsupported);
    assert(!wide && wide.error().code == ErrorCode::Unsupported);
    assert(sc_core::sc_time_stamp() == sc_core::SC_ZERO_TIME);
  }
};
int sc_main(int, char **) {
  ProfileFixture fixture("profile");
  // Unsupported configurations are rejected before elaboration or external transport.
  return 0;
}
