#include "leanat/payload_services.hpp"
#include "leanat/descriptor.hpp"
namespace leanat {
PayloadServiceContext::Scope::Scope(Scope &&other) noexcept
    : context_(other.context_), generation_(other.generation_),
      previous_generation_(other.previous_generation_), previous_(std::move(other.previous_)) {
  other.context_ = nullptr;
}
PayloadServiceContext::Scope &PayloadServiceContext::Scope::operator=(Scope &&other) noexcept {
  if (this != &other) {
    if (context_)
      context_->close(generation_, previous_generation_, std::move(previous_));
    context_ = other.context_;
    generation_ = other.generation_;
    previous_generation_ = other.previous_generation_;
    previous_ = std::move(other.previous_);
    other.context_ = nullptr;
  }
  return *this;
}
PayloadServiceContext::Scope::~Scope() {
  if (context_)
    context_->close(generation_, previous_generation_, std::move(previous_));
}
void PayloadServiceContext::close(std::uint64_t generation, std::uint64_t previous_generation,
                                  std::optional<Binding> previous) noexcept {
  if (active_generation_ != generation || !depth_) {
    poisoned_ = true;
    current_.reset();
    depth_ = 0;
    active_generation_ = 0;
    return;
  }
  current_ = std::move(previous);
  active_generation_ = previous_generation;
  --depth_;
}
Expected<PayloadServiceContext::Scope> PayloadServiceContext::bind(PayloadViewKey key,
                                                                   PayloadAccessContext access) {
  if (poisoned_)
    return fail(ErrorCode::InvalidState, "payload VM scope destruction order");
  if (depth_ == max_depth_)
    return fail(ErrorCode::Capacity, "payload VM scope depth");
  if (generation_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "payload scope generation");
  if (!access.validated || access.hop != key.hop || access.local_side != key.local_side)
    return fail(ErrorCode::WrongOwner, "payload scope binding");
  auto previous = std::move(current_);
  auto old_generation = active_generation_;
  current_ = Binding{std::move(key), std::move(access)};
  active_generation_ = ++generation_;
  ++depth_;
  return Scope(this, active_generation_, old_generation, std::move(previous));
}
Expected<PayloadServiceContext::Binding> PayloadServiceContext::current() const {
  if (poisoned_)
    return fail(ErrorCode::InvalidState, "payload VM scope destruction order");
  if (!current_)
    return fail(ErrorCode::NotReady, "payload VM scope not bound");
  return *current_;
}
namespace {
struct Operation {
  bool write{}, extension{};
  PayloadField field{};
  std::string name;
};
Expected<Operation> parse(const std::string &key) {
  Operation op;
  std::string stem;
  if (key.size() > 6 && key.compare(key.size() - 6, 6, ".write") == 0) {
    op.write = true;
    stem = key.substr(0, key.size() - 6);
  } else if (key.size() > 4 && key.compare(key.size() - 4, 4, ".get") == 0)
    stem = key.substr(0, key.size() - 4);
  else
    return fail(ErrorCode::Schema, "payload provider suffix");
  const std::string prefix = "leanat.payload.", ext = "leanat.extension.";
  if (stem.compare(0, ext.size(), ext) == 0) {
    op.extension = true;
    op.name = stem.substr(ext.size());
    if (op.name.empty())
      return fail(ErrorCode::Schema, "empty extension key");
    return op;
  }
  if (stem.compare(0, prefix.size(), prefix) != 0)
    return fail(ErrorCode::Unsupported, "not a payload provider");
  op.name = stem.substr(prefix.size());
  const std::map<std::string, PayloadField> fields{
      {"command", PayloadField::Command},
      {"address", PayloadField::Address},
      {"data", PayloadField::Data},
      {"streaming_width", PayloadField::StreamingWidth},
      {"byte_enable", PayloadField::ByteEnable},
      {"status", PayloadField::Status},
      {"dmi_hint", PayloadField::DmiHint}};
  auto f = fields.find(op.name);
  if (f == fields.end())
    return fail(ErrorCode::Unsupported, "payload field provider");
  op.field = f->second;
  if (op.write && op.field != PayloadField::Data && op.field != PayloadField::Status &&
      op.field != PayloadField::DmiHint)
    return fail(ErrorCode::InvalidState, "provider writes frozen request field");
  return op;
}
Expected<std::string> value_shape(const Operation &op, const std::vector<exec::Type> &types,
                                  std::uint32_t type) {
  if (type >= types.size())
    return fail(ErrorCode::Schema, "payload value type missing");
  auto &t = types[type];
  if (op.extension && !op.write) {
    if (t.kind != exec::TypeKind::Variant || t.constructors.size() != 2 ||
        !t.constructors[0].empty() || t.constructors[1].size() != 1 ||
        t.constructors[1][0] >= types.size())
      return fail(ErrorCode::Schema, "extension get requires Option Bytes");
    auto &bytes = types[t.constructors[1][0]];
    if (bytes.kind != exec::TypeKind::Bytes)
      return fail(ErrorCode::Schema, "extension option payload");
    return std::string("option(bytes:") + std::to_string(bytes.bound) + ")";
  }
  if (op.extension || op.field == PayloadField::Data || op.field == PayloadField::ByteEnable) {
    if (t.kind != exec::TypeKind::Bytes)
      return fail(ErrorCode::Schema, "payload bytes type");
    return std::string("bytes:") + std::to_string(t.bound);
  }
  if (op.field == PayloadField::DmiHint) {
    if (t.kind != exec::TypeKind::Bool)
      return fail(ErrorCode::Schema, "payload hint bool type");
    return std::string("bool");
  }
  if (t.kind != exec::TypeKind::Bits || t.bound != 64)
    return fail(ErrorCode::Schema, "payload numeric type must be UInt64");
  return std::string("u64");
}
} // namespace
Expected<exec::ServiceSignature> payload_service_signature(std::uint32_t id, const std::string &key,
                                                           const std::vector<exec::Type> &types,
                                                           std::uint32_t txn, std::uint32_t value,
                                                           std::uint32_t unit) {
  auto op = parse(key);
  if (!op)
    return op.error();
  if (txn >= types.size() || types[txn].kind != exec::TypeKind::Handle ||
      types[txn].bound != static_cast<unsigned>(HandleKind::Transaction))
    return fail(ErrorCode::Schema, "payload transaction type");
  auto shape = value_shape(op.value(), types, value);
  if (!shape)
    return shape.error();
  if (op.value().write && (unit >= types.size() || types[unit].kind != exec::TypeKind::Unit))
    return fail(ErrorCode::Schema, "payload write Unit result");
  exec::ServiceSignature s;
  s.id = id;
  s.op = op.value().extension
             ? (op.value().write ? exec::Op::BufferExtensionWrite : exec::Op::ExtensionGet)
             : (op.value().write ? exec::Op::BufferPayloadWrite : exec::Op::PayloadGet);
  s.input_types = {txn};
  if (op.value().write)
    s.input_types.push_back(value);
  s.result_types = {op.value().write ? unit : value};
  s.context_mask = 7;
  s.effect_mask = exec::Transaction;
  s.extra_fuel = 1;
  s.provider_key = key;
  s.provider_version = "1";
  auto canonical = std::string("LeanAT.Payload.v1|") + key + "|txn" +
                   (op.value().write ? "," + shape.value() + "->unit" : "->" + shape.value());
  s.abi_hash = exec::sha256(Bytes(canonical.begin(), canonical.end()));
  return s;
}
Expected<void> register_payload_services(CoreRuntimeBackend &backend, PayloadShadow &shadow,
                                         PayloadServiceContext &context,
                                         const exec::Project &project) {
  auto schema = std::make_shared<exec::Project>();
  schema->types = project.types;
  std::vector<std::pair<exec::ServiceSignature, CoreRuntimeBackend::Provider>> providers;
  for (const auto &s : project.services) {
    if (s.provider_key.compare(0, 15, "leanat.payload.") != 0 &&
        s.provider_key.compare(0, 17, "leanat.extension.") != 0)
      continue;
    auto operation = parse(s.provider_key);
    if (!operation)
      return operation.error();
    auto op = operation.value();
    if (s.input_types.size() != (op.write ? 2u : 1u) || s.result_types.size() != 1)
      return fail(ErrorCode::Schema, "payload service arity");
    auto value = op.write ? s.input_types[1] : s.result_types[0];
    auto expected = payload_service_signature(s.id, s.provider_key, project.types, s.input_types[0],
                                              value, s.result_types[0]);
    if (!expected)
      return expected.error();
    if (expected.value() != s)
      return fail(ErrorCode::Integrity, "payload provider ABI differs from descriptor");
    if (op.extension) {
      auto rule = shadow.describe_extension(op.name);
      if (!rule)
        return rule.error();
      auto type = value;
      if (!op.write)
        type = project.types[type].constructors[1][0];
      if (project.types[type].bound != rule.value().max_bytes)
        return fail(ErrorCode::Schema, "extension codec bound mismatch");
      if (op.write && !rule.value().response_writable)
        return fail(ErrorCode::InvalidState, "extension codec is not writable");
    }
    providers.emplace_back(
        s,
        [&shadow, &context, schema, s, op](const std::vector<Value> &args,
                                           const ExecutionContext &execution,
                                           EventTxn &txn) -> Expected<std::vector<Value>> {
          if (!(s.context_mask & (1u << static_cast<unsigned>(execution.kind))))
            return fail(ErrorCode::InvalidState, "payload service context");
          if (args.size() != s.input_types.size())
            return fail(ErrorCode::TypeMismatch, "payload service arguments");
          for (std::size_t i = 0; i < args.size(); ++i)
            if (!exec::conforms(*schema, s.input_types[i], args[i]))
              return fail(ErrorCode::TypeMismatch, "payload service argument shape");
          auto bound = context.current();
          if (!bound)
            return bound.error();
          auto handle = std::get_if<Handle>(&args[0].data);
          if (!handle || *handle != bound.value().key.txn)
            return fail(ErrorCode::WrongOwner, "payload transaction is not current view");
          const auto &key = bound.value().key;
          const auto &access = bound.value().access;
          auto permission = shadow.validate_access(key, access, txn);
          if (!permission)
            return permission.error();
          Value result;
          if (op.extension) {
            if (op.write) {
              auto written =
                  shadow.buffer_extension(key, op.name, std::get<Bytes>(args[1].data), access, txn);
              if (!written)
                return written.error();
            } else {
              auto read = shadow.read_extension(key, op.name, txn);
              if (!read)
                return read.error();
              Value::Array fields;
              if (read.value())
                fields.push_back(Value(*read.value()));
              result = Value(Value::Array{Value(static_cast<std::uint64_t>(read.value() ? 1 : 0)),
                                          Value(std::move(fields))});
            }
          } else if (op.write) {
            auto written = shadow.buffer_field(key, op.field, args[1], access, txn);
            if (!written)
              return written.error();
          } else {
            auto read = shadow.read_field(key, op.field, txn);
            if (!read)
              return read.error();
            result = std::move(read.value());
          }
          if (!exec::conforms(*schema, s.result_types[0], result))
            return fail(ErrorCode::Capacity, "payload result exceeds descriptor shape");
          return std::vector<Value>{std::move(result)};
        });
  }
  return backend.register_providers(std::move(providers));
}
} // namespace leanat
