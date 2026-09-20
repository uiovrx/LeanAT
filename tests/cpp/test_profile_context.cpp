#include "../conformance/profile_context.hpp"
#include "test_support.hpp"
#include <filesystem>
using namespace leanat;
using namespace conformance::profile;
int main() {
  auto exact = observe(Value{UINT64_MAX}).dump();
  LEANAT_CHECK(exact == "{\"u64\":\"18446744073709551615\"}");
  LEANAT_CHECK(Json(std::string("a\0\n\\\"", 5)).dump() == "\"a\\u0000\\u000a\\\\\\\"\"");
  PayloadSnapshot payload;
  payload.address = UINT64_MAX;
  payload.data = {0, 255};
  payload.byte_enable = {255, 0};
  payload.extensions["x"] = {3, 4};
  WireCall call;
  call.connection = ConnectionId{2};
  call.request = payload;
  Recorder recorder;
  recorder.add("call", call);
  recorder.add("error", fail(ErrorCode::Overflow, "exact"));
  const auto rows = recorder.json().dump();
  LEANAT_CHECK(recorder.size() == 2 && rows.find("\"connection\":\"2\"") != std::string::npos &&
               rows.find("18446744073709551615") != std::string::npos &&
               rows.find("\"extensions\":[{\"key\":\"x\",\"bytes\":[\"3\",\"4\"]}]") !=
                   std::string::npos);
  exec::Project project;
  project.schema_major = 4;
  project.types = {{exec::TypeKind::Bits, 64}};
  project.state_types = {0};
  project.initial_state = {Value{std::uint64_t{7}}};
  exec::ComponentDesc component;
  component.id = 3;
  component.states = {{0, 0, project.initial_state[0]}};
  project.components = {component};
  exec::Program program;
  program.id = 9;
  program.result_types = {0};
  program.effect_mask = exec::Effect::StateRead;
  exec::Block block;
  exec::Instruction read;
  read.op = exec::Op::LoadState;
  read.dest = exec::Reg{0, 0};
  block.instructions = {read};
  block.terminator.values = {{0, 0}};
  program.blocks = {block};
  project.programs = {program};
  exec::InstanceDesc instance;
  instance.id = 20;
  instance.definition = 3;
  instance.state_count = 1;
  instance.handlers = {{4, 9, ContextKind::Timed, "timer", {}, {}, ""}};
  project.instances = {instance};
  exec::SystemMetadata system;
  system.id = 1;
  system.runtime_domain = 7;
  system.original_instances = {{20, 3, {}, {}}};
  project.system_metadata = system;
  auto validated = exec::validate(project);
  LEANAT_CHECK(validated);
  auto encoded = exec::serialize(validated.value());
  LEANAT_CHECK(encoded);
  const auto path = std::filesystem::temp_directory_path() / "leanat-profile-context-test.bin";
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{path};
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(encoded.value().data()),
              static_cast<std::streamsize>(encoded.value().size()));
    LEANAT_CHECK(out);
  }
  const auto digest = digest_hex(exec::sha256(encoded.value()));
  auto mismatch = ProfileContext::load(path.string(), project.profile, std::string(64, '0'));
  LEANAT_CHECK(!mismatch && mismatch.error().code == ErrorCode::Integrity);
  auto wrong = ProfileContext::load(path.string(), "AT-Ext-1.1-draft", digest);
  LEANAT_CHECK(!wrong && wrong.error().code == ErrorCode::Unsupported);
  auto context = ProfileContext::load(path.string(), project.profile, digest);
  LEANAT_CHECK(context);
  auto config = context.value()->config();
  LEANAT_CHECK(config.domain == DomainId{7} &&
               config.instances == std::vector<InstanceId>{InstanceId{20}} &&
               config.descriptor_identity == digest);
  auto manifest = context.value()->manifest(config);
  LEANAT_CHECK(manifest.domain == DomainId{7} && manifest.descriptor_identity == digest);
  config.instance = InstanceId{99};
  bool rejected = false;
  try {
    context.value()->manifest(config);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  LEANAT_CHECK(rejected);
  LEANAT_CHECK(context.value()->handler(20, 4).program_id == 9);
  auto execution = context.value()->execution(9, {Tick{11}, 2});
  LEANAT_CHECK(execution.instance == InstanceId{20} && execution.domain == DomainId{7});
  exec::Interpreter vm(context.value()->validated(), context.value()->state_refs());
  EventTxn txn({}, execution);
  exec::FuelCounter fuel{2};
  auto result = vm.execute_segment(9, execution, {}, txn, fuel);
  LEANAT_CHECK(result && result.value().values == std::vector<Value>{Value{std::uint64_t{7}}} &&
               fuel.remaining == 0 && txn.commit());
  auto envelope = context.value()
                      ->record("C-T00", observe(context.value()->config()),
                               Json::object({{"stop", "Completed"},
                                             {"fuel", observe(std::uint64_t{2})},
                                             {"records", recorder.json()}}),
                               {"unit-fixture"})
                      .dump();
  LEANAT_CHECK(envelope.find("\"wrongProfileRejected\":true") != std::string::npos &&
               envelope.find(digest) != std::string::npos);
}
