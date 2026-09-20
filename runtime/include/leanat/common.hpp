#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace leanat {
enum class ErrorCode {
  InvalidArgument,
  Overflow,
  Capacity,
  StaleHandle,
  WrongOwner,
  WrongDomain,
  AlreadyReleased,
  NotReady,
  InvalidState,
  ProtocolViolation,
  TimeRegression,
  Unsupported,
  FuelExhausted,
  ZenoDetected,
  NotStarted,
  Duplicate,
  Integrity,
  Schema,
  Io,
  ExternalFailure,
  Cancelled,
  TypeMismatch
};
struct Error {
  ErrorCode code;
  std::string message;
};
template <class T, class E = Error> class Expected {
  std::variant<T, E> data_;

public:
  Expected(T value) : data_(std::in_place_index<0>, std::move(value)) {}
  Expected(E error) : data_(std::in_place_index<1>, std::move(error)) {}
  bool has_value() const noexcept {
    return data_.index() == 0;
  }
  explicit operator bool() const noexcept {
    return has_value();
  }
  T &value() {
    return std::get<0>(data_);
  }
  const T &value() const {
    return std::get<0>(data_);
  }
  E &error() {
    return std::get<1>(data_);
  }
  const E &error() const {
    return std::get<1>(data_);
  }
};
template <class E> class Expected<void, E> {
  std::optional<E> error_;

public:
  Expected() = default;
  Expected(E error) : error_(std::move(error)) {}
  bool has_value() const noexcept {
    return !error_;
  }
  explicit operator bool() const noexcept {
    return has_value();
  }
  void value() const {
    if (error_)
      throw std::logic_error("Expected contains error");
  }
  E &error() {
    return error_.value();
  }
  const E &error() const {
    return error_.value();
  }
};
inline Error fail(ErrorCode c, std::string message) {
  return {c, std::move(message)};
}
template <class Tag, class Rep = std::uint32_t> struct Id {
  Rep value{};
  constexpr Id() = default;
  explicit constexpr Id(Rep v) : value(v) {}
  friend constexpr bool operator==(Id a, Id b) {
    return a.value == b.value;
  }
  friend constexpr bool operator!=(Id a, Id b) {
    return !(a == b);
  }
  friend constexpr bool operator<(Id a, Id b) {
    return a.value < b.value;
  }
};
#define LEANAT_ID(Name, Rep)                                                                       \
  struct Name##Tag;                                                                                \
  using Name = Id<Name##Tag, Rep>
LEANAT_ID(Tick, std::uint64_t);
LEANAT_ID(Duration, std::uint64_t);
LEANAT_ID(DomainId, std::uint32_t);
LEANAT_ID(InstanceId, std::uint32_t);
LEANAT_ID(ConnectionId, std::uint32_t);
LEANAT_ID(PortId, std::uint32_t);
LEANAT_ID(TypeId, std::uint32_t);
LEANAT_ID(ProgramId, std::uint32_t);
LEANAT_ID(BlockId, std::uint32_t);
LEANAT_ID(FieldId, std::uint32_t);
LEANAT_ID(ObjectId, std::uint32_t);
LEANAT_ID(MethodId, std::uint32_t);
LEANAT_ID(PhaseId, std::uint32_t);
LEANAT_ID(RegionId, std::uint32_t);
LEANAT_ID(CallId, std::uint64_t);
LEANAT_ID(TransportId, std::uint64_t);
LEANAT_ID(TxnId, std::uint64_t);
LEANAT_ID(BatchId, std::uint64_t);
#undef LEANAT_ID
inline Expected<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a)
    return fail(ErrorCode::Overflow, "addition overflow");
  return a + b;
}
inline Expected<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b) {
  if (a && b > std::numeric_limits<std::uint64_t>::max() / a)
    return fail(ErrorCode::Overflow, "multiplication overflow");
  return a * b;
}
inline Expected<Tick> add_time(Tick a, Duration b) {
  auto n = checked_add(a.value, b.value);
  if (!n)
    return n.error();
  return Tick{n.value()};
}
enum class HandleKind {
  Transaction,
  Hop,
  Event,
  Process,
  Wait,
  Result,
  Consumer,
  Scope,
  Task,
  Access,
  Lease,
  ResourceTicket,
  SpawnTicket,
  Drain,
  GateTicket
};
struct Handle {
  HandleKind kind{HandleKind::Transaction};
  DomainId domain{};
  std::uint32_t store{}, slot{};
  std::uint64_t generation{};
  std::uint64_t owner{};
  friend bool operator==(const Handle &a, const Handle &b) {
    return std::tie(a.kind, a.domain, a.store, a.slot, a.generation, a.owner) ==
           std::tie(b.kind, b.domain, b.store, b.slot, b.generation, b.owner);
  }
  friend bool operator!=(const Handle &a, const Handle &b) {
    return !(a == b);
  }
  friend bool operator<(const Handle &a, const Handle &b) {
    return std::tie(a.kind, a.domain, a.store, a.slot, a.generation, a.owner) <
           std::tie(b.kind, b.domain, b.store, b.slot, b.generation, b.owner);
  }
};
using Bytes = std::vector<std::uint8_t>;
struct Value {
  using Array = std::vector<Value>;
  std::variant<std::monostate, bool, std::uint64_t, std::int64_t, Bytes, Array, Handle> data;
  Value() = default;
  template <class T> explicit Value(T x) : data(std::move(x)) {}
  friend bool operator==(const Value &a, const Value &b) {
    return a.data == b.data;
  }
  friend bool operator!=(const Value &a, const Value &b) {
    return !(a == b);
  }
};
struct SourceSpan {
  std::string file_hash;
  std::uint64_t byte_start{}, byte_end{};
  std::uint32_t line{1}, column{1};
};
struct ReadyKey {
  Tick time{};
  std::uint64_t turn{};
  friend bool operator<(ReadyKey a, ReadyKey b) {
    return std::tie(a.time, a.turn) < std::tie(b.time, b.turn);
  }
  friend bool operator==(ReadyKey a, ReadyKey b) {
    return a.time == b.time && a.turn == b.turn;
  }
  friend bool operator!=(ReadyKey a, ReadyKey b) {
    return !(a == b);
  }
};
enum class Command { Read, Write, Ignore };
enum class ResponseStatus {
  Incomplete,
  Ok,
  GenericError,
  AddressError,
  CommandError,
  BurstError,
  ByteEnableError
};
struct PayloadSnapshot {
  Command command{Command::Ignore};
  std::uint64_t address{};
  Bytes data;
  std::uint64_t streaming_width{1};
  Bytes byte_enable;
  ResponseStatus status{ResponseStatus::Incomplete};
  bool dmi_hint{};
  std::map<std::string, Bytes> extensions;
};
struct ResponseSnapshot {
  ResponseStatus status{ResponseStatus::Incomplete};
  Bytes data;
  bool dmi_hint{};
  std::map<std::string, Bytes> extensions;
};
enum class Flow { Forward, Backward };
enum class Sync { Accepted, Updated, Completed };
inline constexpr PhaseId begin_req{1}, end_req{2}, begin_resp{3}, end_resp{4};
struct WireCall {
  CallId id{};
  ConnectionId connection{};
  TransportId transport{};
  Flow flow{Flow::Forward};
  PhaseId phase{begin_req};
  Tick call_time{};
  Duration incoming_delay{};
  PayloadSnapshot request;
};
struct WireReturn {
  Sync sync{Sync::Accepted};
  std::optional<PhaseId> phase;
  Duration outgoing_delay{};
  std::optional<ResponseSnapshot> response;
};
struct SendIntent {
  ConnectionId connection{};
  Handle txn{};
  Flow flow{Flow::Forward};
  PhaseId phase{begin_req};
  Tick not_before{};
  CallId call_id{};
  TransportId transport{};
  PayloadSnapshot payload;
};
struct WakePoint {
  Tick time{};
  std::uint64_t turn{}, generation{};
};
enum class ContextKind { Timed, Process, Transport, Debug, Dmi };
struct ExecutionContext {
  ContextKind kind{ContextKind::Timed};
  DomainId domain{};
  InstanceId instance{};
  ConnectionId connection{};
  ReadyKey ready{};
  std::uint64_t owner{}, epoch{};
  SourceSpan source;
};
struct TraceEvent {
  std::string kind;
  ReadyKey ready{};
  InstanceId instance{};
  ConnectionId connection{};
  std::vector<Value> values;
  std::string detail;
};
} // namespace leanat

namespace leanat {
// Memory accounting includes every Value node, even empty containers. Invalid
// depth/work/size is represented by SIZE_MAX so bounded callers reject it.
inline constexpr std::size_t value_max_depth = 256, value_max_nodes = 65536;
inline std::size_t bounded_value_bytes(const Value &root) {
  struct Frame {
    const Value *value{};
    std::size_t next{};
    bool visited{};
  };
  std::array<Frame, value_max_depth> stack{};
  std::size_t depth = 1, nodes = 0, total = 0;
  stack[0].value = &root;
  auto add = [&](std::size_t n) {
    if (n > SIZE_MAX - total)
      return false;
    total += n;
    return true;
  };
  while (depth) {
    auto &frame = stack[depth - 1];
    if (!frame.visited) {
      frame.visited = true;
      if (++nodes > value_max_nodes || !add(sizeof(Value)))
        return SIZE_MAX;
      if (auto bytes = std::get_if<Bytes>(&frame.value->data)) {
        if (!add(bytes->capacity()))
          return SIZE_MAX;
      }
      if (auto array = std::get_if<Value::Array>(&frame.value->data)) {
        if (array->capacity() > value_max_nodes)
          return SIZE_MAX;
        const auto spare = array->capacity() - array->size();
        if (spare > SIZE_MAX / sizeof(Value) || !add(spare * sizeof(Value)))
          return SIZE_MAX;
      }
    }
    auto array = std::get_if<Value::Array>(&frame.value->data);
    if (!array || frame.next == array->size()) {
      --depth;
      continue;
    }
    if (array->size() > value_max_nodes || depth == stack.size())
      return SIZE_MAX;
    auto child = &(*array)[frame.next++];
    stack[depth++] = {child, 0, false};
  }
  return total;
}
} // namespace leanat
namespace leanat {
inline std::size_t bounded_extension_bytes(const std::map<std::string, Bytes> &extensions) {
  if (extensions.size() > value_max_nodes)
    return SIZE_MAX;
  std::size_t total = 0;
  auto add = [&](std::size_t n) {
    if (n > SIZE_MAX - total)
      return false;
    total += n;
    return true;
  };
  // Four pointers conservatively cover balanced-tree links, color and alignment.
  for (const auto &entry : extensions)
    if (!add(sizeof(std::map<std::string, Bytes>::value_type)) || !add(4 * sizeof(void *)) ||
        !add(entry.first.capacity()) || !add(1) || !add(entry.second.capacity()))
      return SIZE_MAX;
  return total;
}
inline std::size_t bounded_payload_bytes(const PayloadSnapshot &payload) {
  auto extensions = bounded_extension_bytes(payload.extensions);
  if (extensions == SIZE_MAX)
    return SIZE_MAX;
  std::size_t total = sizeof(PayloadSnapshot);
  auto add = [&](std::size_t n) {
    if (n > SIZE_MAX - total)
      return false;
    total += n;
    return true;
  };
  if (!add(payload.data.capacity()) || !add(payload.byte_enable.capacity()) || !add(extensions))
    return SIZE_MAX;
  return total;
}
inline std::size_t bounded_response_bytes(const ResponseSnapshot &response) {
  auto extensions = bounded_extension_bytes(response.extensions);
  if (extensions == SIZE_MAX)
    return SIZE_MAX;
  std::size_t total = sizeof(ResponseSnapshot);
  if (response.data.capacity() > SIZE_MAX - total)
    return SIZE_MAX;
  total += response.data.capacity();
  if (extensions > SIZE_MAX - total)
    return SIZE_MAX;
  return total + extensions;
}
} // namespace leanat
