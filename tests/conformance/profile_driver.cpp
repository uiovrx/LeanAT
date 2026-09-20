#include "harness.hpp"
#include "leanat/descriptor.hpp"
#include "leanat/interpreter.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

using namespace leanat;
using namespace conformance;
static std::string value(const Value &v) {
  if (std::holds_alternative<std::monostate>(v.data))
    return "{\"unit\":null}";
  if (auto p = std::get_if<bool>(&v.data))
    return std::string("{\"bool\":") + (*p ? "true}" : "false}");
  if (auto p = std::get_if<std::uint64_t>(&v.data))
    return "{\"u64\":" + json(std::to_string(*p)) + "}";
  if (auto p = std::get_if<std::int64_t>(&v.data))
    return "{\"i64\":" + json(std::to_string(*p)) + "}";
  std::string out = "[";
  if (auto p = std::get_if<Bytes>(&v.data)) {
    for (auto b : *p) {
      if (out.size() > 1)
        out += ",";
      out += std::to_string(b);
    }
    return "{\"bytes\":" + out + "]}";
  }
  if (auto p = std::get_if<Value::Array>(&v.data)) {
    for (const auto &x : *p) {
      if (out.size() > 1)
        out += ",";
      out += value(x);
    }
    return "{\"array\":" + out + "]}";
  }
  throw std::runtime_error("profile fixture does not serialize handle values");
}
static std::string values(const std::vector<Value> &xs) {
  std::string out = "[";
  for (const auto &x : xs) {
    if (out.size() > 1)
      out += ",";
    out += value(x);
  }
  return out + "]";
}
int main(int argc, char **argv) {
  if (argc != 3)
    return 2;
  try {
    exec::LoadPolicy policy;
    policy.expected_profile = argv[2];
    std::ifstream file(argv[1], std::ios::binary);
    require(bool(file), "missing descriptor");
    Bytes bytes((std::istreambuf_iterator<char>(file)), {});
    auto loaded = exec::load_descriptor(bytes, policy);
    if (!loaded) {
      std::cout << "{\"loadError\":" << json(loaded.error().message) << "}\n";
      return 3;
    }
    const auto &project = loaded.value().get();
    std::vector<VersionedCell> cells;
    for (const auto &v : project.initial_state)
      cells.push_back({v});
    std::vector<VersionedCell *> pointers;
    for (auto &c : cells)
      pointers.push_back(&c);
    ExecutionContext context;
    context.ready = {Tick{11}, 2};
    context.instance = InstanceId{1};
    context.connection = ConnectionId{2};
    EventTxn txn({}, context);
    exec::FuelCounter fuel{1000};
    exec::Interpreter vm(loaded.value(), pointers);
    auto result = vm.execute_segment(0, context, {}, txn, fuel);
    std::string stop = "Failed", error;
    std::vector<Value> returned;
    std::vector<TraceEvent> traces;
    if (!result) {
      error = result.error().message;
      txn.discard();
    } else if (result.value().kind == exec::SegmentResult::Kind::Returned) {
      auto actions = take(txn.commit());
      require(actions.actions.empty(), "unsupported external action");
      stop = "Completed";
      returned = result.value().values;
      traces = result.value().traces;
    } else {
      error = result.value().error;
      txn.discard();
    }
    std::vector<Value> state;
    for (const auto &c : cells)
      state.push_back(c.value);
    std::cout << "{\"loadedProfile\":" << json(project.profile)
              << ",\"semantic\":{\"stop\":" << json(stop) << ",\"error\":" << json(error)
              << ",\"result\":" << values(returned) << ",\"state\":" << values(state)
              << ",\"trace\":[";
    bool first = true;
    for (const auto &t : traces) {
      if (!first)
        std::cout << ',';
      first = false;
      std::cout << "{\"kind\":" << json(t.kind) << ",\"time\":" << t.ready.time.value
                << ",\"turn\":" << t.ready.turn << ",\"instance\":" << t.instance.value
                << ",\"connection\":" << t.connection.value << ",\"detail\":" << json(t.detail)
                << ",\"values\":" << values(t.values) << '}';
    }
    std::cout << "]},\"fuelUsed\":" << 1000 - fuel.remaining << "}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 4;
  }
}
