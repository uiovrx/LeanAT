#pragma once
#include "resource.hpp"
namespace leanat {
struct PipelineTicket {
  Grant grant;
  Handle owner;
  EventToken ready_event;
  std::uint64_t epoch{};
};
class Pipeline {
  Resource resource_;
  EventQueue *events_;
  Pipeline(Resource r, EventQueue &q) : resource_(std::move(r)), events_(&q) {}

public:
  static Expected<Pipeline> make(Duration latency, Duration ii, std::size_t capacity, EventQueue &,
                                 std::size_t records = 128, DomainId domain = {},
                                 std::uint32_t store = 0);
  Expected<PipelineTicket> submit(EventTxn &, Handle owner, Tick earliest);
  Expected<ResourceCancelDisposition> cancel_pending(EventTxn &t, const PipelineTicket &p);
  Expected<bool> on_ready(EventTxn &, const PipelineTicket &, EventToken received);
  Expected<ResourceSnapshot> inspect(EventTxn &t) const {
    return resource_.inspect(t);
  }
  Resource &resource() {
    return resource_;
  }
};
} // namespace leanat
