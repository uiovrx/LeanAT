#include "harness.hpp"
#include "profile_context.hpp"
#include <fstream>
#include <iostream>
using namespace leanat;
using namespace conformance;
using namespace conformance::profile;
namespace {
void put(Bytes &bytes, std::size_t offset, std::uint64_t value, unsigned width) {
  require(offset <= bytes.size() && width <= bytes.size() - offset, "mutation outside descriptor");
  for (unsigned n = 0; n < width; ++n)
    bytes[offset + n] = std::uint8_t(value >> (n * 8));
}
std::uint32_t word(const Bytes &bytes, std::size_t &offset) {
  require(offset <= bytes.size() && 4 <= bytes.size() - offset, "prefix outside descriptor");
  std::uint32_t result = 0;
  for (unsigned n = 0; n < 4; ++n)
    result |= std::uint32_t(bytes[offset++]) << (8 * n);
  return result;
}
void skip(std::size_t &offset, std::size_t count, const Bytes &bytes) {
  require(offset <= bytes.size() && count <= bytes.size() - offset, "prefix outside descriptor");
  offset += count;
}
void resign(Bytes &bytes) {
  std::fill(bytes.begin() + 72, bytes.begin() + 104, 0);
  auto hash = exec::sha256(bytes);
  std::copy(hash.begin(), hash.end(), bytes.begin() + 72);
}
Expected<Json> project_diagnostic(const Error &actual, const std::string &path,
                                  const std::string &declared_path, ErrorCode expected_code,
                                  const std::string &reason,
                                  std::optional<std::size_t> reader_offset) {
  const auto suffix = reader_offset ? " at byte " + std::to_string(*reader_offset) : "";
  if (path != declared_path || actual.code != expected_code || actual.message != reason + suffix)
    return fail(ErrorCode::Integrity, "descriptor diagnostic projection mismatch");
  return Json::object({{"code", observe(actual.code)},
                       {"reason", actual.message.substr(0, actual.message.size() - suffix.size())},
                       {"fieldPath", path}});
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 6)
    return 2;
  try {
    require(std::string(argv[5]) == "descriptor", "wrong descriptor catalog scenario");
    auto context = take(ProfileContext::load(argv[2], argv[3], argv[4]));
    const auto &original = context->descriptor_bytes();
    require(!context->project().types.empty() && !context->project().state_types.empty() &&
                !context->project().initial_state.empty() && !context->project().programs.empty(),
            "descriptor fixture needs actual types/state/program");
    require(context->project().types.front().kind == exec::TypeKind::Bits &&
                context->project().state_types.front() == 0,
            "declared projection requires first Bits type and state TypeId0");
    std::size_t offset = 104;
    auto profile_length = word(original, offset);
    skip(offset, profile_length, original);
    const auto type_count = word(original, offset);
    require(type_count > 0, "type table empty");
    const auto type_tag = offset;
    for (std::uint32_t n = 0; n < type_count; ++n) {
      skip(offset, 9, original);
      auto fields = word(original, offset);
      skip(offset, std::size_t(fields) * 4, original);
      auto constructors = word(original, offset);
      for (std::uint32_t c = 0; c < constructors; ++c) {
        auto count = word(original, offset);
        skip(offset, std::size_t(count) * 4, original);
      }
    }
    auto state_count = word(original, offset);
    require(state_count > 0, "state type table empty");
    const auto state_type = offset;
    skip(offset, std::size_t(state_count) * 4, original);
    auto value_count = word(original, offset);
    require(value_count > 0, "state values empty");
    const auto value_tag = offset;
    Recorder inputs, observations;
    std::vector<Json> artifact_diagnostics;
    inputs.add("initialState", context->state());
    auto program_id = context->project().programs.front().id;
    auto execution = context->execution(program_id, {Tick{11}, 2});
    inputs.add("positiveContext", execution);
    exec::Interpreter vm(context->validated(), context->state_refs());
    EventTxn txn({}, execution);
    exec::FuelCounter fuel{1000};
    auto positive = take(vm.execute_segment(program_id, execution, {}, txn, fuel));
    require(positive.kind == exec::SegmentResult::Kind::Returned,
            "valid descriptor positive control did not return");
    observations.add("validExecution", positive);
    observations.add("validCommit", take(txn.commit()));
    observations.add("validState", context->state());
    struct Mutation {
      const char *branch;
      const char *path;
      std::size_t offset;
      std::uint64_t value;
      unsigned width;
      ErrorCode code;
      const char *reason;
    };
    const std::vector<Mutation> mutations = {
        {"version", "header.major", 4, 65535, 2, ErrorCode::Unsupported, "schema version"},
        {"type", "types[0].kind", type_tag, 255, 1, ErrorCode::Schema, "unknown type"},
        {"tag", "initialState[0].tag", value_tag, 255, 1, ErrorCode::Schema, "bits width"},
        {"index", "stateTypes[0]", state_type, UINT32_MAX, 4, ErrorCode::Schema,
         "literal type ID"}};
    for (const auto &mutation : mutations) {
      auto bytes = original;
      put(bytes, mutation.offset, mutation.value, mutation.width);
      resign(bytes);
      const auto artifact_path = std::string(argv[1]) + "." + mutation.branch + ".bin";
      {
        std::ofstream artifact(artifact_path, std::ios::binary);
        artifact.write(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        require(bool(artifact), "mutation artifact write failed");
      }
      inputs.add("mutation", Json::object({{"branch", mutation.branch},
                                           {"path", mutation.path},
                                           {"value", mutation.value},
                                           {"width", mutation.width},
                                           {"integrityRecomputed", true}}));
      exec::LoadPolicy policy;
      policy.expected_profile = argv[3];
      std::uint64_t invocations = 0;
      auto loaded = exec::load_descriptor(bytes, policy);
      if (loaded) {
        ++invocations;
        exec::Interpreter invalid_vm(loaded.value());
        EventTxn invalid_txn({}, execution);
        exec::FuelCounter invalid_fuel{1000};
        (void)invalid_vm.execute_segment(program_id, execution, {}, invalid_txn, invalid_fuel);
      }
      require(!loaded, "malformed descriptor was accepted");
      require(loaded.error().code == mutation.code, "malformed descriptor wrong rejection class");
      require(invocations == 0, "malformed descriptor executed");
      // Reader::value consumes the value tag before checking type IDs/type tags.
      const std::optional<std::size_t> reader_offset =
          std::string(mutation.branch) == "version" ? std::nullopt
                                                    : std::optional<std::size_t>{value_tag + 1};
      auto projected = take(project_diagnostic(loaded.error(), mutation.path, mutation.path,
                                               mutation.code, mutation.reason, reader_offset));
      auto wrong_code = loaded.error();
      wrong_code.code = ErrorCode::ExternalFailure;
      auto wrong_reason = loaded.error();
      wrong_reason.message = "wrong reason";
      auto wrong_offset = loaded.error();
      wrong_offset.message = std::string(mutation.reason) + " at byte " +
                             std::to_string(reader_offset.value_or(0) + 1);
      require(!project_diagnostic(wrong_code, mutation.path, mutation.path, mutation.code,
                                  mutation.reason, reader_offset) &&
                  !project_diagnostic(wrong_reason, mutation.path, mutation.path, mutation.code,
                                      mutation.reason, reader_offset) &&
                  !project_diagnostic(loaded.error(), "wrong.field", mutation.path, mutation.code,
                                      mutation.reason, reader_offset) &&
                  !project_diagnostic(wrong_offset, mutation.path, mutation.path, mutation.code,
                                      mutation.reason, reader_offset),
              "diagnostic projection accepted corrupted code/reason/path/offset");
      artifact_diagnostics.push_back(
          Json::object({{"branch", mutation.branch},
                        {"rawDiagnostic", observe(loaded.error())},
                        {"mutationByteOffset", mutation.offset},
                        {"readerByteOffset", observe(reader_offset)},
                        {"artifactFile", artifact_path},
                        {"artifactSha256", digest_hex(exec::sha256(bytes))},
                        {"projectionValidated", true},
                        {"negativeControls", Json::object({{"wrongCodeRejected", true},
                                                           {"wrongReasonRejected", true},
                                                           {"wrongPathRejected", true},
                                                           {"wrongOffsetRejected", true}})}}));
      observations.add("rejectedMutation", Json::object({{"branch", mutation.branch},
                                                         {"error", projected},
                                                         {"handlerInvocations", invocations},
                                                         {"fuel", std::uint64_t{0}}}));
    }
    const auto row = context->record(
        "C-T26", inputs.json(),
        Json::object({{"records", observations.json()},
                      {"stop", "RejectedBeforeExecution"},
                      {"fuel", 1000 - fuel.remaining}}),
        {"version", "type", "tag", "index", "valid-execution-control"}, "Complete",
        Json::object({{"schema", "leanat.descriptor-diagnostic-projection.v1"},
                      {"readerRule", "Reader::value tag consumed before type checks"},
                      {"diagnostics", Json::array(artifact_diagnostics)}}));
    std::ofstream output(argv[1]);
    output << row.dump() << '\n';
    require(bool(output), "descriptor evidence write failed");
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
