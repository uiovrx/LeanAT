import LeanAT.Compiler.Descriptor
import Lean

namespace LeanAT.Compiler
open ExecIR

private def runtimeTemplate : String := include_str "../../templates/systemc/GeneratedRuntime.hpp"
private def join (xs : List String) := String.intercalate "\n" xs
private def comma (xs : List String) := String.intercalate "," xs

/-- Escape UTF-8 bytes with fixed-width octal escapes; adjacent digits cannot extend an escape. -/
def facadeCppString (value : String) : String :=
  "\"" ++ String.join (value.toUTF8.data.toList.map (fun b =>
    let n := b.toNat
    "\\" ++ toString (n / 64) ++ toString (n / 8 % 8) ++ toString (n % 8))) ++ "\""

private def component (p : ExecProject) (i : InstanceDesc) : Except String ComponentDesc :=
  match p.components.find? (fun c => c.id == i.definition) with
  | some c => .ok c | none => .error "FacadeUnknownComponent"

private def cppType (p : ExecProject) (t : Nat) : Except String String :=
  match p.types[t]? with
  | some .bool => .ok "bool"
  | some (.bits width) => if width > 0 && width ≤ 64 then .ok s!"sc_dt::sc_uint<{width}>" else .error "FacadeSidebandWidth"
  | _ => .error "FacadeSidebandType"

private def cppInitial (p : ExecProject) (port : ModelIR.PortDeclIR) : Except String String := do
  let _ ← cppType p port.typeId
  match port.initial with
  | some (.bool true) => pure "true"
  | some (.bool false) => pure "false"
  | some (.bits _ value) => pure s!"{value}ULL"
  | _ => throw "FacadeMissingSidebandInitialValue"

private def endpointConnections (s : ModelIR.SystemIR) (inst endpoint : Nat) (target : Bool) : List (Nat × Nat) :=
  (s.bindings.filterMap fun b =>
    let e := if target then b.sinkEndpoint else b.sourceEndpoint
    if e.instanceId.value == inst && e.endpoint == endpoint then some (e.bindingIndex,b.id.value) else none).mergeSort (fun a b => a.1 ≤ b.1)

private def epName (id : Nat) := s!"endpoint_{id}"
private def iname (id : Nat) := s!"instance_{id}"
private def cname (id : Nat) := s!"Component_{id}"
private def pname (id : Nat) := s!"port_{id}"

private def validateFacade (p : ExecProject) (s : ModelIR.SystemIR) : Except String Unit := do
  if p.instances.isEmpty then throw "FacadeEmptySystem"
  if !p.externalContracts.isEmpty then throw "FacadeExternalBindingRequiresHostContract"
  if !p.capabilities.isEmpty then throw "FacadeOptionalCapabilityBindingUnavailable"
  if !s.addressMaps.isEmpty then throw "FacadeAddressMapProviderRequired"
  for i in p.instances do
    let c ← component p i
    if c.resetPolicy != "cancel-business-drain-wire" then throw "FacadeUnsupportedResetPolicy"
    for endpoint in c.endpoints do
      if !["TlmBaseAtV1","tlm.base.v1"].contains endpoint.protocolRef then throw "FacadeProtocolAdapterRequired"
      if endpoint.busWidth == 0 || endpoint.busWidth > 4096 || endpoint.busWidth % 8 != 0 then throw "FacadeUnsupportedBusWidth"
      let connections := endpointConnections s i.id endpoint.id (endpoint.role == .target)
      if connections.isEmpty || connections.length > endpoint.maxBindings then throw "FacadeUnresolvedEndpointBinding"
      if connections.map Prod.fst != List.range connections.length then throw "FacadeSparseBindingIndex"
    for port in c.sidebands do
      let _ ← cppType p port.typeId
      let _ ← cppInitial p port
      if port.direction == .input then
        let incoming := s.sidebandBindings.filter (fun b => b.sinkPort == .instance ⟨i.id⟩ port.id)
        if incoming.length != 1 then throw "FacadeUnresolvedSidebandInput"
    for handler in i.handlers do
      if handler.context > 1 then throw "FacadeSynchronousHandlerProviderRequired"
      if handler.processCapacity.any (fun c => c.overflow != .reject) then throw "FacadeProcessAdmissionProviderRequired"
      if !(p.programs.any (fun program => program.id == handler.programId)) then throw "FacadeMissingProgram"
  for port in s.topPorts do
    let _ ← cppType p port.typeId
    let _ ← cppInitial p port
    if port.direction == .output then
      let incoming := s.sidebandBindings.filter (fun b => b.sinkPort == .top port.id)
      if incoming.length != 1 then throw "FacadeUnresolvedTopOutput"

private def emitComponent (p : ExecProject) (s : ModelIR.SystemIR) (i : InstanceDesc) : Except String String := do
  let c ← component p i
  let mut members : List String := []
  let mut initializers := ["sc_module(name)"]
  let mut body : List String := []
  for endpoint in c.endpoints do
    let target := endpoint.role == .target
    let kind := if target then "TargetEndpoint" else "InitiatorEndpoint"
    let name := epName endpoint.id
    members := members ++ [s!"  {kind}<{endpoint.busWidth}> {name};"]
    let connections := endpointConnections s i.id endpoint.id target
    let args := comma (connections.map (fun (_,id) => "ConnectionId{" ++ toString id ++ "}"))
    let identity := if target then s!",{i.id},{endpoint.id}" else ""
    initializers := initializers ++ [name ++ "(" ++ facadeCppString name ++ ",engine" ++ identity ++ ",{" ++ args ++ "})"]
  for port in c.sidebands do
    let ty ← cppType p port.typeId
    let dir := if port.direction == .input then "sc_in" else "sc_out"
    members := members ++ [s!"  sc_core::{dir}<{ty}> {pname port.id};"]
    initializers := initializers ++ [pname port.id ++ "(" ++ facadeCppString (pname port.id) ++ ")"]
    if port.direction == .input then
      let sampler := s!"sample_{port.id}"
      members := members ++ [s!"  SignalInput<{ty}> {sampler};"]
      initializers := initializers ++ [sampler ++ "(" ++ facadeCppString sampler ++ s!",engine,{i.id},{port.id})"]
      body := body ++ [s!"    {sampler}.input({pname port.id});"]
  pure (join ([s!"class {cname i.id} : public sc_core::sc_module " ++ "{", "public:"] ++ members ++
    [s!"  {cname i.id}(sc_core::sc_module_name name,Engine& engine):" ++ comma initializers ++ " {",join body,"  }","};"]))

private def emitConfig (p : ExecProject) (s : ModelIR.SystemIR) (hash : ByteArray) : String :=
  join (["inline RuntimeConfig generated_config() {", "  RuntimeConfig c;",
    "  c.domain=DomainId{" ++ toString s.runtimeDomain ++ "};", "  c.instance=InstanceId{" ++ toString ((p.instances.head?).map InstanceDesc.id |>.getD 0) ++ "};",
    "  c.descriptor_identity=" ++ facadeCppString (hex hash) ++ ";",
    "  c.instances={" ++ comma (p.instances.map (fun i => "InstanceId{" ++ toString i.id ++ "}")) ++ "};",
    s!"  c.instance_capacity={p.instances.length};",
    "  c.connections={" ++ comma (s.bindings.map (fun b => "ConnectionId{" ++ toString b.id.value ++ "}")) ++ "};",
    "  c.connection_bindings={" ++ comma (s.bindings.map (fun b => "{ConnectionId{" ++ toString b.id.value ++ "},InstanceId{" ++ toString b.sourceEndpoint.instanceId.value ++ "},InstanceId{" ++ toString b.sinkEndpoint.instanceId.value ++ "}}")) ++ "};",
    "  return c;", "}"])

private def emitTop (p : ExecProject) (s : ModelIR.SystemIR) (hash : ByteArray) : Except String String := do
  let mut members := ["  Engine engine;"]
  let handlers := p.instances.flatMap (fun i => i.handlers.map (fun h =>
    "{" ++ comma [toString i.id,toString h.programId,h.endpoint.map toString |>.getD "std::nullopt",
      if h.context == 1 then "ContextKind::Process" else "ContextKind::Timed",facadeCppString h.trigger,facadeCppString h.source,
      toString (h.processCapacity.map ModelIR.ProcessCapacityIR.maxInstances |>.getD 1)] ++ "}"))
  let connections := s.bindings.map (fun b => "{" ++ comma ([b.id.value,b.sourceEndpoint.instanceId.value,b.sourceEndpoint.endpoint,b.sourceEndpoint.bindingIndex,b.sinkEndpoint.instanceId.value,b.sinkEndpoint.endpoint,b.sinkEndpoint.bindingIndex].map toString) ++ "}")
  let mut initializers := ["sc_module(name)","engine(generated_config(),{" ++ comma (hash.data.toList.map (fun b => toString b.toNat)) ++ "}," ++ facadeCppString p.profile ++ ",{" ++ comma handlers ++ "},{" ++ comma connections ++ "},10000,SegmentBudget{},std::move(bindings))"]
  let mut body : List String := []
  for i in p.instances do
    members := members ++ [s!"  {cname i.id} {iname i.id};"]
    initializers := initializers ++ [iname i.id ++ "(" ++ facadeCppString (iname i.id) ++ ",engine)"]
    let c ← component p i
    for port in c.sidebands do
      if port.direction == .output then
        let ty ← cppType p port.typeId
        let signal := s!"signal_{i.id}_{port.id}"
        members := members ++ [s!"  sc_core::sc_signal<{ty}> {signal};"]
        initializers := initializers ++ [signal ++ "(" ++ facadeCppString signal ++ ")"]
        body := body ++ [s!"    {iname i.id}.{pname port.id}({signal});",s!"    {signal}.write(" ++ (← cppInitial p port) ++ ");"]
        let extract := if p.types[port.typeId]? == some .bool then "std::get<bool>(value.data)" else "std::get<std::uint64_t>(value.data)"
        body := body ++ [s!"    engine.bind_output({i.id},{port.id},[this](const Value& value)" ++ "{" ++ signal ++ ".write(" ++ extract ++ ");});"]
  for port in s.topPorts do
    let ty ← cppType p port.typeId
    let dir := if port.direction == .input then "sc_in" else "sc_out"
    members := members ++ [s!"  sc_core::{dir}<{ty}> {pname port.id};"]
    initializers := initializers ++ [pname port.id ++ "(" ++ facadeCppString (pname port.id) ++ ")"]
  for binding in s.bindings do
    let a := binding.sourceEndpoint
    let b := binding.sinkEndpoint
    let forward := s!"(*{iname a.instanceId.value}.{epName a.endpoint}.sockets.at({a.bindingIndex}))"
    let backward := s!"(*{iname b.instanceId.value}.{epName b.endpoint}.sockets.at({b.bindingIndex}))"
    body := body ++ [s!"    require(systemc::validate_native_profile(engine.bindings().native_profile,{forward},{backward}));",
      s!"    {forward}.bind({backward});",
      "    require(engine.host.bind(ConnectionId{" ++ toString binding.id.value ++ "},",
      "      [this](const SendIntent& intent){return engine.resolve(intent);},",
      "      [this](auto& gp,auto& phase,auto& delay){",
      s!"        if(phase==tlm::BEGIN_REQ || phase==tlm::END_RESP) " ++ "{",
      "          bool origin=phase==tlm::BEGIN_REQ;",
      s!"          auto result={forward}->nb_transport_fw(gp,phase,delay);",
      "          if(origin)engine.release_origin(gp);return result;}",
      s!"        return {backward}->nb_transport_bw(gp,phase,delay);", "      }));"]
  for binding in s.sidebandBindings do
    let source := match binding.sourcePort with
      | .top id => pname id
      | .instance i id => s!"signal_{i.value}_{id}"
    let sink := match binding.sinkPort with
      | .top id => pname id
      | .instance i id => s!"{iname i.value}.{pname id}"
    body := body ++ [s!"    {sink}({source});"]
  body := body ++ ["    engine.start(generated_config());"]
  pure (join (["class Top : public sc_core::sc_module {","public:"] ++ members ++
    ["  explicit Top(sc_core::sc_module_name name,HostBindings bindings={}):" ++ comma initializers ++ " {",join body,"  }","};"]))

/-- Emit fixed adapters only; all business programs execute the checked descriptor in the shared VM. -/
def emitFacade (validated : ExecIR.ValidatedProject) : Except String (List (String × String)) := do
  let p := validated.project
  let some system := p.systemMetadata | throw "FacadeRequiresSelectedSystem"
  validateFacade p system
  let components ← p.instances.mapM (emitComponent p system)
  let hash := computeArtifactHash (serialize validated)
  let top ← emitTop p system hash
  let header := join (["#pragma once","#include \"GeneratedRuntime.hpp\"","namespace leanat_generated {",
    "inline const std::string generated_profile=" ++ facadeCppString p.profile ++ ";",emitConfig p system hash] ++ components ++ [top,"}"])
  pure [("include/GeneratedRuntime.hpp",runtimeTemplate),("include/Facade.hpp",header),
    ("include/Component.hpp","#pragma once\n#include \"Facade.hpp\"\n"),
    ("include/Top.hpp","#pragma once\n#include \"Facade.hpp\"\n"),
    ("src/Component.cpp","#include \"Component.hpp\"\n"),
    ("src/Top.cpp","#include \"Top.hpp\"\n")]

end LeanAT.Compiler
