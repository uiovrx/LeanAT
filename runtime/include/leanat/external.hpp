#pragma once
#include "common.hpp"
namespace leanat {
using ExternCallId = std::uint32_t;
enum class ExternalLayoutKind { Bytes, U64LE, Bool };
struct ExternalLayout {
  ExternalLayoutKind kind{ExternalLayoutKind::Bytes};
  std::uint32_t max_bytes{1024};
};
// Narrow ABI: buffers never overlap; zero input has a non-null sentinel pointer.
// Pointers are borrowed only during the call; output size must not exceed capacity.
using NativePureFunction = std::int32_t (*)(const std::uint8_t *, std::uint32_t, std::uint8_t *,
                                            std::uint32_t, std::uint32_t *);
struct ExternalEffects {
  bool no_wait{}, no_callback{}, no_runtime_write{}, no_pointer_retention{}, no_throw{},
      deterministic{};
};
struct ExternCallDesc {
  ExternCallId id{};
  std::string logical_id, reference_hash, symbol_id, build_id, contract_hash;
  std::uint32_t abi_version{1};
  ExternalLayout input, output;
  bool select_native{};
  ExternalEffects effects;
  std::vector<ExternCallId> dependencies;
};
struct ExternalComparison {
  ExternCallId id;
  std::string build_id, reference_hash;
  Value input, native_result, reference_result;
};
class ExternalCallGate {
public:
  using Reference = std::function<Expected<Value>(ExternalCallGate &, const Value &)>;
  using Precondition = std::function<Expected<bool>(const Value &)>;
  explicit ExternalCallGate(bool native_capability = false, std::size_t scratch_limit = 65536)
      : native_capability_(native_capability), scratch_limit_(scratch_limit) {}
  Expected<void> register_reference(ExternCallDesc, Reference, Precondition);
  Expected<void> register_native(ExternCallId, const std::string &symbol, const std::string &build,
                                 std::uint32_t abi, NativePureFunction);
  Expected<void> seal();
  Expected<const ExternCallDesc *> descriptor(ExternCallId) const;
  Expected<Value> call_pure(ExternCallId, const Value &);
  Expected<Value> call_reference(ExternCallId, const Value &);
  Expected<void> compare(ExternCallId, const Value &);
  const std::optional<ExternalComparison> &last_comparison() const {
    return comparison_;
  }

private:
  struct Entry {
    ExternCallDesc desc;
    Reference reference;
    Precondition precondition;
    NativePureFunction native{};
  };
  std::map<ExternCallId, Entry> entries_;
  std::optional<ExternalComparison> comparison_;
  bool native_capability_{}, sealed_{};
  std::size_t scratch_limit_, reference_depth_{}, call_depth_{};
  Expected<Value> invoke(ExternCallId, const Value &, bool);
};
} // namespace leanat
