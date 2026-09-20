#pragma once
#include "leanat/event_queue.hpp"
namespace leanat::testing {
// Test-only counter positioning. No definition is installed with runtime headers.
// Tests position counters, then execute the ordinary production queue methods.
class EventQueueTestAccess {
public:
  struct SlotSnapshot {
    QueuedEvent queued;
    std::string state;
  };
  struct Snapshot {
    std::vector<SlotSnapshot> slots;
    std::optional<ReadyKey> frontier;
    std::optional<BatchId> batch;
    ReadyKey batch_ready;
    std::vector<std::uint32_t> members;
    std::size_t cursor{}, resolver_cursor{}, reclaim_cursor{}, max_event_bytes{};
    std::optional<std::size_t> resolver_total;
    std::uint64_t next_batch{}, next_sequence{};
  };
  static Snapshot snapshot(const EventQueue &q) {
    Snapshot out;
    const char *states[] = {"free", "reserved", "queued", "active", "done", "retired"};
    for (std::size_t index = 0; index < q.slots_.size(); ++index) {
      const auto &slot = q.slots_[index];
      Handle token{HandleKind::Event, q.domain_,       q.store_, static_cast<std::uint32_t>(index),
                   slot.generation,   slot.event.owner};
      out.slots.push_back({QueuedEvent{token, slot.event, slot.cancelled},
                           states[static_cast<unsigned>(slot.state)]});
    }
    out.frontier = q.frontier_;
    out.batch = q.batch_;
    out.batch_ready = q.batch_ready_;
    out.members = q.members_;
    out.cursor = q.cursor_;
    out.resolver_cursor = q.resolver_cursor_;
    out.reclaim_cursor = q.reclaim_cursor_;
    out.max_event_bytes = q.max_event_bytes_;
    out.resolver_total = q.resolver_total_;
    out.next_batch = q.next_batch_;
    out.next_sequence = q.next_sequence_;
    return out;
  }
  static Expected<void> seed_free_generation(EventQueue &q, std::size_t slot,
                                             std::uint64_t generation) {
    if (q.batch_ || slot >= q.slots_.size() || q.slots_[slot].state != EventQueue::State::Free)
      return fail(ErrorCode::InvalidState, "generation seam requires an idle free slot");
    if (generation < q.slots_[slot].generation)
      return fail(ErrorCode::InvalidArgument, "generation seam cannot rewind");
    q.slots_[slot].generation = generation;
    return {};
  }
  static Expected<void> seed_next_sequence(EventQueue &q, std::uint64_t sequence) {
    if (q.batch_ || q.occupied())
      return fail(ErrorCode::InvalidState, "sequence seam requires an empty queue");
    if (sequence < q.next_sequence_)
      return fail(ErrorCode::InvalidArgument, "sequence seam cannot rewind");
    q.next_sequence_ = sequence;
    return {};
  }
  static Expected<void> seed_next_batch(EventQueue &q, std::uint64_t batch) {
    if (q.batch_ || q.occupied())
      return fail(ErrorCode::InvalidState, "batch seam requires an empty queue");
    if (batch < q.next_batch_)
      return fail(ErrorCode::InvalidArgument, "batch seam cannot rewind");
    q.next_batch_ = batch;
    return {};
  }
  static bool retired(const EventQueue &q, std::size_t slot) {
    return slot < q.slots_.size() && q.slots_[slot].state == EventQueue::State::Retired;
  }
  static std::uint64_t next_sequence(const EventQueue &q) {
    return q.next_sequence_;
  }
  static std::uint64_t next_batch(const EventQueue &q) {
    return q.next_batch_;
  }
};
} // namespace leanat::testing
