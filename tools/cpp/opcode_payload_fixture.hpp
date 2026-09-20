#pragma once
#include "../../tests/support/event_queue_test_access.hpp"
#include "opcode_fixture.hpp"
#include <leanat/payload_services.hpp>

namespace leanat::opcode_test {
namespace payload_fixture_detail {
inline Expected<const Value::Array *> record(const Value &v, std::size_t size) {
  auto p = std::get_if<Value::Array>(&v.data);
  if (!p || p->size() != size)
    return fail(ErrorCode::TypeMismatch, "payload fixture record arity");
  return p;
}
inline Expected<std::uint64_t> number(const Value &v) {
  if (auto p = std::get_if<std::uint64_t>(&v.data))
    return *p;
  return fail(ErrorCode::TypeMismatch, "payload fixture numeric field");
}
inline Expected<bool> boolean(const Value &v) {
  if (auto p = std::get_if<bool>(&v.data))
    return *p;
  return fail(ErrorCode::TypeMismatch, "payload fixture boolean field");
}
inline Expected<Bytes> bytes(const Value &v) {
  if (auto p = std::get_if<Bytes>(&v.data))
    return *p;
  return fail(ErrorCode::TypeMismatch, "payload fixture byte field");
}
inline Expected<Handle> handle(const Value &v, HandleKind kind) {
  auto p = std::get_if<Handle>(&v.data);
  if (!p || p->kind != kind)
    return fail(ErrorCode::TypeMismatch, "payload fixture identity kind");
  return *p;
}
struct Extension {
  std::string name;
  PayloadExtensionRule rule;
  std::optional<Bytes> initial;
};
struct OwnedFixture {
  PayloadShadow shadow;
  PayloadServiceContext services;
  ExecutionContext context;
  PayloadViewKey key;
  PayloadAccessContext access;
  PayloadSnapshot baseline;
  std::vector<Extension> extensions;
  bool target{}, writable{};
  std::uint64_t max_bytes{};
  std::optional<PayloadServiceContext::Scope> scope;
  explicit OwnedFixture(std::size_t max) : shadow(128, max), max_bytes(max) {}

  Expected<Value> read_owned() {
    EventTxn read({}, context);
    auto current = shadow.snapshot(key, read);
    if (!current)
      return current.error();
    Value::Array encoded_extensions;
    for (const auto &e : extensions) {
      auto value = shadow.read_extension(key, e.name, read);
      if (!value)
        return value.error();
      Value::Array fields{Value(Bytes(e.name.begin(), e.name.end())),
                          Value(std::uint64_t(e.rule.max_bytes)),
                          Value(e.rule.request_allowed),
                          Value(e.rule.response_allowed),
                          Value(e.rule.response_writable),
                          value.value() ? Value(*value.value()) : Value{}};
      encoded_extensions.emplace_back(std::move(fields));
    }
    auto &p = current.value();
    return Value(Value::Array{
        Value(key.txn), Value(key.hop), Value(std::uint64_t(key.instance.value)),
        Value(std::uint64_t(key.local_side)), Value(target), Value(writable),
        Value(std::uint64_t(p.command)), Value(p.address), Value(p.streaming_width),
        Value(baseline.data), Value(p.data), Value(p.byte_enable), Value(std::uint64_t(p.status)),
        Value(p.dmi_hint), Value(max_bytes), Value(std::move(encoded_extensions)),
        Value(std::uint64_t(context.connection.value))});
  }
};
} // namespace payload_fixture_detail

// Source setup uses the same owned record codecs as Reference.Storage. No field
// contains an expected answer: the snapshot reconstructs all mutable fields by
// reading the real PayloadShadow after the VM segment commits or rolls back.
inline Expected<ProviderFixture> make_payload_fixture(Runtime &runtime, CoreRuntimeBackend &backend,
                                                      const exec::Project &project,
                                                      const FixtureConfig &config) {
  using namespace payload_fixture_detail;
  auto source = config.environment.find("storage.payload.record");
  auto access_source = config.environment.find("storage.payload.access");
  if (source == config.environment.end() || access_source == config.environment.end())
    return fail(ErrorCode::NotReady, "payload fixture source record/access missing");
  auto fields = record(source->second, 17);
  auto access_fields = record(access_source->second, 9);
  if (!fields)
    return fields.error();
  if (!access_fields)
    return access_fields.error();
  const auto &v = *fields.value();
  auto txn = handle(v[0], HandleKind::Transaction), hop = handle(v[1], HandleKind::Hop);
  auto instance = number(v[2]), side = number(v[3]), command = number(v[6]), address = number(v[7]),
       streaming = number(v[8]), status = number(v[12]), max = number(v[14]);
  auto target = boolean(v[4]), writable = boolean(v[5]), dmi = boolean(v[13]);
  auto baseline = bytes(v[9]), data = bytes(v[10]), mask = bytes(v[11]);
  if (!txn || !hop || !instance || !side || !command || !address || !streaming || !status || !max ||
      !target || !writable || !dmi || !baseline || !data || !mask)
    return fail(ErrorCode::TypeMismatch, "payload fixture source fields");
  if (instance.value() > UINT32_MAX || side.value() > UINT32_MAX || command.value() > 2 ||
      status.value() > 6 || max.value() > SIZE_MAX || !streaming.value() ||
      baseline.value().size() != data.value().size())
    return fail(ErrorCode::InvalidArgument, "payload fixture source range");
  auto connection = number(v[16]);
  if (!connection || connection.value() > UINT32_MAX)
    return fail(ErrorCode::InvalidArgument, "payload fixture connection");
  auto ext_values = std::get_if<Value::Array>(&v[15].data);
  if (!ext_values)
    return fail(ErrorCode::TypeMismatch, "payload fixture extensions");
  auto owned = std::make_shared<OwnedFixture>(std::size_t(max.value()));
  owned->context = config.context;
  owned->context.connection = ConnectionId{std::uint32_t(connection.value())};
  owned->key = {txn.value(), hop.value(), InstanceId{std::uint32_t(instance.value())},
                std::uint32_t(side.value())};
  owned->target = target.value();
  owned->writable = writable.value();
  for (const auto &encoded : *ext_values) {
    auto r = record(encoded, 6);
    if (!r)
      return r.error();
    const auto &e = *r.value();
    auto name = bytes(e[0]);
    auto bound = number(e[1]);
    auto request = boolean(e[2]), response = boolean(e[3]), write = boolean(e[4]);
    if (!name || !bound || !request || !response || !write || bound.value() > SIZE_MAX)
      return fail(ErrorCode::TypeMismatch, "payload fixture extension rule");
    Extension extension{std::string(name.value().begin(), name.value().end()),
                        {std::size_t(bound.value()), request.value(), response.value(),
                         write.value(), false, false},
                        {}};
    if (!std::holds_alternative<std::monostate>(e[5].data)) {
      auto present = bytes(e[5]);
      if (!present)
        return present.error();
      extension.initial = std::move(present.value());
    }
    auto registered = owned->shadow.register_extension(extension.name, extension.rule);
    if (!registered)
      return registered.error();
    if (extension.initial)
      owned->baseline.extensions.emplace(extension.name, *extension.initial);
    owned->extensions.push_back(std::move(extension));
  }
  owned->baseline.command = Command(command.value());
  owned->baseline.address = address.value();
  owned->baseline.streaming_width = streaming.value();
  owned->baseline.status = ResponseStatus(status.value());
  owned->baseline.dmi_hint = dmi.value();
  owned->baseline.data = baseline.value();
  owned->baseline.byte_enable = mask.value();
  auto created = owned->shadow.create(owned->key, owned->baseline, owned->context,
                                      owned->target ? PayloadRole::Target : PayloadRole::Initiator,
                                      owned->writable);
  if (!created)
    return created.error();
  if (data.value() != baseline.value()) {
    EventTxn initialize({}, owned->context);
    auto staged = owned->shadow.buffer_data(owned->key, Value(data.value()), initialize);
    if (!staged)
      return staged.error();
    auto committed = initialize.commit();
    if (!committed)
      return committed.error();
    EventTxn verify({}, owned->context);
    auto actual = owned->shadow.read_data(owned->key, verify);
    if (!actual || actual.value() != Value(data.value()))
      return fail(ErrorCode::InvalidArgument, "payload fixture current data violates mask");
  }
  const auto &a = *access_fields.value();
  auto access_hop = handle(a[0], HandleKind::Hop);
  auto local = number(a[1]), phase = number(a[4]), sync = number(a[5]),
       returned_phase = number(a[6]);
  auto returned = boolean(a[2]), forward = boolean(a[3]), permit = boolean(a[7]),
       validated = boolean(a[8]);
  if (!access_hop || !local || !phase || !sync || !returned_phase || !returned || !forward ||
      !permit || !validated || local.value() > UINT32_MAX || phase.value() > UINT32_MAX ||
      returned_phase.value() > UINT32_MAX || sync.value() > 2)
    return fail(ErrorCode::TypeMismatch, "payload fixture access fields");
  owned->access.hop = access_hop.value();
  owned->access.local_side = std::uint32_t(local.value());
  owned->access.stage = returned.value() ? PayloadAccessStage::Return : PayloadAccessStage::Call;
  owned->access.flow = forward.value() ? Flow::Forward : Flow::Backward;
  owned->access.call_phase = PhaseId{std::uint32_t(phase.value())};
  owned->access.sync = Sync(sync.value());
  if (returned_phase.value())
    owned->access.returned_phase = PhaseId{std::uint32_t(returned_phase.value())};
  owned->access.response_write_permit = permit.value();
  owned->access.validated = validated.value();
  auto registered = register_payload_services(backend, owned->shadow, owned->services, project);
  if (!registered)
    return registered.error();
  auto scope = owned->services.bind(owned->key, owned->access);
  if (!scope)
    return scope.error();
  owned->scope.emplace(std::move(scope.value()));
  ProviderFixture fixture;
  fixture.inputs = config.inputs;
  fixture.lifetime.push_back(owned);
  fixture.snapshot = [owned, &runtime]() {
    std::vector<Json> events;
    for (const auto &slot : testing::EventQueueTestAccess::snapshot(runtime.queue()).slots)
      if (slot.state == "queued" || slot.state == "active" || slot.state == "reserved")
        events.push_back(Json::object({{"state", slot.state}, {"event", observe(slot.queued)}}));
    auto value = owned->read_owned();
    return value ? Json::object(
                       {{"payload", observe(value.value())}, {"events", Json::array(events)}})
                 : Json::object({{"error", observe(value.error())}});
  };
  fixture.identities = [owned]() {
    return Json::object(
        {{"transaction", observe(owned->key.txn)}, {"hop", observe(owned->key.hop)}});
  };
  return fixture;
}
} // namespace leanat::opcode_test
