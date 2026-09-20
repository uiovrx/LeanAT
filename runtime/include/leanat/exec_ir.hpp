#pragma once
#include "common.hpp"
#include <array>
#include <set>
namespace leanat::exec {
enum class TypeKind : std::uint8_t {
  Unit,
  Bool,
  Bits,
  Fin,
  Record,
  Variant,
  Vec,
  BoundedVec,
  Bytes,
  Handle
};
struct Type {
  TypeKind kind{TypeKind::Unit};
  std::uint64_t bound{};
  std::vector<std::uint32_t> fields;
  std::vector<std::vector<std::uint32_t>> constructors;
};
struct Reg {
  std::uint32_t id{}, type{};
};
enum class Op : std::uint8_t {
  Const,
  Move,
  Binary,
  MakeRecord,
  GetField,
  MakeVariant,
  VariantTag,
  VariantGet,
  MakeVec,
  VecGet,
  VecSet,
  SelectValue,
  LoadState,
  BufferStateWrite,
  Check,
  Trace,
  Unary,
  Compare,
  Convert,
  CallPure,
  GetNow,
  GetContextField,
  LoadInput,
  BufferOutputWrite,
  PayloadGet,
  BufferPayloadWrite,
  ExtensionGet,
  BufferExtensionWrite,
  ObjectCall,
  NewTransaction,
  StagePhase,
  AckResponse,
  CancelLocal,
  ResultGet,
  ResultRelease,
  ScheduleEvent,
  CancelEvent,
  SpawnProcess,
  TrySpawnProcess,
  RegisterWait,
  ReadWaitResult,
  SetTransportReturn,
  DebugTransfer,
  GrantRawDmi,
  DenyDmi,
  InvalidateRawDmi,
  WaitGroupNew,
  WaitArm,
  WaitResultGet,
  WaitGroupRelease,
  ScopeNew,
  ScopeTransfer,
  ScopeCancel,
  ScopeClose,
  SubmitTask,
  CancelTask,
  TaskResultGet,
  TaskResultRelease,
  RequestTaskSlot,
  RequestManaged,
  BeginManagedRead,
  BeginManagedWrite,
  InvalidateManaged,
  ReleaseLease,
  CallExternPure,
  Count
};
enum class Binary : std::uint8_t {
  AddWrap,
  SubWrap,
  MulWrap,
  AddChecked,
  DivChecked,
  Eq,
  Lt,
  And,
  Or
};
struct Instruction {
  Op op{Op::Const};
  std::vector<Reg> args;
  std::optional<Reg> dest;
  Value value;
  std::uint32_t immediate{};
  Binary binary{Binary::AddWrap};
  std::string text, source;
};
struct Edge {
  std::uint32_t target{};
  std::vector<Reg> args;
};
enum class TermKind : std::uint8_t { Jump, Branch, Switch, Return, Fail, TransportReturn, Suspend };
struct Terminator {
  TermKind kind{TermKind::Return};
  Reg value;
  Edge yes, no;
  std::vector<std::pair<std::uint64_t, Edge>> cases;
  std::vector<Reg> values;
  std::string error;
};
struct Block {
  std::uint32_t id{};
  std::vector<Reg> parameters;
  std::vector<Instruction> instructions;
  Terminator terminator;
};
struct FrameSlot {
  std::uint32_t type{}, offset{}, align{1};
  std::uint32_t register_id{};
};
enum Effect : std::uint64_t {
  StateRead = 1,
  StateWrite = 2,
  OutputWrite = 4,
  Transaction = 8,
  Wait = 16,
  Spawn = 32,
  Result = 64,
  Object = 128,
  Managed = 256,
  External = 512,
  RawDmi = 1024
};
struct ServiceSignature {
  std::uint32_t id{};
  Op op{Op::ObjectCall};
  std::vector<std::uint32_t> input_types, result_types;
  std::uint32_t context_mask{3};
  std::uint64_t effect_mask{}, extra_fuel{};
  std::string provider_key, provider_version;
  std::array<std::uint8_t, 32> abi_hash{};
  friend bool operator==(const ServiceSignature &a, const ServiceSignature &b) {
    return std::tie(a.id, a.op, a.input_types, a.result_types, a.context_mask, a.effect_mask,
                    a.extra_fuel, a.provider_key, a.provider_version, a.abi_hash) ==
           std::tie(b.id, b.op, b.input_types, b.result_types, b.context_mask, b.effect_mask,
                    b.extra_fuel, b.provider_key, b.provider_version, b.abi_hash);
  }
  friend bool operator!=(const ServiceSignature &a, const ServiceSignature &b) {
    return !(a == b);
  }
};
std::uint64_t required_effect(Op);
std::uint32_t allowed_contexts(Op);
struct Program {
  std::uint32_t id{};
  std::vector<std::uint32_t> input_types, result_types;
  std::vector<Block> blocks;
  std::uint32_t entry{};
  std::string source;
  ContextKind context{ContextKind::Timed};
  std::vector<FrameSlot> frame;
  std::uint32_t frame_bytes{};
  std::uint64_t effect_mask{};
  // Schema 5 source-declared process policy. All-zero/empty preserves legacy behavior.
  std::uint64_t instruction_fuel{};
  std::string owner_policy, result_lifetime_policy;
};
struct MetadataSource {
  std::string file;
  std::uint32_t line{1}, column{1};
};
struct ParameterDesc {
  std::uint32_t id{}, type_id{};
  std::optional<Value> default_value;
  MetadataSource source;
};
struct StateDesc {
  std::uint32_t id{}, type_id{};
  Value initial;
};
enum class EndpointRole : std::uint8_t { Initiator, Target };
struct EndpointDesc {
  std::uint32_t id{};
  EndpointRole role{EndpointRole::Initiator};
  std::uint32_t bus_width{}, max_bindings{}, max_outstanding{}, max_payload_bytes{},
      max_byte_enable_bytes{};
  std::string protocol_ref;
  MetadataSource source;
};
enum class PortDirection : std::uint8_t { Input, Output };
struct PortDesc {
  std::uint32_t id{};
  PortDirection direction{PortDirection::Input};
  std::uint32_t type_id{};
  std::optional<Value> initial;
  MetadataSource source;
};
enum class ProcessOverflow : std::uint8_t { Reject, AwaitSlot, Queue };
struct ProcessCapacity {
  std::uint32_t max_instances{}, frame_bytes_limit{}, result_capacity{};
  ProcessOverflow overflow{ProcessOverflow::Reject};
  std::uint32_t policy_bound{};
};
struct HandlerBinding {
  std::uint32_t local_id{}, program_id{};
  ContextKind context{ContextKind::Timed};
  std::string trigger;
  std::optional<std::uint32_t> endpoint;
  std::optional<ProcessCapacity> process_capacity;
  std::string source;
};
struct ComponentDesc {
  std::uint32_t id{};
  std::vector<ParameterDesc> parameters;
  std::vector<StateDesc> states;
  std::vector<EndpointDesc> endpoints;
  std::vector<PortDesc> sidebands;
  std::string reset_policy, source;
};
using ResolvedConfig = std::vector<std::pair<std::uint32_t, Value>>;
struct InstanceDesc {
  std::uint32_t id{}, definition{}, state_base{}, state_count{};
  std::vector<HandlerBinding> handlers;
  ResolvedConfig resolved_config;
  std::string source;
};
struct OriginalInstance {
  std::uint32_t id{}, definition{};
  ResolvedConfig resolved_config;
  MetadataSource source;
};
struct EndpointRef {
  std::uint32_t instance_id{}, endpoint{}, binding_index{};
};
struct BindingDesc {
  std::uint32_t id{};
  EndpointRef source_endpoint, sink_endpoint;
  MetadataSource source;
};
struct PortRef {
  std::uint8_t tag{};
  std::uint32_t instance_id{}, port_id{};
};
struct SidebandBinding {
  std::uint32_t id{};
  PortRef source_port, sink_port;
  MetadataSource source;
};
struct AddressMapDesc {
  std::uint32_t id{}, decoder_instance_id{}, output_endpoint{}, binding_index{};
  std::uint64_t source_start{}, size{}, target_start{};
  bool alias_declared{};
  MetadataSource source;
};
struct SystemMetadata {
  std::uint32_t id{};
  std::vector<OriginalInstance> original_instances;
  std::vector<BindingDesc> bindings;
  std::vector<PortDesc> top_ports;
  std::vector<SidebandBinding> sideband_bindings;
  std::vector<AddressMapDesc> address_maps;
  std::uint32_t runtime_domain{};
  MetadataSource source;
};
// Standalone constructor metadata retains wire distinctions erased by runtime Value.
struct MetadataLiteral {
  std::uint8_t tag{};
  std::uint32_t width{};
  Value value;
  std::vector<MetadataLiteral> children;
};
struct ExternalContract {
  std::uint32_t id{};
  std::string cpp_type, header, library;
  std::vector<std::pair<std::string, MetadataLiteral>> constructor_mapping;
  std::vector<std::string> requirements, ensures, assumptions, evidence;
  MetadataSource source;
};
struct Project {
  std::vector<Type> types;
  std::vector<std::uint32_t> state_types;
  std::vector<Value> initial_state;
  std::vector<Program> programs;
  std::string profile{"AT-Core-1.1-draft"};
  std::uint16_t schema_major{1};
  std::vector<ServiceSignature> services;
  std::vector<ComponentDesc> components;
  std::vector<InstanceDesc> instances;
  std::optional<SystemMetadata> system_metadata;
  std::vector<ExternalContract> external_contracts;
  std::vector<std::string> capabilities;
};
struct Limits {
  std::size_t max_rows{65536}, max_depth{64}, max_work{1000000}, max_value_elements{1048576},
      max_registers{65536};
};
class ValidatedProject {
  Project project_;
  explicit ValidatedProject(Project p) : project_(std::move(p)) {};
  friend Expected<ValidatedProject> validate(Project, Limits);

public:
  const Project &get() const {
    return project_;
  }
};
Expected<ValidatedProject> validate(Project, Limits = {});
Expected<void> validate_metadata(const Project &, Limits);
bool conforms(const Project &, std::uint32_t, const Value &, std::size_t depth = 64);
} // namespace leanat::exec
