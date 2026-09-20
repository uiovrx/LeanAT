#include "leanat/numeric.hpp"

namespace leanat::numeric {
namespace {
bool valid_width(std::uint32_t width) { return width >= 1 && width <= 64; }
std::uint64_t mask(std::uint32_t width) {
  return width == 64 ? UINT64_MAX : (std::uint64_t{1} << width) - 1;
}
Expected<std::uint64_t> bits(const Value &v, std::uint32_t width) {
  if (!valid_width(width))
    return fail(ErrorCode::TypeMismatch, "NumericTypeMismatch");
  auto n = std::get_if<std::uint64_t>(&v.data);
  if (!n || *n > mask(width))
    return fail(ErrorCode::TypeMismatch, "NumericTypeMismatch");
  return *n;
}
} // namespace
Expected<Value> eval_unary(std::uint32_t mode, const Value &value, std::uint32_t width) {
  if (mode > 2) return fail(ErrorCode::Unsupported, "UnknownUnaryMode");
  if (mode == 0) {
    auto b = std::get_if<bool>(&value.data);
    if (!b) return fail(ErrorCode::TypeMismatch, "NumericTypeMismatch");
    return Value{!*b};
  }
  auto n = bits(value, width);
  if (!n) return n.error();
  return Value{(mode == 1 ? ~n.value() : std::uint64_t{0} - n.value()) & mask(width)};
}
Expected<Value> eval_compare(std::uint32_t mode, const Value &a, const Value &b,
                             std::uint32_t width) {
  if (mode > 5) return fail(ErrorCode::Unsupported, "UnknownCompareMode");
  if (mode <= 1 && width == 0) {
    if (a.data.index() != b.data.index())
      return fail(ErrorCode::TypeMismatch, "NumericTypeMismatch");
    return Value{mode == 0 ? a == b : a != b};
  }
  auto x = bits(a, width), y = bits(b, width);
  if (!x) return x.error();
  if (!y) return y.error();
  switch (mode) {
  case 0: return Value{x.value() == y.value()};
  case 1: return Value{x.value() != y.value()};
  case 2: return Value{x.value() < y.value()};
  case 3: return Value{x.value() <= y.value()};
  case 4: return Value{x.value() > y.value()};
  default: return Value{x.value() >= y.value()};
  }
}
Expected<Value> eval_convert(std::uint32_t mode, const Value &value, std::uint32_t source_width,
                             std::uint32_t dest_width) {
  if (mode > 2) return fail(ErrorCode::Unsupported, "UnknownConvertMode");
  auto n = bits(value, source_width);
  if (!n) return n.error();
  if (!valid_width(dest_width) || (mode == 0 && source_width > dest_width) ||
      (mode != 0 && source_width < dest_width))
    return fail(ErrorCode::TypeMismatch, "NumericTypeMismatch");
  if (mode == 2 && n.value() > mask(dest_width))
    return fail(ErrorCode::Overflow, "ArithmeticOverflow");
  return Value{n.value() & mask(dest_width)};
}
Expected<Value> checked_divide(const Value &a, const Value &b, std::uint32_t width,
                               bool signed_twos_complement) {
  auto x = bits(a, width), y = bits(b, width);
  if (!x) return x.error();
  if (!y) return y.error();
  if (!y.value()) return fail(ErrorCode::InvalidArgument, "DivisionByZero");
  if (!signed_twos_complement) return Value{x.value() / y.value()};
  const auto sign = std::uint64_t{1} << (width - 1), m = mask(width);
  if (x.value() == sign && y.value() == m)
    return fail(ErrorCode::Overflow, "ArithmeticOverflow");
  const bool negative_x = (x.value() & sign) != 0, negative_y = (y.value() & sign) != 0;
  auto magnitude_x = negative_x ? (std::uint64_t{0} - x.value()) & m : x.value();
  auto magnitude_y = negative_y ? (std::uint64_t{0} - y.value()) & m : y.value();
  auto quotient = magnitude_x / magnitude_y;
  return Value{negative_x != negative_y ? (std::uint64_t{0} - quotient) & m : quotient};
}
Expected<Value> checked_shift(const Value &value, std::uint64_t count, std::uint32_t width,
                              bool left) {
  auto n = bits(value, width);
  if (!n) return n.error();
  if (count >= width) return fail(ErrorCode::InvalidArgument, "ShiftOutOfRange");
  return Value{left ? (n.value() << count) & mask(width) : n.value() >> count};
}
} // namespace leanat::numeric
