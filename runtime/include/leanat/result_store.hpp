#pragma once
#include "common.hpp"
#include "event_txn.hpp"
namespace leanat {
struct ResultHandle {
  Handle result;
  Handle consumer;
};
using ConsumerToken = Handle;
struct ResultOwnerHandle {
  Handle result;
};
struct ResultCreate {
  Handle source;
  TypeId type{};
  std::size_t max_bytes{4096};
  std::uint64_t producer_owner{}, consumer_owner{};
};
struct PublicationContext {
  Handle source;
  ReadyKey ready;
  bool validated{};
};
struct PublicationReceipt {
  Handle result;
  ReadyKey ready;
};
struct ReservedResult {
  ResultOwnerHandle reservation;
  ResultHandle consumer;
};
struct ResultOwnership {
  std::size_t consumer_count{}, pin_count{};
  bool published{}, owner_released{}, publishing{};
};
struct ResultSlotSnapshot {
  Handle identity;
  bool alive{}, producer_alive{}, publishing{};
  ResultCreate create;
  std::optional<Value> value;
  std::optional<ReadyKey> ready;
  std::size_t consumer_count{}, pin_count{};
};
struct ResultConsumerSnapshot {
  Handle identity, result;
  bool active{};
};
struct ResultStoreSnapshot {
  std::vector<ResultSlotSnapshot> result_slots;
  std::vector<ResultConsumerSnapshot> consumer_slots;
  std::uint64_t next_result_generation{}, next_consumer_generation{};
  std::size_t pin_count{}, pin_limit{};
};
struct ResultSubscription {
  ResultHandle consumer;
  std::optional<ReadyKey> latched_ready;
};
enum class ReleasePolicy { CurrentConsumer, DropOnTerminal };
class ResultStore;
class PinnedResultView {
  friend class ResultStore;
  std::shared_ptr<const Value> value_;
  std::shared_ptr<void> guard_;
  PinnedResultView(std::shared_ptr<const Value> v, std::shared_ptr<void> g)
      : value_(std::move(v)), guard_(std::move(g)) {};

public:
  const Value &value() const {
    return *value_;
  }
};
class ResultStore {
  struct Impl;
  struct Staged;
  std::shared_ptr<Impl> impl_;
  Expected<Staged *> stage(EventTxn &, std::size_t extra_bytes = 0);
  explicit ResultStore(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

public:
  explicit ResultStore(std::size_t results = 128, std::size_t consumers = 512,
                       std::size_t pins = 256, DomainId domain = DomainId{},
                       std::uint32_t store = 2);
  Expected<ReservedResult> reserve(ResultCreate);
  Expected<ResultCreate> describe(ResultHandle) const;
  Expected<ResultCreate> prepare_describe(const EventTxn &, ResultHandle) const;
  Expected<ReservedResult> prepare_reserve(EventTxn &, ResultCreate);
  Expected<PublicationReceipt> prepare_publish_in(EventTxn &, ResultOwnerHandle, Value,
                                                  PublicationContext);
  Expected<void> prepare_release_owner(EventTxn &, ResultOwnerHandle);
  Expected<Value> prepare_read(const EventTxn &, ResultHandle,
                               std::size_t destination_bytes = SIZE_MAX) const;
  Expected<ReadyKey> prepare_ready(const EventTxn &, ResultHandle) const;
  Expected<bool> prepare_alive(const EventTxn &, ResultOwnerHandle) const;
  Expected<ResultHandle> prepare_retain(EventTxn &, ResultHandle, std::uint64_t owner);
  Expected<ResultHandle> prepare_transfer(EventTxn &, ResultHandle, std::uint64_t owner);
  Expected<void> prepare_release(EventTxn &, ResultHandle);
  Expected<std::unique_ptr<PreparedParticipant>> prepare_publish(ResultOwnerHandle, Value,
                                                                 PublicationContext);
  Expected<PublicationReceipt> publish(ResultOwnerHandle, Value, PublicationContext);
  Expected<ResultHandle> retain(ResultHandle, std::uint64_t owner);
  Expected<ResultSubscription> subscribe(ResultOwnerHandle, std::uint64_t owner);
  Expected<Value>
  read(ResultHandle, std::size_t destination_bytes = std::numeric_limits<std::size_t>::max()) const;
  Expected<Value> take(ResultHandle,
                       std::size_t destination_bytes = std::numeric_limits<std::size_t>::max());
  Expected<PinnedResultView> pin(ResultHandle);
  Expected<void> release(ResultHandle, ReleasePolicy = ReleasePolicy::CurrentConsumer);
  Expected<void> release_owner(ResultOwnerHandle);
  Expected<ReadyKey> published_ready(ResultHandle) const;
  bool alive(ResultOwnerHandle) const;
  Expected<ResultOwnership> inspect_ownership(ResultOwnerHandle) const;
  ResultStoreSnapshot snapshot() const;
  std::size_t occupied() const;
};
std::size_t owned_value_bytes(const Value &);
} // namespace leanat
namespace leanat {
enum class PayloadRole { Initiator, Target };
enum class PayloadAccessStage { Call, Return };
struct PayloadAccessContext {
  Handle hop;
  std::uint32_t local_side{};
  CallId call;
  PayloadAccessStage stage{PayloadAccessStage::Call};
  Flow flow{Flow::Forward};
  PhaseId call_phase{begin_req};
  Sync sync{Sync::Accepted};
  std::optional<PhaseId> returned_phase;
  bool validated{}, response_write_permit{};
};
enum class PayloadField { Command, Address, Data, StreamingWidth, ByteEnable, Status, DmiHint };
struct PayloadExtensionRule {
  std::size_t max_bytes{4096};
  bool request_allowed{true}, response_allowed{}, response_writable{};
  bool request_required{}, response_required{};
};
struct PayloadViewKey {
  Handle txn, hop;
  InstanceId instance;
  std::uint32_t local_side{};
  friend bool operator<(const PayloadViewKey &a, const PayloadViewKey &b) {
    return std::tie(a.txn, a.hop, a.instance, a.local_side) <
           std::tie(b.txn, b.hop, b.instance, b.local_side);
  }
};
// Distinct checked local views: sharing a transport id grants no shadow access.
class PayloadShadow {
  struct View {
    PayloadSnapshot baseline;
    ExecutionContext context;
    PayloadRole role;
    VersionedCell data, status, dmi, response_latched;
    std::map<std::string, VersionedCell> extensions;
    bool writable;
  };
  std::map<PayloadViewKey, View> views_;
  std::size_t capacity_, max_bytes_;
  std::map<std::string, PayloadExtensionRule> extension_rules_;
  Expected<View *> resolve(const PayloadViewKey &, const EventTxn &);
  Expected<View *> resolve(const PayloadViewKey &, const PayloadAccessContext &, const EventTxn &);
  Expected<void> check_extensions(const std::map<std::string, Bytes> &, bool response) const;

public:
  explicit PayloadShadow(std::size_t capacity = 128, std::size_t max_bytes = 65536)
      : capacity_(capacity), max_bytes_(max_bytes) {}
  Expected<void> create(PayloadViewKey, PayloadSnapshot, ExecutionContext, PayloadRole,
                        bool response_writable);
  Expected<void> register_extension(std::string, PayloadExtensionRule);
  Expected<PayloadExtensionRule> describe_extension(const std::string &) const;
  Expected<Value> read_field(const PayloadViewKey &, PayloadField, const EventTxn &);
  Expected<void> buffer_field(const PayloadViewKey &, PayloadField, Value,
                              const PayloadAccessContext &, EventTxn &);
  Expected<std::optional<Bytes>> read_extension(const PayloadViewKey &, const std::string &,
                                                const EventTxn &);
  Expected<void> buffer_extension(const PayloadViewKey &, const std::string &, Bytes,
                                  const PayloadAccessContext &, EventTxn &);
  Expected<PayloadSnapshot> project_call(const PayloadViewKey &, const PayloadAccessContext &,
                                         const EventTxn &);
  Expected<ResponseSnapshot> project_response(const PayloadViewKey &, const PayloadAccessContext &,
                                              const EventTxn &);
  Expected<Value> read_data(const PayloadViewKey &, const EventTxn &);
  Expected<PayloadSnapshot> snapshot(const PayloadViewKey &, const EventTxn &);
  Expected<void> validate_target_access(const PayloadViewKey &, const EventTxn &,
                                        bool writing_response);
  Expected<void> validate_access(const PayloadViewKey &, const PayloadAccessContext &,
                                 const EventTxn &);
  Expected<void> buffer_data(const PayloadViewKey &, Value, EventTxn &);
  Expected<void> deliver_response(const PayloadViewKey &, ResponseSnapshot, EventTxn &);
  Expected<void> deliver_response(const PayloadViewKey &, ResponseSnapshot,
                                  const PayloadAccessContext &, EventTxn &);
  Expected<void> erase(const PayloadViewKey &);
};
} // namespace leanat
namespace leanat {
enum class StorageMilestoneKind { RequestReleased, ResponseReady, Terminal };
struct MilestoneValue {
  ReadyKey ready;
  Value value;
  bool implicit{};
  CallId origin;
};
class MilestoneStore {
  std::size_t capacity_;
  std::size_t max_bytes_;
  std::map<std::pair<Handle, StorageMilestoneKind>, MilestoneValue> values_;

public:
  explicit MilestoneStore(std::size_t capacity = 384, std::size_t max_bytes = 4096)
      : capacity_(capacity), max_bytes_(max_bytes) {}
  Expected<void> mark(Handle hop, StorageMilestoneKind kind, MilestoneValue value,
                      bool validated_exchange);
  Expected<MilestoneValue> read(Handle hop, StorageMilestoneKind kind) const;
  Expected<void> release(Handle hop);
};
} // namespace leanat
