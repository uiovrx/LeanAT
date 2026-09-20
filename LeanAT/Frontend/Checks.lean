import LeanAT.ModelIR.Validation
import LeanAT.Protocol.Validation
import LeanAT.Frontend.Equality

namespace LeanAT.Frontend
inductive Context where | timed | process | transport | debug | dmi | pureFunction deriving BEq, Repr, Inhabited
def Context.toModel : Context → ModelIR.ContextKind
  | .timed => .timedHandler
  | .process => .process
  | .transport => .transportEntry
  | .debug => .debugEntry
  | .dmi => .dmiEntry
  | .pureFunction => .pureFunction
inductive Role where | initiator | target deriving BEq, Repr, Inhabited
structure Port where
  name : String
  role : Role
  width : Nat
  protocol : String := "TlmBaseAtV1"
  maxBindings : Nat := 1
  maxOutstanding : Nat
  maxPayloadBytes : Nat
  maxByteEnableBytes : Nat
  pendingRequestsPerLink : Nat := 1
  deriving Repr, BEq
structure Sideband where
  name : String
  direction : ModelIR.Direction
  typeId : Nat
  initial : Option Value := none
  deriving Repr, BEq
structure Component where
  program : ModelIR.Project
  contexts : List Context
  ports : List Port := []
  sidebands : List Sideband := []
  requirements : List String := []
  handlerNames : List String := []
  extended : Bool := false
  protocols : List Protocol.Package := []
  source : SourceSpan := {}
  deriving Repr, BEq

def exportComponent (c : Component) (id : Nat) : ModelIR.ComponentIR := {
  id := ⟨id⟩, states := c.program.states, handlers := c.program.handlers, source := c.source
  endpoints := c.ports.zipIdx |>.map (fun (p,n) => {
    id := n, role := if p.role == .initiator then .initiator else .target,
    busWidth := p.width, maxBindings := p.maxBindings, maxOutstanding := p.maxOutstanding,
    maxPayloadBytes := p.maxPayloadBytes, maxByteEnableBytes := p.maxByteEnableBytes,
    protocolRef := if p.protocol == "TlmBaseAtV1" then "tlm.base.v1" else p.protocol, source := c.source })
  sidebands := c.sidebands.zipIdx |>.map (fun (p,n) => {id := n, direction := p.direction, typeId := p.typeId, initial := p.initial, source := c.source})
}

def componentProject (c : Component) : ModelIR.Project :=
  if c.program.handlers.any (·.endpoint.isSome) || !c.sidebands.isEmpty then
    { c.program with states := [], handlers := [], components := [exportComponent c 0] }
  else c.program
def exprReadsStateAux : Nat → ModelIR.Expr → Bool
  | 0, _ => true
  | _ + 1, .state _ _ => true
  | fuel + 1, .select _ a b c => exprReadsStateAux fuel a || exprReadsStateAux fuel b || exprReadsStateAux fuel c
  | fuel + 1, .binary _ _ a b | fuel + 1, .index _ a b | fuel + 1, .compare _ _ a b => exprReadsStateAux fuel a || exprReadsStateAux fuel b
  | fuel + 1, .field _ a _ | fuel + 1, .unary _ _ a | fuel + 1, .convert _ _ a | fuel + 1, .variantTag _ a | fuel + 1, .variantGet _ a _ _ => exprReadsStateAux fuel a
  | fuel + 1, .vecSet _ a b c => exprReadsStateAux fuel a || exprReadsStateAux fuel b || exprReadsStateAux fuel c
  | fuel + 1, .makeRecord _ xs | fuel + 1, .makeVariant _ _ xs | fuel + 1, .makeVec _ xs | fuel + 1, .callPure _ _ xs => xs.any (exprReadsStateAux fuel)
  | _, _ => false
def exprReadsState := exprReadsStateAux 4096
-- Every accepted instruction has a fixed effect; no caller supplied effect mask.
def checkBodyWithServices (services : List ModelIR.ServiceIR) (ctx : Context) : Nat → List ModelIR.Stmt → Except String Bool
  | 0, _ => .error "ExpansionLimit"
  | _ + 1, [] => .ok false
  | fuel + 1, s :: rest => do
    let terminal ← match s with
      | .readNow _ => do
        if ctx == .pureFunction then throw "IllegalEffect: pure function reads time"
        pure false
      | .unsupported feature => throw ("UnsupportedSyntax: " ++ feature)
      | .serviceCall _ id args => do
        let some service := services.find? (·.id == id) | throw "UnknownService"
        let bit := match ctx with | .timed => 1 | .process => 2 | .transport => 4 | .debug => 8 | .dmi => 16 | .pureFunction => 32
        let some (allowed,minimumEffects) := ModelIR.serviceContract service.opcode | throw "UnknownServiceOpcode"
        if Nat.land allowed bit == 0 || Nat.land service.contextMask bit == 0 then throw "ServiceForbiddenInContext"
        if Nat.land service.effectMask minimumEffects != minimumEffects then throw "ForgedServiceEffects"
        if (ctx == .pureFunction || ctx == .transport || ctx == .debug || ctx == .dmi) && args.any exprReadsState then throw "IllegalEffect: service argument reads business state"
        pure false
      | .await _ _ _ => do
        if ctx != .process then throw "AwaitForbiddenInContext"
        pure false
      | .writeState _ e => do
        if ctx == .pureFunction || ctx == .transport || ctx == .dmi then throw "IllegalEffect: synchronous mutable state write"
        if ctx == .debug && exprReadsState e then throw "IllegalEffect: debug expression reads business state"
        pure false
      | .letVal _ e | .check e _ => do
        if (ctx == .pureFunction || ctx == .transport || ctx == .dmi) && exprReadsState e then throw "IllegalEffect: synchronous future state read"
        pure false
      | .emit _ _ => do
        if ctx == .pureFunction || ctx == .transport || ctx == .debug || ctx == .dmi then throw "IllegalEffect: synchronous event emission"
        pure false
      | .ret values => do
        if ctx == .transport then throw "UnsupportedSyntax: transport requires returnTransport, unavailable in schema v1"
        if (ctx == .pureFunction || ctx == .dmi || ctx == .debug) && values.any exprReadsState then throw "IllegalEffect: synchronous return reads business state"
        pure true
      | .transportReturn value => do
        if ctx != .transport then throw "TransportReturnForbiddenInContext"
        if exprReadsState value then throw "IllegalEffect: transport return reads future state"
        pure true
      | .fail _ => pure true
      | .branch c yes no => do
        if (ctx == .pureFunction || ctx == .transport || ctx == .dmi) && exprReadsState c then throw "IllegalEffect: synchronous future state read"
        let y ← checkBodyWithServices services ctx fuel yes
        let n ← checkBodyWithServices services ctx fuel no
        pure (y && n)
      | .repeat n body => do
        if n > 4096 then throw "ExpansionLimit: static loop exceeds 4096"
        let terminal ← checkBodyWithServices services ctx fuel body
        if terminal && n > 1 then throw "ReturnInsideRepeatedBody"
        pure (terminal && n > 0)
    if terminal && !rest.isEmpty then throw "StatementAfterReturn"
    if terminal then pure true else checkBodyWithServices services ctx fuel rest

def checkBody (ctx : Context) := checkBodyWithServices [] ctx

def checkComponent (c : Component) : Except String Unit := do
  if !c.program.components.isEmpty || !c.program.systems.isEmpty || !c.program.externalContracts.isEmpty then throw "UnsupportedNestedProject"
  let _ ← ModelIR.validateSchema (componentProject c)
  if c.contexts.length != c.program.handlers.length then throw "MissingHandlerContext"
  if !c.handlerNames.isEmpty && c.handlerNames.length != c.program.handlers.length then throw "MissingHandlerIdentity"
  if !Protocol.unique c.handlerNames then throw "DuplicateHandlerIdentity"
  if !c.protocols.isEmpty && !c.extended then throw "MissingCapability: custom protocol requires Ext"
  if !Protocol.unique (c.protocols.map (·.name)) then throw "DuplicateProtocolIdentity"
  for p in c.protocols do Protocol.validate p
  if !Protocol.unique (c.ports.map (·.name)) then throw "DuplicatePort"
  if !Protocol.unique (c.sidebands.map (·.name)) then throw "DuplicateSideband"
  for p in c.sidebands do
    match c.program.types[p.typeId]? with
    | some .bool | some (.bits _) => pure ()
    | _ => throw "InvalidSidebandType"
    if p.direction == .output && p.initial.isNone then throw "UninitializedOutput"
  for p in c.ports do
    if p.width == 0 || p.width % 8 != 0 then throw "InvalidBusWidth"
    if p.maxBindings == 0 || p.maxOutstanding == 0 then throw "MissingCapacity"
    if p.pendingRequestsPerLink != 1 then throw "PendingRequestsPerLinkMustBeOne"
    if p.protocol != "TlmBaseAtV1" && !(c.protocols.any (·.name == p.protocol)) then throw "UnknownProtocol"
  for (h, ctx) in c.program.handlers.zip c.contexts do
    if h.context != ctx.toModel then throw "HandlerContextMismatch"
    let terminal ← checkBodyWithServices c.program.services ctx 4096 h.body
    if !terminal then throw "MissingReturn: all normal paths must return"

structure Instance where
  name : String
  definition : String
  component : Component
  deriving Repr, BEq
structure Endpoint where
  instanceId : Nat
  port : Nat
  deriving Repr, BEq
structure Binding where
  source : Endpoint
  target : Endpoint
  deriving Repr, BEq
structure AddressMap where
  sourceStart : Nat
  size : Nat
  targetStart : Nat
  binding : Nat
  deriving Repr, BEq
structure System where
  instances : List Instance
  bindings : List Binding := []
  maps : List AddressMap := []
  topPorts : List Sideband := []
  sidebandBindings : List (ModelIR.SidebandPortRef × ModelIR.SidebandPortRef) := []
  types : TypeEnvironment := []
  source : SourceSpan := {}
  deriving Repr, BEq
def getPort (s : System) (e : Endpoint) : Except String Port := do
  let some i := s.instances[e.instanceId]? | throw "UnknownInstance"
  let some p := i.component.ports[e.port]? | throw "UnknownPort"
  pure p

def sameComponent (a b : Component) : Bool :=
  a.ports == b.ports &&
  listEqWith (fun x y => x.name == y.name && x.direction == y.direction && x.typeId == y.typeId &&
    (match x.initial,y.initial with | none,none => true | some v,some w => valueEq 4096 v w | _,_ => false)) a.sidebands b.sidebands &&
  a.contexts == b.contexts && a.requirements == b.requirements &&
  a.handlerNames == b.handlerNames && a.extended == b.extended && a.protocols == b.protocols &&
  a.program.version == b.program.version && a.program.types == b.program.types && a.program.profile == b.program.profile &&
  a.program.enabledCapabilities == b.program.enabledCapabilities &&
  a.program.services == b.program.services &&
  listEqWith (fun x y => x.id == y.id && x.typeId == y.typeId && valueEq 4096 x.initial y.initial) a.program.states b.program.states &&
  listEqWith (fun x y => x.id == y.id && x.context == y.context && x.trigger == y.trigger && x.endpoint == y.endpoint && x.parameters == y.parameters && x.processCapacity == y.processCapacity && x.declaredResultTypes == y.declaredResultTypes && listEqWith (stmtEq 4096) x.body y.body) a.program.handlers b.program.handlers

def getSideband (s : System) : ModelIR.SidebandPortRef → Except String (Sideband × Bool)
  | .top id => do
    let some p := s.topPorts[id]? | throw "UnknownTopPort"
    pure (p, p.direction == .input)
  | .instance id port => do
    let some i := s.instances[id.value]? | throw "UnknownSidebandInstance"
    let some p := i.component.sidebands[port]? | throw "UnknownSidebandPort"
    pure (p, p.direction == .output)

def checkSidebands (s : System) : Except String Unit := do
  if !Protocol.unique (s.topPorts.map (·.name)) then throw "DuplicateTopPort"
  for p in s.topPorts do
    match s.types[p.typeId]? with
    | some .bool | some (.bits _) => pure ()
    | _ => throw "InvalidTopPortType"
    if p.direction == .input && p.initial.isNone then throw "UninitializedTopInput"
    if p.initial.any (fun v => !(s.types[p.typeId]?).any (fun t => conforms s.types 64 t v)) then throw "InvalidTopInitialValue"
  if !Protocol.unique (s.sidebandBindings.map (·.2)) then throw "MultipleSidebandDrivers"
  for (source,sink) in s.sidebandBindings do
    let (a,asource) ← getSideband s source
    let (b,bsource) ← getSideband s sink
    if !asource || bsource then throw "SidebandDirection"
    if a.typeId != b.typeId then throw "SidebandTypeMismatch"
  for (p,id) in s.topPorts.zipIdx do
    if p.direction == .output && !(s.sidebandBindings.any (fun b => b.2 == .top id)) then throw "UndrivenTopOutput"
  for (i,id) in s.instances.zipIdx do
    for (p,port) in i.component.sidebands.zipIdx do
      if p.direction == .input && !(s.sidebandBindings.any (fun b => b.2 == .instance ⟨id⟩ port)) then throw "UndrivenInstanceInput"
def checkSystem (s : System) : Except String Unit := do
  checkSidebands s
  if !Protocol.unique (s.instances.map (·.name)) then throw "DuplicateInstance"
  for i in s.instances do
    if i.definition.isEmpty then throw "MissingDefinitionIdentity"
    checkComponent i.component
    for j in s.instances do
      if i.definition == j.definition && !sameComponent i.component j.component then throw "ConflictingDefinitionIdentity"
      let a := i.component.program.types
      let b := j.component.program.types
      if a.take (min a.length b.length) != b.take (min a.length b.length) then throw "UnsupportedSchemaMerge: type table prefixes conflict"
      for x in i.component.program.services do
        for y in j.component.program.services do
          if x.id == y.id && x != y then throw "ConflictingServiceIdentity"
    let a := s.types
    let b := i.component.program.types
    if a.take (min a.length b.length) != b.take (min a.length b.length) then throw "SystemTypeTableMismatch"
  if !Protocol.unique s.bindings then throw "DuplicateBinding"
  for b in s.bindings do
    let a ← getPort s b.source
    let z ← getPort s b.target
    if a.role != .initiator || z.role != .target then throw "BindingDirection"
    if a.width != z.width then throw "BindingWidthMismatch"
    if a.protocol != z.protocol then throw "IncompatibleProtocolTraits: explicit adapter required"
  for i in List.range s.instances.length do
    let some inst := s.instances[i]? | throw "UnknownInstance"
    for j in List.range inst.component.ports.length do
      let some p := inst.component.ports[j]? | throw "UnknownPort"
      let count := (s.bindings.filter (fun b => b.source == ⟨i,j⟩ || b.target == ⟨i,j⟩)).length
      if count == 0 then throw "UnboundPort"
      if count > p.maxBindings then throw "TooManyBindings"
  for m in s.maps do
    if m.size == 0 || m.sourceStart + m.size > 2^64 || m.targetStart + m.size > 2^64 then throw "AddressMapOverflow"
    if m.binding ≥ s.bindings.length then throw "UnknownMapBinding"
  for i in List.range s.maps.length do
    for j in List.range i do
      let some a := s.maps[i]? | throw "UnknownMap"
      let some b := s.maps[j]? | throw "UnknownMap"
      let some ab := s.bindings[a.binding]? | throw "UnknownMapBinding"
      let some bb := s.bindings[b.binding]? | throw "UnknownMapBinding"
      if ab.source == bb.source && a.sourceStart < b.sourceStart + b.size && b.sourceStart < a.sourceStart + a.size then throw "OverlappingAddressMap"

structure ExternComponent where
  cppType : String
  component : Component
  contract : String
  header : String := ""
  library : String := ""
  deriving Repr, BEq
def checkExtern (e : ExternComponent) : Except String Unit := do
  if e.cppType.isEmpty || e.contract.isEmpty then throw "MissingExternContract"
  if !e.component.program.handlers.isEmpty then throw "ExternCannotDefineLocalHandlers"
  checkComponent e.component

def withSource (c : Component) (source : SourceSpan) : Component :=
  { c with source, program := { c.program with handlers := (c.program.handlers.zipIdx).map (fun (h,i) =>
    { h with source, context := (c.contexts[i]?).map Context.toModel |>.getD h.context }) } }

def withExternSource (e : ExternComponent) (source : SourceSpan) : ExternComponent :=
  { e with component := withSource e.component source }

def exportExtern (e : ExternComponent) : ModelIR.Project := {
  types := e.component.program.types
  components := [exportComponent e.component 0]
  externalContracts := [{id := 0, cppType := e.cppType, header := e.header, library := e.library, constructorMapping := [], requires := [], ensures := [], assumptions := [e.contract], evidence := [], source := e.component.source}]
}

/-- Hierarchical export preserves shared definition IDs and distinct instance IDs.
The executable scalar projection remains the component's `program`. -/
def exportSystem (s : System) : ModelIR.Project := Id.run do
  let definitions := (s.instances.map (·.definition)).mergeSort (· ≤ ·) |>.eraseDups
  let instanceNames := (s.instances.map (·.name)).mergeSort (· ≤ ·)
  let iid := fun old =>
    ((s.instances[old]?).map (fun i => (instanceNames.findIdx? (· == i.name)).getD instanceNames.length)).getD instanceNames.length
  let components := definitions.zipIdx |>.filterMap (fun (name,id) =>
    (s.instances.find? (·.definition == name)).map (fun i => exportComponent i.component id))
  let instances := instanceNames.zipIdx |>.filterMap (fun (name,id) =>
    (s.instances.find? (·.name == name)).map (fun i => ({
      id := ⟨id⟩, definition := ⟨(definitions.findIdx? (· == i.definition)).getD definitions.length⟩,
      source := i.component.source } : ModelIR.InstanceIR)))
  let bindings := s.bindings.zipIdx |>.map (fun (b,id) => ({
    id := ⟨id⟩, sourceEndpoint := ⟨⟨iid b.source.instanceId⟩,b.source.port,((s.bindings.take id).filter (·.source == b.source)).length⟩,
    sinkEndpoint := ⟨⟨iid b.target.instanceId⟩,b.target.port,((s.bindings.take id).filter (·.target == b.target)).length⟩, source := s.source } : ModelIR.BindingIR))
  let maps := s.maps.zipIdx |>.filterMap (fun (m,id) =>
    (s.bindings[m.binding]?).map (fun b => ({
      id, decoder := ⟨iid b.source.instanceId⟩,
      outputEndpoint := b.source.port, bindingIndex := ((s.bindings.take m.binding).filter (·.source == b.source)).length, sourceStart := m.sourceStart,
      size := m.size, targetStart := m.targetStart, source := s.source} : ModelIR.AddressMapIR)))
  let ref := fun p => match p with | .top n => ModelIR.SidebandPortRef.top n | .instance n p => .instance ⟨iid n.value⟩ p
  let signals := s.sidebandBindings.zipIdx |>.map (fun ((a,b),id) => ({id, sourcePort := ref a, sinkPort := ref b, source := s.source} : ModelIR.SidebandBindingIR))
  let topPorts := s.topPorts.zipIdx |>.map (fun (p,id) => ({id, direction := p.direction, typeId := p.typeId, initial := p.initial, source := s.source} : ModelIR.PortDeclIR))
  let types := s.instances.foldl (fun best i => if i.component.program.types.length > best.length then i.component.program.types else best) s.types
  let services := (s.instances.flatMap (·.component.program.services)).eraseDups
  return {
    types
    services
    components
    topSystemId := some 0
    systems := [{id := 0, instances, bindings, addressMaps := maps, topPorts, sidebandBindings := signals, runtimeDomain := 0, source := s.source}]
  }

end LeanAT.Frontend





