#pragma once
#include "common.hpp"
#include <atomic>
namespace leanat::storage_detail {
// One finite counter per program; no registry retains stores or handles.
inline std::uint32_t allocate_store_incarnation() {
  static std::atomic<std::uint64_t> next{1};
  auto candidate = next.load(std::memory_order_relaxed);
  for (;;) {
    if (candidate > UINT32_MAX)
      throw std::overflow_error("storage incarnation exhausted");
    if (next.compare_exchange_weak(candidate, candidate + 1, std::memory_order_relaxed))
      return static_cast<std::uint32_t>(candidate);
  }
}
} // namespace leanat::storage_detail
