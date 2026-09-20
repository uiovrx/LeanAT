#include <tlm>
const tlm::tlm_phase &conformance_phase_b() {
  struct B : tlm::tlm_phase {
    B() : tlm::tlm_phase(typeid(B), "conformance.marker.b") {}
  };
  static const B value;
  return value;
}
