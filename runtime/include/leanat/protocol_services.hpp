#pragma once
#include "admission.hpp"
#include "runtime_services.hpp"
namespace leanat {
struct ProtocolServiceConfig {
  std::size_t max_payload_bytes{};
  // Required for BEGIN_RESP so response data comes from the current committed/staged payload view.
  std::function<Expected<PayloadSnapshot>(Handle, ConnectionId, const ExecutionContext &,
                                          EventTxn &)>
      payload_snapshot;
};
// ABI: NewTransaction(connection,transport,generation,command,address,Bytes)->Txn;
// StagePhase(txn,connection,phase,notBefore)->Unit; AckResponse(txn,connection,notBefore)->Unit.
Expected<void> register_protocol_service(CoreRuntimeBackend &, Runtime &, AdmissionStore &,
                                         exec::ServiceSignature, ProtocolServiceConfig);
} // namespace leanat
