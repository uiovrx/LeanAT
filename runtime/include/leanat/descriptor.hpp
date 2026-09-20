#pragma once
#include "exec_ir.hpp"
#include <array>
namespace leanat::exec {
struct LoadPolicy {
  std::size_t max_file_bytes{16777216}, max_string_bytes{65536}, memory_budget{67108864};
  Limits limits;
  std::string expected_profile{"AT-Core-1.1-draft"};
  std::vector<std::string> allowed_capabilities;
};
std::array<std::uint8_t, 32> sha256(const Bytes &);
Expected<Bytes> serialize(const ValidatedProject &);
Expected<ValidatedProject> load_descriptor(const Bytes &, LoadPolicy = {});
} // namespace leanat::exec
