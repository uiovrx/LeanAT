#pragma once
#include "leanat/cancel_scope.hpp"
#include "leanat/result_store.hpp"
#include "objects.hpp"
namespace leanat {
enum class QueuePublication { LocalUnpublished, PublishedProtocol };
enum class QueueOwnership { ValueOnly, MoveOwned, RetainReadOnly };
struct QueueEntry {
  std::uint64_t id{};
  Value value;
  std::optional<Handle> owner;
  QueuePublication publication{QueuePublication::LocalUnpublished};
  std::optional<ResultHandle> consumer;
};
struct QueueRemoval {
  std::vector<std::uint64_t> entry_ids;
};
class BoundedQueue {
  std::size_t capacity_, max_value_bytes_, scan_budget_;
  DomainId domain_;
  VersionedCell cell_;
  std::function<Expected<void>(Handle)> owner_validator_;
  ResultStore *results_{};
  CancelScopeStore *scopes_{};
  std::uint64_t consumer_owner_{};

public:
  BoundedQueue(const BoundedQueue &) = delete;
  BoundedQueue &operator=(const BoundedQueue &) = delete;
  BoundedQueue(BoundedQueue &&) = default;
  BoundedQueue &operator=(BoundedQueue &&) = default;
  void set_scope_store(CancelScopeStore &scopes) {
    scopes_ = &scopes;
  }
  explicit BoundedQueue(std::size_t capacity, std::size_t max_value_bytes = 4096,
                        std::size_t scan_budget = 4096, DomainId domain = {});
  void set_owner_validator(std::function<Expected<void>(Handle)> f) {
    owner_validator_ = std::move(f);
  }
  void set_consumer_store(ResultStore &r, std::uint64_t queue_owner) {
    results_ = &r;
    consumer_owner_ = queue_owner;
  }
  Expected<bool> try_push_owned(EventTxn &, ResultHandle, std::optional<Handle> scope = {},
                                QueueOwnership = QueueOwnership::MoveOwned);
  Expected<bool> try_push(EventTxn &, Value, std::optional<Handle> owner = {},
                          QueueOwnership = QueueOwnership::ValueOnly);
  Expected<std::optional<QueueEntry>> try_pop(EventTxn &);
  Expected<std::size_t> size(EventTxn &) const;
  Expected<std::vector<QueueEntry>> inspect_entries(EventTxn &) const;
  Expected<Value> snapshot(EventTxn &t) const {
    auto checked = inspect_entries(t);
    if (!checked)
      return checked.error();
    return t.read(cell_);
  }
  Expected<void> mark_published(EventTxn &, std::uint64_t entry_id);
  Expected<QueueRemoval> remove_unpublished_owned(EventTxn &, Handle owner);
  Expected<QueueRemoval> transfer_drain_owned(EventTxn &, Handle owner, Handle drain,
                                              std::size_t destination_capacity);
};
using Queue = BoundedQueue;
using CancelAwareQueue = BoundedQueue;
} // namespace leanat
