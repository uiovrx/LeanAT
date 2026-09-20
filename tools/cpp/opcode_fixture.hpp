#pragma once
#include "../../tests/conformance/profile_observations.hpp"
#include <functional>
#include <leanat/runtime_services.hpp>
#include <memory>

namespace leanat::opcode_test {
using conformance::profile::Json;
using conformance::profile::observe;

// Source-catalog setup, never an expected result. Owned values cross the boundary.
struct FixtureConfig {
  std::string case_id;
  std::string variant;
  ExecutionContext context;
  std::vector<Value> inputs;
  std::map<std::string, Value> environment;
  // Shared actual interpreter observer for explicitly executed child programs.
  exec::InterpreterObserver *observer{};
};

// A family adapter registers its real providers and retains their concrete stores.
// Snapshots must read those stores; no expected-value lookup is allowed.
struct ProviderFixture {
  std::vector<Value> inputs;
  std::vector<std::shared_ptr<void>> lifetime;
  std::function<Json()> snapshot;
  // Named semantic objects map to raw identities. Preserve all identity fields;
  // distinct consumers/leases/events must remain distinct objects.
  std::function<Json()> identities;
  // Publish already committed native provider effects before observing state.
  std::function<Expected<void>()> after_commit;
  // Explicit host lifecycle handling of the committed segment exit.
  std::function<Expected<void>(const exec::SegmentResult &)> after_segment;
};

using FixtureFactory = std::function<Expected<ProviderFixture>(
    Runtime &, CoreRuntimeBackend &, const exec::Project &, const FixtureConfig &)>;
} // namespace leanat::opcode_test
