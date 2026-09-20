#pragma once
#include "runtime_services.hpp"
namespace leanat {
class PayloadServiceContext {
public:
  struct Binding {
    PayloadViewKey key;
    PayloadAccessContext access;
  };
  class Scope {
    friend class PayloadServiceContext;
    PayloadServiceContext *context_{};
    std::uint64_t generation_{}, previous_generation_{};
    std::optional<Binding> previous_;
    Scope(PayloadServiceContext *c, std::uint64_t g, std::uint64_t previous_generation,
          std::optional<Binding> previous)
        : context_(c), generation_(g), previous_generation_(previous_generation),
          previous_(std::move(previous)) {}

  public:
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    Scope(Scope &&) noexcept;
    Scope &operator=(Scope &&) noexcept;
    ~Scope();
  };

private:
  std::optional<Binding> current_;
  std::uint64_t generation_{}, active_generation_{};
  std::size_t depth_{}, max_depth_{16};
  bool poisoned_{};
  void close(std::uint64_t, std::uint64_t, std::optional<Binding>) noexcept;

public:
  explicit PayloadServiceContext(std::size_t depth = 16) : max_depth_(depth) {
    if (!depth)
      throw std::invalid_argument("payload scope depth");
  }
  PayloadServiceContext(const PayloadServiceContext &) = delete;
  PayloadServiceContext &operator=(const PayloadServiceContext &) = delete;
  Expected<Scope> bind(PayloadViewKey, PayloadAccessContext);
  Expected<Binding> current() const;
};
// Canonical ABI: TxnHandle -> value for reads, TxnHandle,value -> Unit for writes.
// Hash input is UTF-8 LeanAT.Payload.v1|<provider-key>|txn[,...]-><shape>.
Expected<exec::ServiceSignature> payload_service_signature(std::uint32_t id, const std::string &key,
                                                           const std::vector<exec::Type> &types,
                                                           std::uint32_t txn_type,
                                                           std::uint32_t value_type,
                                                           std::uint32_t unit_type = 0);
Expected<void> register_payload_services(CoreRuntimeBackend &, PayloadShadow &,
                                         PayloadServiceContext &, const exec::Project &);
} // namespace leanat
