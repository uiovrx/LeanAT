#include "leanat/admission.hpp"
#include "leanat/drain.hpp"
namespace leanat {
Expected<AdmissionDisposition> stage_admission(EventTxn &tx, AdmissionStore &store,
                                               DrainStore &drains, const AdmissionRequest &request,
                                               bool lane_free, std::optional<Handle> parent) {
  if (tx.context().owner != request.owner)
    return fail(ErrorCode::WrongOwner, "admission request owner differs from event owner");
  auto save = tx.checkpoint();
  if (!save)
    return save.error();
  auto admission = store.prepare();
  if (!admission)
    return admission.error();
  auto drain = drains.prepare();
  if (!drain)
    return drain.error();
  auto input = request;
  bool deferred = !parent && !drain.value()->view().admits_root(tx.context().instance);
  input.defer_service = deferred;
  auto admitted = admission.value()->draft().admit(input, lane_free);
  if (!admitted)
    return admitted.error();
  Responsibility responsibility;
  responsibility.transaction = admitted.value().txn;
  responsibility.instance = tx.context().instance;
  responsibility.epoch = tx.context().epoch;
  responsibility.hops.push_back({admitted.value().hop, false, false, false, false});
  auto registered =
      deferred ? drain.value()->view().defer_root(std::move(responsibility))
               : drain.value()->view().register_responsibility(std::move(responsibility), parent);
  if (!registered)
    return registered.error();
  auto stage = tx.stage_participant(std::move(admission.value()));
  if (!stage)
    return stage.error();
  stage = tx.stage_participant(std::move(drain.value()));
  if (!stage) {
    auto reverted = tx.rollback(std::move(save.value()));
    if (!reverted)
      return reverted.error();
    return stage.error();
  }
  return admitted.value();
}
Expected<std::optional<ServicePermit>> AdmissionStore::promote_pending(ConnectionId c, Tick now,
                                                                       const DrainStore &drains) {
  auto mutation = touch();
  if (!mutation)
    return mutation.error();
  for (auto &p : hops_) {
    auto &h = p.second;
    if (h.connection == c && h.pending && h.reset_deferred) {
      bool registered = false;
      for (auto &r : drains.stop_report())
        if (r.transaction == h.txn) {
          registered = true;
          break;
        }
      if (!registered)
        return fail(ErrorCode::StaleHandle, "deferred root lacks drain responsibility");
      if (drains.is_deferred(h.txn))
        return std::optional<ServicePermit>{};
      h.reset_deferred = false;
    }
  }
  return promote_pending(c, now);
}
} // namespace leanat
