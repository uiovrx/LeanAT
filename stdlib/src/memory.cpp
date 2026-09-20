#include "leanat/memory.hpp"
#include "leanat/result_store.hpp"
namespace leanat {
Expected<ResponseStatus> Memory::transfer(EventTxn &t, PayloadShadow &shadow,
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
  auto result = transfer(t, input.value());
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
Expected<Memory> Memory::make(std::size_t n, Bytes b) {
  if (!n)
    return fail(ErrorCode::InvalidArgument, "memory size must be positive");
  if (b.empty())
    b.resize(n);
  if (b.size() != n)
    return fail(ErrorCode::InvalidArgument, "initial memory size mismatch");
  return Memory{std::move(b)};
}
ResponseStatus Memory::validate_transfer(const PayloadSnapshot &p) const {
  if (p.command == Command::Ignore)
    return ResponseStatus::Ok;
  if (p.command != Command::Read && p.command != Command::Write)
    return ResponseStatus::CommandError;
  if (p.data.empty() || !p.streaming_width)
    return ResponseStatus::BurstError;
  for (auto b : p.byte_enable)
    if (b != 0 && b != 255)
      return ResponseStatus::ByteEnableError;
  auto span = std::min<std::uint64_t>(p.data.size(), p.streaming_width);
  if (p.address >= size() || span > size() - p.address)
    return ResponseStatus::AddressError;
  return ResponseStatus::Ok;
}
Expected<MemoryTransferResult> Memory::transfer(EventTxn &t, const PayloadSnapshot &p) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  MemoryTransferResult r{validate_transfer(p), p.data};
  if (r.status != ResponseStatus::Ok || p.command == Command::Ignore)
    return r;
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto b = std::get<Bytes>(v.value().data);
  for (std::size_t i = 0; i < p.data.size(); ++i)
    if (p.byte_enable.empty() || p.byte_enable[i % p.byte_enable.size()]) {
      auto a = p.address + i % p.streaming_width;
      if (p.command == Command::Read)
        r.data[i] = b[a];
      else
        b[a] = p.data[i];
    }
  if (p.command == Command::Write) {
    auto w = t.buffer(cell_, Value(std::move(b)));
    if (!w)
      return w.error();
  }
  return r;
}
Expected<Bytes> Memory::read_bytes(EventTxn &t, std::uint64_t a, std::size_t n) const {
  auto c = object_context(t);
  if (!c)
    return c.error();
  if (!n)
    return Bytes{};
  if (a >= size() || n > size() - a)
    return fail(ErrorCode::InvalidArgument, "memory range");
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto &b = std::get<Bytes>(v.value().data);
  return Bytes(b.begin() + a, b.begin() + a + n);
}
Expected<void> Memory::write_bytes(EventTxn &t, std::uint64_t a, const Bytes &b,
                                   const Bytes &mask) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  if (b.empty())
    return {};
  if (a >= size() || b.size() > size() - a)
    return fail(ErrorCode::InvalidArgument, "memory range");
  for (auto x : mask)
    if (x != 0 && x != 255)
      return fail(ErrorCode::InvalidArgument, "memory byte enable");
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto bytes = std::get<Bytes>(v.value().data);
  for (std::size_t i = 0; i < b.size(); ++i)
    if (mask.empty() || mask[i % mask.size()])
      bytes[a + i] = b[i];
  return t.buffer(cell_, Value(std::move(bytes)));
}
Expected<MemoryDebugResult> Memory::debug_transfer(EventTxn &t, Command cmd, std::uint64_t a,
                                                   const Bytes &input, std::size_t limit) {
  auto c = object_context(t, true);
  if (!c)
    return c.error();
  MemoryDebugResult r{0, input};
  if ((cmd != Command::Read && cmd != Command::Write) || a >= size())
    return r;
  r.count = std::min({input.size(), limit, size() - static_cast<std::size_t>(a)});
  auto v = t.read(cell_);
  if (!v)
    return v.error();
  auto b = std::get<Bytes>(v.value().data);
  for (std::size_t i = 0; i < r.count; ++i)
    if (cmd == Command::Read)
      r.data[i] = b[a + i];
    else
      b[a + i] = input[i];
  if (cmd == Command::Write) {
    auto w = t.buffer(cell_, Value(std::move(b)));
    if (!w)
      return w.error();
  }
  return r;
}
Expected<void> Memory::clear(EventTxn &t) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  return t.buffer(cell_, Value(Bytes(size(), 0)));
}
Expected<void> Memory::reset(EventTxn &t) {
  auto c = object_context(t);
  if (!c)
    return c.error();
  return t.buffer(cell_, Value(initial_));
}
} // namespace leanat
