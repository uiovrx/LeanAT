#include <tlm>
const tlm::tlm_phase &phase_b() {
  struct MarkerB : tlm::tlm_phase {
    MarkerB() : tlm::tlm_phase(typeid(MarkerB), "vendor.marker_b") {}
  };
  static const MarkerB value;
  return value;
}
