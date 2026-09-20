#include "leanat/external.hpp"
#include <cctype>
namespace leanat {
namespace {
Expected<Bytes> encode(const Value &v, ExternalLayout l) {
  Bytes b;
  if (l.kind == ExternalLayoutKind::Bytes) {
    auto p = std::get_if<Bytes>(&v.data);
    if (!p)
      return fail(ErrorCode::TypeMismatch, "expected byte buffer");
    if (p->size() > l.max_bytes)
      return fail(ErrorCode::Capacity, "layout input limit");
    b = *p;
  } else if (l.kind == ExternalLayoutKind::U64LE) {
    auto p = std::get_if<std::uint64_t>(&v.data);
    if (!p)
      return fail(ErrorCode::TypeMismatch, "expected u64");
    for (unsigned i = 0; i < 8; ++i)
      b.push_back(static_cast<std::uint8_t>(*p >> (i * 8)));
  } else {
    auto p = std::get_if<bool>(&v.data);
    if (!p)
      return fail(ErrorCode::TypeMismatch, "expected bool");
    b.push_back(*p ? 1 : 0);
  }
  if (b.size() > l.max_bytes)
    return fail(ErrorCode::Capacity, "layout input limit");
  return b;
}
Expected<Value> decode(const Bytes &b, ExternalLayout l) {
  if (b.size() > l.max_bytes)
    return fail(ErrorCode::Schema, "output exceeds layout");
  if (l.kind == ExternalLayoutKind::Bytes)
    return Value{b};
  if (l.kind == ExternalLayoutKind::Bool) {
    if (b.size() != 1 || b[0] > 1)
      return fail(ErrorCode::Schema, "invalid bool output");
    return Value{b[0] != 0};
  }
  if (b.size() != 8)
    return fail(ErrorCode::Schema, "invalid u64 output");
  std::uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i)
    n |= std::uint64_t(b[i]) << (8 * i);
  return Value{n};
}
bool identifier(const std::string &s) {
  if (s.empty())
    return false;
  for (unsigned char c : s)
    if (!std::isalnum(c) && c != '_')
      return false;
  return true;
}
} // namespace
Expected<void> ExternalCallGate::register_reference(ExternCallDesc d, Reference r, Precondition p) {
  if (sealed_)
    return fail(ErrorCode::InvalidState, "gate sealed");
  if (entries_.size() >= 256)
    return fail(ErrorCode::Capacity, "external registration table full");
  if (static_cast<unsigned>(d.input.kind) > 2 || static_cast<unsigned>(d.output.kind) > 2)
    return fail(ErrorCode::Schema, "unknown external layout");
  if (!r || !p || d.logical_id.empty() || d.reference_hash.empty() || d.contract_hash.empty() ||
      d.abi_version != 1 || !identifier(d.symbol_id))
    return fail(ErrorCode::InvalidArgument, "incomplete reference/ABI/precondition identity");
  if (d.input.max_bytes > scratch_limit_ || d.output.max_bytes > scratch_limit_ - d.input.max_bytes)
    return fail(ErrorCode::Capacity, "scratch bound");
  if ((d.input.kind == ExternalLayoutKind::U64LE && d.input.max_bytes != 8) ||
      (d.output.kind == ExternalLayoutKind::U64LE && d.output.max_bytes != 8) ||
      (d.input.kind == ExternalLayoutKind::Bool && d.input.max_bytes != 1) ||
      (d.output.kind == ExternalLayoutKind::Bool && d.output.max_bytes != 1))
    return fail(ErrorCode::Schema, "fixed width layout mismatch");
  if (entries_.count(d.id))
    return fail(ErrorCode::Duplicate, "call id");
  auto id = d.id;
  entries_.emplace(id, Entry{std::move(d), std::move(r), std::move(p), nullptr});
  return {};
}
Expected<void> ExternalCallGate::register_native(ExternCallId id, const std::string &s,
                                                 const std::string &b, std::uint32_t a,
                                                 NativePureFunction fn) {
  if (sealed_)
    return fail(ErrorCode::InvalidState, "gate sealed");
  auto it = entries_.find(id);
  if (it == entries_.end())
    return fail(ErrorCode::InvalidArgument, "unknown reference");
  auto &e = it->second;
  if (!native_capability_)
    return fail(ErrorCode::Unsupported, "native capability disabled");
  if (!fn || s != e.desc.symbol_id || b.empty() || b != e.desc.build_id || a != e.desc.abi_version)
    return fail(ErrorCode::Integrity, "native ABI/build/symbol mismatch");
  auto f = e.desc.effects;
  if (!f.no_wait || !f.no_callback || !f.no_runtime_write || !f.no_pointer_retention ||
      !f.no_throw || !f.deterministic)
    return fail(ErrorCode::Unsupported, "required native effect contracts undeclared");
  if (e.native)
    return fail(ErrorCode::Duplicate, "native already registered");
  e.native = fn;
  return {};
}
Expected<void> ExternalCallGate::seal() {
  for (auto &kv : entries_) {
    auto &e = kv.second;
    for (auto d : e.desc.dependencies)
      if (!entries_.count(d))
        return fail(ErrorCode::Schema, "missing reference dependency");
    if (e.desc.select_native && (!native_capability_ || !e.native))
      return fail(ErrorCode::Unsupported, "selected native unavailable");
  }
  sealed_ = true;
  return {};
}
Expected<Value> ExternalCallGate::invoke(ExternCallId id, const Value &v, bool reference) {
  if (!sealed_)
    return fail(ErrorCode::InvalidState, "gate not sealed");
  auto it = entries_.find(id);
  if (it == entries_.end())
    return fail(ErrorCode::InvalidArgument, "unknown external call");
  if (call_depth_ >= 128)
    return fail(ErrorCode::FuelExhausted, "external recursion limit");
  auto &e = it->second;
  auto in = encode(v, e.desc.input);
  if (!in)
    return in.error();
  struct Depth {
    std::size_t &d;
    Depth(std::size_t &x) : d(x) {
      ++d;
    }
    ~Depth() {
      --d;
    }
  } depth(call_depth_);
  const bool use_reference = reference || reference_depth_ || !e.desc.select_native;
  std::optional<Depth> reference_guard;
  if (use_reference) {
    reference_guard.emplace(reference_depth_);
  }
  try {
    auto precondition = e.precondition(v);
    if (!precondition) {
      return precondition.error();
    }
    if (!precondition.value())
      return fail(ErrorCode::InvalidArgument, "external precondition failed");
    if (use_reference) {
      auto r = e.reference(*this, v);
      if (!r)
        return r.error();
      auto check = encode(r.value(), e.desc.output);
      if (!check)
        return check.error();
      return r;
    }
    Bytes out(e.desc.output.max_bytes ? e.desc.output.max_bytes : 1);
    std::uint32_t size = 0;
    std::uint8_t sentinel = 0;
    auto status = e.native(in.value().empty() ? &sentinel : in.value().data(),
                           static_cast<std::uint32_t>(in.value().size()), out.data(),
                           e.desc.output.max_bytes, &size);
    if (status != 0)
      return fail(ErrorCode::ExternalFailure, "native status " + std::to_string(status));
    if (size > e.desc.output.max_bytes)
      return fail(ErrorCode::Overflow, "native output length exceeds capacity");
    out.resize(size);
    return decode(out, e.desc.output);
  } catch (...) {
    return fail(ErrorCode::ExternalFailure, "external contract exception (cross-ABI if native)");
  }
}
Expected<Value> ExternalCallGate::call_pure(ExternCallId id, const Value &v) {
  return invoke(id, v, false);
}
Expected<Value> ExternalCallGate::call_reference(ExternCallId id, const Value &v) {
  return invoke(id, v, true);
}
Expected<void> ExternalCallGate::compare(ExternCallId id, const Value &v) {
  comparison_.reset();
  auto entry = entries_.find(id);
  if (entry == entries_.end() || !entry->second.desc.select_native || !entry->second.native) {
    return fail(ErrorCode::Unsupported, "differential comparison requires selected native");
  }
  auto n = call_pure(id, v);
  if (!n)
    return n.error();
  auto r = call_reference(id, v);
  if (!r)
    return r.error();
  comparison_ = ExternalComparison{
      id,       entries_.at(id).desc.build_id, entries_.at(id).desc.reference_hash, v, n.value(),
      r.value()};
  if (n.value() != r.value())
    return fail(ErrorCode::Integrity, "reference mismatch for call " + std::to_string(id) +
                                          " build " + entries_.at(id).desc.build_id);
  return {};
}
} // namespace leanat
namespace leanat {
Expected<const ExternCallDesc *> ExternalCallGate::descriptor(ExternCallId id) const {
  if (!sealed_) {
    return fail(ErrorCode::InvalidState, "external gate not sealed");
  }
  auto found = entries_.find(id);
  if (found == entries_.end()) {
    return fail(ErrorCode::InvalidArgument, "unknown external call");
  }
  return &found->second.desc;
}
} // namespace leanat
