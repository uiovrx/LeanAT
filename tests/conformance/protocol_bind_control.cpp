#include <tlm>
void compatible_bind(tlm::tlm_initiator_socket<32> &initiator, tlm::tlm_target_socket<32> &target) {
  initiator.bind(target);
}
