import LeanAT.ExecIR.Schema
import LeanAT.ModelIR.Validation

namespace LeanAT.ExecIR

private def require (condition : Bool) (message : String) : Except String Unit := if condition then pure () else throw message
private def unique (ids : List Nat) := ids.eraseDups.length == ids.length
private def bounded (s : String) := !s.isEmpty && s.toUTF8.size ≤ 65536
private def boundedSpan (s : SourceSpan) := bounded s.file && s.line > 0 && s.line < 2^32 && s.column > 0 && s.column < 2^32
private def boundedPort (p : ModelIR.PortDeclIR) := p.id < 2^32 && boundedSpan p.source

private def metadataWeight : Nat → Value → Option Nat
  | 0,_ => none
  | _+1,.unit | _+1,.bool _ | _+1,.handle _ => some 1
  | _+1,.bits width value => if width > 0 && width ≤ 64 && value < 2^width then some 1 else none
  | _+1,.bytes bytes => if bytes.length ≤ 65536 then some (bytes.length+1) else none
  | depth+1,.record fields | depth+1,.vec fields => do
    if fields.length > 65536 then none else fields.foldlM (fun size value => do
      let next ← metadataWeight depth value
      if size+next > 1000000 then none else some (size+next)) 1
  | depth+1,.variant tag fields => do
    if tag ≥ 2^64 || fields.length > 65536 then none else fields.foldlM (fun size value => do
      let next ← metadataWeight depth value
      if size+next > 1000000 then none else some (size+next)) 1

def validateProjectMetadata (p : ExecProject) : Except String Unit := do
  if p.schemaMajor < 4 then
    require (p.components.isEmpty && p.instances.isEmpty && p.systemMetadata.isNone && p.externalContracts.isEmpty && p.capabilities.isEmpty) "MetadataRequiresV4"
    return
  if p.schemaMajor ≥ 5 && p.systemMetadata.isNone && p.components.isEmpty && p.instances.isEmpty && p.externalContracts.isEmpty && p.capabilities.isEmpty then return
  let some system := p.systemMetadata | throw "MissingSystemMetadata"
  require (p.capabilities.length ≤ 65536 && p.capabilities.eraseDups.length == p.capabilities.length && p.capabilities.all bounded) "CapabilityEncodingLimit"
  require (p.components.length ≤ 65536 && p.instances.length ≤ 65536 && unique (p.components.map ComponentDesc.id) && unique (p.instances.map InstanceDesc.id)) "DuplicateMetadataIdentityOrLimit"
  require (system.id < 2^32 && system.runtimeDomain < 2^32 && boundedSpan system.source) "SystemEncodingLimit"
  let modelComponents := p.components.map (fun c => ({
    id := ⟨c.id⟩,parameters := c.parameters,states := c.states,endpoints := c.endpoints,
    sidebands := c.sidebands,resetPolicy := c.resetPolicy,source := {file := c.source}} : ModelIR.ComponentIR))
  let model : ModelIR.Project := {
    types := p.types
    components := modelComponents
    systems := [system]
    topSystemId := some system.id
    externalContracts := p.externalContracts
    enabledCapabilities := p.capabilities}
  ModelIR.validateMetadata model
  for c in p.components do
    require (c.id < 2^32 && bounded c.source && bounded c.resetPolicy) "ComponentEncodingLimit"
    require (c.parameters.length ≤ 65536 && c.states.length ≤ 65536 && c.endpoints.length ≤ 65536 && c.sidebands.length ≤ 65536) "ComponentTableLimit"
    for parameter in c.parameters do require (parameter.id < 2^32 && boundedSpan parameter.source) "ParameterEncodingLimit"
    for state in c.states do require (state.id < 2^32) "StateIdEncodingLimit"
    for e in c.endpoints do
      require ([e.id,e.busWidth,e.maxBindings,e.maxOutstanding,e.maxPayloadBytes,e.maxByteEnableBytes].all (· < 2^32) && bounded e.protocolRef && boundedSpan e.source) "EndpointEncodingLimit"
    require (c.sidebands.all boundedPort) "PortEncodingLimit"
  require (system.instances.length == p.instances.length) "ResolvedInstanceCount"
  let mut expectedBase := 0
  let mut ownedPrograms : List Nat := []
  for inst in p.instances do
    let some c := p.components.find? (fun c => c.id == inst.definition) | throw "InstanceComponentReference"
    let some original := system.instances.find? (fun i => i.id.value == inst.id) | throw "ResolvedInstanceReference"
    require (original.definition.value == inst.definition && original.resolvedConfig == inst.resolvedConfig) "ResolvedConfigMismatch"
    require ([inst.id,inst.definition,inst.stateBase,inst.stateCount].all (· < 2^32) && bounded inst.source && inst.resolvedConfig.length ≤ 65536 && inst.handlers.length ≤ 65536) "InstanceEncodingLimit"
    require (inst.stateBase == expectedBase && inst.stateCount == c.states.length) "InstanceStateRange"
    require ((p.stateTypes.drop inst.stateBase).take inst.stateCount == c.states.map ModelIR.StateSlot.typeId && (p.initialState.drop inst.stateBase).take inst.stateCount == c.states.map ModelIR.StateSlot.initial) "InstanceInitialStateMismatch"
    expectedBase := expectedBase+inst.stateCount
    let keys := inst.handlers.map (fun h => (h.localId,h.context,h.endpoint))
    require (keys.eraseDups.length == keys.length) "DuplicateHandlerBinding"
    for h in inst.handlers do
      let some program := p.programs.find? (fun pr => pr.id == h.programId) | throw "HandlerProgramReference"
      require (h.localId < 2^32 && h.context == program.context && bounded h.trigger && bounded h.source && !ownedPrograms.contains h.programId) "HandlerBindingMetadata"
      ownedPrograms := h.programId::ownedPrograms
      require (h.endpoint.all (fun id => c.endpoints.any (fun e => e.id == id))) "HandlerEndpointReference"
      if h.context == 2 || h.context == 3 || h.context == 4 then require h.endpoint.isSome "SynchronousHandlerEndpoint"
      if h.context == 1 then
        let some capacity := h.processCapacity | throw "MissingProcessCapacity"
        require (capacity.maxInstances > 0 && capacity.maxInstances ≤ 65536 && capacity.resultCapacity > 0 && capacity.resultCapacity ≤ 65536 && program.frameBytes ≤ capacity.frameBytesLimit && capacity.frameBytesLimit < 2^32) "ProcessCapacity"
        require (match capacity.overflow with | .reject => true | .awaitSlot n | .queue n => n > 0 && n ≤ 65536) "ProcessOverflowCapacity"
      else require h.processCapacity.isNone "UnexpectedProcessCapacity"
      for block in program.blocks do
        for instruction in block.instructions do
          if instruction.op == .loadState || instruction.op == .bufferStateWrite then
            require (instruction.immediate ≥ inst.stateBase && instruction.immediate < inst.stateBase+inst.stateCount) "CrossInstanceStateAccess"
  require (expectedBase == p.initialState.length) "UnownedGlobalState"
  let pureTargets := p.programs.flatMap (fun pr => pr.blocks.flatMap (fun b => (b.instructions.filter (fun i => i.op == .callPure)).map Instruction.immediate))
  require (p.programs.all (fun pr => ownedPrograms.contains pr.id || pureTargets.contains pr.id)) "UnownedProgram"
  require (system.topPorts.all boundedPort && system.bindings.length ≤ 65536 && system.sidebandBindings.length ≤ 65536 && system.addressMaps.length ≤ 65536) "SystemTableLimit"
  for b in system.bindings do require ([b.id.value,b.sourceEndpoint.instanceId.value,b.sourceEndpoint.endpoint,b.sourceEndpoint.bindingIndex,b.sinkEndpoint.instanceId.value,b.sinkEndpoint.endpoint,b.sinkEndpoint.bindingIndex].all (· < 2^32) && boundedSpan b.source) "BindingEncodingLimit"
  for b in system.sidebandBindings do require (b.id < 2^32 && boundedSpan b.source) "SidebandEncodingLimit"
  for m in system.addressMaps do require ([m.id,m.decoder.value,m.outputEndpoint,m.bindingIndex].all (· < 2^32) && [m.sourceStart,m.size,m.targetStart].all (· < 2^64) && boundedSpan m.source) "AddressMapEncodingLimit"
  require (p.externalContracts.length ≤ 65536) "ExternalTableLimit"
  let mut literalWork := 0
  for e in p.externalContracts do
    require (e.id < 2^32 && [e.cppType,e.header,e.library].all bounded && boundedSpan e.source && e.constructorMapping.length ≤ 65536) "ExternalEncodingLimit"
    require ((e.requires ++ e.ensures ++ e.assumptions ++ e.evidence ++ e.constructorMapping.map Prod.fst).all bounded) "ExternalStringLimit"
    for (_,value) in e.constructorMapping do
      let some weight := metadataWeight 64 value | throw "ExternalLiteralEncodingLimit"
      literalWork := literalWork+weight
      require (literalWork ≤ 1000000) "ExternalLiteralWorkLimit"

end LeanAT.ExecIR
