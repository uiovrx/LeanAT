#pragma once
#include "objects.hpp"
namespace leanat {
class PayloadShadow;
struct PayloadViewKey;
struct MemoryTransferResult {
  ResponseStatus status{ResponseStatus::Ok};
  Bytes data;
};
struct MemoryDebugResult {
  std::size_t count{};
  Bytes data;
};
class Memory {
  VersionedCell cell_;
  Bytes initial_;
  explicit Memory(Bytes b) : cell_{Value(b), 0, 0, true, true}, initial_(std::move(b)) {}

public:
  static Expected<Memory> make(std::size_t size, Bytes initial = {});
  const std::uint8_t *data() const {
    return std::get<Bytes>(cell_.value.data).data();
  }
  std::size_t size() const {
    return initial_.size();
  }
  std::uint64_t version() const {
    return cell_.version;
  }
  VersionedCell &backing() {
    return cell_;
  }
  ResponseStatus validate_transfer(const PayloadSnapshot &) const;
  Expected<MemoryTransferResult> transfer(EventTxn &, const PayloadSnapshot &);
  Expected<ResponseStatus> transfer(EventTxn &, PayloadShadow &, const PayloadViewKey &);
  Expected<Bytes> read_bytes(EventTxn &, std::uint64_t, std::size_t) const;
  Expected<void> write_bytes(EventTxn &, std::uint64_t, const Bytes &, const Bytes &mask = {});
  Expected<MemoryDebugResult> debug_transfer(EventTxn &, Command, std::uint64_t, const Bytes &,
                                             std::size_t max_bytes = 65536);
  Expected<void> clear(EventTxn &);
  Expected<void> reset(EventTxn &);
};
} // namespace leanat
