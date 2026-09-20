#pragma once
#include "profile_observations.hpp"
#include <fstream>
#include <leanat/descriptor.hpp>

namespace conformance::profile {
inline std::string digest_hex(const std::array<std::uint8_t, 32> &value) {
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (auto byte : value) {
    out += hex[byte >> 4];
    out += hex[byte & 15];
  }
  return out;
}
class ProfileContext {
  exec::ValidatedProject validated_;
  std::string digest_;
  Bytes bytes_;
  std::vector<VersionedCell> state_;
  explicit ProfileContext(exec::ValidatedProject validated, std::string digest, Bytes bytes)
      : validated_(std::move(validated)), digest_(std::move(digest)), bytes_(std::move(bytes)) {
    state_.reserve(project().initial_state.size());
    for (const auto &value : project().initial_state)
      state_.push_back({value});
  }

public:
  ProfileContext(const ProfileContext &) = delete;
  ProfileContext &operator=(const ProfileContext &) = delete;
  static Expected<std::unique_ptr<ProfileContext>> load(const std::string &path,
                                                        const std::string &expected_profile,
                                                        const std::string &expected_sha256) {
    try {
      if (expected_profile != "AT-Core-1.1-draft" && expected_profile != "AT-Ext-1.1-draft")
        return fail(ErrorCode::InvalidArgument, "unknown selected fixture profile");
      if (expected_sha256.size() != 64 ||
          !std::all_of(expected_sha256.begin(), expected_sha256.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return fail(ErrorCode::InvalidArgument,
                    "catalog SHA256 must be 64 lowercase hexadecimal characters");
      exec::LoadPolicy policy;
      policy.expected_profile = expected_profile;
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      if (!file)
        return fail(ErrorCode::Io, "cannot open profile descriptor");
      const auto length = file.tellg();
      if (length < 0 || static_cast<std::uint64_t>(length) > policy.max_file_bytes)
        return fail(ErrorCode::Capacity, "profile descriptor file limit");
      Bytes bytes(static_cast<std::size_t>(length));
      file.seekg(0);
      if (!bytes.empty() && !file.read(reinterpret_cast<char *>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size())))
        return fail(ErrorCode::Io, "cannot read full profile descriptor");
      auto digest = digest_hex(exec::sha256(bytes));
      if (digest != expected_sha256)
        return fail(ErrorCode::Integrity, "catalog descriptor digest mismatch");
      auto loaded = exec::load_descriptor(bytes, policy);
      if (!loaded)
        return loaded.error();
      if (loaded.value().get().schema_major != 4 || !loaded.value().get().system_metadata ||
          loaded.value().get().instances.empty())
        return fail(ErrorCode::Schema,
                    "profile fixture requires actual v4 system/instance metadata");
      // E40 inputs are Core-only even when compiled with the extension profile.
      if (!loaded.value().get().capabilities.empty())
        return fail(ErrorCode::Unsupported, "E40 fixture declares extension capabilities");
      for (const auto &p : loaded.value().get().programs)
        for (const auto &b : p.blocks)
          for (const auto &i : b.instructions)
            if (i.op >= exec::Op::WaitGroupNew)
              return fail(ErrorCode::Unsupported, "E40 fixture contains extension opcode");
      auto wrong_policy = policy;
      wrong_policy.expected_profile =
          expected_profile == "AT-Core-1.1-draft" ? "AT-Ext-1.1-draft" : "AT-Core-1.1-draft";
      auto wrong = exec::load_descriptor(bytes, wrong_policy);
      if (wrong || wrong.error().code != ErrorCode::Unsupported ||
          wrong.error().message != "host profile mismatch")
        return fail(ErrorCode::Integrity, "wrong profile negative control failed");
      return std::unique_ptr<ProfileContext>(
          new ProfileContext(std::move(loaded.value()), std::move(digest), std::move(bytes)));
    } catch (const std::bad_alloc &) {
      return fail(ErrorCode::Capacity, "profile context allocation");
    } catch (const std::exception &e) {
      return fail(ErrorCode::Io, e.what());
    }
  }
  const exec::ValidatedProject &validated() const {
    return validated_;
  }
  const exec::Project &project() const {
    return validated_.get();
  }
  const std::string &descriptor_sha256() const {
    return digest_;
  }
  const Bytes &descriptor_bytes() const {
    return bytes_;
  }
  const exec::InstanceDesc &instance(std::uint32_t id) const {
    for (const auto &i : project().instances)
      if (i.id == id)
        return i;
    throw std::invalid_argument("instance absent from loaded profile descriptor");
  }
  const exec::BindingDesc &binding(std::uint32_t id) const {
    for (const auto &b : project().system_metadata->bindings)
      if (b.id == id)
        return b;
    throw std::invalid_argument("connection absent from loaded profile descriptor");
  }
  const exec::Program &program(std::uint32_t id) const {
    for (const auto &p : project().programs)
      if (p.id == id)
        return p;
    throw std::invalid_argument("program absent from loaded profile descriptor");
  }
  const exec::HandlerBinding &handler(std::uint32_t instance_id, std::uint32_t local_id) const {
    for (const auto &h : instance(instance_id).handlers)
      if (h.local_id == local_id)
        return h;
    throw std::invalid_argument("handler absent from loaded profile descriptor");
  }
  const exec::ComponentDesc &component(std::uint32_t instance_id) const {
    const auto definition = instance(instance_id).definition;
    for (const auto &c : project().components)
      if (c.id == definition)
        return c;
    throw std::invalid_argument("component absent from loaded profile descriptor");
  }
  // Host limits remain explicit test inputs; all identity fields are overwritten from the artifact.
  RuntimeConfig config(RuntimeConfig limits = {}) const {
    limits.domain = DomainId{project().system_metadata->runtime_domain};
    limits.instance = InstanceId{project().instances.front().id};
    limits.descriptor_identity = digest_;
    limits.instances.clear();
    limits.connections.clear();
    limits.connection_bindings.clear();
    for (const auto &i : project().instances)
      limits.instances.push_back(InstanceId{i.id});
    limits.instance_capacity = std::max(limits.instance_capacity, limits.instances.size());
    for (const auto &b : project().system_metadata->bindings) {
      limits.connections.push_back(ConnectionId{b.id});
      limits.connection_bindings.push_back({ConnectionId{b.id},
                                            InstanceId{b.source_endpoint.instance_id},
                                            InstanceId{b.sink_endpoint.instance_id}});
    }
    return limits;
  }
  HostBindingManifest manifest(const RuntimeConfig &actual) const {
    auto derived = config(actual);
    if (actual.domain != derived.domain || actual.instance != derived.instance ||
        actual.descriptor_identity != digest_ || actual.instances != derived.instances ||
        actual.connections != derived.connections ||
        actual.connection_bindings != derived.connection_bindings)
      throw std::invalid_argument("runtime identity/topology differs from loaded descriptor");
    HostBindingManifest m;
    m.domain = actual.domain;
    m.descriptor_identity = digest_;
    m.connections = actual.connections;
    m.clock_grid_valid = true;
    m.capabilities_valid = true;
    m.instances = actual.instances;
    m.connection_bindings = actual.connection_bindings;
    return m;
  }
  std::vector<VersionedCell> &state() {
    return state_;
  }
  const std::vector<VersionedCell> &state() const {
    return state_;
  }
  std::vector<VersionedCell *> state_refs() {
    std::vector<VersionedCell *> out;
    for (auto &cell : state_)
      out.push_back(&cell);
    return out;
  }
  ExecutionContext execution(std::uint32_t program_id, ReadyKey ready, ConnectionId connection = {},
                             std::uint64_t owner = 0, std::uint64_t epoch = 0) const {
    const auto &p = program(program_id);
    ExecutionContext c;
    c.kind = p.context;
    c.domain = DomainId{project().system_metadata->runtime_domain};
    c.ready = ready;
    c.connection = connection;
    c.owner = owner;
    c.epoch = epoch;
    bool found = false;
    for (const auto &i : project().instances)
      for (const auto &h : i.handlers)
        if (h.program_id == program_id) {
          c.instance = InstanceId{i.id};
          found = true;
        }
    if (!found)
      throw std::invalid_argument("program has no descriptor instance binding");
    if (connection.value) {
      const auto &b = binding(connection.value);
      if (b.source_endpoint.instance_id != c.instance.value &&
          b.sink_endpoint.instance_id != c.instance.value)
        throw std::invalid_argument("execution connection does not belong to program instance");
    }
    return c;
  }
  Json record(const std::string &id, Json input, Json observations,
              const std::vector<std::string> &branches, const std::string &coverage = "Complete",
              Json artifact_provenance = {}) const {
    if (coverage != "Complete" && coverage != "Partial")
      throw std::invalid_argument("unknown profile case coverage");
    if (branches.empty())
      throw std::invalid_argument("complete profile case requires branch identities");
    return Json::object({{"schema", "leanat.profile-case.v1"},
                         {"id", id},
                         {"profile", project().profile},
                         {"descriptorSha256", digest_},
                         {"wrongProfileRejected", true},
                         {"assertionsPassed", true},
                         {"input", std::move(input)},
                         {"observations", std::move(observations)},
                         {"coverage", coverage},
                         {"artifactProvenance", std::move(artifact_provenance)},
                         {"branches", observe(branches)}});
  }
};
} // namespace conformance::profile
