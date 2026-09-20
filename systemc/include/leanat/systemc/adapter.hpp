#pragma once
#include <leanat/protocol.hpp>
#include <leanat/runtime.hpp>
#include <set>
#include <systemc>
#include <tlm>
namespace leanat::systemc {
enum class NativeByteOrder { Little, Big, Unknown };
struct NativeProfile {
  NativeByteOrder byte_order{NativeByteOrder::Little};
  unsigned bus_width{32};
};
NativeByteOrder native_byte_order() noexcept;
Expected<void> validate_native_host(const NativeProfile &);
Expected<void> validate_native_profile(const NativeProfile &, const tlm::tlm_base_socket_if &,
                                       const tlm::tlm_base_socket_if &);
class TimeCodec {
  std::uint64_t units_;

public:
  explicit TimeCodec(std::uint64_t resolution_units_per_tick = 1);
  Expected<Tick> to_tick(const sc_core::sc_time &) const;
  Expected<Duration> to_duration(const sc_core::sc_time &) const;
  Expected<sc_core::sc_time> from_tick(Tick) const;
  Expected<sc_core::sc_time> from_duration(Duration) const;
  Expected<Tick> effective_time(const sc_core::sc_time &, const sc_core::sc_time &) const;
  Expected<sc_core::sc_time> wake_delay(Tick, const sc_core::sc_time &) const;
};
class NativeGuard {
  sc_core::sc_time time_;
  std::uint64_t delta_;
  const sc_core::sc_object *process_;

public:
  NativeGuard();
  ~NativeGuard();
  NativeGuard(const NativeGuard &) = delete;
  Expected<void> unchanged() const;
  static Expected<void> blocking_allowed();
  static bool active();
};
class TransportPin {
  tlm::tlm_generic_payload *gp_{};

public:
  TransportPin() = default;
  explicit TransportPin(tlm::tlm_generic_payload &);
  TransportPin(TransportPin &&) noexcept;
  TransportPin &operator=(TransportPin &&) noexcept;
  TransportPin(const TransportPin &) = delete;
  ~TransportPin();
  tlm::tlm_generic_payload &get() const {
    return *gp_;
  }
};
class PayloadBridge {
  std::size_t max_data_, max_mask_;

public:
  PayloadBridge(std::size_t max_data = 1048576, std::size_t max_mask = 65536)
      : max_data_(max_data), max_mask_(max_mask) {}
  Expected<TransportPin> pin_nb(tlm::tlm_generic_payload &) const;
  Expected<TransportPin> make_managed(const PayloadSnapshot &) const;
  Expected<PayloadSnapshot> snapshot(tlm::tlm_generic_payload &) const;
  Expected<PayloadSnapshot> snapshot_call(tlm::tlm_generic_payload &, PhaseId) const;
  Expected<ResponseSnapshot> response(tlm::tlm_generic_payload &) const;
  Expected<void> write_response(tlm::tlm_generic_payload &, const ResponseSnapshot &) const;
};
class PhaseCodec {
public:
  static Expected<PhaseId> decode(const tlm::tlm_phase &);
  static Expected<tlm::tlm_phase> encode(PhaseId);
};
class RegisteredPhaseCodec {
  struct Entry {
    std::string key;
    tlm::tlm_phase native;
    bool ignorable{};
  };
  std::map<PhaseId, Entry> entries_;
  std::size_t capacity_;
  bool frozen_{};

public:
  explicit RegisteredPhaseCodec(std::size_t capacity = 64);
  Expected<void> register_phase(std::string, PhaseId, const tlm::tlm_phase &,
                                bool ignorable = false);
  Expected<PhaseId> decode(const tlm::tlm_phase &) const;
  Expected<tlm::tlm_phase> encode(PhaseId) const;
  Expected<std::string> key(PhaseId) const;
  bool is_ignorable(PhaseId) const;
  void freeze() {
    frozen_ = true;
  }
};
class RuntimeDomain : public sc_core::sc_module {
  struct Work {
    Tick time;
    std::uint64_t ordinal;
    std::function<void()> action;
  };
  std::vector<Work> work_;
  sc_core::sc_event wake_;
  std::optional<Tick> armed_;
  std::size_t capacity_, budget_;
  std::uint64_t ordinal_{}, calls_{};
  bool started_{}, stopped_{};
  void dispatch();
  void rearm();

public:
  SC_HAS_PROCESS(RuntimeDomain);
  const DomainId id;
  const TimeCodec time;
  RuntimeDomain(sc_core::sc_module_name, DomainId, TimeCodec = TimeCodec{},
                std::size_t capacity = 1024, std::size_t budget = 64);
  Expected<void> start();
  void stop();
  bool stopped() const {
    return stopped_;
  }
  Expected<void> schedule(Tick, std::function<void()>);
  CallId next_call();
  std::size_t pending() const {
    return work_.size();
  }
};
class NbBridge {
  struct Live {
    TransportId transport;
    std::uint64_t generation;
    TransportPin pin;
    Handle ledger;
  };
  RuntimeDomain &domain_;
  ProtocolEngine engine_;
  PayloadBridge payload_;
  RegisteredPhaseCodec phases_;
  std::optional<WireCall> last_ignored_;
  std::uint64_t ignored_calls_{};
  ConnectionId connection_;
  std::map<tlm::tlm_generic_payload *, Live> live_;
  std::uint64_t next_{1};
  std::size_t capacity_;
  std::vector<std::unique_ptr<WireCall>> current_calls_;

public:
  using NativeCall = std::function<tlm::tlm_sync_enum(tlm::tlm_generic_payload &, tlm::tlm_phase &,
                                                      sc_core::sc_time &)>;
  using Handler = std::function<Expected<WireReturn>(const WireCall &)>;
  NbBridge(RuntimeDomain &, InstanceId, ConnectionId, std::size_t capacity = 128);
  Expected<void> register_ignorable(std::string, PhaseId, const tlm::tlm_phase &);
  std::uint64_t ignored_calls() const {
    return ignored_calls_;
  }
  const std::optional<WireCall> &last_ignored() const {
    return last_ignored_;
  }
  Expected<WireReturn> exchange(Flow, tlm::tlm_generic_payload &, tlm::tlm_phase &,
                                sc_core::sc_time &, const NativeCall &);
  Expected<tlm::tlm_sync_enum> receive(Flow, tlm::tlm_generic_payload &, tlm::tlm_phase &,
                                       sc_core::sc_time &, const Handler &);
  std::size_t active() const {
    return live_.size();
  }
};
class BlockingBridge {
  RuntimeDomain &domain_;
  PayloadBridge payload_;
  std::size_t capacity_, active_{};

public:
  using Service = std::function<Expected<ResponseSnapshot>(const PayloadSnapshot &)>;
  BlockingBridge(RuntimeDomain &d, std::size_t capacity = 64) : domain_(d), capacity_(capacity) {}
  Expected<void> transport(tlm::tlm_generic_payload &, sc_core::sc_time &, Duration,
                           const Service &);
  static Expected<unsigned> debug(tlm::tlm_generic_payload &, std::size_t,
                                  const std::function<bool(std::uint64_t, std::uint8_t &, bool)> &);
};
class RawDmiHost {
  struct Grant {
    std::uint64_t start, end;
    std::function<void(std::uint64_t, std::uint64_t)> invalidate;
  };
  std::shared_ptr<Bytes> bytes_;
  std::uint64_t base_, generation_{1};
  std::size_t capacity_;
  bool invalidating_{};
  TimeCodec time_;
  Duration read_, write_;
  std::vector<Grant> grants_;

public:
  RawDmiHost(std::shared_ptr<Bytes>, std::uint64_t, TimeCodec, Duration, Duration,
             bool timing_strict = false, std::size_t capacity = 128);
  Expected<bool> get(tlm::tlm_generic_payload &, tlm::tlm_dmi &,
                     std::function<void(std::uint64_t, std::uint64_t)>);
  Expected<void> invalidate(std::uint64_t, std::uint64_t);
  Expected<void> replace(std::shared_ptr<Bytes>);
  std::uint64_t generation() const {
    return generation_;
  }
};
// Only the identity base protocol is admitted; other protocols require a checked stateful adapter.
struct ProtocolAdapter {
  static Expected<WireCall> map_call(const WireCall &, const std::string &source,
                                     const std::string &destination);
};
struct AdapterRule {
  std::uint32_t before{}, after{};
  Flow flow{};
  PhaseId phase;
  Sync sync{};
  std::optional<PhaseId> returned;
  std::string guard;
  std::uint32_t request_lanes{}, response_lanes{}, actions{};
};
struct AdapterContract {
  std::vector<PhaseId> phases;
  std::vector<AdapterRule> rules;
};
enum class AdapterDirection { LeftToRight, RightToLeft };
class CheckedAdapter {
  std::map<PhaseId, PhaseId> forward_, reverse_;

public:
  static Expected<CheckedAdapter> validate(const AdapterContract &, const AdapterContract &,
                                           const std::map<PhaseId, PhaseId> &);
  Expected<PhaseId> phase(PhaseId, AdapterDirection) const;
  Expected<WireCall> call(const WireCall &, AdapterDirection) const;
  Expected<WireReturn> result(const WireReturn &, AdapterDirection original_call_direction) const;
};
class EventPump : public sc_core::sc_module {
  Runtime &runtime_;
  TimeCodec codec_;
  std::uint32_t budget_;
  sc_core::sc_event wake_;
  std::optional<WakePoint> armed_;
  void dispatch();

public:
  SC_HAS_PROCESS(EventPump);
  EventPump(sc_core::sc_module_name, Runtime &, TimeCodec = TimeCodec{}, std::uint32_t budget = 64);
  Expected<void> arm(std::optional<WakePoint>);
};
class RuntimeHostAdapter : public RuntimeHost {
  struct Binding {
    std::function<Expected<tlm::tlm_generic_payload *>(const SendIntent &)> resolve;
    NbBridge::NativeCall call;
  };
  TimeCodec codec_;
  PayloadBridge payload_;
  std::map<ConnectionId, Binding> bindings_;
  struct Inbound {
    TransportId id;
    ConnectionId connection;
    Handle ledger;
    TransportPin pin;
  };
  using NativeKey = std::pair<tlm::tlm_generic_payload *, ConnectionId>;
  std::map<NativeKey, Inbound> inbound_;
  struct ActiveCall {
    WireCall call;
    tlm::tlm_generic_payload *gp;
    const sc_core::sc_object *process;
  };
  std::vector<std::unique_ptr<ActiveCall>> native_calls_;
  std::map<CallId, NativeKey> pending_returns_;
  std::size_t registry_capacity_, native_capacity_, call_capacity_;
  std::uint64_t next_transport_{1};

public:
  Runtime runtime;
  EventPump pump;
  std::function<void(PortId, const Value &)> output;
  std::function<void(InstanceId, PortId, const Value &)> instance_output;
  std::function<bool(ConnectionId, std::uint64_t, std::uint8_t &, bool)> debug_byte;
  std::function<void(const TraceEvent &)> trace;
  RuntimeHostAdapter(sc_core::sc_module_name, const RuntimeConfig &, TimeCodec = TimeCodec{},
                     std::uint32_t budget = 64);
  Expected<void> bind(ConnectionId,
                      std::function<Expected<tlm::tlm_generic_payload *>(const SendIntent &)>,
                      NbBridge::NativeCall);
  Expected<WireReturn> transport(const SendIntent &) override;
  Expected<tlm::tlm_sync_enum> receive(ConnectionId, Flow, tlm::tlm_generic_payload &,
                                       tlm::tlm_phase &, sc_core::sc_time &);
  Expected<tlm::tlm_generic_payload *> resolve_transport(TransportId);
  Expected<void> blocking(ConnectionId, tlm::tlm_generic_payload &, sc_core::sc_time &);
  Expected<unsigned> debug(ConnectionId, tlm::tlm_generic_payload &, std::size_t budget = 65536);
  void recorded_return(CallId, bool wire_terminal) override;
  void arm(std::optional<WakePoint>) override;
  void publish_output(PortId, const Value &) override;
  void publish_output(InstanceId, PortId, const Value &) override;
  void emit_trace(const TraceEvent &) override;
};
class BoolSignalInput : public sc_core::sc_module {
  Runtime &runtime_;
  TimeCodec codec_;
  InstanceId instance_;
  PortId port_;
  bool sample_start_;
  void sample();

public:
  sc_core::sc_in<bool> input;
  SC_HAS_PROCESS(BoolSignalInput);
  BoolSignalInput(sc_core::sc_module_name, Runtime &, InstanceId, PortId, TimeCodec = TimeCodec{},
                  bool sample_start = false);
};
} // namespace leanat::systemc
