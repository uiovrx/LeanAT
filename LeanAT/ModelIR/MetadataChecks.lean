import LeanAT.ModelIR.Schema
namespace LeanAT.ModelIR
private def require (condition : Bool) (error : String) : Except String Unit :=
  if condition then .ok () else .error error
private def resolveDefinition (p : Project) (system : SystemIR) (id : InstanceId) : Except String ComponentIR := do
  let some instanceRow := system.instances.find? (fun i => i.id == id) | throw "UnknownSidebandInstance"
  let some definition := p.components.find? (fun c => c.id == instanceRow.definition) | throw "UnknownDefinition"
  pure definition
private def resolvePort (p : Project) (system : SystemIR) (ref : SidebandPortRef) : Except String (TypeId × Bool) := do
  match ref with
  | .top id =>
    let some port := system.topPorts.find? (fun port => port.id == id) | throw "UnknownTopPort"
    pure (port.typeId,port.direction == .input)
  | .instance id portId =>
    let definition ← resolveDefinition p system id
    let some port := definition.sidebands.find? (fun port => port.id == portId) | throw "UnknownInstancePort"
    pure (port.typeId,port.direction == .output)
/-- Structural topology checks independent of frontend syntax and of any emitter. -/
def validateTopology (p : Project) : Except String Unit := do
  for system in p.systems do
    require ((system.topPorts.map PortDeclIR.id).eraseDups.length == system.topPorts.length) "DuplicateTopPort"
    require ((system.sidebandBindings.map SidebandBindingIR.id).eraseDups.length == system.sidebandBindings.length) "DuplicateSidebandBinding"
    require ((system.bindings.map BindingIR.sourceEndpoint).eraseDups.length == system.bindings.length) "DuplicateSocketSource"
    for port in system.topPorts do
      let some schema := p.types[port.typeId]? | throw "UnknownTopPortType"
      require (!port.initial.any (fun value => !conforms p.types (p.types.length+1) schema value)) "InvalidTopPortInitial"
    for inst in system.instances do
      let definition ← resolveDefinition p system inst.id
      require ((inst.resolvedConfig.map Prod.fst).eraseDups.length == inst.resolvedConfig.length) "DuplicateResolvedConfig"
      for (id,value) in inst.resolvedConfig do
        let some param := definition.parameters.find? (fun param => param.id == id) | throw "UnknownResolvedParameter"
        let some schema := p.types[param.typeId]? | throw "UnknownParameterType"
        require (conforms p.types (p.types.length+1) schema value) "ResolvedParameterTypeMismatch"
      for param in definition.parameters do
        require (param.defaultValue.isSome || inst.resolvedConfig.any (fun pair => pair.1 == param.id)) "MissingResolvedParameter"
      for port in definition.sidebands do
        if port.direction == .input && port.initial.isNone then
          require (system.sidebandBindings.any (fun binding => binding.sinkPort == .instance inst.id port.id)) "UndrivenInstanceInput"
    for binding in system.sidebandBindings do
      let (sourceType,sourceDirection) ← resolvePort p system binding.sourcePort
      let (sinkType,sinkDirection) ← resolvePort p system binding.sinkPort
      require (sourceDirection && !sinkDirection) "SidebandDirectionMismatch"
      require (sourceType == sinkType) "SidebandTypeMismatch"
    require ((system.sidebandBindings.map SidebandBindingIR.sinkPort).eraseDups.length == system.sidebandBindings.length) "MultipleSidebandDrivers"
    for port in system.topPorts do
      if port.direction == .output && port.initial.isNone then
        require (system.sidebandBindings.any (fun binding => binding.sinkPort == .top port.id)) "UndrivenTopOutput"
    require (system.addressMaps.length * system.addressMaps.length ≤ 1000000) "AddressMapValidationBudget"
    for map in system.addressMaps do
      let definition ← resolveDefinition p system map.decoder
      let some endpoint := definition.endpoints.find? (fun e => e.id == map.outputEndpoint) | throw "UnknownAddressMapEndpoint"
      require (endpoint.role == .initiator && map.bindingIndex < endpoint.maxBindings) "InvalidAddressMapEndpoint"
      require (system.bindings.any (fun binding => binding.sourceEndpoint == ⟨map.decoder,map.outputEndpoint,map.bindingIndex⟩)) "UnboundAddressMapEndpoint"
    for (left,i) in system.addressMaps.zipIdx do
      for (right,j) in system.addressMaps.zipIdx do
        if i < j && left.decoder == right.decoder && left.sourceStart < right.sourceStart+right.size && right.sourceStart < left.sourceStart+left.size then
          require (left.aliasDeclared && right.aliasDeclared) "AddressMapOverlapRequiresAlias"
end LeanAT.ModelIR
