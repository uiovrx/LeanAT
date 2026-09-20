#pragma once
#include "leanat/admission.hpp"
#include "leanat/common.hpp"
namespace leanat {
struct AddressRegion {
  std::uint64_t source_start{}, size{}, target_start{};
  ConnectionId target{};
  std::uint8_t raw_permissions{3};
  bool affine{true};
};
struct Route {
  std::uint64_t map_version{};
  std::size_t index{};
  std::uint64_t map_identity{};
};
struct CrossbarConfig {
  std::vector<AddressRegion> regions;
  std::vector<ConnectionId> upstreams, downstreams;
  std::size_t max_routes{64}, max_grants{64};
  Duration request_delay{}, response_delay{};
};
struct RoutedRequest {
  Handle handle, txn;
  ConnectionId upstream, downstream;
  TransportId transport;
  std::uint64_t ingress{}, egress{};
  PayloadSnapshot payload;
  bool terminal{}, draining{};
  std::size_t references{1};
};
struct RouteDmiGrant {
  std::uint8_t *pointer{};
  std::uint64_t start{}, end{};
  std::uint8_t permissions{};
  Duration read_latency{}, write_latency{};
};
struct RouteDmiQuery {
  ConnectionId upstream;
  std::uint64_t address{};
  Command command{Command::Read};
  std::uint8_t permissions{3};
};
struct RouteInvalidation {
  ConnectionId upstream;
  std::uint64_t start{}, end{};
};
class Crossbar {
  CrossbarConfig config_;
  DomainId domain_;
  std::uint64_t version_, identity_, next_{1};
  std::map<Handle, RoutedRequest> routes_;
  struct Grant {
    Route route;
    ConnectionId upstream;
    std::uint64_t start, end;
  };
  std::vector<Grant> grants_;
  Expected<const AddressRegion *> region(Route) const;

public:
  Crossbar(CrossbarConfig, DomainId, std::uint64_t version);
  Crossbar(const Crossbar &) = delete;
  Crossbar &operator=(const Crossbar &) = delete;
  static Expected<void> validate_map(const CrossbarConfig &);
  std::optional<Route> decode(std::uint64_t) const;
  Expected<std::uint64_t> translate(Route, std::uint64_t) const;
  Expected<PayloadSnapshot> translate_payload(Route, const PayloadSnapshot &) const;
  Expected<Handle> forward(AdmissionStore &, Route, Handle txn, ConnectionId upstream, TransportId,
                           const PayloadSnapshot &, std::uint64_t transport_generation = 1);
  Expected<RoutedRequest> inspect(Handle) const;
  Expected<ConnectionId> route_response(Handle) const;
  Expected<void> retain(Handle);
  Expected<void> release(Handle);
  Expected<void> terminal(Handle);
  Expected<void> cancel(Handle);
  Expected<Tick> request_ready(Tick) const;
  Expected<Tick> response_ready(Tick) const;
  Expected<std::size_t> debug_prefix(Route, std::uint64_t, std::size_t, std::size_t) const;
  Expected<std::optional<RouteDmiGrant>> translate_dmi(Route, const RouteDmiGrant &,
                                                       const RouteDmiQuery &,
                                                       Duration read_cost = {},
                                                       Duration write_cost = {});
  Expected<std::vector<RouteInvalidation>> invalidate(ConnectionId, std::uint64_t, std::uint64_t,
                                                      std::size_t capacity);
  std::size_t outstanding() const {
    return routes_.size();
  }
};
} // namespace leanat
#include "leanat/payload_helpers.hpp"
#include "leanat/runtime.hpp"
namespace leanat {
class CrossbarSession {
  Crossbar &crossbar_;
  Runtime &runtime_;
  AdmissionStore &admission_;
  ExecutionContext context_;
  ServicePermit upstream_;
  std::optional<Handle> child_;
  PayloadSnapshot request_, translated_;
  std::optional<RequestGateTicket> gate_;
  std::optional<RequestPermit> permit_;
  std::optional<ResponsePermit> response_permit_;
  Tick request_time_{}, response_time_{};
  bool started_{}, queued_{}, child_started_{}, gate_retired_{}, response_ready_{},
      response_queued_{}, response_sent_{}, child_ack_{}, cancelled_{}, finished_{},
      admission_retired_{}, local_finished_{}, child_retired_{}, upstream_retired_{};
  Expected<void> check(const ExecutionContext &) const;
  Expected<bool> finish_retirement();
  Expected<void> prepare_response(const ExecutionContext &, ResponseSnapshot, Tick);

public:
  CrossbarSession(Crossbar &x, Runtime &r, AdmissionStore &a)
      : crossbar_(x), runtime_(r), admission_(a) {}
  CrossbarSession(const CrossbarSession &) = delete;
  CrossbarSession &operator=(const CrossbarSession &) = delete;
  Expected<void> start(const ExecutionContext &, ServicePermit);
  Expected<bool> advance(const ExecutionContext &);
  Expected<CancelDisposition> cancel(const ExecutionContext &, CancelReason = CancelReason::User);
  std::optional<Handle> child() const {
    return child_;
  }
  bool finished() const {
    return finished_;
  }
};
} // namespace leanat
