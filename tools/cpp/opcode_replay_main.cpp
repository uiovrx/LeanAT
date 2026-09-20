#include "../../tests/conformance/opcode_protocol_fixture.hpp"
#include "opcode_object_fixture.hpp"
#include "opcode_optional_fixture.hpp"
#include "opcode_payload_fixture.hpp"
#include "opcode_profile.hpp"
#include "opcode_static.hpp"
#include "opcode_sideband_fixture.hpp"
#include "opcode_values.hpp"
#if __has_include("opcode_core_fixture.hpp")
#include "opcode_core_fixture.hpp"
#define LEANAT_HAS_OPCODE_CORE 1
#endif
#if __has_include("opcode_structured_fixture.hpp")
#include "opcode_structured_fixture.hpp"
#define LEANAT_HAS_OPCODE_STRUCTURED 1
#endif
#include <iostream>
#include <leanat/descriptor.hpp>
#ifdef LEANAT_OPCODE_SYSTEMC
#include <systemc>
#endif

namespace leanat::opcode_test {
template <class T> T need(Expected<T> value) {
  if (!value)
    throw std::runtime_error(value.error().message);
  return std::move(value.value());
}
inline void need(Expected<void> value) {
  if (!value)
    throw std::runtime_error(value.error().message);
}
inline std::string quote(const std::string &s) {
  return Json(s).dump();
}
inline uint64_t optional_nat(const InputJson &j, const char *key, uint64_t fallback) {
  auto *p = j.find(key);
  return p ? p->natural() : fallback;
}
inline std::string hex(const std::array<uint8_t, 32> &hash) {
  static const char *a = "0123456789abcdef";
  std::string out;
  for (auto b : hash) {
    out += a[b >> 4];
    out += a[b & 15];
  }
  return out;
}
struct Host : RuntimeHost {
  conformance::profile::Recorder records;
  Expected<WireReturn> transport(const SendIntent &i) override {
    records.add("unexpectedTransport", i);
    return fail(ErrorCode::Unsupported, "opcode fixture has no implicit external peer");
  }
  void arm(std::optional<WakePoint> point) override {
    if (point)
      records.add("arm", Json::object({{"time", observe(point->time)},
                                       {"turn", point->turn},
                                       {"generation", point->generation}}));
  }
  void publish_output(PortId p, const Value &v) override {
    records.add("output", Json::object({{"port", observe(p)}, {"value", observe(v)}}));
  }
  void publish_output(InstanceId i, PortId p, const Value &v) override {
    records.add(
        "output",
        Json::object({{"instance", observe(i)}, {"port", observe(p)}, {"value", observe(v)}}));
  }
  void emit_trace(const TraceEvent &t) override {
    records.add("trace", t);
  }
  void observe_milestone(const RuntimeMilestoneObservation &m) override {
    records.add("milestone", m);
  }
};
class OpcodeLog : public exec::InterpreterObserver {
  const exec::Project &project_;
  size_t bytes_{};

public:
  std::vector<std::string> rows;
  explicit OpcodeLog(const exec::Project &p) : project_(p) {}

protected:
  bool on_opcode(const exec::OpcodeObservation &o) override {
    if (rows.size() >= 100000)
      return false;
    std::string row = "{\"stage\":" +
                      quote(o.stage == exec::OpcodeObservation::Stage::Entered     ? "entered"
                            : o.stage == exec::OpcodeObservation::Stage::Completed ? "completed"
                                                                                   : "error") +
                      ",\"program\":" + std::to_string(o.program_id) +
                      ",\"block\":" + std::to_string(o.block_id) +
                      ",\"instruction\":" + std::to_string(o.instruction_index) +
                      ",\"callDepth\":" + std::to_string(o.call_depth) +
                      ",\"opcode\":" + std::to_string(unsigned(o.instruction.op)) +
                      ",\"source\":" + quote(o.instruction.source) + ",\"fuelBefore\":\"" +
                      std::to_string(o.fuel_before) + "\",\"fuelAfter\":\"" +
                      std::to_string(o.fuel_after) + "\",\"arguments\":[";
    for (size_t i = 0; i < o.instruction.args.size(); ++i) {
      if (i)
        row += ',';
      row += typed_value(project_, o.argument(i), o.instruction.args[i].type);
    }
    row += "],\"result\":";
    row += o.result && o.instruction.dest
               ? typed_value(project_, *o.result, o.instruction.dest->type)
               : "null";
    row += ",\"error\":";
    row += o.error ? observe(*o.error).dump() : "null";
    row += '}';
    if (row.size() > 8 * 1024 * 1024 - bytes_)
      return false;
    bytes_ += row.size();
    rows.push_back(std::move(row));
    return true;
  }
};
inline std::string rows_json(const std::vector<std::string> &rows) {
  std::string out = "[";
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i)
      out += ',';
    out += rows[i];
  }
  return out + "]";
}
ProviderFixture prepare(const std::string &family, Runtime &r, CoreRuntimeBackend &b,
                        const exec::Project &p, const FixtureConfig &c) {
  if (family == "pure" || family == "numeric") {
    if (!p.services.empty())
      throw std::runtime_error("pure fixture contains unbound services");
    ProviderFixture f;
    f.inputs = c.inputs;
    f.snapshot = []() { return Json::object({}); };
    f.identities = []() { return Json::array({}); };
    return f;
  }
  if (family == "payload")
    return need(make_payload_fixture(r, b, p, c));
  if (family == "sideband")
    return need(bind_sideband_fixture(r, b, p, c));
  if (family == "optional")
    return need(make_optional_fixture(r, b, p, c));
  if (family == "object" || family == "objects")
    return need(make_object_fixture(r, b, p, c));
  if (family == "protocol")
    return need(make_protocol_fixture(r, b, p, c));
#ifdef LEANAT_HAS_OPCODE_CORE
  if (family == "core")
    return need(bind_core_fixture(r, b, p, c));
#endif
#ifdef LEANAT_HAS_OPCODE_STRUCTURED
  if (family == "structured")
    return need(make_structured_fixture(r, b, p, c));
#endif
  throw std::runtime_error("actual provider fixture unavailable: " + family);
}
std::string run_case(const InputJson &request) {
  exact_fields(request, {"schema", "caseId", "variant", "providerFamily", "profile", "programId",
                         "descriptor", "descriptorHash", "input"});
  if (request.at("schema").string() != "leanat.opcode-request.v1")
    throw std::runtime_error("opcode request schema mismatch");
  auto validated = load_opcode_request(request);
  const auto descriptor_hash = request.at("descriptorHash").string();
  const auto &p = validated.get();
  auto program_id = request.at("programId").natural(UINT32_MAX);
  auto program = std::find_if(p.programs.begin(), p.programs.end(),
                              [&](const auto &item) { return item.id == program_id; });
  if (program == p.programs.end())
    throw std::runtime_error("program missing");
  const auto &input = request.at("input");
  if (input.at("schema").string() != "leanat.reference-input.v1")
    throw std::runtime_error("reference input schema mismatch");
  const auto &context = input.at("context");
  FixtureConfig c;
  c.case_id = request.at("caseId").string();
  c.variant = request.at("variant").string();
  c.context.kind = ContextKind(context.at("kind").natural(4));
  c.context.domain = DomainId{uint32_t(context.at("domain").natural(UINT32_MAX))};
  c.context.instance = InstanceId{uint32_t(context.at("instanceId").natural(UINT32_MAX))};
  c.context.connection = ConnectionId{uint32_t(optional_nat(context, "connection", 0))};
  c.context.ready = {Tick{context.at("now").natural()}, context.at("turn").natural()};
  c.context.owner = context.at("owner").natural();
  for (const auto &v : input.at("inputs").array())
    c.inputs.push_back(decode_value(v));
  for (const auto &e : context.at("environment").array()) {
    if (e.at("name").string() == "profile.segmentBytes" || e.at("name").string() == "profile.valueNodeBytes")
      if (e.at("value").at("kind").string() != "bits" || e.at("value").at("width").natural() != 64)
        throw std::runtime_error("native profile fields require Bits64");
    if (!c.environment.emplace(e.at("name").string(), decode_value(e.at("value"))).second)
      throw std::runtime_error("duplicate environment name");
  }
  auto charge = c.environment.find("profile.valueNodeBytes");
  if (charge == c.environment.end() || std::get<uint64_t>(charge->second.data) != sizeof(Value))
    throw std::runtime_error("profile.valueNodeBytes differs from actual sizeof(Value)");
  RuntimeConfig config;
  config.domain = c.context.domain;
  config.instance = c.context.instance;
  config.descriptor_identity = descriptor_hash;
  config.connections = {ConnectionId{c.context.connection.value ? c.context.connection.value : 1}};
  if (auto *world = input.find("world"))
    config.event_capacity =
        world->find("maxEvents") ? size_t(world->at("maxEvents").natural(1048576)) : 1024;
  if (auto *world = input.find("world"))
    apply_world_profile(*world, config);
  Host host;
  Runtime runtime(config, host);
  need(runtime.start({config.domain, config.descriptor_identity, config.connections, true, true}));
  CoreRuntimeBackend backend(runtime, p);
  OpcodeLog log(p);
  c.observer = &log;
  auto fixture = prepare(request.at("providerFamily").string(), runtime, backend, p, c);
  need(backend.freeze());
  auto initial_provider = fixture.snapshot ? fixture.snapshot() : Json();
  auto initial_ids = fixture.identities ? fixture.identities() : Json();
  std::vector<Value> initial;
  for (const auto &v : input.at("committed").array())
    initial.push_back(decode_value(v));
  if (initial.size() != p.state_types.size())
    throw std::runtime_error("initial state arity differs");
  std::vector<VersionedCell> cells;
  cells.reserve(initial.size());
  for (size_t i = 0; i < initial.size(); ++i) {
    if (!exec::conforms(p, p.state_types[i], initial[i]))
      throw std::runtime_error("initial state type mismatch");
    cells.push_back({initial[i]});
  }
  std::vector<VersionedCell *> pointers;
  for (auto &cell : cells)
    pointers.push_back(&cell);
  SegmentBudget budget;
  auto segment_bytes = c.environment.find("profile.segmentBytes");
  if (segment_bytes == c.environment.end() || !std::holds_alternative<uint64_t>(segment_bytes->second.data))
    throw std::runtime_error("profile.segmentBytes must declare the native staging budget");
  budget.bytes = size_t(std::get<uint64_t>(segment_bytes->second.data));
  budget.writes = 4096;
  budget.actions = 4096;
  budget.events = 4096;
  EventTxn txn(budget, c.context);
  const uint64_t fuel = input.at("fuel").natural();
  exec::FuelCounter counter{fuel};
  exec::Interpreter vm(validated, pointers, &backend, &log);
  auto executed = vm.execute_segment(uint32_t(program_id), c.context, fixture.inputs, txn, counter);
  bool ok = false;
  std::string error = "null", exit = "failed", returned = "[]";
  CommittedActions actions;
  if (!executed)
    error = observe(executed.error()).dump();
  else if (executed.value().kind == exec::SegmentResult::Kind::Failed)
    error = Json(executed.value().error).dump();
  else {
    auto commit = runtime.commit_segment(txn);
    if (!commit)
      error = observe(commit.error()).dump();
    else {
      actions = std::move(commit.value());
      ok = true;
      exit = executed.value().kind == exec::SegmentResult::Kind::Returned    ? "returned"
             : executed.value().kind == exec::SegmentResult::Kind::Suspended ? "suspended"
                                                                             : "transportReturn";
      if (executed.value().kind == exec::SegmentResult::Kind::Returned)
        returned = typed_values(p, executed.value().values, program->result_types);
      if (executed.value().kind == exec::SegmentResult::Kind::TransportReturned) {
        std::optional<uint32_t> result_type;
        for (const auto &block : program->blocks)
          if (block.terminator.kind == exec::TermKind::TransportReturn) {
            if (result_type && *result_type != block.terminator.value.type)
              throw std::runtime_error("ambiguous transport return schema");
            result_type = block.terminator.value.type;
          }
        if (!result_type)
          throw std::runtime_error("missing transport return schema");
        returned = typed_values(p, executed.value().values, {*result_type});
      }
      if (fixture.after_segment) {
        auto effect = fixture.after_segment(executed.value());
        if (!effect) {
          ok = false;
          error = observe(effect.error()).dump();
          exit = "hostLifecycleFailed";
        }
      }
      if (ok && fixture.after_commit) {
        auto effect = fixture.after_commit();
        if (!effect) {
          ok = false;
          error = observe(effect.error()).dump();
          exit = "hostEffectFailed";
        }
      }
    }
  }
  if (txn.is_open())
    need(txn.discard());
  std::vector<Value> committed;
  for (const auto &cell : cells)
    committed.push_back(cell.value);
  auto final_provider = fixture.snapshot ? fixture.snapshot() : Json();
  auto final_ids = fixture.identities ? fixture.identities() : Json();
  std::string out =
      "{\"schema\":\"leanat.opcode-native-output.v1\",\"caseId\":" + quote(c.case_id) +
      ",\"variant\":" + quote(c.variant) + ",\"descriptorHash\":" + quote(descriptor_hash) +
      ",\"ok\":" + (ok ? "true" : "false") + ",\"exit\":" + quote(exit) + ",\"error\":" + error +
      ",\"remainingFuel\":\"" + std::to_string(counter.remaining) + "\",\"returned\":" + returned +
      ",\"committed\":" + typed_values(p, committed, p.state_types) +
      ",\"preparedInputs\":" + typed_values(p, fixture.inputs, program->input_types) +
      ",\"initialProvider\":" + initial_provider.dump() + ",\"provider\":" + final_provider.dump() +
      ",\"initialIdentities\":" + initial_ids.dump() + ",\"identities\":" + final_ids.dump() +
      ",\"actions\":" + observe(actions).dump() + ",\"hostEvents\":" + host.records.json().dump() +
      ",\"opcodeObservationComplete\":" + (log.complete() ? "true" : "false") +
      ",\"opcodeEvents\":" + rows_json(log.rows) +
      ",\"rawSegment\":" + (executed ? observe(executed.value()).dump() : "null") +
      ",\"actualValueNodeBytes\":\"" + std::to_string(sizeof(Value)) +
      "\",\"stageBudget\":" + Json::object({{"bytes",budget.bytes},{"writes",budget.writes},{"actions",budget.actions},{"events",budget.events}}).dump() + "}";
  if (out.size() > 16 * 1024 * 1024)
    throw std::runtime_error("complete opcode output bound");
  return out;
}
} // namespace leanat::opcode_test

#ifdef LEANAT_OPCODE_SYSTEMC
struct OpcodeKernel : sc_core::sc_module {
  const leanat::opcode_test::InputJson &request;
  std::string output;
  std::exception_ptr error;
  SC_HAS_PROCESS(OpcodeKernel);
  OpcodeKernel(sc_core::sc_module_name n, const leanat::opcode_test::InputJson &r)
      : sc_module(n), request(r) {
    SC_THREAD(run);
  }
  void run() {
    try {
      auto time = request.at("input").at("context").at("now").natural();
      if (time)
        sc_core::wait(sc_core::sc_time::from_value(time));
      output = leanat::opcode_test::run_case(request);
    } catch (...) {
      error = std::current_exception();
    }
    sc_core::sc_stop();
  }
};
int sc_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
  using namespace leanat::opcode_test;
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "Usage: leanat_opcode_replay REQUEST.json\n"
                   "       leanat_opcode_replay --host-info\n"
                   "Execute a descriptor verified by hash with actual registered providers.\n"
                   "REQUEST uses leanat.opcode-request.v1; output retains full values,\n"
                   "provider state and actual opcode observations. See docs/USAGE.md.\n";
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--describe") {
      auto request = read_input_json(argv[2]);
      std::cout << describe_opcode_request(request).dump() << std::endl;
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--host-info") {
      std::cout << "{\"schema\":\"leanat.opcode-host.v1\",\"valueNodeBytes\":\""
                << sizeof(leanat::Value) << "\"}" << std::endl;
      return 0;
    }
    if (argc != 2)
      throw std::runtime_error("usage: leanat_opcode_replay request.json");
    auto request = read_input_json(argv[1]);
#ifdef LEANAT_OPCODE_SYSTEMC
    sc_core::sc_report_handler::set_actions(sc_core::SC_INFO, sc_core::SC_DO_NOTHING);
    OpcodeKernel kernel("opcode_kernel", request);
    sc_core::sc_start();
    if (kernel.error)
      std::rethrow_exception(kernel.error);
    std::cout << kernel.output << std::endl;
#else
    std::cout << run_case(request) << std::endl;
#endif
    return 0;
  } catch (const std::exception &e) {
    std::cout << "{\"schema\":\"leanat.opcode-native-error.v1\",\"error\":" << quote(e.what())
              << "}" << std::endl;
    return 2;
  }
}
