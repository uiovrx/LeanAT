#include "leanat/register_bank.hpp"
#include "leanat/result_store.hpp"
namespace leanat {
Expected<ResponseStatus> RegisterBank::access(EventTxn &t, PayloadShadow &shadow,
                                              const PayloadViewKey &key) {
  auto input = shadow.snapshot(key, t);
  if (!input)
    return input.error();
  auto permission = shadow.validate_target_access(key, t, input.value().command == Command::Read);
  if (!permission)
    return permission.error();
  auto cp = t.checkpoint();
  if (!cp)
    return cp.error();
  auto result = access(t, input.value());
  if (!result)
    return result.error();
  if (result.value().status == ResponseStatus::Ok && input.value().command == Command::Read) {
    auto data = shadow.buffer_data(key, Value(std::move(result.value().data)), t);
    if (!data) {
      t.rollback(std::move(cp.value()));
      return data.error();
    }
  }
  return result.value().status;
}
namespace {
bool bit(const Bytes &b, std::size_t i) {
  return (b[i / 8] >> (i % 8)) & 1;
}
void put(Bytes &b, std::size_t i, bool v) {
  auto m = std::uint8_t(1u << (i % 8));
  b[i / 8] = std::uint8_t((b[i / 8] & ~m) | (v ? m : 0));
}
std::size_t physical(const RegisterSpec &r, std::size_t bitIndex) {
  return (r.endian == RegisterEndian::Little ? bitIndex / 8 : r.width_bits / 8 - 1 - bitIndex / 8) *
             8 +
         bitIndex % 8;
}
} // namespace
std::optional<std::pair<std::size_t, std::size_t>> RegisterBank::locate(std::uint64_t a) const {
  for (std::size_t i = 0; i < specs_.size(); ++i)
    if (a >= specs_[i].offset && a - specs_[i].offset < specs_[i].width_bits / 8)
      return std::make_pair(i, static_cast<std::size_t>(a - specs_[i].offset));
  return {};
}
Expected<RegisterBank> RegisterBank::make(std::vector<RegisterSpec> s, std::size_t max) {
  RegisterBank b;
  std::map<FieldId, bool> ids;
  std::size_t total = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    auto &r = s[i];
    if (!r.width_bits || r.width_bits % 8 || r.width_bits > max || r.allowed_access_bytes.empty() ||
        (r.alignment_bytes && !*r.alignment_bytes))
      return fail(ErrorCode::Schema, "register width/access/alignment");
    auto end = checked_add(r.offset, r.width_bits / 8 - 1);
    if (!end)
      return end.error();
    for (std::size_t j = 0; j < i; ++j) {
      auto &v = s[j];
      if (r.offset <= v.offset + v.width_bits / 8 - 1 && v.offset <= end.value())
        return fail(ErrorCode::Schema, "overlapping registers");
    }
    for (auto n : r.allowed_access_bytes)
      if (!n || n > r.width_bits / 8)
        return fail(ErrorCode::Schema, "invalid access size");
    std::vector<bool> used(r.width_bits);
    for (auto &f : r.fields) {
      if (!f.width || f.lsb > r.width_bits || f.width > r.width_bits - f.lsb ||
          !ids.emplace(f.id, true).second)
        return fail(ErrorCode::Schema, "invalid field");
      if (f.reset.empty())
        f.reset.resize((f.width + 7) / 8);
      if (f.reset.size() != (f.width + 7) / 8 || (f.width % 8 && (f.reset.back() >> (f.width % 8))))
        return fail(ErrorCode::Schema, "field reset out of range");
      for (std::size_t k = f.lsb; k < f.lsb + f.width; ++k) {
        if (used[k])
          return fail(ErrorCode::Schema, "overlapping fields");
        used[k] = true;
      }
    }
    b.starts_.push_back(total);
    total += r.width_bits / 8;
  }
  b.initial_.resize(total);
  for (std::size_t i = 0; i < s.size(); ++i)
    for (auto &f : s[i].fields)
      for (std::size_t k = 0; k < f.width; ++k)
        put(b.initial_, b.starts_[i] * 8 + physical(s[i], f.lsb + k), bit(f.reset, k));
  b.specs_ = std::move(s);
  b.cell_.value = Value(b.initial_);
  b.cell_.epoch_independent = true;
  return b;
}
Expected<MemoryTransferResult> RegisterBank::access(EventTxn &t, const PayloadSnapshot &p) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  MemoryTransferResult out{ResponseStatus::Ok, p.data};
  if (p.command == Command::Ignore)
    return out;
  if (p.command != Command::Read && p.command != Command::Write) {
    out.status = ResponseStatus::CommandError;
    return out;
  }
  if (p.data.empty() || p.streaming_width < p.data.size()) {
    out.status = ResponseStatus::BurstError;
    return out;
  }
  for (auto m : p.byte_enable)
    if (m != 0 && m != 255) {
      out.status = ResponseStatus::ByteEnableError;
      return out;
    }
  auto loc = locate(p.address);
  if (!loc) {
    out.status = ResponseStatus::AddressError;
    return out;
  }
  auto idx = loc->first, offset = loc->second;
  auto &r = specs_[idx];
  if (p.data.size() > r.width_bits / 8 - offset) {
    out.status = ResponseStatus::AddressError;
    return out;
  }
  if (std::find(r.allowed_access_bytes.begin(), r.allowed_access_bytes.end(), p.data.size()) ==
      r.allowed_access_bytes.end()) {
    out.status = ResponseStatus::BurstError;
    return out;
  }
  if (p.address % r.alignment_bytes.value_or(p.data.size())) {
    out.status = ResponseStatus::AddressError;
    return out;
  }
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto bytes = std::get<Bytes>(v.value().data);
  bool dirty = false;
  for (std::size_t j = 0; j < p.data.size(); ++j) {
    if (!p.byte_enable.empty() && !p.byte_enable[j % p.byte_enable.size()])
      continue;
    if (p.command == Command::Read)
      out.data[j] = 0;
    for (std::size_t k = 0; k < 8; ++k) {
      auto logical =
          (r.endian == RegisterEndian::Little ? offset + j : r.width_bits / 8 - 1 - offset - j) *
              8 +
          k;
      const FieldSpec *f = nullptr;
      for (auto &x : r.fields)
        if (logical >= x.lsb && logical - x.lsb < x.width) {
          f = &x;
          break;
        }
      if (!f)
        continue;
      auto at = (starts_[idx] + offset + j) * 8 + k;
      bool old = bit(bytes, at), incoming = (p.data[j] >> k) & 1;
      if (p.command == Command::Read) {
        if (f->access != RegisterAccess::WO && old)
          out.data[j] |= std::uint8_t(1u << k);
        if (f->access == RegisterAccess::RC) {
          put(bytes, at, false);
          dirty = true;
        }
      } else {
        switch (f->access) {
        case RegisterAccess::RW:
        case RegisterAccess::WO:
          put(bytes, at, incoming);
          dirty = true;
          break;
        case RegisterAccess::W1C:
          put(bytes, at, old && !incoming);
          dirty = true;
          break;
        case RegisterAccess::W1S:
          put(bytes, at, old || incoming);
          dirty = true;
          break;
        default:
          break;
        }
      }
    }
  }
  if (dirty) {
    auto w = t.buffer(cell_, Value(std::move(bytes)));
    if (!w)
      return w.error();
  }
  return out;
}
Expected<MemoryDebugResult> RegisterBank::peek_poke(EventTxn &t, Command cmd, std::uint64_t a,
                                                    const Bytes &in, std::size_t max) {
  auto c = object_context(t, true);
  if (!c)
    return c.error();
  MemoryDebugResult out{0, in};
  if (cmd != Command::Read && cmd != Command::Write)
    return out;
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto bytes = std::get<Bytes>(v.value().data);
  for (std::size_t i = 0; i < std::min(max, in.size()); ++i) {
    auto addr = checked_add(a, i);
    if (!addr)
      break;
    auto loc = locate(addr.value());
    if (!loc)
      break;
    auto &r = specs_[loc->first];
    auto at = starts_[loc->first] + loc->second;
    if (cmd == Command::Read)
      out.data[i] = 0;
    for (std::size_t k = 0; k < 8; ++k) {
      auto logical =
          (r.endian == RegisterEndian::Little ? loc->second : r.width_bits / 8 - 1 - loc->second) *
              8 +
          k;
      bool implemented = false;
      for (auto &f : r.fields)
        if (logical >= f.lsb && logical - f.lsb < f.width)
          implemented = true;
      if (implemented) {
        if (cmd == Command::Read) {
          if (bit(bytes, at * 8 + k))
            out.data[i] |= std::uint8_t(1u << k);
        } else
          put(bytes, at * 8 + k, (in[i] >> k) & 1);
      }
    }
    ++out.count;
  }
  if (cmd == Command::Write && out.count) {
    auto w = t.buffer(cell_, Value(std::move(bytes)));
    if (!w)
      return w.error();
  }
  return out;
}
Expected<Bytes> RegisterBank::read_field(EventTxn &t, FieldId id) const {
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto &bytes = std::get<Bytes>(v.value().data);
  for (std::size_t i = 0; i < specs_.size(); ++i)
    for (auto &f : specs_[i].fields)
      if (f.id == id) {
        Bytes out((f.width + 7) / 8);
        for (std::size_t k = 0; k < f.width; ++k)
          put(out, k, bit(bytes, starts_[i] * 8 + physical(specs_[i], f.lsb + k)));
        return out;
      }
  return fail(ErrorCode::InvalidArgument, "unknown field");
}
Expected<void> RegisterBank::stage_field_update(EventTxn &t, FieldId id, const Bytes &in) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto bytes = std::get<Bytes>(v.value().data);
  for (std::size_t i = 0; i < specs_.size(); ++i)
    for (auto &f : specs_[i].fields)
      if (f.id == id) {
        if (in.size() != (f.width + 7) / 8 || (f.width % 8 && (in.back() >> (f.width % 8))))
          return fail(ErrorCode::TypeMismatch, "field value width");
        for (std::size_t k = 0; k < f.width; ++k)
          put(bytes, starts_[i] * 8 + physical(specs_[i], f.lsb + k), bit(in, k));
        return t.buffer(cell_, Value(std::move(bytes)));
      }
  return fail(ErrorCode::InvalidArgument, "unknown field");
}
Expected<void> RegisterBank::reset(EventTxn &t) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  return t.buffer(cell_, Value(initial_));
}
} // namespace leanat
