#pragma once
#include "leanat/admission.hpp"
#include "leanat/common.hpp"
#include "leanat/event_txn.hpp"
#include "leanat/protocol.hpp"
#include "leanat/runtime.hpp"
namespace leanat {
struct PayloadLimits {
  std::size_t data_bytes{65536}, mask_bytes{65536}, extension_bytes{65536}, extension_count{16},
      extension_key_bytes{128};
};
struct OptionalExtensionSchema {
  std::string key, schema;
  std::uint32_t version{};
  TypeId type;
  bool ignorable{true};
};
class PayloadBuilder {
  PayloadLimits limits_;
  std::map<std::string, OptionalExtensionSchema> schemas_;

public:
  explicit PayloadBuilder(PayloadLimits limits = {}) : limits_(limits) {}
  Expected<void> register_extension(OptionalExtensionSchema);
  Expected<PayloadSnapshot> read(std::uint64_t, std::size_t) const;
  Expected<PayloadSnapshot> write(std::uint64_t, Bytes) const;
  PayloadSnapshot ignore(std::uint64_t) const;
  Expected<PayloadSnapshot> with_streaming_width(PayloadSnapshot, std::uint64_t) const;
  Expected<PayloadSnapshot> with_byte_enable(PayloadSnapshot, Bytes) const;
  Expected<PayloadSnapshot> with_optional_extension(PayloadSnapshot,
                                                    const OptionalExtensionSchema &, Bytes) const;
};
// A checked lowering description. Runtime executes these steps with its own
// gate/wait/result capabilities; constructing a plan never advances wire state.
class ResponseAcknowledger {
  ProtocolEngine &protocol_;
  Handle hop_;
  std::uint64_t epoch_;

public:
  ResponseAcknowledger(ProtocolEngine &protocol, Handle hop, std::uint64_t epoch)
      : protocol_(protocol), hop_(hop), epoch_(epoch) {}
  Expected<bool> stage_ack(EventTxn &, Tick when, CallId, PayloadSnapshot,
                           Runtime *runtime = nullptr, std::optional<Handle> transaction = {});
};
enum class TransactionStep {
  CreateTransaction,
  AwaitRequestGate,
  StageBeginRequest,
  AwaitResponse,
  AckIfNeeded,
  AwaitTerminal,
  CopyOwnedResult,
  ReleaseOwnConsumer
};
struct TransactionPlan {
  ConnectionId endpoint;
  PayloadSnapshot payload;
  std::vector<TransactionStep> steps;
};
Expected<TransactionPlan> plan_transact(const ExecutionContext &, ConnectionId, PayloadSnapshot,
                                        const PayloadLimits &, bool initiator = true,
                                        bool base_protocol = true);
} // namespace leanat

namespace leanat {
enum class TransactState {
  Fresh,
  GateWaiting,
  RequestQueued,
  ResponseWaiting,
  TerminalWaiting,
  Complete,
  Cancelled
};
struct TxnResult {
  ResponseStatus status{ResponseStatus::Incomplete};
  Bytes data;
  std::map<std::string, Bytes> extensions;
  std::optional<Tick> terminal_effective_time;
  std::optional<CancelReason> local_cancel;
  std::optional<Handle> drain;
  bool success() const {
    return !local_cancel && status == ResponseStatus::Ok;
  }
};
Value encode_txn_result(const TxnResult &);
class TransactOperation {
  Runtime &runtime_;
  AdmissionStore &admission_;
  PayloadLimits limits_;
  std::size_t result_bytes_;
  TransactState state_{TransactState::Fresh};
  ExecutionContext context_;
  ConnectionId connection_{};
  TransportId transport_{};
  Handle transaction_{}, hop_{};
  PayloadSnapshot request_;
  std::optional<RequestGateTicket> gate_;
  std::optional<RequestPermit> permit_;
  std::optional<ReservedResult> storage_;
  std::optional<TxnResult> response_, result_;
  bool wire_started_{}, external_gate_{}, gate_retired_{}, admission_retired_{}, drain_finished_{},
      ledger_retired_{};
  Expected<void> validate_context(const ExecutionContext &, bool cancellation = false) const;
  Expected<void> queue_request(const ExecutionContext &, std::optional<RequestPermit>);
  Expected<void> observe_sent(const ExecutionContext &);
  Expected<void> publish_result(TxnResult, ReadyKey);

public:
  TransactOperation(Runtime &, AdmissionStore &, PayloadLimits = {},
                    std::size_t result_bytes = 262144);
  TransactOperation(const TransactOperation &) = delete;
  TransactOperation &operator=(const TransactOperation &) = delete;
  Expected<void> start(const ExecutionContext &, ConnectionId, TransportId, PayloadSnapshot,
                       bool external_request_gate = false);
  Expected<void> publish_with_permit(const ExecutionContext &, RequestPermit);
  Expected<bool> advance(const ExecutionContext &);
  Expected<CancelDisposition> cancel(const ExecutionContext &, CancelReason = CancelReason::User);
  Expected<TxnResult> result() const;
  bool published() const;
  Expected<bool> reap(const ExecutionContext &);
  Expected<ResultHandle> subscribe_result(std::uint64_t owner);
  Handle transaction() const {
    return transaction_;
  }
  Handle hop() const {
    return hop_;
  }
  TransactState state() const {
    return state_;
  }
};
} // namespace leanat
namespace leanat {
Expected<TxnResult> decode_txn_result(const Value &);
class FinishResponseOperation {
  Runtime &runtime_;
  TransactOperation &producer_;
  std::size_t destination_bytes_;
  std::optional<ResultHandle> consumer_;
  std::optional<TxnResult> result_;
  std::uint64_t owner_{}, epoch_{};
  bool started_{};

public:
  FinishResponseOperation(Runtime &r, TransactOperation &p, std::size_t bytes = 262144)
      : runtime_(r), producer_(p), destination_bytes_(bytes) {}
  FinishResponseOperation(const FinishResponseOperation &) = delete;
  FinishResponseOperation &operator=(const FinishResponseOperation &) = delete;
  Expected<void> start(const ExecutionContext &, ResultHandle transferred_consumer);
  Expected<bool> advance(const ExecutionContext &);
  Expected<void> cancel(const ExecutionContext &);
  Expected<TxnResult> result() const;
};
} // namespace leanat

namespace leanat {
Expected<void> release_task_result_when_done(EventTxn &, Runtime &, ResultHandle);
}
