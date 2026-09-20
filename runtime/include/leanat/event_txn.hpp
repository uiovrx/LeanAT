#pragma once
#include "common.hpp"
#include "event_queue.hpp"
namespace leanat {
// Stable backing cell. Provider adapters prepare owned values before staging.
struct VersionedCell {
  Value value;
  std::uint64_t version{}, epoch{};
  bool stable_bytes{}, epoch_independent{};
};
struct SegmentBudget {
  std::size_t writes{128}, actions{128}, bytes{65536}, events{128};
};
struct CommittedActions {
  std::vector<SendIntent> actions;
  std::size_t cursor{};
  bool failed{};
};
class PreparedParticipant {
public:
  virtual ~PreparedParticipant() = default;
  virtual std::size_t reserved_bytes() const noexcept = 0;
  virtual const void *identity() const noexcept {
    return nullptr;
  }
  virtual Expected<void> validate() const = 0;
  virtual Expected<void> validate_prepare() const {
    return validate();
  }
  virtual void apply() noexcept = 0;
  virtual void discard() noexcept = 0;
};
class EventTxn {
  struct Write {
    VersionedCell *cell;
    std::uint64_t version;
    Value value;
  };
  ExecutionContext context_;
  SegmentBudget budget_;
  std::vector<Write> writes_;
  std::vector<SendIntent> actions_;
  std::vector<std::pair<EventQueue *, EventReservation>> events_;
  std::vector<std::unique_ptr<PreparedParticipant>> participants_;
  std::vector<std::size_t> participant_bytes_;
  std::vector<std::pair<EventQueue *, EventToken>> cancels_;
  std::size_t bytes_{};
  bool closed_{}, committed_{};

public:
  struct Savepoint {
  private:
    friend class EventTxn;
    const EventTxn *owner{};
    std::vector<Write> writes;
    std::vector<std::size_t> participant_bytes;
    std::size_t bytes{}, actions{}, events{}, cancels{}, participants{};
  };
  EventTxn(SegmentBudget, ExecutionContext);
  ~EventTxn();
  Expected<void> stage_participant(std::unique_ptr<PreparedParticipant>);
  PreparedParticipant *participant(const void *key) const;
  Expected<Savepoint> checkpoint() const;
  Expected<void> rollback(Savepoint &&);
  Expected<void> validate_effects_since(const Savepoint &, bool writes, bool events,
                                        bool actions) const;
  EventTxn(const EventTxn &) = delete;
  EventTxn &operator=(const EventTxn &) = delete;
  EventTxn(EventTxn &&) = default;
  const ExecutionContext &context() const {
    return context_;
  }
  bool is_open() const noexcept {
    return !closed_;
  }
  std::size_t remaining_bytes() const noexcept {
    return budget_.bytes - bytes_;
  }
  Expected<void> reserve_bytes(std::size_t bytes);
  Expected<void> reserve_participant_growth(PreparedParticipant &, std::size_t new_bytes);
  Expected<bool> can_cancel(const EventQueue &, EventToken) const;
  Expected<Value> read(const VersionedCell &) const;
  Expected<void> buffer(VersionedCell &, Value);
  Expected<void> stage_action(SendIntent);
  Expected<EventToken> stage_event(EventQueue &, EventDraft);
  Expected<void> stage_cancel(EventQueue &, EventToken);
  Expected<CommittedActions> commit(std::uint64_t current_epoch);
  Expected<CommittedActions> commit() {
    return commit(context_.epoch);
  }
  Expected<void> discard();
};
Expected<void> publish_actions(CommittedActions &,
                               const std::function<Expected<void>(const SendIntent &)> &);
} // namespace leanat
