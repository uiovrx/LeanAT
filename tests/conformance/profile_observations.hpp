#pragma once
#include <initializer_list>
#include <leanat/interpreter.hpp>
#include <leanat/runtime.hpp>
#include <sstream>
#include <type_traits>

namespace conformance::profile {
// Integers are decimal JSON strings so no consumer can round a 64-bit identity.
class Json {
  std::string text_;
  struct Encoded {};
  Json(std::string text, Encoded) : text_(std::move(text)) {}
  static std::string quote(const std::string &value) {
    static const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : value) {
      if (c == '"' || c == '\\') {
        out += '\\';
        out += char(c);
      } else if (c < 32) {
        out += "\\u00";
        out += hex[c >> 4];
        out += hex[c & 15];
      } else
        out += char(c);
    }
    return out + '"';
  }

public:
  Json() : text_("null") {}
  Json(std::nullptr_t) : Json() {}
  Json(bool value) : text_(value ? "true" : "false") {}
  Json(const char *value) : text_(quote(value)) {}
  Json(const std::string &value) : text_(quote(value)) {}
  template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
  Json(T value) : text_(quote(std::to_string(value))) {}
  static Json object(std::initializer_list<std::pair<std::string, Json>> entries) {
    std::string out = "{";
    std::vector<std::string> keys;
    for (const auto &entry : entries) {
      if (std::find(keys.begin(), keys.end(), entry.first) != keys.end())
        throw std::invalid_argument("duplicate observation key");
      keys.push_back(entry.first);
      if (out.size() > 1)
        out += ',';
      out += quote(entry.first) + ':' + entry.second.dump();
    }
    return Json(out + '}', Encoded{});
  }
  static Json array(const std::vector<Json> &entries) {
    std::string out = "[";
    for (const auto &entry : entries) {
      if (out.size() > 1)
        out += ',';
      out += entry.dump();
    }
    return Json(out + ']', Encoded{});
  }
  const std::string &dump() const {
    return text_;
  }
};
using namespace leanat;
inline Json observe(const Json &v) {
  return v;
}
inline Json observe(const std::string &v) {
  return Json(v);
}
inline Json observe(const char *v) {
  return Json(v);
}
inline Json observe(bool v) {
  return Json(v);
}
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
inline Json observe(T v) {
  return Json(v);
}
template <class T, std::enable_if_t<std::is_enum_v<T>, int> = 0> inline Json observe(T v) {
  return Json(static_cast<std::underlying_type_t<T>>(v));
}
template <class Tag, class Rep> inline Json observe(Id<Tag, Rep> v) {
  return observe(v.value);
}
inline Json observe(const Handle &);
inline Json observe(const Value &);
inline Json observe(const Error &);
inline Json observe(const ReadyKey &);
inline Json observe(const EventKey &);
inline Json observe(const EventDraft &);
inline Json observe(const QueuedEvent &);
inline Json observe(const PayloadSnapshot &);
inline Json observe(const ResponseSnapshot &);
inline Json observe(const WireCall &);
inline Json observe(const WireReturn &);
inline Json observe(const SendIntent &);
inline Json observe(const TraceEvent &);
inline Json observe(const VersionedCell &);
inline Json observe(const ProtocolMilestone &);
inline Json observe(const LedgerIdentity &);
inline Json observe(const WireSnapshot &);
inline Json observe(const ExchangeResult &);
inline Json observe(const RuntimeMilestoneObservation &);
inline Json observe(const RuntimeConfig &);
inline Json observe(const ExecutionContext &);
inline Json observe(const CommittedActions &);
inline Json observe(const exec::SegmentResult &);
template <class T> inline Json observe(const std::vector<T> &values) {
  std::vector<Json> out;
  out.reserve(values.size());
  for (const auto &value : values)
    out.push_back(observe(value));
  return Json::array(out);
}
template <class T> inline Json observe(const std::optional<T> &value) {
  return value ? observe(*value) : Json();
}
inline Json observe(const std::map<std::string, Bytes> &values) {
  std::vector<Json> out;
  for (const auto &v : values)
    out.push_back(Json::object({{"key", v.first}, {"bytes", observe(v.second)}}));
  return Json::array(out);
}
inline Json observe(const Handle &v) {
  return Json::object({{"kind", observe(v.kind)},
                       {"domain", observe(v.domain)},
                       {"store", v.store},
                       {"slot", v.slot},
                       {"generation", v.generation},
                       {"owner", v.owner}});
}
inline Json observe(const Value &v) {
  if (std::holds_alternative<std::monostate>(v.data))
    return Json::object({{"unit", Json()}});
  if (auto p = std::get_if<bool>(&v.data))
    return Json::object({{"bool", *p}});
  if (auto p = std::get_if<std::uint64_t>(&v.data))
    return Json::object({{"u64", *p}});
  if (auto p = std::get_if<std::int64_t>(&v.data))
    return Json::object({{"i64", *p}});
  if (auto p = std::get_if<Bytes>(&v.data))
    return Json::object({{"bytes", observe(*p)}});
  if (auto p = std::get_if<Value::Array>(&v.data))
    return Json::object({{"array", observe(*p)}});
  return Json::object({{"handle", observe(std::get<Handle>(v.data))}});
}
inline Json observe(const Error &v) {
  return Json::object({{"code", observe(v.code)}, {"message", v.message}});
}
inline Json observe(const ReadyKey &v) {
  return Json::object({{"time", observe(v.time)}, {"turn", v.turn}});
}
inline Json observe(const EventKey &v) {
  return Json::object({{"time", observe(v.time)},
                       {"turn", v.turn},
                       {"stage", observe(v.stage)},
                       {"instance", observe(v.instance)},
                       {"connection", observe(v.connection)},
                       {"sequence", v.sequence}});
}
inline Json observe(const EventDraft &v) {
  return Json::object({{"key", observe(v.key)},
                       {"value", observe(v.value)},
                       {"owner", v.owner},
                       {"epoch", v.epoch}});
}
inline Json observe(const QueuedEvent &v) {
  return Json::object(
      {{"token", observe(v.token)}, {"event", observe(v.event)}, {"cancelled", v.cancelled}});
}
inline Json observe(const PayloadSnapshot &v) {
  return Json::object({{"command", observe(v.command)},
                       {"address", v.address},
                       {"data", observe(v.data)},
                       {"streamingWidth", v.streaming_width},
                       {"byteEnable", observe(v.byte_enable)},
                       {"status", observe(v.status)},
                       {"dmiHint", v.dmi_hint},
                       {"extensions", observe(v.extensions)}});
}
inline Json observe(const ResponseSnapshot &v) {
  return Json::object({{"status", observe(v.status)},
                       {"data", observe(v.data)},
                       {"dmiHint", v.dmi_hint},
                       {"extensions", observe(v.extensions)}});
}
inline Json observe(const WireCall &v) {
  return Json::object({{"id", observe(v.id)},
                       {"connection", observe(v.connection)},
                       {"transport", observe(v.transport)},
                       {"flow", observe(v.flow)},
                       {"phase", observe(v.phase)},
                       {"callTime", observe(v.call_time)},
                       {"incomingDelay", observe(v.incoming_delay)},
                       {"request", observe(v.request)}});
}
inline Json observe(const WireReturn &v) {
  return Json::object({{"sync", observe(v.sync)},
                       {"phase", observe(v.phase)},
                       {"outgoingDelay", observe(v.outgoing_delay)},
                       {"response", observe(v.response)}});
}
inline Json observe(const SendIntent &v) {
  return Json::object({{"connection", observe(v.connection)},
                       {"transaction", observe(v.txn)},
                       {"flow", observe(v.flow)},
                       {"phase", observe(v.phase)},
                       {"notBefore", observe(v.not_before)},
                       {"callId", observe(v.call_id)},
                       {"transport", observe(v.transport)},
                       {"payload", observe(v.payload)}});
}
inline Json observe(const TraceEvent &v) {
  return Json::object({{"kind", v.kind},
                       {"ready", observe(v.ready)},
                       {"instance", observe(v.instance)},
                       {"connection", observe(v.connection)},
                       {"values", observe(v.values)},
                       {"detail", v.detail}});
}
inline Json observe(const VersionedCell &v) {
  return Json::object({{"value", observe(v.value)},
                       {"version", v.version},
                       {"epoch", v.epoch},
                       {"stableBytes", v.stable_bytes},
                       {"epochIndependent", v.epoch_independent}});
}
inline Json observe(const ProtocolMilestone &v) {
  return Json::object({{"kind", observe(v.kind)},
                       {"time", observe(v.time)},
                       {"implicit", v.implicit},
                       {"response", observe(v.response)}});
}
inline Json observe(const LedgerIdentity &v) {
  return Json::object({{"domain", observe(v.domain)},
                       {"localSide", observe(v.local_side)},
                       {"connection", observe(v.connection)},
                       {"transport", observe(v.transport)},
                       {"transportGeneration", v.transport_generation}});
}
inline Json observe(const WireSnapshot &v) {
  return Json::object({{"identity", observe(v.identity)},
                       {"state", observe(v.state)},
                       {"lastTiming", observe(v.last_timing)},
                       {"faulted", v.faulted},
                       {"pending", v.pending},
                       {"callOrdinal", v.call_ordinal},
                       {"protocolState", v.protocol_state}});
}
inline Json observe(const ExchangeResult &v) {
  return Json::object({{"callId", observe(v.call_id)},
                       {"hop", observe(v.hop)},
                       {"nextState", observe(v.next_state)},
                       {"milestones", observe(v.milestones)},
                       {"traceTags", observe(v.trace_tags)},
                       {"needsAck", v.needs_ack},
                       {"wireTerminal", v.wire_terminal},
                       {"ignored", v.ignored}});
}
inline Json observe(const RuntimeMilestoneObservation &v) {
  return Json::object({{"key", observe(v.key)},
                       {"event", observe(v.event)},
                       {"identity", observe(v.identity)},
                        {"causal", Json::object({{"hop", observe(v.causal.hop)},
                                                {"ready", observe(v.causal.ready)},
                                                {"callId", observe(v.causal.call_id)},
                                                {"milestone", observe(v.causal.milestone)}})}});
}
inline Json observe(const ExecutionContext &v) {
  return Json::object({{"kind", observe(v.kind)},
                       {"domain", observe(v.domain)},
                       {"instance", observe(v.instance)},
                       {"connection", observe(v.connection)},
                       {"ready", observe(v.ready)},
                       {"owner", v.owner},
                       {"epoch", v.epoch},
                       {"source", Json::object({{"fileHash", v.source.file_hash},
                                                {"byteStart", v.source.byte_start},
                                                {"byteEnd", v.source.byte_end},
                                                {"line", v.source.line},
                                                {"column", v.source.column}})}});
}
inline Json observe(const CommittedActions &v) {
  return Json::object(
      {{"actions", observe(v.actions)}, {"cursor", v.cursor}, {"failed", v.failed}});
}
inline Json observe(const exec::SegmentResult &v) {
  return Json::object({{"kind", observe(v.kind)},
                       {"values", observe(v.values)},
                       {"error", v.error},
                       {"fuelUsed", v.fuel_used},
                       {"traces", observe(v.traces)},
                       {"suspendedWait", observe(v.suspended_wait)},
                       {"resumeBlock", observe(v.resume_block)},
                       {"liveValues", observe(v.live_values)}});
}
inline Json observe(const RuntimeConfig &v) {
  std::vector<Json> bindings;
  for (const auto &b : v.connection_bindings)
    bindings.push_back(Json::object({{"connection", observe(b.connection)},
                                     {"initiator", observe(b.initiator)},
                                     {"target", observe(b.target)}}));
  // Artifact identity is deliberately emitted in the record provenance, not semantic config.
  return Json::object({{"domain", observe(v.domain)},
                       {"instance", observe(v.instance)},
                       {"instances", observe(v.instances)},
                       {"connections", observe(v.connections)},
                       {"connectionBindings", Json::array(bindings)},
                       {"instanceCapacity", v.instance_capacity},
                       {"eventCapacity", v.event_capacity},
                       {"ledgerCapacity", v.ledger_capacity},
                       {"callCapacity", v.call_capacity},
                       {"callsPerLedger", v.calls_per_ledger},
                       {"intentCapacity", v.intent_capacity},
                       {"frameCapacity", v.frame_capacity},
                       {"waitCapacity", v.wait_capacity},
                       {"liveCapacity", v.live_capacity},
                       {"drainCapacity", v.drain_capacity},
                       {"hopsPerTransaction", v.hops_per_transaction},
                       {"resultCapacity", v.result_capacity},
                       {"consumerCapacity", v.consumer_capacity},
                       {"pinCapacity", v.pin_capacity},
                       {"maxEvents", v.max_events},
                       {"maxEventsPerTick", v.max_events_per_tick},
                       {"ingressBytes", v.ingress_bytes},
                       {"nestingCapacity", v.nesting_capacity},
                       {"blockingCapacity", v.blocking_capacity}});
}
class Recorder {
  std::vector<Json> records_;
  std::size_t bytes_{};

public:
  template <class T> void add(const std::string &kind, const T &value) {
    auto row =
        Json::object({{"ordinal", records_.size()}, {"kind", kind}, {"value", observe(value)}});
    if (records_.size() >= 100000 || row.dump().size() > 64 * 1024 * 1024 - bytes_)
      throw std::length_error("complete observation budget exceeded");
    bytes_ += row.dump().size();
    records_.push_back(std::move(row));
  }
  Json json() const {
    return Json::array(records_);
  }
  std::size_t size() const {
    return records_.size();
  }
};
} // namespace conformance::profile
