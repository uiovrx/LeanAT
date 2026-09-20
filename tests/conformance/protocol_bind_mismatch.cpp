#include <tlm>
struct VendorProtocol {
  using tlm_payload_type = tlm::tlm_generic_payload;
  using tlm_phase_type = tlm::tlm_phase;
};
void mismatched_bind(tlm::tlm_initiator_socket<32> &initiator,
                     tlm::tlm_target_socket<32, VendorProtocol> &target) {
  initiator.bind(target);
}
