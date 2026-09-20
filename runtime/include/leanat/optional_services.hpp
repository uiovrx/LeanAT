#pragma once
#include "external.hpp"
#include "managed.hpp"
#include "runtime_services.hpp"
namespace leanat {
// Bind executable validated pure programs, including their complete delivered closure.
Expected<void> register_external_reference_program(ExternalCallGate &, ExternCallDesc,
                                                   std::shared_ptr<const exec::ValidatedProject>,
                                                   ProgramId reference, ProgramId precondition,
                                                   std::uint64_t fuel = 100000);
// Factories independently validate the concrete ABI and produce host-owned identities.
Expected<exec::ServiceSignature> external_service_signature(const exec::Project &,
                                                            std::uint32_t service_id,
                                                            std::uint32_t input_type,
                                                            std::uint32_t except_type,
                                                            const ExternalCallGate &, ExternCallId);
Expected<void> register_external_service(CoreRuntimeBackend &, const exec::Project &,
                                         exec::ServiceSignature, ExternalCallGate &, ExternCallId);
Expected<exec::ServiceSignature> managed_service_signature(const exec::Project &,
                                                           std::uint32_t service_id, exec::Op,
                                                           std::vector<std::uint32_t> inputs,
                                                           std::uint32_t result_type);
struct ManagedProviderOptions {
  bool permit_invalidation{};
  std::uint64_t invalidation_owner{};
};
Expected<void> register_managed_service(CoreRuntimeBackend &, const exec::Project &,
                                        exec::ServiceSignature, ManagedAccessManager &,
                                        ResultStore &, ManagedProviderOptions = {});
} // namespace leanat
