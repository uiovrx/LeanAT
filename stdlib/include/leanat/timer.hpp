#pragma once
#include "leanat/event_txn.hpp"
namespace leanat {
struct TimerConfig {
  std::optional<Tick> deadline;
  Duration period{};
};
class Timer {
  TimerConfig config_;
  std::uint64_t owner_, epoch_;
  InstanceId instance_;
  DomainId domain_;
  VersionedCell state_;
  Expected<void> context(const ExecutionContext &) const;

public:
  Timer(TimerConfig, std::uint64_t owner, std::uint64_t epoch, InstanceId instance = {},
        DomainId domain = {});
  Timer(const Timer &) = delete;
  Timer &operator=(const Timer &) = delete;
  Expected<EventToken> arm(EventTxn &, EventQueue &);
  Expected<bool> cancel(EventTxn &, EventQueue &, EventToken);
  Expected<bool> cancel_active(EventTxn &, EventQueue &);
  Expected<bool> on_timer(EventTxn &, EventQueue &, EventToken);
  Expected<void> reset(EventTxn &, EventQueue &);
  Expected<void> reset_epoch(EventTxn &, EventQueue &);
  Expected<std::optional<EventToken>> active(const EventTxn &) const;
};
class Interrupt {
  std::uint64_t owner_, epoch_;
  InstanceId instance_;
  DomainId domain_;
  PortId output_;
  bool reset_value_;
  VersionedCell state_, output_value_;
  struct PulseCleanup;
  class PulseCommit;
  std::unique_ptr<PulseCleanup> cleanup_;
  std::function<void(PortId, bool)> output_sink_;
  bool last_published_;
  void publish_committed() noexcept;
  Expected<void> context(const ExecutionContext &) const;

public:
  Interrupt(std::uint64_t owner, std::uint64_t epoch, InstanceId, PortId, bool reset_value = false,
            DomainId domain = {});
  ~Interrupt();
  void bind_output(std::function<void(PortId, bool)> sink) {
    output_sink_ = std::move(sink);
  }
  Expected<bool> dispatch_clear(EventQueue &, EventToken);
  Interrupt(const Interrupt &) = delete;
  Interrupt &operator=(const Interrupt &) = delete;
  Expected<void> set_level(EventTxn &, EventQueue &, bool);
  Expected<EventToken> pulse(EventTxn &, EventQueue &, Duration);
  Expected<bool> on_clear(EventTxn &, EventQueue &, EventToken);
  Expected<void> reset(EventTxn &, EventQueue &);
  Expected<void> reset_epoch(EventTxn &, EventQueue &);
  bool committed_level() const {
    return std::get<bool>(output_value_.value.data);
  }
  Expected<bool> shadow_level(const EventTxn &) const;
  PortId output() const {
    return output_;
  }
};
} // namespace leanat
