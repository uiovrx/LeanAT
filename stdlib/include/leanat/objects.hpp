#pragma once
#include "leanat/event_txn.hpp"
namespace leanat {
enum ObjectEffect : std::uint32_t {
  ObjectRead = 1,
  ObjectWrite = 2,
  ObjectPeek = 4,
  ObjectPoke = 8,
  ObjectSchedule = 16,
  ObjectSocket = 32,
  ObjectWait = 64
};
struct MethodDesc {
  MethodId id;
  std::vector<TypeId> arguments, results;
  std::uint32_t effects{};
  std::vector<ContextKind> contexts;
  std::size_t max_argument_bytes{65536};
  std::size_t max_result_bytes{65536};
};
struct ProviderDesc {
  std::uint32_t kind{}, abi{1};
  std::string key, version, hash, reference, build_id, evidence{"tested"};
  std::vector<MethodDesc> methods;
};
struct ObjectDesc {
  ObjectId id;
  std::uint32_t kind{};
  DomainId domain;
  InstanceId instance;
};
struct ObjectResult {
  std::vector<Value> values;
};
class ObjectProvider {
public:
  virtual ~ObjectProvider() = default;
  virtual Expected<ObjectResult> prepare(ObjectId, MethodId, const std::vector<Value> &,
                                         EventTxn &) = 0;
};
class ObjectRegistry {
  struct Entry {
    ProviderDesc desc;
    std::shared_ptr<ObjectProvider> provider;
  };
  std::map<std::uint32_t, Entry> entries_;
  std::map<ObjectId, ObjectDesc> objects_;
  bool frozen_{};
  std::size_t capacity_;

public:
  explicit ObjectRegistry(std::size_t capacity = 128) : capacity_(capacity) {}
  Expected<void> register_provider(ProviderDesc, std::shared_ptr<ObjectProvider>);
  Expected<void> register_object(ObjectDesc);
  Expected<void> freeze();
  Expected<MethodDesc> lookup(std::uint32_t, MethodId) const;
  Expected<ObjectResult> prepare(ObjectId, MethodId, const std::vector<Value> &,
                                 const std::vector<TypeId> &, EventTxn &);
};
Expected<void> object_context(const EventTxn &, bool debug = false);
class Memory;
class RegisterBank;
class Resource;
class BoundedQueue;
class Pipeline;
using StandardObject =
    std::variant<std::shared_ptr<Memory>, std::shared_ptr<RegisterBank>, std::shared_ptr<Resource>,
                 std::shared_ptr<BoundedQueue>, std::shared_ptr<Pipeline>>;
struct StandardObjectBinding {
  ObjectDesc desc;
  StandardObject object;
};
// Type IDs used by this versioned intrinsic ABI: 1=u64, 2=bytes, 3=handle,
// 4=owning Value, 5=bool. All calls additionally validate variant structure.
enum class StandardIntrinsic : std::uint32_t {
  Transfer = 1,
  ReadBytes = 2,
  WriteBytes = 3,
  Clear = 4,
  Reset = 5,
  Debug = 6,
  ReadField = 7,
  UpdateField = 8,
  Reserve = 9,
  ReserveDefault = 10,
  Cancel = 11,
  Complete = 12,
  Inspect = 13,
  Push = 14,
  Pop = 15,
  Size = 16,
  Submit = 17,
  Ready = 18,
  PushOwned = 19,
  RemoveOwned = 20,
  TransferDrain = 21,
  MarkPublished = 22
};
Expected<void> register_standard_objects(ObjectRegistry &,
                                         const std::vector<StandardObjectBinding> &);
} // namespace leanat
