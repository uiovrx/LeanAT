#include "leanat/protocol.hpp"
namespace leanat {
class ProtocolBinding final : public PreparedParticipant {
  ProtocolEngine *engine_;
  Handle hop_;
  std::map<Handle, ProtocolEngine::Ledger> node_;
  bool active_{true};
  const void *reservation_key_{};

public:
  ProtocolBinding(ProtocolEngine &e, Handle h, LedgerIdentity id) : engine_(&e), hop_(h) {
    WireSnapshot snapshot;
    snapshot.identity = id;
    snapshot.protocol_state = e.package_ ? e.package_->initial : 0;
    node_.emplace(h, ProtocolEngine::Ledger{snapshot, h, {}, {}, Command::Ignore, 0});
    reservation_key_ = &e.prepared_bindings_.emplace(h, id).first->second;
  }
  ~ProtocolBinding() override {
    discard();
  }
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this) + sizeof(ProtocolEngine::Ledger) + sizeof(LedgerIdentity) +
           20 * sizeof(void *);
  }
  const void *identity() const noexcept override { return reservation_key_; }
  Expected<WireSnapshot> inspect(const ProtocolEngine &engine, Handle hop) const {
    if (&engine != engine_ || hop != hop_)
      return fail(ErrorCode::WrongOwner, "foreign prepared ledger");
    auto checked = validate();
    if (!checked) return checked.error();
    return node_.at(hop_).snapshot;
  }
  Expected<void> validate() const override {
    if (!active_ || !engine_->prepared_bindings_.count(hop_) || engine_->ledgers_.count(hop_))
      return fail(ErrorCode::InvalidState, "protocol binding reservation changed");
    return {};
  }
  void apply() noexcept override {
    if (!active_)
      return;
    engine_->ledgers_.merge(node_);
    engine_->prepared_bindings_.erase(hop_);
    active_ = false;
  }
  void discard() noexcept override {
    if (active_)
      engine_->prepared_bindings_.erase(hop_);
    active_ = false;
  }
};
class ProtocolRetirement final : public PreparedParticipant {
  ProtocolEngine &engine_;
  EventTxn &transaction_;
  Handle hop_;
  bool active_{true};
public:
  ProtocolRetirement(ProtocolEngine &engine, EventTxn &tx, Handle hop)
      : engine_(engine), transaction_(tx), hop_(hop) {
    engine_.prepared_retirements_.emplace(hop_, &transaction_);
  }
  ~ProtocolRetirement() override { discard(); }
  std::size_t reserved_bytes() const noexcept override {
    return sizeof(*this) + sizeof(std::pair<const Handle, const EventTxn *>) + 8 * sizeof(void *);
  }
  Expected<void> validate() const override {
    auto reserved = engine_.prepared_retirements_.find(hop_);
    if (!active_ || reserved == engine_.prepared_retirements_.end() || reserved->second != &transaction_)
      return fail(ErrorCode::InvalidState, "protocol retirement reservation changed");
    auto wire = engine_.prepared_inspect(transaction_, hop_);
    if (!wire) return wire.error();
    if (wire.value().state != WireState::Idle || wire.value().pending ||
        wire.value().faulted || wire.value().call_ordinal)
      return fail(ErrorCode::InvalidState, "cannot retire a started ledger");
    return {};
  }
  void apply() noexcept override {
    if (!active_) return;
    engine_.ledgers_.erase(hop_);
    engine_.prepared_retirements_.erase(hop_);
    active_ = false;
  }
  void discard() noexcept override {
    if (active_) engine_.prepared_retirements_.erase(hop_);
    active_ = false;
  }
};
Expected<void> ProtocolEngine::stage_retire_unstarted(EventTxn &tx, Handle hop) {
  if (!tx.is_open() || tx.context().domain != domain_ || tx.context().instance != side_ ||
      tx.context().owner != hop.owner)
    return fail(ErrorCode::WrongOwner, "ledger retirement segment context");
  if (prepared_retirements_.count(hop))
    return fail(ErrorCode::Duplicate, "ledger retirement already staged");
  auto proof = prepared_inspect(tx, hop);
  if (!proof) return proof.error();
  if (proof.value().state != WireState::Idle || proof.value().pending ||
      proof.value().faulted || proof.value().call_ordinal)
    return fail(ErrorCode::InvalidState, "cannot retire a started ledger");
  return tx.stage_participant(std::make_unique<ProtocolRetirement>(*this, tx, hop));
}
Expected<WireSnapshot> ProtocolEngine::prepared_inspect(const EventTxn &tx, Handle hop) const {
  auto live = inspect(hop);
  if (live) return live;
  if (!tx.is_open() || tx.context().domain != domain_ || tx.context().instance != side_ ||
      tx.context().owner != hop.owner)
    return fail(ErrorCode::WrongOwner, "prepared ledger segment context");
  auto reserved = prepared_bindings_.find(hop);
  if (reserved == prepared_bindings_.end())
    return fail(ErrorCode::StaleHandle, "no prepared ledger binding");
  auto participant = dynamic_cast<const ProtocolBinding *>(tx.participant(&reserved->second));
  if (!participant)
    return fail(ErrorCode::WrongOwner, "prepared ledger belongs to another segment");
  return participant->inspect(*this, hop);
}
Expected<void> ProtocolEngine::prepare_bind_ledger(EventTxn &tx, Handle h, ConnectionId c,
                                                   TransportId t, std::uint64_t generation) {
  if (h.domain != domain_ || h.kind != HandleKind::Hop || !h.generation || !generation ||
      tx.context().domain != domain_ || tx.context().owner != h.owner ||
      tx.context().instance != side_)
    return fail(ErrorCode::WrongOwner, "prepared local ledger identity/context");
  if (ledgers_.size() + prepared_bindings_.size() >= ledger_limit_)
    return fail(ErrorCode::Capacity, "ledger binding capacity");
  if (ledgers_.count(h) || prepared_bindings_.count(h))
    return fail(ErrorCode::Duplicate, "ledger already bound/reserved");
  for (auto &p : ledgers_)
    if (p.second.snapshot.identity.connection == c && p.second.snapshot.identity.transport == t)
      return fail(ErrorCode::Duplicate, "local transport already bound");
  for (auto &p : prepared_bindings_)
    if (p.second.connection == c && p.second.transport == t)
      return fail(ErrorCode::Duplicate, "local transport already reserved");
  return tx.stage_participant(std::make_unique<ProtocolBinding>(
      *this, h, LedgerIdentity{domain_, side_, c, t, generation}));
}
} // namespace leanat
