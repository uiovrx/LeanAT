#pragma once
#include "memory.hpp"
namespace leanat {
enum class RegisterAccess { RO, RW, WO, W1C, W1S, RC };
enum class RegisterEndian { Little, Big };
struct FieldSpec {
  FieldId id;
  std::size_t lsb{}, width{};
  RegisterAccess access{RegisterAccess::RW};
  Bytes reset;
};
struct RegisterSpec {
  std::uint64_t offset{};
  std::size_t width_bits{};
  std::vector<FieldSpec> fields;
  std::vector<std::size_t> allowed_access_bytes;
  std::optional<std::size_t> alignment_bytes;
  RegisterEndian endian{RegisterEndian::Little};
};
class RegisterBank {
  std::vector<RegisterSpec> specs_;
  std::vector<std::size_t> starts_;
  VersionedCell cell_;
  Bytes initial_;
  std::optional<std::pair<std::size_t, std::size_t>> locate(std::uint64_t) const;

public:
  static Expected<RegisterBank> make(std::vector<RegisterSpec>, std::size_t max_bits = 4096);
  Expected<MemoryTransferResult> access(EventTxn &, const PayloadSnapshot &);
  Expected<ResponseStatus> access(EventTxn &, PayloadShadow &, const PayloadViewKey &);
  Expected<MemoryDebugResult> peek_poke(EventTxn &, Command, std::uint64_t, const Bytes &,
                                        std::size_t max_bytes = 65536);
  Expected<Bytes> read_field(EventTxn &, FieldId) const;
  Expected<void> stage_field_update(EventTxn &, FieldId, const Bytes &);
  Expected<void> reset(EventTxn &);
  Expected<Value> snapshot(EventTxn &t) const {
    auto checked = object_context(t, t.context().kind == ContextKind::Debug);
    if (!checked)
      return checked.error();
    return t.read(cell_);
  }
};
using RegBank = RegisterBank;
} // namespace leanat
