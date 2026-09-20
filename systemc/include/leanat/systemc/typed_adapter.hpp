#pragma once
#include "adapter.hpp"
#include <type_traits>
namespace leanat::systemc {
// Transparent adapter. Buffer validity across asynchronous phases remains the initiator's contract.
// Canonical left protocol phases are the base four-phase IDs; right registration uses real phase
// objects.
template <class LeftTypes = tlm::tlm_base_protocol_types,
          class RightTypes = tlm::tlm_base_protocol_types>
class TypedSocketAdapter : public sc_core::sc_module,
                           public tlm::tlm_fw_transport_if<LeftTypes>,
                           public tlm::tlm_bw_transport_if<RightTypes> {
  static_assert(std::is_same_v<typename LeftTypes::tlm_payload_type, tlm::tlm_generic_payload> &&
                    std::is_same_v<typename RightTypes::tlm_payload_type, tlm::tlm_generic_payload>,
                "custom payload unsupported");
  static_assert(std::is_same_v<typename LeftTypes::tlm_phase_type, tlm::tlm_phase> &&
                    std::is_same_v<typename RightTypes::tlm_phase_type, tlm::tlm_phase>,
                "custom phase unsupported");
  CheckedAdapter mapping_;
  std::map<PhaseId, tlm::tlm_phase> native_;
  PayloadBridge payload_;
  std::map<tlm::tlm_generic_payload *, TransportPin> pins_;
  std::size_t capacity_;
  PhaseId right_id(const tlm::tlm_phase &p) {
    for (auto &i : native_)
      if (i.second == p)
        return i.first;
    throw std::runtime_error("unregistered native vendor phase");
  }
  tlm::tlm_phase right_phase(PhaseId p) {
    auto i = native_.find(p);
    if (i == native_.end())
      throw std::runtime_error("missing native phase registration");
    return i->second;
  }
  tlm::tlm_sync_enum forward(bool fw, tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                             sc_core::sc_time &d) {
    NativeGuard guard;
    auto callpin = payload_.pin_nb(g);
    if (!callpin)
      throw std::runtime_error(callpin.error().message);
    auto original = p;
    auto delay = d;
    PhaseId canonical;
    if (fw) {
      auto c = PhaseCodec::decode(p);
      if (!c)
        throw std::runtime_error(c.error().message);
      canonical = c.value();
      if (canonical == begin_req) {
        if (pins_.count(&g) || pins_.size() >= capacity_)
          throw std::runtime_error("adapter live transport capacity/duplicate");
        auto pin = payload_.pin_nb(g);
        pins_.emplace(&g, std::move(pin.value()));
      } else if (!pins_.count(&g))
        throw std::runtime_error("unknown adapter transport");
      auto ph = mapping_.phase(canonical, AdapterDirection::LeftToRight);
      if (!ph)
        throw std::runtime_error(ph.error().message);
      p = right_phase(ph.value());
    } else {
      if (!pins_.count(&g))
        throw std::runtime_error("unknown backward adapter transport");
      auto ph = mapping_.phase(right_id(p), AdapterDirection::RightToLeft);
      if (!ph)
        throw std::runtime_error(ph.error().message);
      canonical = ph.value();
      auto encoded = PhaseCodec::encode(canonical);
      if (!encoded)
        throw std::runtime_error(encoded.error().message);
      p = encoded.value();
    }
    auto sync = fw ? right->nb_transport_fw(g, p, d) : left->nb_transport_bw(g, p, d);
    auto check = guard.unchanged();
    if (!check)
      throw std::runtime_error(check.error().message);
    if (sync == tlm::TLM_ACCEPTED) {
      p = original;
      d = delay;
    } else if (sync == tlm::TLM_UPDATED) {
      if (fw) {
        auto id = mapping_.phase(right_id(p), AdapterDirection::RightToLeft);
        if (!id)
          throw std::runtime_error(id.error().message);
        auto ph = PhaseCodec::encode(id.value());
        if (!ph)
          throw std::runtime_error(ph.error().message);
        p = ph.value();
      } else {
        auto id = PhaseCodec::decode(p);
        if (!id)
          throw std::runtime_error(id.error().message);
        auto ph = mapping_.phase(id.value(), AdapterDirection::LeftToRight);
        if (!ph)
          throw std::runtime_error(ph.error().message);
        p = right_phase(ph.value());
      }
    } else if (sync != tlm::TLM_COMPLETED)
      throw std::runtime_error("invalid adapter sync");
    if (sync == tlm::TLM_COMPLETED || canonical == end_resp ||
        (sync == tlm::TLM_UPDATED && canonical == begin_resp))
      pins_.erase(&g);
    return sync;
  }

public:
  tlm::tlm_target_socket<32, LeftTypes> left;
  tlm::tlm_initiator_socket<32, RightTypes> right;
  TypedSocketAdapter(sc_core::sc_module_name n, CheckedAdapter m,
                     std::map<PhaseId, tlm::tlm_phase> phases, std::size_t capacity = 128)
      : sc_module(n), mapping_(std::move(m)), native_(std::move(phases)), capacity_(capacity),
        left("left"), right("right") {
    auto profile = validate_native_profile(NativeProfile{}, left, right);
    if (!profile)
      throw std::invalid_argument(profile.error().message);
    if (!capacity_ || native_.size() != 4)
      throw std::invalid_argument(
          "typed base adapter needs four native phases and finite capacity");
    std::set<unsigned> registered;
    for (const auto &entry : native_) {
      if (!registered.insert(static_cast<unsigned>(entry.second)).second)
        throw std::invalid_argument("native phase mapping is not injective");
      auto canonical = mapping_.phase(entry.first, AdapterDirection::RightToLeft);
      if (!canonical || !PhaseCodec::encode(canonical.value()))
        throw std::invalid_argument("native phase not in checked base mapping");
    }
    left.bind(*this);
    right.bind(*this);
  }
  tlm::tlm_sync_enum nb_transport_fw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                                     sc_core::sc_time &d) override {
    return forward(true, g, p, d);
  }
  tlm::tlm_sync_enum nb_transport_bw(tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                                     sc_core::sc_time &d) override {
    return forward(false, g, p, d);
  }
  void b_transport(tlm::tlm_generic_payload &g, sc_core::sc_time &d) override {
    auto c = NativeGuard::blocking_allowed();
    if (!c)
      throw std::runtime_error(c.error().message);
    right->b_transport(g, d);
  }
  unsigned transport_dbg(tlm::tlm_generic_payload &g) override {
    NativeGuard guard;
    auto n = right->transport_dbg(g);
    auto c = guard.unchanged();
    if (!c)
      throw std::runtime_error(c.error().message);
    return n;
  }
  bool get_direct_mem_ptr(tlm::tlm_generic_payload &, tlm::tlm_dmi &d) override {
    d.init();
    return false;
  }
  void invalidate_direct_mem_ptr(sc_dt::uint64, sc_dt::uint64) override {}
};
} // namespace leanat::systemc
