#pragma once
#include "opcode_values.hpp"
#include <set>

namespace leanat::opcode_test {
inline bool optional_bool(const InputJson &j, const char *name, bool fallback) {
  auto value = j.find(name);
  return value ? value->boolean() : fallback;
}
inline bool null_json(const InputJson &j) {
  return std::holds_alternative<std::nullptr_t>(j.value);
}
inline void known_fields(const InputJson &j, std::initializer_list<const char *> fields) {
  for (const auto &entry : j.object()) {
    bool known = false;
    for (auto field : fields)
      known = known || entry.first == field;
    if (!known)
      throw std::runtime_error("unknown profile field: " + entry.first);
  }
}
inline void apply_world_profile(const InputJson &world, RuntimeConfig &config) {
  if (auto pins = world.find("maxPins"))
    config.pin_capacity = size_t(pins->natural(1048576));
  struct Rule {
    bool per_slot, persistent, allow_max;
  };
  std::map<std::string, Rule> groups;
  std::set<std::pair<uint64_t, uint64_t>> rule_keys;
  if (auto rules = world.find("allocationRules"))
    for (const auto &rule : rules->array()) {
      known_fields(rule,
                   {"kind", "store", "group", "perSlot", "persistent", "allowMax", "capacity"});
      auto kind = rule.at("kind").natural(14), store = rule.at("store").natural(UINT32_MAX);
      auto group = rule.at("group").string();
      if (group.empty() || !rule_keys.emplace(kind, store).second)
        throw std::runtime_error("invalid allocator rule identity");
      Rule policy{optional_bool(rule, "perSlot", false), optional_bool(rule, "persistent", true),
                  optional_bool(rule, "allowMax", true)};
      auto inserted = groups.emplace(group, policy);
      if (!inserted.second && (inserted.first->second.per_slot != policy.per_slot ||
                               inserted.first->second.persistent != policy.persistent ||
                               inserted.first->second.allow_max != policy.allow_max))
        throw std::runtime_error("allocator group policy differs");
      if (auto capacity = rule.find("capacity"); capacity && !null_json(*capacity)) {
        auto limit = capacity->natural(uint64_t(UINT32_MAX) + 1);
        if ((kind == 5 && store == 20) || (kind == 6 && store == 21)) {
          if (limit > 1048576)
            throw std::runtime_error("native allocator host capacity bound");
          if (kind == 5)
            config.result_capacity = size_t(limit);
          else
            config.consumer_capacity = size_t(limit);
        }
      }
    }
  std::set<std::tuple<std::string, uint64_t, std::optional<uint64_t>>> counters;
  if (auto values = world.find("allocationCounters"))
    for (const auto &counter : values->array()) {
      known_fields(counter, {"group", "domain", "slot", "nextGeneration", "persistent", "retired"});
      auto group = counter.at("group").string();
      auto domain = counter.at("domain").natural(UINT32_MAX);
      std::optional<uint64_t> slot;
      if (auto v = counter.find("slot"); v && !null_json(*v))
        slot = v->natural(UINT32_MAX);
      if (group.empty() || !counters.emplace(group, domain, slot).second)
        throw std::runtime_error("invalid allocator counter identity");
      bool sentinel = false;
      if (auto next = counter.find("nextGeneration")) {
        sentinel = next->integer_text() == "18446744073709551616";
        if (!sentinel && !next->natural())
          throw std::runtime_error("zero allocator generation");
      }
      auto persistent = optional_bool(counter, "persistent", true);
      if (optional_bool(counter, "retired", false) && !sentinel)
        throw std::runtime_error("retired allocator counter is not exhausted");
      if (auto rule = groups.find(group);
          rule != groups.end() &&
          (rule->second.per_slot != slot.has_value() || rule->second.persistent != persistent))
        throw std::runtime_error("allocator counter policy differs");
    }
}
} // namespace leanat::opcode_test
