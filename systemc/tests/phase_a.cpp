#include <tlm>
const tlm::tlm_phase &phase_a() {
  struct MarkerA : tlm::tlm_phase {
    MarkerA() : tlm::tlm_phase(typeid(MarkerA), "vendor.marker_a") {}
  };
  static const MarkerA value;
  return value;
}
