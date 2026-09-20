#pragma once
#include "leanat/common.hpp"

namespace leanat::numeric {
// Existing ExecIR immediate tags. Width is supplied by the validated type schema.
Expected<Value> eval_unary(std::uint32_t mode, const Value &, std::uint32_t width = 0);
Expected<Value> eval_compare(std::uint32_t mode, const Value &, const Value &,
                             std::uint32_t width = 0);
Expected<Value> eval_convert(std::uint32_t mode, const Value &, std::uint32_t source_width,
                             std::uint32_t dest_width);
// Explicit checked helpers; never implicit replacements for source Lean operators.
// Signed division accepts/returns width-bit two's-complement encodings.
Expected<Value> checked_divide(const Value &, const Value &, std::uint32_t width,
                               bool signed_twos_complement = false);
Expected<Value> checked_shift(const Value &, std::uint64_t count, std::uint32_t width,
                              bool left);
} // namespace leanat::numeric
