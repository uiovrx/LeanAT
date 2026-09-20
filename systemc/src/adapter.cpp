#include <climits>
#include <cstring>
#include <leanat/systemc/adapter.hpp>
namespace leanat::systemc {
NativeByteOrder native_byte_order() noexcept {
  static_assert(CHAR_BIT == 8 && sizeof(std::uint32_t) == 4,
                "native profile requires 8-bit bytes and exact 32-bit integers");
  const std::uint32_t probe = 0x01020304u;
  unsigned char bytes[sizeof(probe)];
  std::memcpy(bytes, &probe, sizeof(probe));
  if (bytes[0] == 4 && bytes[1] == 3 && bytes[2] == 2 && bytes[3] == 1)
    return NativeByteOrder::Little;
  if (bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4)
    return NativeByteOrder::Big;
  return NativeByteOrder::Unknown;
}
Expected<void> validate_native_host(const NativeProfile &profile) {
  if (profile.byte_order != NativeByteOrder::Little || profile.bus_width != 32)
    return fail(ErrorCode::Unsupported,
                "native profile supports only little endian and BUSWIDTH 32");
  if (native_byte_order() != profile.byte_order)
    return fail(ErrorCode::Unsupported, "actual host byte order does not match native profile");
  return {};
}
Expected<void> validate_native_profile(const NativeProfile &profile,
                                       const tlm::tlm_base_socket_if &left,
                                       const tlm::tlm_base_socket_if &right) {
  auto host = validate_native_host(profile);
  if (!host)
    return host.error();
  if (left.get_bus_width() != profile.bus_width || right.get_bus_width() != profile.bus_width)
    return fail(ErrorCode::Unsupported, "actual socket BUSWIDTH does not match native profile");
  return {};
}
namespace {
std::map<const sc_core::sc_object *, unsigned> guards;
const sc_core::sc_object *process() {
  return sc_core::sc_get_current_process_handle().get_process_object();
}
ResponseStatus status(tlm::tlm_response_status s) {
  switch (s) {
  case tlm::TLM_OK_RESPONSE:
    return ResponseStatus::Ok;
  case tlm::TLM_INCOMPLETE_RESPONSE:
    return ResponseStatus::Incomplete;
  case tlm::TLM_ADDRESS_ERROR_RESPONSE:
    return ResponseStatus::AddressError;
  case tlm::TLM_COMMAND_ERROR_RESPONSE:
    return ResponseStatus::CommandError;
  case tlm::TLM_BURST_ERROR_RESPONSE:
    return ResponseStatus::BurstError;
  case tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE:
    return ResponseStatus::ByteEnableError;
  default:
    return ResponseStatus::GenericError;
  }
}
tlm::tlm_response_status status(ResponseStatus s) {
  switch (s) {
  case ResponseStatus::Ok:
    return tlm::TLM_OK_RESPONSE;
  case ResponseStatus::Incomplete:
    return tlm::TLM_INCOMPLETE_RESPONSE;
  case ResponseStatus::AddressError:
    return tlm::TLM_ADDRESS_ERROR_RESPONSE;
  case ResponseStatus::CommandError:
    return tlm::TLM_COMMAND_ERROR_RESPONSE;
  case ResponseStatus::BurstError:
    return tlm::TLM_BURST_ERROR_RESPONSE;
  case ResponseStatus::ByteEnableError:
    return tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE;
  default:
    return tlm::TLM_GENERIC_ERROR_RESPONSE;
  }
}
} // namespace
TimeCodec::TimeCodec(std::uint64_t u) : units_(u) {
  if (!u)
    throw std::invalid_argument("zero SystemC timebase");
}
Expected<Tick> TimeCodec::to_tick(const sc_core::sc_time &t) const {
  if (t.value() % units_)
    return fail(ErrorCode::InvalidArgument, "off-grid SystemC time");
  return Tick{t.value() / units_};
}
Expected<Duration> TimeCodec::to_duration(const sc_core::sc_time &t) const {
  auto n = to_tick(t);
  if (!n)
    return n.error();
  return Duration{n.value().value};
}
Expected<sc_core::sc_time> TimeCodec::from_tick(Tick t) const {
  auto n = checked_mul(t.value, units_);
  if (!n)
    return n.error();
  return sc_core::sc_time::from_value(n.value());
}
Expected<sc_core::sc_time> TimeCodec::from_duration(Duration t) const {
  return from_tick(Tick{t.value});
}
Expected<Tick> TimeCodec::effective_time(const sc_core::sc_time &n,
                                         const sc_core::sc_time &d) const {
  auto a = to_tick(n);
  if (!a)
    return a.error();
  auto b = to_duration(d);
  if (!b)
    return b.error();
  return add_time(a.value(), b.value());
}
Expected<sc_core::sc_time> TimeCodec::wake_delay(Tick t, const sc_core::sc_time &n) const {
  auto a = to_tick(n);
  if (!a)
    return a.error();
  if (t < a.value())
    return fail(ErrorCode::TimeRegression, "past wake deadline");
  return from_duration(Duration{t.value - a.value().value});
}
NativeGuard::NativeGuard()
    : time_(sc_core::sc_time_stamp()), delta_(sc_core::sc_delta_count()), process_(process()) {
  ++guards[process_];
}
NativeGuard::~NativeGuard() {
  if (!--guards[process_])
    guards.erase(process_);
}
bool NativeGuard::active() {
  auto i = guards.find(process());
  return i != guards.end() && i->second;
}
Expected<void> NativeGuard::unchanged() const {
  if (sc_core::sc_time_stamp() != time_ || sc_core::sc_delta_count() != delta_)
    return fail(ErrorCode::ProtocolViolation, "native untimed callback waited");
  return {};
}
Expected<void> NativeGuard::blocking_allowed() {
  auto p = sc_core::sc_get_current_process_handle();
  if (!p.valid() || p.proc_kind() != sc_core::SC_THREAD_PROC_ || active())
    return fail(ErrorCode::InvalidState, "blocking requires unguarded SC_THREAD");
  return {};
}
TransportPin::TransportPin(tlm::tlm_generic_payload &g) : gp_(&g) {
  if (!g.has_mm())
    throw std::invalid_argument("TransportPin requires MM");
  g.acquire();
}
TransportPin::TransportPin(TransportPin &&p) noexcept : gp_(std::exchange(p.gp_, nullptr)) {}
TransportPin &TransportPin::operator=(TransportPin &&p) noexcept {
  if (this != &p) {
    if (gp_)
      gp_->release();
    gp_ = std::exchange(p.gp_, nullptr);
  }
  return *this;
}
TransportPin::~TransportPin() {
  if (gp_)
    gp_->release();
}
Expected<TransportPin> PayloadBridge::pin_nb(tlm::tlm_generic_payload &g) const {
  if (!g.has_mm())
    return fail(ErrorCode::InvalidArgument, "nb payload has no MM");
  return TransportPin(g);
}
Expected<PayloadSnapshot> PayloadBridge::snapshot(tlm::tlm_generic_payload &g) const {
  if (g.get_data_length() > max_data_ ||
      (g.get_byte_enable_ptr() && g.get_byte_enable_length() > max_mask_))
    return fail(ErrorCode::Capacity, "payload limit");
  if ((g.get_data_length() && !g.get_data_ptr()))
    return fail(ErrorCode::InvalidArgument, "null payload buffer");
  PayloadSnapshot p;
  p.command = g.is_read() ? Command::Read : g.is_write() ? Command::Write : Command::Ignore;
  p.address = g.get_address();
  p.streaming_width = g.get_streaming_width();
  p.status = status(g.get_response_status());
  p.dmi_hint = g.is_dmi_allowed();
  if (g.get_data_length())
    p.data.assign(g.get_data_ptr(), g.get_data_ptr() + g.get_data_length());
  if (g.get_byte_enable_ptr() && g.get_byte_enable_length())
    p.byte_enable.assign(g.get_byte_enable_ptr(),
                         g.get_byte_enable_ptr() + g.get_byte_enable_length());
  return p;
}
Expected<ResponseSnapshot> PayloadBridge::response(tlm::tlm_generic_payload &g) const {
  ResponseSnapshot r;
  r.status = status(g.get_response_status());
  if (r.status == ResponseStatus::Incomplete)
    return fail(ErrorCode::NotReady, "response incomplete");
  r.dmi_hint = g.is_dmi_allowed();
  if (g.is_read()) {
    auto length = g.get_data_length();
    if (length > max_data_)
      return fail(ErrorCode::Capacity, "response data bound");
    if (length && !g.get_data_ptr())
      return fail(ErrorCode::InvalidArgument, "null response data");
    if (length)
      r.data.assign(g.get_data_ptr(), g.get_data_ptr() + length);
  }
  return r;
}
Expected<PayloadSnapshot> PayloadBridge::snapshot_call(tlm::tlm_generic_payload &g,
                                                       PhaseId p) const {
  if (p == begin_req)
    return snapshot(g);
  if (p == end_req || p == end_resp)
    return PayloadSnapshot{};
  if (p != begin_resp)
    return fail(ErrorCode::Unsupported, "unknown payload stage");
  auto r = response(g);
  if (!r)
    return r.error();
  PayloadSnapshot out;
  out.status = r.value().status;
  out.data = std::move(r.value().data);
  out.dmi_hint = r.value().dmi_hint;
  return out;
}
Expected<void> PayloadBridge::write_response(tlm::tlm_generic_payload &g,
                                             const ResponseSnapshot &r) const {
  if (r.status == ResponseStatus::Incomplete)
    return fail(ErrorCode::NotReady, "response incomplete");
  if (!r.extensions.empty())
    return fail(ErrorCode::Unsupported, "extension codec not registered");
  if (g.is_read() &&
      (r.data.size() != g.get_data_length() || (g.get_data_length() && !g.get_data_ptr())))
    return fail(ErrorCode::InvalidArgument, "response buffer size");
  if (g.is_read())
    for (std::size_t i = 0; i < r.data.size(); ++i)
      if (!g.get_byte_enable_ptr() || !g.get_byte_enable_length() ||
          g.get_byte_enable_ptr()[i % g.get_byte_enable_length()])
        g.get_data_ptr()[i] = r.data[i];
  g.set_response_status(status(r.status));
  g.set_dmi_allowed(r.dmi_hint);
  return {};
}
Expected<PhaseId> PhaseCodec::decode(const tlm::tlm_phase &p) {
  if (p == tlm::BEGIN_REQ)
    return begin_req;
  if (p == tlm::END_REQ)
    return end_req;
  if (p == tlm::BEGIN_RESP)
    return begin_resp;
  if (p == tlm::END_RESP)
    return end_resp;
  return fail(ErrorCode::Unsupported, "unregistered phase");
}
Expected<tlm::tlm_phase> PhaseCodec::encode(PhaseId p) {
  if (p == begin_req)
    return tlm::tlm_phase(tlm::BEGIN_REQ);
  if (p == end_req)
    return tlm::tlm_phase(tlm::END_REQ);
  if (p == begin_resp)
    return tlm::tlm_phase(tlm::BEGIN_RESP);
  if (p == end_resp)
    return tlm::tlm_phase(tlm::END_RESP);
  return fail(ErrorCode::Unsupported, "unregistered phase");
}
RuntimeDomain::RuntimeDomain(sc_core::sc_module_name n, DomainId d, TimeCodec t, std::size_t c,
                             std::size_t b)
    : sc_module(n), capacity_(c), budget_(b), id(d), time(t) {
  if (!c || !b)
    throw std::invalid_argument("zero domain capacity/budget");
  SC_METHOD(dispatch);
  sensitive << wake_;
  dont_initialize();
}
Expected<void> RuntimeDomain::start() {
  if (stopped_)
    return fail(ErrorCode::InvalidState, "domain stopped");
  if (started_)
    return fail(ErrorCode::Duplicate, "domain already started");
  started_ = true;
  rearm();
  return {};
}
void RuntimeDomain::stop() {
  stopped_ = true;
  armed_.reset();
  wake_.cancel();
}
CallId RuntimeDomain::next_call() {
  if (calls_ == UINT64_MAX)
    throw std::overflow_error("call identity exhausted");
  return CallId{++calls_};
}
Expected<void> RuntimeDomain::schedule(Tick t, std::function<void()> f) {
  if (stopped_)
    return fail(ErrorCode::InvalidState, "domain stopped");
  auto dt = time.wake_delay(t, sc_core::sc_time_stamp());
  if (!dt)
    return dt.error();
  if (work_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "domain event capacity");
  if (ordinal_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "event ordinal exhausted");
  work_.push_back({t, ++ordinal_, std::move(f)});
  if (started_)
    rearm();
  return {};
}
void RuntimeDomain::rearm() {
  wake_.cancel();
  armed_.reset();
  if (!started_ || stopped_ || work_.empty())
    return;
  auto i = std::min_element(work_.begin(), work_.end(), [](auto &a, auto &b) {
    return std::tie(a.time, a.ordinal) < std::tie(b.time, b.ordinal);
  });
  armed_ = i->time;
  auto d = time.wake_delay(*armed_, sc_core::sc_time_stamp());
  if (!d) {
    stop();
    SC_REPORT_ERROR("LeanAT", d.error().message.c_str());
    return;
  }
  wake_.notify(d.value());
}
void RuntimeDomain::dispatch() {
  if (stopped_ || !started_ || !armed_)
    return;
  auto now = time.to_tick(sc_core::sc_time_stamp());
  if (!now) {
    stop();
    return;
  }
  if (now.value() < *armed_)
    return;
  std::vector<Work> ready;
  std::stable_sort(work_.begin(), work_.end(), [](auto &a, auto &b) {
    return std::tie(a.time, a.ordinal) < std::tie(b.time, b.ordinal);
  });
  while (!work_.empty() && work_.front().time == *armed_ && ready.size() < budget_) {
    ready.push_back(std::move(work_.front()));
    work_.erase(work_.begin());
  }
  for (auto &w : ready) {
    if (stopped_)
      break;
    try {
      w.action();
    } catch (...) {
      stop();
      throw;
    }
  }
  rearm();
}
NbBridge::NbBridge(RuntimeDomain &d, InstanceId s, ConnectionId c, std::size_t n)
    : domain_(d), engine_(d.id, s, n, n, 1024), connection_(c), capacity_(n) {}
Expected<void> NbBridge::register_ignorable(std::string key, PhaseId id,
                                            const tlm::tlm_phase &native) {
  auto registered = phases_.register_phase(std::move(key), id, native, true);
  if (!registered)
    return registered.error();
  return engine_.allow_ignorable(id);
}
Expected<WireReturn> NbBridge::exchange(Flow flow, tlm::tlm_generic_payload &g, tlm::tlm_phase &p,
                                        sc_core::sc_time &delay, const NativeCall &fn) {
  if (domain_.stopped())
    return fail(ErrorCode::InvalidState, "stopped domain");
  NativeGuard guard;
  phases_.freeze();
  auto phase = phases_.decode(p);
  if (!phase)
    return phase.error();
  auto callpin = payload_.pin_nb(g);
  if (!callpin)
    return callpin.error();
  if (phases_.is_ignorable(phase.value())) {
    auto now = domain_.time.to_tick(sc_core::sc_time_stamp());
    if (!now)
      return now.error();
    auto incoming = domain_.time.to_duration(delay);
    if (!incoming)
      return incoming.error();
    if (ignored_calls_ == UINT64_MAX)
      return fail(ErrorCode::Overflow, "ignored call count exhausted");
    auto live = live_.find(&g);
    TransportId transport = live == live_.end() ? TransportId{} : live->second.transport;
    last_ignored_ = WireCall{domain_.next_call(), connection_, transport,        flow,
                             phase.value(),       now.value(), incoming.value(), {}};
    ++ignored_calls_;
    return WireReturn{};
  }
  auto snap = payload_.snapshot_call(g, phase.value());
  if (!snap)
    return snap.error();
  auto now = domain_.time.to_tick(sc_core::sc_time_stamp());
  if (!now)
    return now.error();
  auto in = domain_.time.to_duration(delay);
  if (!in)
    return in.error();
  auto i = live_.find(&g);
  if (i == live_.end()) {
    if (phase.value() != begin_req)
      return fail(ErrorCode::StaleHandle, "phase for unknown GP");
    if (live_.size() >= capacity_)
      return fail(ErrorCode::Capacity, "transport capacity");
    auto ledger = engine_.create_ledger(connection_, TransportId{next_}, next_);
    if (!ledger)
      return ledger.error();
    auto pin = payload_.pin_nb(g);
    if (!pin)
      return pin.error();
    i = live_.emplace(&g, Live{TransportId{next_}, next_, std::move(pin.value()), ledger.value()})
            .first;
    ++next_;
  }
  WireCall call{domain_.next_call(), connection_, i->second.transport, flow,
                phase.value(),       now.value(), in.value(),          snap.value()};
  auto ticket = engine_.begin_call(i->second.ledger, call);
  if (!ticket)
    return ticket.error();
  auto original_phase = p;
  auto original_delay = delay;
  auto data_ptr = phase.value() == begin_req ? g.get_data_ptr() : nullptr;
  auto mask_ptr = phase.value() == begin_req ? g.get_byte_enable_ptr() : nullptr;
  auto mask_length = phase.value() == begin_req ? g.get_byte_enable_length() : 0;
  if (current_calls_.size() >= capacity_) {
    engine_.fail_call(ticket.value(), fail(ErrorCode::Capacity, "native call stack capacity"));
    return fail(ErrorCode::Capacity, "native call stack capacity");
  }
  current_calls_.push_back(std::make_unique<WireCall>(call));
  tlm::tlm_sync_enum sync;
  try {
    sync = fn(g, p, delay);
    current_calls_.pop_back();
  } catch (...) {
    current_calls_.pop_back();
    engine_.fail_call(ticket.value(),
                      fail(ErrorCode::ExternalFailure, "native callback exception"));
    domain_.stop();
    throw;
  }
  if (phase.value() == begin_req &&
      (g.get_command() != (snap.value().command == Command::Read    ? tlm::TLM_READ_COMMAND
                           : snap.value().command == Command::Write ? tlm::TLM_WRITE_COMMAND
                                                                    : tlm::TLM_IGNORE_COMMAND) ||
       g.get_data_ptr() != data_ptr || g.get_byte_enable_ptr() != mask_ptr ||
       g.get_data_length() != snap.value().data.size() ||
       g.get_byte_enable_length() != mask_length ||
       g.get_streaming_width() != snap.value().streaming_width)) {
    auto e = fail(ErrorCode::ProtocolViolation, "native peer changed frozen request fields");
    engine_.fail_call(ticket.value(), e);
    domain_.stop();
    return e;
  }
  auto checked = guard.unchanged();
  if (!checked) {
    engine_.fail_call(ticket.value(), checked.error());
    domain_.stop();
    return checked.error();
  }
  WireReturn result;
  if (sync == tlm::TLM_ACCEPTED) {
    result.sync = Sync::Accepted;
    p = original_phase;
    delay = original_delay;
  } else if (sync == tlm::TLM_UPDATED || sync == tlm::TLM_COMPLETED) {
    result.sync = sync == tlm::TLM_UPDATED ? Sync::Updated : Sync::Completed;
    auto out = domain_.time.to_duration(delay);
    if (!out) {
      engine_.fail_call(ticket.value(), out.error());
      domain_.stop();
      return out.error();
    }
    result.outgoing_delay = out.value();
    if (sync == tlm::TLM_UPDATED) {
      auto rp = phases_.decode(p);
      if (!rp) {
        engine_.fail_call(ticket.value(), rp.error());
        domain_.stop();
        return rp.error();
      }
      result.phase = rp.value();
    }
    if (phase.value() == begin_req && (sync == tlm::TLM_COMPLETED || result.phase == begin_resp)) {
      auto r = payload_.response(g);
      if (!r) {
        engine_.fail_call(ticket.value(), r.error());
        domain_.stop();
        return r.error();
      }
      result.response = r.value();
    }
  } else {
    engine_.fail_call(ticket.value(), fail(ErrorCode::ProtocolViolation, "invalid sync"));
    domain_.stop();
    return fail(ErrorCode::ProtocolViolation, "invalid sync");
  }
  auto done = engine_.end_call(ticket.value(), result);
  if (!done) {
    domain_.stop();
    return done.error();
  }
  if (done.value().wire_terminal) {
    auto h = i->second.ledger;
    live_.erase(i);
    auto retired = engine_.retire_ledger(h);
    if (!retired)
      return retired.error();
  }
  return result;
}
Expected<tlm::tlm_sync_enum> NbBridge::receive(Flow flow, tlm::tlm_generic_payload &g,
                                               tlm::tlm_phase &p, sc_core::sc_time &d,
                                               const Handler &handler) {
  auto r = exchange(flow, g, p, d, [&](auto &gp, auto &phase, auto &delay) {
    auto ret = handler(*current_calls_.back());
    if (!ret) {
      throw std::runtime_error(ret.error().message);
    }
    if (ret.value().sync == Sync::Accepted)
      return tlm::TLM_ACCEPTED;
    if (ret.value().response) {
      auto w = payload_.write_response(gp, *ret.value().response);
      if (!w)
        throw std::runtime_error(w.error().message);
    }
    auto out = domain_.time.from_duration(ret.value().outgoing_delay);
    if (!out)
      throw std::runtime_error(out.error().message);
    delay = out.value();
    if (ret.value().sync == Sync::Updated) {
      if (!ret.value().phase)
        throw std::runtime_error("missing updated phase");
      auto e = phases_.encode(*ret.value().phase);
      if (!e)
        throw std::runtime_error(e.error().message);
      phase = e.value();
      return tlm::TLM_UPDATED;
    }
    return tlm::TLM_COMPLETED;
  });
  if (!r)
    return r.error();
  return r.value().sync == Sync::Accepted  ? tlm::TLM_ACCEPTED
         : r.value().sync == Sync::Updated ? tlm::TLM_UPDATED
                                           : tlm::TLM_COMPLETED;
}
Expected<void> BlockingBridge::transport(tlm::tlm_generic_payload &gp, sc_core::sc_time &delay,
                                         Duration service, const Service &fn) {
  auto context = NativeGuard::blocking_allowed();
  if (!context)
    return context.error();
  if (active_ >= capacity_)
    return fail(ErrorCode::Capacity, "blocking capacity");
  for (unsigned i = 0; i < tlm::max_num_extensions(); ++i)
    if (gp.get_extension(i))
      return fail(ErrorCode::Unsupported, "blocking extensions require registered clone policy");
  auto request = payload_.snapshot(gp);
  if (!request)
    return request.error();
  auto clone = payload_.make_managed(request.value());
  if (!clone)
    return clone.error();
  auto owned = std::make_shared<TransportPin>(std::move(clone.value()));
  auto arrival = domain_.time.effective_time(sc_core::sc_time_stamp(), delay);
  if (!arrival)
    return arrival.error();
  auto done = add_time(arrival.value(), service);
  if (!done)
    return done.error();
  struct Latch {
    sc_core::sc_event wake;
    bool ready{};
    std::optional<Expected<ResponseSnapshot>> result;
  };
  auto latch = std::make_shared<Latch>();
  ++active_;
  auto scheduled = domain_.schedule(done.value(), [latch, owned, request = request.value(), fn] {
    latch->result = fn(request);
    latch->ready = true;
    latch->wake.notify(sc_core::SC_ZERO_TIME);
  });
  if (!scheduled) {
    --active_;
    return scheduled.error();
  }
  try {
    while (!latch->ready)
      sc_core::wait(latch->wake);
  } catch (...) {
    domain_.stop(); // The fixed service bridge cannot resume after its caller is unwound.
    --active_;
    throw;
  }
  --active_;
  if (!*latch->result)
    return latch->result->error();
  auto w = payload_.write_response(gp, latch->result->value());
  if (!w)
    return w.error();
  delay = sc_core::SC_ZERO_TIME;
  return {};
}
Expected<unsigned>
BlockingBridge::debug(tlm::tlm_generic_payload &g, std::size_t budget,
                      const std::function<bool(std::uint64_t, std::uint8_t &, bool)> &fn) {
  NativeGuard guard;
  if (g.get_data_length() && !g.get_data_ptr())
    return fail(ErrorCode::InvalidArgument, "null debug data");
  if (!g.is_read() && !g.is_write())
    return 0u;
  unsigned n = 0;
  while (n < g.get_data_length() && n < budget) {
    auto address = checked_add(g.get_address(), n);
    if (!address || !fn(address.value(), g.get_data_ptr()[n], g.is_write()))
      break;
    ++n;
  }
  auto check = guard.unchanged();
  if (!check)
    return check.error();
  return n;
}
RawDmiHost::RawDmiHost(std::shared_ptr<Bytes> b, std::uint64_t base, TimeCodec t, Duration r,
                       Duration w, bool strict, std::size_t c)
    : bytes_(std::move(b)), base_(base), capacity_(c), time_(t), read_(r), write_(w) {
  if (strict)
    throw std::invalid_argument("raw DMI incompatible with timing-strict");
  if (!bytes_ || bytes_->empty() || !checked_add(base_, bytes_->size() - 1))
    throw std::invalid_argument("invalid DMI backing");
}
Expected<bool> RawDmiHost::get(tlm::tlm_generic_payload &g, tlm::tlm_dmi &d,
                               std::function<void(std::uint64_t, std::uint64_t)> f) {
  d.init();
  d.set_start_address(g.get_address());
  d.set_end_address(g.get_address());
  if (NativeGuard::active())
    return fail(ErrorCode::InvalidState, "nested DMI query in guarded callback");
  NativeGuard guard;
  if (invalidating_)
    return fail(ErrorCode::InvalidState, "DMI invalidation reentry");
  if (g.get_address() < base_ || g.get_address() - base_ >= bytes_->size() ||
      grants_.size() >= capacity_ || (!g.is_read() && !g.is_write()))
    return false;
  auto r = time_.from_duration(read_), w = time_.from_duration(write_);
  if (!r)
    return r.error();
  if (!w)
    return w.error();
  auto end = base_ + bytes_->size() - 1;
  grants_.push_back({base_, end, std::move(f)});
  d.set_dmi_ptr(bytes_->data());
  d.set_start_address(base_);
  d.set_end_address(end);
  d.allow_read_write();
  d.set_read_latency(r.value());
  d.set_write_latency(w.value());
  return true;
}
Expected<void> RawDmiHost::invalidate(std::uint64_t start, std::uint64_t end) {
  if (start > end)
    return fail(ErrorCode::InvalidArgument, "invalid invalidation range");
  if (invalidating_)
    return fail(ErrorCode::InvalidState, "recursive invalidation");
  NativeGuard guard;
  invalidating_ = true;
  try {
    for (auto &g : grants_)
      if (g.start <= end && start <= g.end)
        g.invalidate(g.start, g.end);
  } catch (...) {
    invalidating_ = false;
    return fail(ErrorCode::ExternalFailure, "invalidation callback failed; backing retained");
  }
  auto check = guard.unchanged();
  if (!check) {
    invalidating_ = false;
    return check.error();
  }
  grants_.erase(std::remove_if(grants_.begin(), grants_.end(),
                               [&](auto &g) { return g.start <= end && start <= g.end; }),
                grants_.end());
  invalidating_ = false;
  return {};
}
Expected<void> RawDmiHost::replace(std::shared_ptr<Bytes> b) {
  if (!b || b->empty() || !checked_add(base_, b->size() - 1))
    return fail(ErrorCode::InvalidArgument, "invalid backing");
  if (generation_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "DMI generation exhausted");
  auto i = invalidate(0, UINT64_MAX);
  if (!i)
    return i.error();
  bytes_ = std::move(b);
  ++generation_;
  return {};
}
Expected<WireCall> ProtocolAdapter::map_call(const WireCall &c, const std::string &s,
                                             const std::string &d) {
  if (s != "tlm.base" || d != "tlm.base")
    return fail(ErrorCode::Unsupported,
                "nonidentity protocol adapter requires checked state/lane/guard mapping");
  return c;
}
} // namespace leanat::systemc
namespace leanat::systemc {
EventPump::EventPump(sc_core::sc_module_name n, Runtime &r, TimeCodec c, std::uint32_t b)
    : sc_module(n), runtime_(r), codec_(c), budget_(b) {
  if (!b)
    throw std::invalid_argument("zero pump budget");
  SC_METHOD(dispatch);
  sensitive << wake_;
  dont_initialize();
}
Expected<void> EventPump::arm(std::optional<WakePoint> w) {
  wake_.cancel();
  armed_.reset();
  if (runtime_.stopped() || !w)
    return {};
  auto d = codec_.wake_delay(w->time, sc_core::sc_time_stamp());
  if (!d)
    return d.error();
  armed_ = w;
  wake_.notify(d.value());
  return {};
}
void EventPump::dispatch() {
  if (!armed_ || runtime_.stopped())
    return;
  auto now = codec_.to_tick(sc_core::sc_time_stamp());
  if (!now) {
    SC_REPORT_ERROR("LeanAT", now.error().message.c_str());
    return;
  }
  if (now.value() < armed_->time)
    return;
  armed_.reset();
  auto p = runtime_.pump_batch(now.value(), budget_);
  if (!p) {
    wake_.cancel();
    SC_REPORT_ERROR("LeanAT", p.error().message.c_str());
    return;
  }
  auto a = arm(p.value().progress == ProgressDisposition::ContinueDispatch ? p.value().next_wake
                                                                           : std::nullopt);
  if (!a)
    SC_REPORT_ERROR("LeanAT", a.error().message.c_str());
}
} // namespace leanat::systemc

namespace leanat::systemc {
RuntimeHostAdapter::RuntimeHostAdapter(sc_core::sc_module_name n, const RuntimeConfig &c,
                                       TimeCodec t, std::uint32_t b)
    : codec_(t), registry_capacity_(c.ledger_capacity), native_capacity_(c.nesting_capacity),
      call_capacity_(c.call_capacity), runtime(c, *this), pump(n, runtime, t, b) {
  auto profile = validate_native_host(NativeProfile{});
  if (!profile)
    throw std::invalid_argument(profile.error().message);
}
Expected<void>
RuntimeHostAdapter::bind(ConnectionId c,
                         std::function<Expected<tlm::tlm_generic_payload *>(const SendIntent &)> r,
                         NbBridge::NativeCall f) {
  if (!r || !f)
    return fail(ErrorCode::InvalidArgument, "empty native binding");
  if (bindings_.count(c))
    return fail(ErrorCode::Duplicate, "duplicate native binding");
  bindings_.emplace(c, Binding{std::move(r), std::move(f)});
  return {};
}
void RuntimeHostAdapter::arm(std::optional<WakePoint> w) {
  auto a = pump.arm(w);
  if (!a)
    SC_REPORT_ERROR("LeanAT", a.error().message.c_str());
}
void RuntimeHostAdapter::publish_output(PortId p, const Value &v) {
  if (output)
    output(p, v);
}
void RuntimeHostAdapter::emit_trace(const TraceEvent &t) {
  if (trace)
    trace(t);
}
Expected<WireReturn> RuntimeHostAdapter::transport(const SendIntent &i) {
  if (native_calls_.size() >= native_capacity_)
    return fail(ErrorCode::Capacity, "native host call stack capacity");
  if (pending_returns_.size() >= call_capacity_ || pending_returns_.count(i.call_id))
    return fail(ErrorCode::Capacity, "native pending return capacity/duplicate");
  if (i.transport.value >= next_transport_) {
    if (i.transport.value == UINT64_MAX)
      return fail(ErrorCode::Overflow, "transport identity exhausted");
    next_transport_ = i.transport.value + 1;
  }
  auto b = bindings_.find(i.connection);
  if (b == bindings_.end())
    return fail(ErrorCode::InvalidArgument, "unbound native connection");
  auto gp = b->second.resolve(i);
  if (!gp)
    return gp.error();
  if (!gp.value())
    return fail(ErrorCode::InvalidArgument, "null native payload");
  auto &g = *gp.value();
  auto pin = payload_.pin_nb(g);
  if (!pin)
    return pin.error();
  auto phase = PhaseCodec::encode(i.phase);
  if (!phase)
    return phase.error();
  auto now = codec_.to_tick(sc_core::sc_time_stamp());
  if (!now)
    return now.error();
  if (now.value() < i.not_before)
    return fail(ErrorCode::NotReady, "outbound intent not yet effective");
  if (i.phase == begin_resp) {
    auto wr =
        payload_.write_response(g, ResponseSnapshot{i.payload.status, i.payload.data,
                                                    i.payload.dmi_hint, i.payload.extensions});
    if (!wr)
      return wr.error();
  }
  Expected<PayloadSnapshot> snap =
      i.phase == begin_req ? payload_.snapshot(g) : Expected<PayloadSnapshot>{i.payload};
  if (!snap)
    return snap.error();
  if (i.phase == begin_resp) {
    auto actual = payload_.response(g);
    if (!actual)
      return actual.error();
    snap.value().status = actual.value().status;
    if (g.is_read())
      snap.value().data = actual.value().data;
    snap.value().dmi_hint = actual.value().dmi_hint;
  }
  WireCall call{i.call_id, i.connection, i.transport, i.flow,
                i.phase,   now.value(),  Duration{},  snap.value()};
  bool registered = false;
  if (i.phase == begin_req) {
    auto live = inbound_.find(NativeKey{&g, i.connection});
    if (live != inbound_.end() &&
        (live->second.id != i.transport || live->second.connection != i.connection))
      return fail(ErrorCode::WrongOwner, "GP already belongs to another host transport");
    if (live == inbound_.end()) {
      if (inbound_.size() >= registry_capacity_)
        return fail(ErrorCode::Capacity, "native transport registry full");
      auto held = payload_.pin_nb(g);
      if (!held)
        return held.error();
      inbound_.emplace(NativeKey{&g, i.connection},
                       Inbound{i.transport, i.connection, Handle{}, std::move(held.value())});
      registered = true;
    }
  }
  pending_returns_.emplace(i.call_id, NativeKey{&g, i.connection});
  auto start = runtime.start_outbound(i, call);
  if (!start) {
    pending_returns_.erase(i.call_id);
    if (registered)
      inbound_.erase(NativeKey{&g, i.connection});
    return start.error();
  }
  NativeGuard guard;
  sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
  native_calls_.push_back(std::make_unique<ActiveCall>(ActiveCall{call, &g, process()}));
  tlm::tlm_sync_enum result;
  try {
    result = b->second.call(g, phase.value(), delay);
  } catch (...) {
    native_calls_.pop_back();
    throw;
  }
  native_calls_.pop_back();
  auto no_wait = guard.unchanged();
  if (!no_wait)
    return no_wait.error();
  WireReturn ret;
  if (result == tlm::TLM_ACCEPTED) {
    return ret;
  }
  if (result != tlm::TLM_UPDATED && result != tlm::TLM_COMPLETED)
    return fail(ErrorCode::ProtocolViolation, "invalid native sync");
  ret.sync = result == tlm::TLM_UPDATED ? Sync::Updated : Sync::Completed;
  auto d = codec_.to_duration(delay);
  if (!d)
    return d.error();
  ret.outgoing_delay = d.value();
  if (result == tlm::TLM_UPDATED) {
    auto p = PhaseCodec::decode(phase.value());
    if (!p)
      return p.error();
    ret.phase = p.value();
  }
  if (i.phase == begin_req && (ret.sync == Sync::Completed || ret.phase == begin_resp)) {
    auto r = payload_.response(g);
    if (!r)
      return r.error();
    ret.response = r.value();
  }
  return ret;
}
} // namespace leanat::systemc

namespace leanat::systemc {
Expected<CheckedAdapter> CheckedAdapter::validate(const AdapterContract &l,
                                                  const AdapterContract &r,
                                                  const std::map<PhaseId, PhaseId> &mapping) {
  if (l.phases.empty() || l.phases.size() > 256 || l.rules.empty() || l.rules.size() > 4096 ||
      l.phases.size() != r.phases.size() || l.rules.size() != r.rules.size() ||
      mapping.size() != l.phases.size())
    return fail(ErrorCode::Unsupported, "incomplete or oversized adapter contract");
  CheckedAdapter a;
  a.forward_ = mapping;
  std::set<PhaseId> lp(l.phases.begin(), l.phases.end()), rp(r.phases.begin(), r.phases.end());
  if (lp.size() != l.phases.size() || rp.size() != r.phases.size())
    return fail(ErrorCode::Duplicate, "duplicate contract phase");
  for (auto &m : mapping) {
    if (!lp.count(m.first) || !rp.count(m.second) || a.reverse_.count(m.second))
      return fail(ErrorCode::InvalidArgument, "phase map not bijective");
    a.reverse_[m.second] = m.first;
  }
  std::vector<bool> used(r.rules.size());
  for (auto &x : l.rules) {
    if (!lp.count(x.phase) || (x.returned && !lp.count(*x.returned)))
      return fail(ErrorCode::InvalidArgument, "rule phase absent");
    bool matched = false;
    for (std::size_t i = 0; i < r.rules.size(); ++i) {
      auto &y = r.rules[i];
      if (used[i] || x.before != y.before || x.after != y.after || x.flow != y.flow ||
          a.forward_.at(x.phase) != y.phase || x.sync != y.sync || x.guard != y.guard ||
          x.request_lanes != y.request_lanes || x.response_lanes != y.response_lanes ||
          x.actions != y.actions || bool(x.returned) != bool(y.returned))
        continue;
      if (x.returned && a.forward_.at(*x.returned) != *y.returned)
        continue;
      used[i] = true;
      matched = true;
      break;
    }
    if (!matched)
      return fail(ErrorCode::Unsupported,
                  "protocol state/guard/lane/lifecycle relation is not isomorphic");
  }
  return a;
}
Expected<PhaseId> CheckedAdapter::phase(PhaseId p, AdapterDirection d) const {
  auto &m = d == AdapterDirection::LeftToRight ? forward_ : reverse_;
  auto i = m.find(p);
  if (i == m.end())
    return fail(ErrorCode::Unsupported, "unmapped phase");
  return i->second;
}
Expected<WireCall> CheckedAdapter::call(const WireCall &c, AdapterDirection d) const {
  auto p = phase(c.phase, d);
  if (!p)
    return p.error();
  auto out = c;
  out.phase = p.value();
  return out;
}
Expected<WireReturn> CheckedAdapter::result(const WireReturn &r, AdapterDirection d) const {
  auto out = r;
  if (r.sync == Sync::Accepted) {
    out.phase.reset();
    out.outgoing_delay = Duration{};
    out.response.reset();
    return out;
  }
  if (r.sync == Sync::Updated) {
    if (!r.phase)
      return fail(ErrorCode::ProtocolViolation, "updated without phase");
    auto p = phase(*r.phase, d == AdapterDirection::LeftToRight ? AdapterDirection::RightToLeft
                                                                : AdapterDirection::LeftToRight);
    if (!p)
      return p.error();
    out.phase = p.value();
  } else
    out.phase.reset();
  return out;
}
} // namespace leanat::systemc
namespace leanat::systemc {
namespace {
struct ManagedNative : tlm::tlm_mm_interface {
  Bytes data, mask;
  tlm::tlm_generic_payload gp;
  ManagedNative(const PayloadSnapshot &p) : data(p.data), mask(p.byte_enable), gp(this) {
    gp.set_command(p.command == Command::Read    ? tlm::TLM_READ_COMMAND
                   : p.command == Command::Write ? tlm::TLM_WRITE_COMMAND
                                                 : tlm::TLM_IGNORE_COMMAND);
    gp.set_address(p.address);
    gp.set_data_ptr(data.empty() ? nullptr : data.data());
    gp.set_data_length(static_cast<unsigned>(data.size()));
    gp.set_byte_enable_ptr(mask.empty() ? nullptr : mask.data());
    gp.set_byte_enable_length(static_cast<unsigned>(mask.size()));
    gp.set_streaming_width(static_cast<unsigned>(p.streaming_width));
    gp.set_response_status(status(p.status));
    gp.set_dmi_allowed(p.dmi_hint);
  }
  void free(tlm::tlm_generic_payload *) override {
    delete this;
  }
};
} // namespace
Expected<TransportPin> PayloadBridge::make_managed(const PayloadSnapshot &p) const {
  if (p.data.size() > max_data_ || p.byte_enable.size() > max_mask_ || p.data.size() > UINT_MAX ||
      p.byte_enable.size() > UINT_MAX || p.streaming_width > UINT_MAX)
    return fail(ErrorCode::Capacity, "managed payload size");
  if (!p.extensions.empty())
    return fail(ErrorCode::Unsupported, "managed extension codec absent");
  auto *n = new ManagedNative(p);
  return TransportPin(n->gp);
}
} // namespace leanat::systemc

namespace leanat::systemc {
Expected<tlm::tlm_generic_payload *> RuntimeHostAdapter::resolve_transport(TransportId id) {
  for (auto &i : inbound_)
    if (i.second.id == id)
      return i.first.first;
  return fail(ErrorCode::StaleHandle, "unknown host transport");
}
Expected<tlm::tlm_sync_enum> RuntimeHostAdapter::receive(ConnectionId connection, Flow flow,
                                                         tlm::tlm_generic_payload &gp,
                                                         tlm::tlm_phase &phase,
                                                         sc_core::sc_time &delay) {
  NativeGuard guard;
  auto p = PhaseCodec::decode(phase);
  if (!p)
    return p.error();
  auto pin = payload_.pin_nb(gp);
  if (!pin)
    return pin.error();
  auto snapshot = payload_.snapshot_call(gp, p.value());
  if (!snapshot)
    return snapshot.error();
  auto now = codec_.to_tick(sc_core::sc_time_stamp());
  if (!now)
    return now.error();
  auto dt = codec_.to_duration(delay);
  if (!dt)
    return dt.error();
  auto side = runtime.call_side(connection, flow, false);
  if (!side)
    return side.error();
  auto engine = runtime.protocol(side.value());
  if (!engine)
    return engine.error();
  auto i = inbound_.find(NativeKey{&gp, connection});
  const ActiveCall *active = native_calls_.empty() ? nullptr : native_calls_.back().get();
  const bool controlled = active && active->gp == &gp && active->process == process() &&
                          active->call.connection == connection && active->call.flow == flow;
  bool created = false;
  if (i == inbound_.end()) {
    if (p.value() != begin_req)
      return fail(ErrorCode::StaleHandle, "unknown ingress transport");
    if (inbound_.size() >= registry_capacity_ || next_transport_ == UINT64_MAX)
      return fail(ErrorCode::Capacity, "ingress transport capacity");
    TransportId id{};
    if (controlled)
      id = active->call.transport;
    else
      for (const auto &existing : inbound_)
        if (existing.first.first == &gp) {
          id = existing.second.id;
          break;
        }
    if (!id.value)
      id = TransportId{next_transport_++};
    auto held = payload_.pin_nb(gp);
    i = inbound_
            .emplace(NativeKey{&gp, connection},
                     Inbound{id, connection, Handle{}, std::move(held.value())})
            .first;
    created = true;
  }
  if (i->second.connection != connection)
    return fail(ErrorCode::WrongOwner, "GP belongs to different ingress connection");
  auto ledger = engine.value()->find_ledger(connection, i->second.id);
  bool ledger_created = false;
  if (!ledger && p.value() == begin_req && ledger.error().code == ErrorCode::StaleHandle) {
    ledger = engine.value()->create_ledger(connection, i->second.id);
    ledger_created = bool(ledger);
  }
  if (!ledger) {
    if (created)
      inbound_.erase(i);
    return ledger.error();
  }
  auto rollback_unstarted = [&] {
    bool discarded =
        ledger_created && bool(engine.value()->discard_unstarted_ledger(ledger.value()));
    if (created && discarded)
      inbound_.erase(NativeKey{&gp, connection});
  };
  Expected<CallId> id = controlled ? Expected<CallId>{active->call.id}
                                   : runtime.allocate_call_id(CallOrigin::ExternalIngress);
  if (!id) {
    rollback_unstarted();
    return id.error();
  }
  WireCall call{id.value(), connection,  i->second.id, flow,
                p.value(),  now.value(), dt.value(),   snapshot.value()};
  auto ret = runtime.ingress(call);
  if (!ret) {
    if (!controlled)
      runtime.release_call_id(id.value());
    rollback_unstarted();
    return ret.error();
  }
  auto committed_failure = [&](Error error) -> Expected<tlm::tlm_sync_enum> {
    runtime.report_host_failure(error);
    if (!controlled && (ret.value().sync == Sync::Completed || p.value() == end_resp))
      inbound_.erase(NativeKey{&gp, connection});
    return error;
  };
  auto unchanged = guard.unchanged();
  if (!unchanged)
    return committed_failure(unchanged.error());
  if (ret.value().sync != Sync::Accepted) {
    auto d = codec_.from_duration(ret.value().outgoing_delay);
    if (!d)
      return committed_failure(d.error());
    if (ret.value().response) {
      auto w = payload_.write_response(gp, *ret.value().response);
      if (!w)
        return committed_failure(w.error());
    }
    if (ret.value().sync == Sync::Updated) {
      if (!ret.value().phase)
        return committed_failure(fail(ErrorCode::ProtocolViolation, "updated phase missing"));
      auto out = PhaseCodec::encode(*ret.value().phase);
      if (!out)
        return committed_failure(out.error());
      phase = out.value();
    }
    delay = d.value();
  }
  if (!controlled && (ret.value().sync == Sync::Completed || p.value() == end_resp)) {
    inbound_.erase(NativeKey{&gp, connection});
  }
  return ret.value().sync == Sync::Accepted  ? tlm::TLM_ACCEPTED
         : ret.value().sync == Sync::Updated ? tlm::TLM_UPDATED
                                             : tlm::TLM_COMPLETED;
}
} // namespace leanat::systemc
namespace leanat::systemc {
BoolSignalInput::BoolSignalInput(sc_core::sc_module_name n, Runtime &r, InstanceId i, PortId p,
                                 TimeCodec c, bool initial)
    : sc_module(n), runtime_(r), codec_(c), instance_(i), port_(p), sample_start_(initial),
      input("input") {
  SC_METHOD(sample);
  sensitive << input;
  if (!sample_start_)
    dont_initialize();
}
void BoolSignalInput::sample() {
  auto t = codec_.to_tick(sc_core::sc_time_stamp());
  if (!t) {
    SC_REPORT_ERROR("LeanAT", t.error().message.c_str());
    return;
  }
  EventDraft e;
  auto ready = runtime_.queue().successor(t.value());
  if (!ready) {
    SC_REPORT_ERROR("LeanAT", ready.error().message.c_str());
    return;
  }
  e.key.time = ready.value().time;
  e.key.turn = ready.value().turn;
  e.key.stage = EventStage::Input;
  e.key.instance = instance_;
  auto epoch = runtime_.drains().epoch(instance_);
  if (!epoch) {
    runtime_.report_host_failure(epoch.error());
    SC_REPORT_ERROR("LeanAT", epoch.error().message.c_str());
    return;
  }
  e.epoch = epoch.value();
  e.value = Value{Value::Array{Value{std::uint64_t(port_.value)}, Value{input.read()}}};
  auto q = runtime_.schedule(std::move(e));
  if (!q)
    SC_REPORT_ERROR("LeanAT", q.error().message.c_str());
}
} // namespace leanat::systemc
namespace leanat::systemc {
RegisteredPhaseCodec::RegisteredPhaseCodec(std::size_t capacity) : capacity_(capacity) {
  if (capacity < 4 || capacity > 4096)
    throw std::invalid_argument("phase registry capacity");
  entries_.emplace(begin_req, Entry{"tlm.base.BEGIN_REQ", tlm::BEGIN_REQ, false});
  entries_.emplace(end_req, Entry{"tlm.base.END_REQ", tlm::END_REQ, false});
  entries_.emplace(begin_resp, Entry{"tlm.base.BEGIN_RESP", tlm::BEGIN_RESP, false});
  entries_.emplace(end_resp, Entry{"tlm.base.END_RESP", tlm::END_RESP, false});
}
Expected<void> RegisteredPhaseCodec::register_phase(std::string key, PhaseId id,
                                                    const tlm::tlm_phase &native, bool ignorable) {
  if (frozen_)
    return fail(ErrorCode::InvalidState, "phase registry is frozen");
  if (key.empty() || key.size() > 256 || id.value <= 4 || static_cast<unsigned>(native) <= 4)
    return fail(ErrorCode::InvalidArgument,
                "custom phase must have stable key and actual custom declaration");
  if (entries_.size() >= capacity_)
    return fail(ErrorCode::Capacity, "phase registry full");
  if (entries_.count(id))
    return fail(ErrorCode::Duplicate, "duplicate stable phase id");
  for (const auto &e : entries_)
    if (e.second.key == key || e.second.native == native)
      return fail(ErrorCode::Duplicate, "duplicate phase key/native declaration");
  entries_.emplace(id, Entry{std::move(key), native, ignorable});
  return {};
}
Expected<PhaseId> RegisteredPhaseCodec::decode(const tlm::tlm_phase &p) const {
  for (const auto &e : entries_)
    if (e.second.native == p)
      return e.first;
  return fail(ErrorCode::Unsupported, "undeclared native phase");
}
Expected<tlm::tlm_phase> RegisteredPhaseCodec::encode(PhaseId p) const {
  auto i = entries_.find(p);
  if (i == entries_.end())
    return fail(ErrorCode::Unsupported, "unknown stable phase id");
  return i->second.native;
}
Expected<std::string> RegisteredPhaseCodec::key(PhaseId p) const {
  auto i = entries_.find(p);
  if (i == entries_.end())
    return fail(ErrorCode::Unsupported, "unknown stable phase key");
  return i->second.key;
}
bool RegisteredPhaseCodec::is_ignorable(PhaseId p) const {
  auto i = entries_.find(p);
  return i != entries_.end() && i->second.ignorable;
}
} // namespace leanat::systemc
namespace leanat::systemc {
void RuntimeHostAdapter::recorded_return(CallId id, bool terminal) {
  auto pending = pending_returns_.find(id);
  if (pending == pending_returns_.end())
    return;
  if (terminal)
    inbound_.erase(pending->second);
  pending_returns_.erase(pending);
}
void RuntimeHostAdapter::publish_output(InstanceId instance, PortId port, const Value &value) {
  if (instance_output)
    instance_output(instance, port, value);
  else
    publish_output(port, value);
}
Expected<unsigned> RuntimeHostAdapter::debug(ConnectionId connection, tlm::tlm_generic_payload &gp,
                                             std::size_t budget) {
  if (!debug_byte)
    return 0u;
  return BlockingBridge::debug(gp, budget,
                               [&](std::uint64_t address, std::uint8_t &byte, bool write) {
                                 return debug_byte(connection, address, byte, write);
                               });
}
Expected<void> RuntimeHostAdapter::blocking(ConnectionId connection, tlm::tlm_generic_payload &gp,
                                            sc_core::sc_time &delay) {
  auto allowed = NativeGuard::blocking_allowed();
  if (!allowed)
    return allowed.error();
  for (unsigned i = 0; i < tlm::max_num_extensions(); ++i)
    if (gp.get_extension(i))
      return fail(ErrorCode::Unsupported, "blocking native extension clone policy absent");
  auto request = payload_.snapshot(gp);
  if (!request)
    return request.error();
  auto clone = payload_.make_managed(request.value());
  if (!clone)
    return clone.error();
  auto arrival = codec_.effective_time(sc_core::sc_time_stamp(), delay);
  if (!arrival)
    return arrival.error();
  struct Completion {
    sc_core::sc_event wake;
    bool ready{};
    std::optional<Expected<ResponseSnapshot>> response;
  };
  auto completion = std::make_shared<Completion>();
  auto submission = runtime.submit_blocking(connection, request.value(), arrival.value(),
                                            [completion](Expected<ResponseSnapshot> result) {
                                              completion->response = std::move(result);
                                              completion->ready = true;
                                              completion->wake.notify(sc_core::SC_ZERO_TIME);
                                            });
  if (!submission)
    return submission.error();
  try {
    while (!completion->ready)
      sc_core::wait(completion->wake);
  } catch (...) {
    auto cancelled = runtime.cancel_blocking(submission.value());
    if (!cancelled)
      runtime.report_host_failure(cancelled.error());
    throw;
  }
  if (!*completion->response)
    return completion->response->error();
  auto written = payload_.write_response(gp, completion->response->value());
  if (!written) {
    runtime.report_host_failure(written.error());
    return written.error();
  }
  delay = sc_core::SC_ZERO_TIME;
  return {};
}
} // namespace leanat::systemc
