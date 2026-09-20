#include <tlm>
const tlm::tlm_phase &conformance_phase_a() {
  struct A : tlm::tlm_phase {
    A() : tlm::tlm_phase(typeid(A), "conformance.marker.a") {}
  };
  static const A value;
  return value;
}
