#pragma once
#include "protocol.hpp"
namespace leanat {
struct RouteSnapshot {
  PortId upstream{}, downstream{};
  std::uint64_t original_address{}, local_address{};
};
enum class ResponseOrder { InOrder, ReadyOrder };
struct AdmissionLimits {
  std::size_t transactions{}, hops{}, services{}, responses{}, request_gate_tickets{};
};
struct AdmissionRequest {
  ConnectionId connection{};
  TransportId transport{};
  std::uint64_t transport_generation{1}, owner{};
  Tick in_time{};
  RouteSnapshot route;
  PayloadSnapshot request;
  bool defer_service{};
};
struct ServicePermit {
  Handle txn, hop;
  Tick ready{};
};
struct AdmissionDisposition {
  Handle txn, hop;
  bool retained_pending{};
  std::optional<ServicePermit> service;
};
struct HopRecord {
  Handle txn, hop;
  ConnectionId connection{};
  TransportId transport{};
  std::uint64_t transport_generation{};
  RouteSnapshot route;
  Tick in_time{};
  bool wire_terminal{}, semantic_terminal{}, pending{}, servicing{};
  std::uint64_t sequence{};
  PayloadSnapshot owned_request;
  bool reset_deferred{};
};
enum class RequestGateState { Pending, Granted, Consumed, Cancelled, Sent };
struct RequestGateTicket {
  Handle handle;
};
struct RequestPermit {
  Handle handle;
};
struct ResponsePermit {
  Handle handle, hop;
  ResponseSnapshot response;
};
struct AdmissionGateSnapshot {
  Handle handle, txn;
  ConnectionId connection;
  RequestGateState state;
  ReadyKey ready;
  std::uint64_t sequence;
};
struct AdmissionResponseSnapshot {
  Handle hop;
  ReadyKey ready;
  ResponseSnapshot response;
  std::optional<Handle> permit;
  bool sent;
};
struct AdmissionStoreSnapshot {
  AdmissionLimits limits;
  ResponseOrder order;
  std::uint64_t next_generation, next_sequence, revision;
  std::size_t active_services, reserved_responses;
  bool prepared;
  std::vector<AdmissionGateSnapshot> gates;
  std::vector<AdmissionResponseSnapshot> responses;
  std::map<ConnectionId, bool> wire_busy;
};
class AdmissionUpdate;
class DrainStore;
class AdmissionStore {
  friend class AdmissionUpdate;
  AdmissionStore(const AdmissionStore &) = default;
  AdmissionStore &operator=(AdmissionStore &&) = default;
  struct Gate {
    Handle handle, txn;
    ConnectionId connection;
    RequestGateState state;
    ReadyKey ready;
    std::uint64_t sequence;
  };
  struct Response {
    Handle hop;
    ReadyKey ready;
    ResponseSnapshot snapshot;
    std::optional<Handle> permit;
    bool sent{};
  };
  DomainId domain_;
  InstanceId side_;
  std::uint32_t store_id_;
  AdmissionLimits limits_;
  ResponseOrder order_;
  std::uint64_t generation_{1}, sequence_{};
  std::size_t active_services_{}, reserved_responses_{};
  std::map<Handle, HopRecord> hops_;
  std::map<Handle, Gate> gates_;
  std::map<Handle, Response> responses_;
  std::map<ConnectionId, bool> wire_busy_;
  std::uint64_t revision_{};
  AdmissionUpdate *prepared_{};
  AdmissionStore *identity_source_{};
  Expected<void> touch();
  Expected<Handle> fresh(HandleKind, std::uint64_t);
  bool valid_txn(Handle) const;

public:
  AdmissionStoreSnapshot snapshot() const {
    AdmissionStoreSnapshot out{limits_,
                               order_,
                               generation_,
                               sequence_,
                               revision_,
                               active_services_,
                               reserved_responses_,
                               prepared_ != nullptr,
                               {},
                               {},
                               wire_busy_};
    for (const auto &entry : gates_) {
      const auto &g = entry.second;
      out.gates.push_back({g.handle, g.txn, g.connection, g.state, g.ready, g.sequence});
    }
    for (const auto &entry : responses_) {
      const auto &r = entry.second;
      out.responses.push_back({r.hop, r.ready, r.snapshot, r.permit, r.sent});
    }
    return out;
  }
  AdmissionStore(DomainId, InstanceId, AdmissionLimits, ResponseOrder = ResponseOrder::InOrder);
  Expected<std::unique_ptr<AdmissionUpdate>> prepare();
  Expected<std::unique_ptr<AdmissionUpdate>> prepare(EventTxn &);
  Expected<AdmissionDisposition> admit(const AdmissionRequest &, bool request_lane_free);
  Expected<AdmissionDisposition> create_initiator(const AdmissionRequest &);
  Expected<void> retire_unstarted(Handle hop, ProtocolEngine &);
  Expected<void> stage_retire_unstarted(EventTxn &, Handle hop, ProtocolEngine &);
  Expected<std::optional<ServicePermit>> promote_pending(ConnectionId, Tick);
  Expected<std::optional<ServicePermit>> promote_pending(ConnectionId, Tick, const DrainStore &);
  Expected<Handle> create_hop(Handle, ConnectionId, TransportId, std::uint64_t, RouteSnapshot);
  Expected<HopRecord> inspect(Handle) const;
  Expected<HopRecord> lookup(Handle txn, ConnectionId) const;
  Expected<void> queue_response(ServicePermit, ReadyKey, ResponseSnapshot);
  Expected<std::optional<ResponsePermit>> select_response(ConnectionId, bool response_lane_free);
  Expected<void> cancel_response_permit(ResponsePermit);
  Expected<void> response_sent(ResponsePermit);
  Expected<void> mark_wire_terminal(Handle, const ExchangeResult &);
  Expected<void> mark_semantic_terminal(Handle, Tick);
  Expected<void> sync_terminal(Handle, const ProtocolEngine &, Tick effective_time);
  Expected<void> retire(Handle);
  Expected<RequestGateTicket> request_request_gate(Handle, ConnectionId, ReadyKey);
  Expected<std::optional<RequestPermit>> try_request_permit(Handle, ConnectionId, ReadyKey);
  Expected<RequestGateState> gate_state(RequestGateTicket) const;
  Expected<ReadyKey> gate_ready(RequestGateTicket) const;
  Expected<RequestPermit> consume_request_gate(RequestGateTicket);
  Expected<void> validate_request_permit(RequestPermit, Handle txn, ConnectionId) const;
  Expected<void> cancel_request_gate(RequestGateTicket, ReadyKey cancel_commit);
  Expected<void> cancel_request_permit(RequestPermit, ReadyKey cancel_commit);
  Expected<void> request_sent(RequestPermit);
  Expected<void> retire_request_ticket(RequestGateTicket);
  Expected<void> update_request_lane(ConnectionId, bool free, ReadyKey grant_commit);
  std::size_t active_services() const {
    return active_services_;
  }
  DomainId domain() const {
    return domain_;
  }
  InstanceId local_side() const {
    return side_;
  }
};
// Prepare all admission operations through draft(), then stage this participant
// alongside EventTxn intents. Discard leaves the original store unchanged.
class AdmissionUpdate final : public PreparedParticipant {
  friend class AdmissionStore;
  AdmissionStore *target_;
  AdmissionStore staged_;
  std::uint64_t revision_;
  bool finished_{};
  AdmissionUpdate *previous_{};
  explicit AdmissionUpdate(AdmissionStore &, AdmissionUpdate *previous = nullptr);
  AdmissionUpdate(const AdmissionUpdate &) = delete;
  AdmissionUpdate &operator=(const AdmissionUpdate &) = delete;

public:
  struct Checkpoint {
  private:
    friend class AdmissionUpdate;
    std::unique_ptr<AdmissionStore> state;
  };
  ~AdmissionUpdate() override;
  AdmissionStore &draft() {
    return staged_;
  }
  Expected<Checkpoint> checkpoint() const;
  Expected<void> rollback(Checkpoint &&);
  Expected<void> validate() const override;
  std::size_t reserved_bytes() const noexcept override;
  const void *identity() const noexcept override {
    return target_;
  }
  void apply() noexcept override;
  void discard() noexcept override;
};
class DrainStore;
Expected<AdmissionDisposition> stage_admission(EventTxn &, AdmissionStore &, DrainStore &,
                                               const AdmissionRequest &, bool request_lane_free,
                                               std::optional<Handle> verified_parent = {});
} // namespace leanat
