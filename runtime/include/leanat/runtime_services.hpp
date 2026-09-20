#pragma once
#include "interpreter.hpp"
#include "runtime.hpp"
namespace leanat {
Expected<WireReturn> decode_transport_return(const Value &);
// A trusted host builds this finite registry from its actual providers, then seals it.
// The descriptor is checked against this independent registry before VM execution.
class CoreRuntimeBackend final : public exec::InterpreterBackend {
public:
  using Provider = std::function<Expected<std::vector<Value>>(
      const std::vector<Value> &, const ExecutionContext &, EventTxn &)>;

private:
  struct Binding {
    exec::ServiceSignature signature;
    Provider provider;
  };
  Runtime &runtime_;
  std::size_t capacity_;
  bool frozen_{};
  std::map<std::uint32_t, Binding> bindings_;
  std::optional<Handle> current_process_;
  const exec::Project *core_types_{};

public:
  explicit CoreRuntimeBackend(Runtime &, std::size_t provider_capacity = 128);
  CoreRuntimeBackend(Runtime &, const exec::Project &, std::size_t provider_capacity = 128);
  Expected<void> register_provider(exec::ServiceSignature, Provider);
  Expected<void> register_providers(std::vector<std::pair<exec::ServiceSignature, Provider>>);
  Expected<void> register_core(exec::ServiceSignature);
  Expected<void> register_core(exec::ServiceSignature, const exec::Project &);
  Expected<void> freeze();
  Expected<void> bind_current_process(Handle);
  Expected<void> check_signature(const exec::ServiceSignature &) const override;
  Expected<std::vector<Value>> invoke(const exec::ServiceSignature &, const std::vector<Value> &,
                                      const ExecutionContext &, EventTxn &) override;
  Expected<void> prepare_suspend(Handle, BlockId, std::vector<Value>, TypeId,
                                 const ExecutionContext &, EventTxn &) override;
  Expected<exec::ResumeInput> prepare_resume(const SuspensionToken &, const ExecutionContext &,
                                             EventTxn &) override;
};
} // namespace leanat
