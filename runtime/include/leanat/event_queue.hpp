#pragma once
#include "common.hpp"
namespace leanat {
namespace testing {
class EventQueueTestAccess;
}
enum class EventStage { Reset, Cancel, Input, Internal, WaitResolve, Resume, Output };
struct EventKey {
  Tick time{};
  std::uint64_t turn{};
  EventStage stage{EventStage::Internal};
  InstanceId instance{};
  ConnectionId connection{};
  std::uint64_t sequence{};
  friend bool operator<(const EventKey &a, const EventKey &b) {
    return std::tie(a.time, a.turn, a.stage, a.instance, a.connection, a.sequence) <
           std::tie(b.time, b.turn, b.stage, b.instance, b.connection, b.sequence);
  }
};
struct EventDraft {
  EventKey key;
  Value value;
  std::uint64_t owner{}, epoch{};
};
using EventToken = Handle;
struct QueuedEvent {
  EventToken token;
  EventDraft event;
  bool cancelled{};
};
struct ClosedBatchSlice {
  BatchId id;
  ReadyKey ready;
  std::size_t cursor{};
  std::vector<QueuedEvent> members;
  bool members_complete{};
};
enum class ExecutionDisposition { Committed, Cancelled, Failed };
class EventQueue;
class EventReservation {
  friend class EventQueue;
  EventQueue *queue_{};
  Handle token_{};
  EventReservation(EventQueue *q, Handle h) : queue_(q), token_(h) {};

public:
  Handle token() const {
    return token_;
  }
  EventReservation() = default;
  EventReservation(const EventReservation &) = delete;
  EventReservation &operator=(const EventReservation &) = delete;
  EventReservation(EventReservation &&) noexcept;
  EventReservation &operator=(EventReservation &&) noexcept;
  ~EventReservation();
};
class EventQueue {
  friend class EventReservation;
  friend class testing::EventQueueTestAccess;
  enum class State { Free, Reserved, Queued, Active, Done, Retired };
  struct Slot {
    State state{State::Free};
    std::uint64_t generation{};
    EventDraft event;
    bool cancelled{};
  };
  std::vector<Slot> slots_;
  DomainId domain_;
  std::uint32_t store_;
  std::optional<ReadyKey> frontier_;
  std::optional<BatchId> batch_;
  ReadyKey batch_ready_{};
  std::vector<std::uint32_t> members_;
  std::size_t cursor_{}, resolver_cursor_{};
  std::size_t reclaim_cursor_{};
  std::size_t max_event_bytes_{};
  std::optional<std::size_t> resolver_total_;
  std::uint64_t next_batch_{1}, next_sequence_{1};
  Expected<std::uint32_t> validate(Handle) const;
  void abandon(Handle) noexcept;

public:
  explicit EventQueue(std::size_t capacity = 256, DomainId domain = DomainId{},
                      std::uint32_t store = 1, std::size_t max_event_bytes = 65536);
  EventQueue(const EventQueue &) = delete;
  EventQueue &operator=(const EventQueue &) = delete;
  EventQueue(EventQueue &&) = delete;
  EventQueue &operator=(EventQueue &&) = delete;
  Expected<EventReservation> prepare(EventDraft);
  Expected<EventToken> enqueue(EventReservation &&);
  Expected<EventToken> enqueue(EventDraft);
  Expected<void> retarget(EventReservation &, EventDraft);
  Expected<bool> is_active(EventToken) const;
  Expected<EventDraft> active_event(EventToken) const;
  Expected<void> preflight(const EventReservation &) const;
  Expected<void> validate_cancel(EventToken) const;
  Expected<bool> cancel(EventToken);
  Expected<bool> can_cancel(EventToken) const;
  Expected<std::optional<ClosedBatchSlice>> pop_batch(Tick, std::uint32_t budget);
  Expected<void> ack_executed(BatchId, EventToken, ExecutionDisposition);
  Expected<bool> resolver_step(BatchId, std::uint32_t budget, std::size_t total_work);
  Expected<void> finish_batch(BatchId);
  std::optional<WakePoint> next_wakeup() const;
  std::size_t collect_stale(std::uint32_t budget);
  std::size_t occupied() const;
  Expected<ReadyKey> successor(Tick) const;
};
inline Expected<Duration> elapsed(Tick later, Tick earlier) {
  if (later < earlier)
    return fail(ErrorCode::TimeRegression, "negative elapsed");
  return Duration{later.value - earlier.value};
}
} // namespace leanat
