#include "leanat/descriptor.hpp"
#include <cstring>
namespace leanat::exec {
std::array<std::uint8_t, 32> sha256(const Bytes &input) {
  static constexpr std::uint32_t K[] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  std::uint32_t h[] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  Bytes data = input;
  auto bits = std::uint64_t(data.size()) * 8;
  data.push_back(128);
  while (data.size() % 64 != 56)
    data.push_back(0);
  for (int i = 7; i >= 0; --i)
    data.push_back(std::uint8_t(bits >> (8 * i)));
  auto rot = [](std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); };
  for (std::size_t off = 0; off < data.size(); off += 64) {
    std::uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
      w[i] = 0;
      for (unsigned j = 0; j < 4; ++j)
        w[i] = (w[i] << 8) | data[off + i * 4 + j];
    }
    for (unsigned i = 16; i < 64; ++i) {
      auto s0 = rot(w[i - 15], 7) ^ rot(w[i - 15], 18) ^ (w[i - 15] >> 3),
           s1 = rot(w[i - 2], 17) ^ rot(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], z = h[7];
    for (unsigned i = 0; i < 64; ++i) {
      auto s1 = rot(e, 6) ^ rot(e, 11) ^ rot(e, 25), ch = (e & f) ^ (~e & g),
           t1 = z + s1 + ch + K[i] + w[i], s0 = rot(a, 2) ^ rot(a, 13) ^ rot(a, 22),
           maj = (a & b) ^ (a & c) ^ (b & c), t2 = s0 + maj;
      z = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += z;
  }
  std::array<std::uint8_t, 32> out{};
  for (unsigned i = 0; i < 8; ++i)
    for (unsigned j = 0; j < 4; ++j)
      out[i * 4 + j] = std::uint8_t(h[i] >> (24 - j * 8));
  return out;
}
namespace {
std::array<std::uint8_t, 32> schema_hash(unsigned version) {
  std::string s = version == 1   ? "LeanAT.ExecIR.v1.core16"
                  : version == 2 ? "LeanAT.ExecIR.v2"
                  : version == 3 ? "LeanAT.ExecIR.v3"
                  : version == 4 ? "LeanAT.ExecIR.v4"
                                 : "LeanAT.ExecIR.v5";
  return sha256(Bytes(s.begin(), s.end()));
}
struct Writer {
  Bytes b;
  void n(std::uint64_t x, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
      b.push_back(std::uint8_t(x >> (8 * i)));
  }
  void str(const std::string &s) {
    n(s.size(), 4);
    b.insert(b.end(), s.begin(), s.end());
  }
  template <class T, class F> void vec(const std::vector<T> &v, F f) {
    n(v.size(), 4);
    for (auto &x : v)
      f(x);
  }
  void reg(Reg r) {
    n(r.id, 4);
    n(r.type, 4);
  }
  void regs(const std::vector<Reg> &r) {
    vec(r, [&](Reg x) { reg(x); });
  }
  void ids(const std::vector<std::uint32_t> &r) {
    vec(r, [&](auto x) { n(x, 4); });
  }
  void value(const Project &p, std::optional<std::uint32_t> tid, const Value &v) {
    if (!tid) {
      n(0, 1);
      return;
    }
    auto &t = p.types.at(*tid);
    switch (t.kind) {
    case TypeKind::Bytes:
      n(6, 1);
      vec(std::get<Bytes>(v.data), [&](auto x) { n(x, 1); });
      break;
    case TypeKind::Handle: {
      n(7, 1);
      auto h = std::get<Handle>(v.data);
      n(static_cast<unsigned>(h.kind), 1);
      n(h.domain.value, 4);
      n(h.store, 4);
      n(h.slot, 4);
      n(h.generation, 8);
      n(h.owner, 8);
      break;
    }
    case TypeKind::Unit:
      n(0, 1);
      break;
    case TypeKind::Bool:
      n(1, 1);
      n(std::get<bool>(v.data), 1);
      break;
    case TypeKind::Bits:
    case TypeKind::Fin:
      n(2, 1);
      n(t.kind == TypeKind::Bits ? t.bound : 64, 4);
      n(std::get<std::uint64_t>(v.data), 8);
      break;
    case TypeKind::Record: {
      n(3, 1);
      auto &a = std::get<Value::Array>(v.data);
      n(a.size(), 4);
      for (std::size_t i = 0; i < a.size(); ++i)
        value(p, t.fields[i], a[i]);
      break;
    }
    case TypeKind::Variant: {
      n(4, 1);
      auto &a = std::get<Value::Array>(v.data);
      auto tag = std::get<std::uint64_t>(a[0].data);
      n(tag, 8);
      auto &fs = std::get<Value::Array>(a[1].data);
      n(fs.size(), 4);
      for (std::size_t i = 0; i < fs.size(); ++i)
        value(p, t.constructors[tag][i], fs[i]);
      break;
    }
    case TypeKind::Vec:
    case TypeKind::BoundedVec:
      n(5, 1);
      vec(std::get<Value::Array>(v.data), [&](auto &x) { value(p, t.fields[0], x); });
      break;
    }
  }
  void edge(const Edge &e) {
    n(e.target, 4);
    regs(e.args);
  }
  void term(const Terminator &t) {
    n(static_cast<unsigned>(t.kind), 1);
    switch (t.kind) {
    case TermKind::TransportReturn:
      reg(t.value);
      break;
    case TermKind::Suspend:
      reg(t.value);
      n(t.yes.target, 4);
      regs(t.values);
      break;
    case TermKind::Jump:
      edge(t.yes);
      break;
    case TermKind::Branch:
      reg(t.value);
      edge(t.yes);
      edge(t.no);
      break;
    case TermKind::Switch:
      reg(t.value);
      vec(t.cases, [&](auto &c) {
        n(c.first, 8);
        edge(c.second);
      });
      edge(t.no);
      break;
    case TermKind::Return:
      regs(t.values);
      break;
    case TermKind::Fail:
      str(t.error);
      break;
    default:
      throw std::runtime_error("unsupported terminator");
    }
  }
  void source(const MetadataSource &s) {
    str(s.file);
    n(s.line, 4);
    n(s.column, 4);
  }
  void port(const Project &p, const PortDesc &v) {
    n(v.id, 4);
    n(static_cast<unsigned>(v.direction), 1);
    n(v.type_id, 4);
    n(bool(v.initial), 1);
    if (v.initial)
      value(p, v.type_id, *v.initial);
    source(v.source);
  }
  std::uint32_t param_type(const Project &p, std::uint32_t component, std::uint32_t id) {
    for (auto &c : p.components)
      if (c.id == component)
        for (auto &v : c.parameters)
          if (v.id == id)
            return v.type_id;
    throw std::runtime_error("config parameter type");
  }
  void config(const Project &p, std::uint32_t component, const ResolvedConfig &cfg) {
    vec(cfg, [&](auto &v) {
      n(v.first, 4);
      value(p, param_type(p, component, v.first), v.second);
    });
  }
  void endpoint_ref(const EndpointRef &r) {
    n(r.instance_id, 4);
    n(r.endpoint, 4);
    n(r.binding_index, 4);
  }
  void port_ref(const PortRef &r) {
    n(r.tag, 1);
    n(r.instance_id, 4);
    n(r.port_id, 4);
  }
  void literal(const MetadataLiteral &v) {
    n(v.tag, 1);
    switch (v.tag) {
    case 0:
      break;
    case 1:
      n(std::get<bool>(v.value.data), 1);
      break;
    case 2:
      n(v.width, 4);
      n(std::get<std::uint64_t>(v.value.data), 8);
      break;
    case 3:
    case 5:
      vec(v.children, [&](auto &x) { literal(x); });
      break;
    case 4:
      n(std::get<std::uint64_t>(v.value.data), 8);
      vec(v.children, [&](auto &x) { literal(x); });
      break;
    case 6:
      vec(std::get<Bytes>(v.value.data), [&](auto x) { n(x, 1); });
      break;
    case 7: {
      auto h = std::get<Handle>(v.value.data);
      n(static_cast<unsigned>(h.kind), 1);
      n(h.domain.value, 4);
      n(h.store, 4);
      n(h.slot, 4);
      n(h.generation, 8);
      n(h.owner, 8);
      break;
    }
    default:
      throw std::runtime_error("metadata literal tag");
    }
  }
  void metadata(const Project &p) {
    vec(p.components, [&](auto &c) {
      n(c.id, 4);
      vec(c.parameters, [&](auto &v) {
        n(v.id, 4);
        n(v.type_id, 4);
        n(bool(v.default_value), 1);
        if (v.default_value)
          value(p, v.type_id, *v.default_value);
        source(v.source);
      });
      vec(c.states, [&](auto &v) {
        n(v.id, 4);
        n(v.type_id, 4);
        value(p, v.type_id, v.initial);
      });
      vec(c.endpoints, [&](auto &e) {
        n(e.id, 4);
        n(static_cast<unsigned>(e.role), 1);
        n(e.bus_width, 4);
        n(e.max_bindings, 4);
        n(e.max_outstanding, 4);
        n(e.max_payload_bytes, 4);
        n(e.max_byte_enable_bytes, 4);
        str(e.protocol_ref);
        source(e.source);
      });
      vec(c.sidebands, [&](auto &v) { port(p, v); });
      str(c.reset_policy);
      str(c.source);
    });
    vec(p.instances, [&](auto &i) {
      n(i.id, 4);
      n(i.definition, 4);
      n(i.state_base, 4);
      n(i.state_count, 4);
      vec(i.handlers, [&](auto &h) {
        n(h.local_id, 4);
        n(h.program_id, 4);
        n(static_cast<unsigned>(h.context), 1);
        str(h.trigger);
        n(bool(h.endpoint), 1);
        if (h.endpoint)
          n(*h.endpoint, 4);
        n(bool(h.process_capacity), 1);
        if (h.process_capacity) {
          auto &c = *h.process_capacity;
          n(c.max_instances, 4);
          n(c.frame_bytes_limit, 4);
          n(c.result_capacity, 4);
          n(static_cast<unsigned>(c.overflow), 1);
          n(c.policy_bound, 4);
        }
        str(h.source);
      });
      config(p, i.definition, i.resolved_config);
      str(i.source);
    });
    n(bool(p.system_metadata), 1);
    if (p.system_metadata) {
      auto &s = *p.system_metadata;
      n(s.id, 4);
      vec(s.original_instances, [&](auto &i) {
        n(i.id, 4);
        n(i.definition, 4);
        config(p, i.definition, i.resolved_config);
        source(i.source);
      });
      vec(s.bindings, [&](auto &b) {
        n(b.id, 4);
        endpoint_ref(b.source_endpoint);
        endpoint_ref(b.sink_endpoint);
        source(b.source);
      });
      vec(s.top_ports, [&](auto &v) { port(p, v); });
      vec(s.sideband_bindings, [&](auto &b) {
        n(b.id, 4);
        port_ref(b.source_port);
        port_ref(b.sink_port);
        source(b.source);
      });
      vec(s.address_maps, [&](auto &m) {
        n(m.id, 4);
        n(m.decoder_instance_id, 4);
        n(m.output_endpoint, 4);
        n(m.binding_index, 4);
        n(m.source_start, 8);
        n(m.size, 8);
        n(m.target_start, 8);
        n(m.alias_declared, 1);
        source(m.source);
      });
      n(s.runtime_domain, 4);
      source(s.source);
    }
    vec(p.external_contracts, [&](auto &e) {
      n(e.id, 4);
      str(e.cpp_type);
      str(e.header);
      str(e.library);
      vec(e.constructor_mapping, [&](auto &v) {
        str(v.first);
        literal(v.second);
      });
      for (auto *xs : {&e.requirements, &e.ensures, &e.assumptions, &e.evidence})
        vec(*xs, [&](auto &s) { str(s); });
      source(e.source);
    });
    vec(p.capabilities, [&](auto &s) { str(s); });
  }
};
bool utf8(const std::string &s) {
  for (std::size_t i = 0; i < s.size();) {
    unsigned c = (unsigned char)s[i++];
    if (c < 128)
      continue;
    unsigned len = c >= 0xc2 && c <= 0xdf   ? 1
                   : c >= 0xe0 && c <= 0xef ? 2
                   : c >= 0xf0 && c <= 0xf4 ? 3
                                            : 99;
    if (len == 99 || len > s.size() - i)
      return false;
    unsigned code = c & ((1u << (6 - len)) - 1);
    for (unsigned j = 0; j < len; ++j) {
      auto q = (unsigned char)s[i++];
      if ((q & 0xc0) != 0x80)
        return false;
      code = (code << 6) | (q & 63);
    }
    if ((len == 1 && code < 128) || (len == 2 && code < 2048) || (len == 3 && code < 65536) ||
        code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}
struct ParseError {
  Error e;
};
struct Reader {
  const Bytes &b;
  const LoadPolicy &policy;
  std::size_t pos{}, memory{}, nodes{};
  [[noreturn]] void bad(std::string s) {
    throw ParseError{fail(ErrorCode::Schema, std::move(s) + " at byte " + std::to_string(pos))};
  }
  void charge(std::size_t n) {
    if (n > policy.memory_budget - memory)
      bad("memory budget");
    memory += n;
  }
  std::uint64_t n(unsigned width) {
    if (width > b.size() - pos)
      bad("truncated");
    std::uint64_t x = 0;
    for (unsigned i = 0; i < width; ++i)
      x |= std::uint64_t(b[pos++]) << (8 * i);
    return x;
  }
  std::size_t count(std::size_t size = 1, std::size_t limit = 0) {
    auto c = n(4);
    if (c > (limit ? limit : policy.limits.max_rows) || c > b.size() - pos ||
        ++nodes > policy.limits.max_work)
      bad("collection bound");
    if (c > policy.memory_budget / size)
      bad("allocation overflow");
    charge(std::size_t(c) * size);
    return std::size_t(c);
  }
  std::string str() {
    auto c = n(4);
    if (c > policy.max_string_bytes || c > b.size() - pos)
      bad("string bound");
    charge(std::size_t(c));
    std::string s(b.begin() + pos, b.begin() + pos + std::size_t(c));
    pos += std::size_t(c);
    if (!utf8(s))
      bad("invalid UTF8");
    return s;
  }
  Reg reg() {
    Reg r;
    r.id = std::uint32_t(n(4));
    r.type = std::uint32_t(n(4));
    return r;
  }
  std::vector<Reg> regs() {
    std::vector<Reg> v;
    auto c = count(sizeof(Reg));
    v.reserve(c);
    while (c--)
      v.push_back(reg());
    return v;
  }
  std::vector<std::uint32_t> ids() {
    std::vector<std::uint32_t> v;
    auto c = count(4);
    v.reserve(c);
    while (c--)
      v.push_back(std::uint32_t(n(4)));
    return v;
  }
  Value value(const Project &p, std::optional<std::uint32_t> tid, std::size_t depth = 1) {
    if (depth > policy.limits.max_depth || ++nodes > policy.limits.max_work)
      bad("value depth/work");
    charge(sizeof(Value));
    auto tag = n(1);
    if (!tid) {
      if (tag)
        bad("unused literal must be unit");
      return {};
    }
    if (*tid >= p.types.size())
      bad("literal type ID");
    auto &t = p.types[*tid];
    if (t.kind == TypeKind::Bytes) {
      if (tag != 6)
        bad("bytes tag");
      auto c = count(1, policy.limits.max_value_elements);
      if (c > t.bound)
        bad("bytes capacity");
      Bytes out;
      out.reserve(c);
      while (c--)
        out.push_back(std::uint8_t(n(1)));
      return Value{std::move(out)};
    }
    if (t.kind == TypeKind::Handle) {
      if (tag != 7)
        bad("handle tag");
      Handle h;
      auto kind = n(1);
      if (kind != t.bound || kind > static_cast<unsigned>(HandleKind::GateTicket))
        bad("handle kind");
      h.kind = HandleKind(kind);
      h.domain = DomainId{std::uint32_t(n(4))};
      h.store = std::uint32_t(n(4));
      h.slot = std::uint32_t(n(4));
      h.generation = n(8);
      h.owner = n(8);
      return Value{h};
    }
    if (t.kind == TypeKind::Unit) {
      if (tag)
        bad("unit tag");
      return {};
    }
    if (t.kind == TypeKind::Bool) {
      if (tag != 1)
        bad("bool tag");
      auto x = n(1);
      if (x > 1)
        bad("noncanonical Bool");
      return Value{bool(x)};
    }
    if (t.kind == TypeKind::Bits || t.kind == TypeKind::Fin) {
      if (tag != 2 || n(4) != (t.kind == TypeKind::Bits ? t.bound : 64))
        bad("bits width");
      return Value{n(8)};
    }
    Value::Array a;
    if (t.kind == TypeKind::Record) {
      if (tag != 3)
        bad("record tag");
      auto c = count(sizeof(Value));
      if (c != t.fields.size())
        bad("record arity");
      for (auto type : t.fields)
        a.push_back(value(p, type, depth + 1));
      return Value{std::move(a)};
    }
    if (t.kind == TypeKind::Variant) {
      if (tag != 4)
        bad("variant tag");
      auto which = n(8);
      if (which >= t.constructors.size())
        bad("variant constructor");
      auto c = count(sizeof(Value));
      if (c != t.constructors[which].size())
        bad("variant arity");
      for (auto type : t.constructors[which])
        a.push_back(value(p, type, depth + 1));
      return Value{Value::Array{Value{which}, Value{std::move(a)}}};
    }
    if (t.kind != TypeKind::Vec && t.kind != TypeKind::BoundedVec)
      bad("unknown type");
    if (tag != 5 || t.fields.size() != 1)
      bad("vector tag");
    auto c = count(sizeof(Value), policy.limits.max_value_elements);
    if (t.kind == TypeKind::Vec ? c != t.bound : c > t.bound)
      bad("vector size");
    while (c--)
      a.push_back(value(p, t.fields[0], depth + 1));
    return Value{std::move(a)};
  }
  Edge edge() {
    Edge e;
    e.target = std::uint32_t(n(4));
    e.args = regs();
    return e;
  }
  Terminator term() {
    Terminator t;
    t.kind = TermKind(n(1));
    switch (t.kind) {
    case TermKind::TransportReturn:
      t.value = reg();
      break;
    case TermKind::Suspend:
      t.value = reg();
      t.yes.target = std::uint32_t(n(4));
      t.values = regs();
      break;
    case TermKind::Jump:
      t.yes = edge();
      break;
    case TermKind::Branch:
      t.value = reg();
      t.yes = edge();
      t.no = edge();
      break;
    case TermKind::Switch: {
      t.value = reg();
      auto c = count(sizeof(std::pair<std::uint64_t, Edge>));
      while (c--) {
        auto tag = n(8);
        auto e = edge();
        t.cases.emplace_back(tag, std::move(e));
      }
      t.no = edge();
      break;
    }
    case TermKind::Return:
      t.values = regs();
      break;
    case TermKind::Fail:
      t.error = str();
      break;
    default:
      bad("unsupported terminator");
    }
    return t;
  }
  bool flag() {
    auto v = n(1);
    if (v > 1)
      bad("noncanonical option/Bool");
    return bool(v);
  }
  template <class T, class F> std::vector<T> rows(F f) {
    auto c = count(sizeof(T));
    std::vector<T> out;
    out.reserve(c);
    while (c--)
      out.push_back(f());
    return out;
  }
  MetadataSource source() {
    MetadataSource s;
    s.file = str();
    s.line = std::uint32_t(n(4));
    s.column = std::uint32_t(n(4));
    return s;
  }
  PortDesc port(const Project &p) {
    PortDesc v;
    v.id = std::uint32_t(n(4));
    v.direction = PortDirection(n(1));
    v.type_id = std::uint32_t(n(4));
    if (flag())
      v.initial = value(p, v.type_id);
    v.source = source();
    return v;
  }
  std::uint32_t param_type(const Project &p, std::uint32_t component, std::uint32_t id) {
    for (auto &c : p.components) {
      if (++nodes > policy.limits.max_work)
        bad("config lookup budget");
      if (c.id == component)
        for (auto &v : c.parameters) {
          if (++nodes > policy.limits.max_work)
            bad("config lookup budget");
          if (v.id == id)
            return v.type_id;
        }
    }
    bad("config parameter type");
  }
  ResolvedConfig config(const Project &p, std::uint32_t component) {
    return rows<std::pair<std::uint32_t, Value>>([&]() {
      auto id = std::uint32_t(n(4));
      auto type = param_type(p, component, id);
      return std::make_pair(id, value(p, type));
    });
  }
  EndpointRef endpoint_ref() {
    EndpointRef r;
    r.instance_id = std::uint32_t(n(4));
    r.endpoint = std::uint32_t(n(4));
    r.binding_index = std::uint32_t(n(4));
    return r;
  }
  PortRef port_ref() {
    PortRef r;
    r.tag = std::uint8_t(n(1));
    r.instance_id = std::uint32_t(n(4));
    r.port_id = std::uint32_t(n(4));
    return r;
  }
  MetadataLiteral literal(std::size_t depth = 1) {
    if (depth > policy.limits.max_depth || ++nodes > policy.limits.max_work)
      bad("metadata literal budget");
    MetadataLiteral v;
    v.tag = std::uint8_t(n(1));
    switch (v.tag) {
    case 0:
      break;
    case 1:
      v.value = Value{flag()};
      break;
    case 2:
      v.width = std::uint32_t(n(4));
      if (!v.width || v.width > 64)
        bad("metadata bits width");
      v.value = Value{n(8)};
      if (v.width < 64 && std::get<std::uint64_t>(v.value.data) >= (std::uint64_t{1} << v.width))
        bad("metadata bits range");
      break;
    case 3:
    case 5:
      v.children = rows<MetadataLiteral>([&]() { return literal(depth + 1); });
      break;
    case 4:
      v.value = Value{n(8)};
      v.children = rows<MetadataLiteral>([&]() { return literal(depth + 1); });
      break;
    case 6: {
      Bytes bytes;
      auto c = count(1, policy.limits.max_value_elements);
      bytes.reserve(c);
      while (c--)
        bytes.push_back(std::uint8_t(n(1)));
      v.value = Value{std::move(bytes)};
      break;
    }
    case 7: {
      Handle h;
      auto kind = n(1);
      if (kind > static_cast<unsigned>(HandleKind::GateTicket))
        bad("metadata handle kind");
      h.kind = HandleKind(kind);
      h.domain = DomainId{std::uint32_t(n(4))};
      h.store = std::uint32_t(n(4));
      h.slot = std::uint32_t(n(4));
      h.generation = n(8);
      h.owner = n(8);
      v.value = Value{h};
      break;
    }
    default:
      bad("metadata literal tag");
    }
    return v;
  }
  void metadata(Project &p) {
    p.components = rows<ComponentDesc>([&]() {
      ComponentDesc c;
      c.id = std::uint32_t(n(4));
      c.parameters = rows<ParameterDesc>([&]() {
        ParameterDesc v;
        v.id = std::uint32_t(n(4));
        v.type_id = std::uint32_t(n(4));
        if (flag())
          v.default_value = value(p, v.type_id);
        v.source = source();
        return v;
      });
      c.states = rows<StateDesc>([&]() {
        StateDesc v;
        v.id = std::uint32_t(n(4));
        v.type_id = std::uint32_t(n(4));
        v.initial = value(p, v.type_id);
        return v;
      });
      c.endpoints = rows<EndpointDesc>([&]() {
        EndpointDesc e;
        e.id = std::uint32_t(n(4));
        e.role = EndpointRole(n(1));
        e.bus_width = std::uint32_t(n(4));
        e.max_bindings = std::uint32_t(n(4));
        e.max_outstanding = std::uint32_t(n(4));
        e.max_payload_bytes = std::uint32_t(n(4));
        e.max_byte_enable_bytes = std::uint32_t(n(4));
        e.protocol_ref = str();
        e.source = source();
        return e;
      });
      c.sidebands = rows<PortDesc>([&]() { return port(p); });
      c.reset_policy = str();
      c.source = str();
      return c;
    });
    p.instances = rows<InstanceDesc>([&]() {
      InstanceDesc i;
      i.id = std::uint32_t(n(4));
      i.definition = std::uint32_t(n(4));
      i.state_base = std::uint32_t(n(4));
      i.state_count = std::uint32_t(n(4));
      i.handlers = rows<HandlerBinding>([&]() {
        HandlerBinding h;
        h.local_id = std::uint32_t(n(4));
        h.program_id = std::uint32_t(n(4));
        h.context = ContextKind(n(1));
        h.trigger = str();
        if (flag())
          h.endpoint = std::uint32_t(n(4));
        if (flag()) {
          ProcessCapacity c;
          c.max_instances = std::uint32_t(n(4));
          c.frame_bytes_limit = std::uint32_t(n(4));
          c.result_capacity = std::uint32_t(n(4));
          c.overflow = ProcessOverflow(n(1));
          c.policy_bound = std::uint32_t(n(4));
          h.process_capacity = c;
        }
        h.source = str();
        return h;
      });
      i.resolved_config = config(p, i.definition);
      i.source = str();
      return i;
    });
    if (flag()) {
      SystemMetadata s;
      s.id = std::uint32_t(n(4));
      s.original_instances = rows<OriginalInstance>([&]() {
        OriginalInstance i;
        i.id = std::uint32_t(n(4));
        i.definition = std::uint32_t(n(4));
        i.resolved_config = config(p, i.definition);
        i.source = source();
        return i;
      });
      s.bindings = rows<BindingDesc>([&]() {
        BindingDesc b;
        b.id = std::uint32_t(n(4));
        b.source_endpoint = endpoint_ref();
        b.sink_endpoint = endpoint_ref();
        b.source = source();
        return b;
      });
      s.top_ports = rows<PortDesc>([&]() { return port(p); });
      s.sideband_bindings = rows<SidebandBinding>([&]() {
        SidebandBinding b;
        b.id = std::uint32_t(n(4));
        b.source_port = port_ref();
        b.sink_port = port_ref();
        b.source = source();
        return b;
      });
      s.address_maps = rows<AddressMapDesc>([&]() {
        AddressMapDesc m;
        m.id = std::uint32_t(n(4));
        m.decoder_instance_id = std::uint32_t(n(4));
        m.output_endpoint = std::uint32_t(n(4));
        m.binding_index = std::uint32_t(n(4));
        m.source_start = n(8);
        m.size = n(8);
        m.target_start = n(8);
        m.alias_declared = flag();
        m.source = source();
        return m;
      });
      s.runtime_domain = std::uint32_t(n(4));
      s.source = source();
      p.system_metadata = std::move(s);
    }
    p.external_contracts = rows<ExternalContract>([&]() {
      ExternalContract e;
      e.id = std::uint32_t(n(4));
      e.cpp_type = str();
      e.header = str();
      e.library = str();
      e.constructor_mapping = rows<std::pair<std::string, MetadataLiteral>>([&]() {
        auto key = str();
        auto val = literal();
        return std::make_pair(std::move(key), std::move(val));
      });
      e.requirements = rows<std::string>([&]() { return str(); });
      e.ensures = rows<std::string>([&]() { return str(); });
      e.assumptions = rows<std::string>([&]() { return str(); });
      e.evidence = rows<std::string>([&]() { return str(); });
      e.source = source();
      return e;
    });
    p.capabilities = rows<std::string>([&]() { return str(); });
  }
};
} // namespace
Expected<Bytes> serialize(const ValidatedProject &vp) {
  try {
    auto &p = vp.get();
    Writer w;
    w.b = {'L', 'A', 'T', 'R'};
    w.n(p.schema_major, 2);
    w.n(0, 2);
    w.n(0, 8);
    w.n(1, 4);
    w.n(1, 4);
    w.n(104, 8);
    w.n(0, 8);
    auto sh = schema_hash(p.schema_major);
    w.b.insert(w.b.end(), sh.begin(), sh.end());
    w.b.resize(104);
    w.str(p.profile);
    w.vec(p.types, [&](auto &t) {
      w.n(static_cast<unsigned>(t.kind), 1);
      w.n(t.bound, 8);
      w.ids(t.fields);
      w.vec(t.constructors, [&](auto &fs) { w.ids(fs); });
    });
    w.ids(p.state_types);
    w.n(p.initial_state.size(), 4);
    for (std::size_t i = 0; i < p.initial_state.size(); ++i)
      w.value(p, p.state_types[i], p.initial_state[i]);
    w.vec(p.programs, [&](auto &pr) {
      w.n(pr.id, 4);
      w.ids(pr.input_types);
      w.ids(pr.result_types);
      w.vec(pr.blocks, [&](auto &b) {
        w.n(b.id, 4);
        w.regs(b.parameters);
        w.vec(b.instructions, [&](auto &i) {
          w.n(static_cast<unsigned>(i.op), 1);
          w.regs(i.args);
          w.n(bool(i.dest), 1);
          if (i.dest)
            w.reg(*i.dest);
          w.value(p, i.op == Op::Const ? std::optional<std::uint32_t>{i.dest->type} : std::nullopt,
                  i.value);
          w.n(i.immediate, 4);
          w.n(static_cast<unsigned>(i.binary), 1);
          w.str(i.text);
          w.str(i.source);
        });
        w.term(b.terminator);
      });
      w.n(pr.entry, 4);
      w.str(pr.source);
      if (p.schema_major >= 2) {
        w.n(static_cast<unsigned>(pr.context), 1);
        w.vec(pr.frame, [&](auto &slot) {
          w.n(slot.type, 4);
          w.n(slot.offset, 4);
          w.n(slot.align, 4);
          if (p.schema_major >= 3)
            w.n(slot.register_id, 4);
        });
        w.n(pr.frame_bytes, 4);
        w.n(pr.effect_mask, 8);
        if (p.schema_major >= 5) {
          w.n(pr.instruction_fuel, 8);
          w.str(pr.owner_policy);
          w.str(pr.result_lifetime_policy);
        }
      }
    });
    if (p.schema_major >= 2)
      w.vec(p.services, [&](auto &s) {
        w.n(s.id, 4);
        w.n(static_cast<unsigned>(s.op), 1);
        w.ids(s.input_types);
        w.ids(s.result_types);
        w.n(s.context_mask, 4);
        w.n(s.effect_mask, 8);
        w.n(s.extra_fuel, 8);
        w.str(s.provider_key);
        w.str(s.provider_version);
        w.b.insert(w.b.end(), s.abi_hash.begin(), s.abi_hash.end());
      });
    if (p.schema_major >= 4)
      w.metadata(p);
    for (unsigned i = 0; i < 8; ++i) {
      w.b[8 + i] = std::uint8_t(std::uint64_t(w.b.size()) >> (i * 8));
      w.b[32 + i] = std::uint8_t(std::uint64_t(w.b.size() - 104) >> (i * 8));
    }
    auto digest = sha256(w.b);
    std::copy(digest.begin(), digest.end(), w.b.begin() + 72);
    return w.b;
  } catch (const std::exception &e) {
    return fail(ErrorCode::Capacity, e.what());
  }
}
Expected<ValidatedProject> load_descriptor(const Bytes &bytes, LoadPolicy policy) {
  try {
    policy.limits.max_depth = std::min(policy.limits.max_depth, std::size_t{64});
    if (bytes.size() < 104 || bytes.size() > policy.max_file_bytes ||
        bytes.size() > policy.memory_budget / 3)
      return fail(ErrorCode::Capacity, "descriptor file bound");
    Reader r{bytes, policy};
    r.charge(bytes.size() * 3);
    if (r.n(4) != 0x5254414c)
      return fail(ErrorCode::Schema, "invalid magic");
    auto version = r.n(2);
    if ((version < 1 || version > 5) || r.n(2) != 0)
      return fail(ErrorCode::Unsupported, "schema version");
    if (r.n(8) != bytes.size() || r.n(4) != 1 || r.n(4) != 1 || r.n(8) != 104 ||
        r.n(8) != bytes.size() - 104)
      return fail(ErrorCode::Schema, "section directory or file length");
    auto sh = schema_hash(static_cast<unsigned>(version));
    for (auto x : sh)
      if (r.n(1) != x)
        return fail(ErrorCode::Schema, "schema hash");
    std::array<std::uint8_t, 32> claimed{};
    for (auto &x : claimed)
      x = std::uint8_t(r.n(1));
    Bytes copy = bytes;
    std::fill(copy.begin() + 72, copy.begin() + 104, 0);
    if (sha256(copy) != claimed)
      return fail(ErrorCode::Integrity, "content integrity mismatch");
    Project p;
    p.schema_major = static_cast<std::uint16_t>(version);
    p.profile = r.str();
    if (p.profile != policy.expected_profile)
      return fail(ErrorCode::Unsupported, "host profile mismatch");
    auto c = r.count(sizeof(Type));
    while (c--) {
      Type t;
      t.kind = TypeKind(r.n(1));
      t.bound = r.n(8);
      t.fields = r.ids();
      auto rows = r.count(sizeof(std::vector<std::uint32_t>));
      while (rows--)
        t.constructors.push_back(r.ids());
      p.types.push_back(std::move(t));
    }
    p.state_types = r.ids();
    c = r.count(sizeof(Value));
    if (c != p.state_types.size())
      r.bad("state count");
    for (auto t : p.state_types)
      p.initial_state.push_back(r.value(p, t));
    c = r.count(sizeof(Program));
    while (c--) {
      Program pr;
      pr.id = std::uint32_t(r.n(4));
      pr.input_types = r.ids();
      pr.result_types = r.ids();
      auto bs = r.count(sizeof(Block));
      while (bs--) {
        Block b;
        b.id = std::uint32_t(r.n(4));
        b.parameters = r.regs();
        auto is = r.count(sizeof(Instruction));
        while (is--) {
          Instruction i;
          i.op = Op(r.n(1));
          if (static_cast<unsigned>(i.op) >=
              (version == 1 ? 16u : static_cast<unsigned>(Op::Count)))
            r.bad("unsupported opcode");
          i.args = r.regs();
          auto has = r.n(1);
          if (has > 1)
            r.bad("destination Bool");
          if (has)
            i.dest = r.reg();
          if (i.op == Op::Const && !has)
            r.bad("constant destination");
          i.value = r.value(p, i.op == Op::Const ? std::optional<std::uint32_t>{i.dest->type}
                                                 : std::nullopt);
          i.immediate = std::uint32_t(r.n(4));
          i.binary = Binary(r.n(1));
          if (static_cast<unsigned>(i.binary) > 8)
            r.bad("binary tag");
          i.text = r.str();
          i.source = r.str();
          b.instructions.push_back(std::move(i));
        }
        b.terminator = r.term();
        pr.blocks.push_back(std::move(b));
      }
      pr.entry = std::uint32_t(r.n(4));
      pr.source = r.str();
      if (version >= 2) {
        pr.context = ContextKind(r.n(1));
        auto slots = r.count(sizeof(FrameSlot));
        while (slots--) {
          FrameSlot slot;
          slot.type = std::uint32_t(r.n(4));
          slot.offset = std::uint32_t(r.n(4));
          slot.align = std::uint32_t(r.n(4));
          if (version >= 3)
            slot.register_id = std::uint32_t(r.n(4));
          pr.frame.push_back(slot);
        }
        pr.frame_bytes = std::uint32_t(r.n(4));
        pr.effect_mask = r.n(8);
        if (version >= 5) {
          pr.instruction_fuel = r.n(8);
          pr.owner_policy = r.str();
          pr.result_lifetime_policy = r.str();
        }
      }
      p.programs.push_back(std::move(pr));
    }
    if (version >= 2) {
      auto count = r.count(sizeof(ServiceSignature));
      while (count--) {
        ServiceSignature s;
        s.id = std::uint32_t(r.n(4));
        s.op = Op(r.n(1));
        s.input_types = r.ids();
        s.result_types = r.ids();
        s.context_mask = std::uint32_t(r.n(4));
        s.effect_mask = r.n(8);
        s.extra_fuel = r.n(8);
        s.provider_key = r.str();
        s.provider_version = r.str();
        for (auto &x : s.abi_hash)
          x = std::uint8_t(r.n(1));
        p.services.push_back(std::move(s));
      }
    }
    if (version >= 4)
      r.metadata(p);
    for (auto &cap : p.capabilities)
      if (std::find(policy.allowed_capabilities.begin(), policy.allowed_capabilities.end(), cap) ==
          policy.allowed_capabilities.end())
        return fail(ErrorCode::Unsupported, "host capability not permitted: " + cap);
    if (r.pos != bytes.size())
      r.bad("trailing bytes");
    return validate(std::move(p), policy.limits);
  } catch (const ParseError &e) {
    return e.e;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Capacity, "descriptor allocation");
  } catch (const std::exception &e) {
    return fail(ErrorCode::Schema, e.what());
  }
}
} // namespace leanat::exec
