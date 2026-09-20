#pragma once
#include "opcode_fixture.hpp"
#include "opcode_json.hpp"

namespace leanat::opcode_test {
inline void exact_fields(const InputJson &j, std::initializer_list<const char *> names) {
  if (j.object().size() != names.size())
    throw std::runtime_error("unexpected value field");
  for (auto name : names)
    (void)j.at(name);
}
inline uint64_t decimal_string(const InputJson &j, uint64_t limit = UINT64_MAX) {
  (void)j.string();
  return j.natural(limit);
}
inline Value decode_value(const InputJson &j, size_t depth = 0) {
  if (depth > 64)
    throw std::runtime_error("value depth bound");
  const auto kind = j.at("kind").string();
  if (kind == "unit") {
    exact_fields(j, {"kind"});
    return Value{};
  }
  if (kind == "bool") {
    exact_fields(j, {"kind", "value"});
    return Value{j.at("value").boolean()};
  }
  if (kind == "bits") {
    exact_fields(j, {"kind", "width", "value"});
    auto width = j.at("width").natural(64);
    if (!width)
      throw std::runtime_error("zero bits width");
    auto n = decimal_string(j.at("value"));
    if (width < 64 && n >= (uint64_t{1} << width))
      throw std::runtime_error("bits value overflow");
    return Value{n};
  }
  if (kind == "bytes") {
    exact_fields(j, {"kind", "data"});
    Bytes b;
    for (const auto &v : j.at("data").array())
      b.push_back(uint8_t(v.natural(255)));
    return Value{b};
  }
  if (kind == "record" || kind == "vec" || kind == "variant") {
    if (kind == "variant")
      exact_fields(j, {"kind", "tag", "fields"});
    else if (kind == "vec")
      exact_fields(j, {"kind", "values"});
    else
      exact_fields(j, {"kind", "fields"});
    Value::Array values;
    for (const auto &v : j.at(kind == "vec" ? "values" : "fields").array())
      values.push_back(decode_value(v, depth + 1));
    if (kind == "variant")
      return Value{Value::Array{Value{j.at("tag").natural(UINT32_MAX)}, Value{values}}};
    return Value{values};
  }
  if (kind == "handle") {
    exact_fields(j, {"kind", "identity"});
    const auto &i = j.at("identity");
    exact_fields(i, {"kind", "domain", "store", "slot", "generation", "owner"});
    Handle h;
    h.kind = HandleKind(i.at("kind").natural(14));
    h.domain = DomainId{uint32_t(decimal_string(i.at("domain"), UINT32_MAX))};
    h.store = uint32_t(decimal_string(i.at("store"), UINT32_MAX));
    h.slot = uint32_t(decimal_string(i.at("slot"), UINT32_MAX));
    h.generation = decimal_string(i.at("generation"));
    h.owner = decimal_string(i.at("owner"));
    return Value{h};
  }
  throw std::runtime_error("unsupported rich value kind: " + kind);
}
inline std::string rich_handle(Handle h) {
  return "{\"kind\":" + std::to_string(unsigned(h.kind)) + ",\"domain\":\"" +
         std::to_string(h.domain.value) + "\",\"store\":\"" + std::to_string(h.store) +
         "\",\"slot\":\"" + std::to_string(h.slot) + "\",\"generation\":\"" +
         std::to_string(h.generation) + "\",\"owner\":\"" + std::to_string(h.owner) + "\"}";
}
inline std::string typed_value(const exec::Project &p, const Value &value, uint32_t type,
                               size_t depth = 0) {
  if (depth > 64 || type >= p.types.size())
    throw std::runtime_error("typed value schema bound");
  const auto &t = p.types[type];
  if (!exec::conforms(p, type, value))
    throw std::runtime_error("observed value violates actual declared type");
  using K = exec::TypeKind;
  if (t.kind == K::Unit)
    return "{\"kind\":\"unit\"}";
  if (t.kind == K::Bool)
    return std::string("{\"kind\":\"bool\",\"value\":") +
           (std::get<bool>(value.data) ? "true}" : "false}");
  if (t.kind == K::Bits || t.kind == K::Fin)
    return "{\"kind\":\"bits\",\"width\":" + std::to_string(t.kind == K::Fin ? 64 : t.bound) +
           ",\"value\":\"" + std::to_string(std::get<uint64_t>(value.data)) + "\"}";
  if (t.kind == K::Handle)
    return "{\"kind\":\"handle\",\"identity\":" + rich_handle(std::get<Handle>(value.data)) + "}";
  if (t.kind == K::Bytes) {
    std::string out = "{\"kind\":\"bytes\",\"data\":[";
    const auto &b = std::get<Bytes>(value.data);
    for (size_t n = 0; n < b.size(); ++n) {
      if (n)
        out += ',';
      out += std::to_string(b[n]);
    }
    return out + "]}";
  }
  const auto &a = std::get<Value::Array>(value.data);
  const Value::Array *elements = &a;
  std::vector<uint32_t> fields;
  std::string out;
  if (t.kind == K::Variant) {
    auto tag = std::get<uint64_t>(a[0].data);
    elements = &std::get<Value::Array>(a.at(1).data);
    fields = t.constructors.at(size_t(tag));
    out = "{\"kind\":\"variant\",\"tag\":" + std::to_string(tag) + ",\"fields\":[";
  } else if (t.kind == K::Record) {
    fields = t.fields;
    out = "{\"kind\":\"record\",\"fields\":[";
  } else {
    fields.assign(a.size(), t.fields.at(0));
    out = "{\"kind\":\"vec\",\"values\":[";
  }
  for (size_t n = 0; n < elements->size(); ++n) {
    if (n)
      out += ',';
    out += typed_value(p, elements->at(n), fields.at(n), depth + 1);
  }
  return out + "]}";
}
inline std::string typed_values(const exec::Project &p, const std::vector<Value> &v,
                                const std::vector<uint32_t> &types) {
  if (v.size() != types.size())
    throw std::runtime_error("observed result arity mismatch");
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i)
      out += ',';
    out += typed_value(p, v[i], types[i]);
  }
  return out + "]";
}
} // namespace leanat::opcode_test
