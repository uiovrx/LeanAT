#include "leanat/pipeline.hpp"
namespace leanat {
Expected<ResourceCancelDisposition> Pipeline::cancel_pending(EventTxn &t, const PipelineTicket &p) {
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto cancelled = resource_.cancel_pending(t, p.grant.ticket);
  if (!cancelled)
    return cancelled.error();
  if (cancelled.value() == ResourceCancelDisposition::CancelledUnpublished) {
    auto event = t.stage_cancel(*events_, p.ready_event);
    if (!event) {
      t.rollback(std::move(cp.value()));
      return event.error();
    }
  }
  return cancelled;
}
Expected<Pipeline> Pipeline::make(Duration latency, Duration ii, std::size_t cap, EventQueue &q,
                                  std::size_t records, DomainId d, std::uint32_t store) {
  auto r = Resource::make({ResourceKind::Pipeline, cap, latency, ii, records}, d, store);
  if (!r)
    return r.error();
  return Pipeline{std::move(r.value()), q};
}
Expected<PipelineTicket> Pipeline::submit(EventTxn &t, Handle owner, Tick earliest) {
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto g = resource_.reserve_default(t, owner, earliest);
  if (!g)
    return g.error();
  EventDraft e;
  e.key.time = g.value().finish;
  e.key.stage = EventStage::Internal;
  e.key.instance = t.context().instance;
  e.key.connection = t.context().connection;
  e.owner = owner.owner;
  e.epoch = t.context().epoch;
  e.value = Value(g.value().ticket);
  if (e.key.time == t.context().ready.time) {
    if (t.context().ready.turn == UINT64_MAX) {
      t.rollback(std::move(cp.value()));
      return fail(ErrorCode::Overflow, "pipeline successor turn overflow");
    }
    e.key.turn = t.context().ready.turn + 1;
  }
  auto token = t.stage_event(*events_, std::move(e));
  if (!token) {
    t.rollback(std::move(cp.value()));
    return token.error();
  }
  return PipelineTicket{g.value(), owner, token.value(), t.context().epoch};
}
Expected<bool> Pipeline::on_ready(EventTxn &t, const PipelineTicket &p, EventToken received) {
  auto active = events_->active_event(received);
  if (!active)
    return active.error();
  const auto &event = active.value();
  if (event.key.time != t.context().ready.time || event.key.turn != t.context().ready.turn ||
      event.owner != t.context().owner || event.epoch != p.epoch ||
      !std::holds_alternative<Handle>(event.value.data) ||
      std::get<Handle>(event.value.data) != p.grant.ticket)
    return fail(ErrorCode::InvalidState, "pipeline ready authoritative event mismatch");
  if (received != p.ready_event)
    return fail(ErrorCode::StaleHandle, "pipeline ready event mismatch");
  if (p.owner.owner != p.grant.ticket.owner)
    return fail(ErrorCode::WrongOwner, "pipeline owner mismatch");
  auto done = resource_.complete(t, p.grant.ticket, t.context().ready.time);
  if (!done)
    return done.error();
  return done.value() && p.epoch == t.context().epoch;
}
} // namespace leanat
