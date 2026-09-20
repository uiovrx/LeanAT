#pragma once
#include "leanat/runtime_services.hpp"
#include "objects.hpp"
namespace leanat {
struct ObjectServiceConfig {
  std::uint32_t first_service_id{1000};
  std::size_t max_bytes{4096}, max_entries{128};
  // Exact pre-existing ExecIR type for each ValueOnly queue element.
  std::map<ObjectId, std::uint32_t> queue_element_types;
  std::map<ObjectId, std::array<std::uint8_t, 32>> queue_element_hashes;
  struct QueueProtocolFields {
    std::uint32_t transaction_field{}, hop_field{};
  };
  std::map<ObjectId, QueueProtocolFields> queue_protocol_fields;
};
struct ObjectServiceEntry {
  ObjectId object;
  StandardIntrinsic method;
  exec::ServiceSignature signature;
};
class ObjectServices : public std::enable_shared_from_this<ObjectServices> {
  struct Impl;
  std::shared_ptr<Impl> impl_;
  explicit ObjectServices(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}

public:
  static Expected<std::array<std::uint8_t, 32>> type_hash(const exec::Project &, std::uint32_t);
  // Appends concrete bounded types/services atomically; existing type indices are preserved.
  static Expected<std::shared_ptr<ObjectServices>>
  create(exec::Project &, const std::vector<StandardObjectBinding> &, ObjectServiceConfig = {});
  static Expected<std::shared_ptr<ObjectServices>>
  bind_existing(const exec::Project &, const std::vector<StandardObjectBinding> &,
                ObjectServiceConfig = {});
  Expected<void> register_into(CoreRuntimeBackend &);
  const std::vector<ObjectServiceEntry> &entries() const;
  Expected<exec::ServiceSignature> find(ObjectId, StandardIntrinsic) const;
  Expected<void> observe_queue_publication(ObjectId, Runtime &, EventTxn &);
  struct QueueCancelReport {
    std::vector<std::uint64_t> removed, transferred;
  };
  Expected<QueueCancelReport> cancel_queue_scope(ObjectId, Runtime &, EventTxn &, Handle scope,
                                                 Handle drain, std::size_t drain_capacity);
};
} // namespace leanat
