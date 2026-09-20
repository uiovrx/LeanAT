import LeanAT.ModelIR.MetadataChecks
namespace LeanAT.ModelIR
private def references : TypeSchema → List TypeId
  | .record ts => ts
  | .variant cs => cs.flatten
  | .vec t _ | .boundedVec t _ => [t]
  | _ => []
private def checkType (env : TypeEnvironment) : Nat → List Nat → Nat → Except String Unit
  | 0, _, _ => .error "TypeGraphBudget"
  | fuel+1, path, id => do
    if path.contains id then throw "RecursiveValueLayout"
    let some t := env[id]? | throw "UnknownType"
    match t with
    | .bits w => if w == 0 || w > 64 then throw "UnsupportedWidth"
    | .fin n => if n > 2^64 then throw "UnsupportedFinBound"
    | _ => pure ()
    for child in references t do checkType env fuel (id::path) child
private def checkTypeWork : Nat → Array TypeSchema → Array Nat → List (Nat × Bool) → Except String Unit
  | _, _, _, [] => .ok ()
  | 0, _, _, _::_ => .error "TypeGraphBudget"
  | fuel+1, schemas, colors, (id,exitNode)::rest => do
    let some schema := schemas[id]? | throw "UnknownType"
    if exitNode then checkTypeWork fuel schemas (colors.set! id 2) rest
    else if colors[id]! == 1 then throw "RecursiveValueLayout"
    else if colors[id]! == 2 then checkTypeWork fuel schemas colors rest
    else
      match schema with
      | .bits width => if width == 0 || width > 64 then throw "UnsupportedWidth"
      | .fin bound => if bound > 2^64 then throw "UnsupportedFinBound"
      | _ => pure ()
      checkTypeWork fuel schemas (colors.set! id 1) ((references schema).map (fun child => (child,false)) ++ [(id,true)] ++ rest)
/-- Cached structurally recursive DFS. The exact graph work bound also permits kernel reduction. -/
def checkTypes (env : TypeEnvironment) (workBudget : Nat := 1000000) : Except String Unit := do
  if env.length > 65536 then throw "TypeTableLimit"
  let graphWork := env.foldl (fun count schema => count + (references schema).length) (2*env.length)
  if graphWork > workBudget then throw "TypeGraphBudget"
  checkTypeWork graphWork env.toArray (Array.replicate env.length 0) ((List.range env.length).map (fun id => (id,false)))
private def lookupTy (xs : List (Nat × TypeId)) (id : Nat) : Option TypeId :=
  (xs.find? (fun x => x.1 == id)).map Prod.snd

def checkExpr (p : Project) (locals : List (Nat × TypeId)) : Nat → Expr → Except String Unit
  | 0, _ => .error "ExpressionBudget"
  | fuel+1, e => do
    let some ty := p.types[e.typeId]? | throw "UnknownType"
    let check := checkExpr p locals fuel
    match e with
    | .literal _ v => if !conforms p.types (p.types.length+1) ty v then throw "InvalidLiteral"
    | .local t id => if lookupTy locals id != some t then throw "UnknownOrMistypedLocal"
    | .state t id => if (p.states.find? (fun s => s.id == id)).map StateSlot.typeId != some t then throw "UnknownOrMistypedState"
    | .select t c y n =>
      check c; check y; check n
      if p.types[c.typeId]? != some .bool || y.typeId != t || n.typeId != t then throw "SelectTypeMismatch"
    | .binary t op a b =>
      check a; check b
      if a.typeId != b.typeId then throw "OperandTypeMismatch"
      match op with
      | .eq => if p.types[t]? != some .bool then throw "ComparisonTypeMismatch"
      | .lt =>
        if p.types[t]? != some .bool then throw "ComparisonTypeMismatch"
        match p.types[a.typeId]? with
        | some (.bits _) | some (.fin _) => pure ()
        | _ => throw "OrderedComparisonRequiresInteger"
      | .and | .or => if p.types[t]? != some .bool || p.types[a.typeId]? != some .bool then throw "BooleanTypeMismatch"
      | _ =>
        if t != a.typeId then throw "ArithmeticTypeMismatch"
        match p.types[t]? with
        | some (.bits _) => pure ()
        | _ => throw "ArithmeticRequiresBits"
    | .field t obj index =>
      check obj
      let some (.record fields) := p.types[obj.typeId]? | throw "ExpectedRecord"
      if fields[index]? != some t then throw "FieldTypeMismatch"
    | .index t obj idx =>
      check obj; check idx
      match p.types[obj.typeId]? with
      | some (.vec element _) | some (.boundedVec element _) => if element != t then throw "ElementTypeMismatch"
      | _ => throw "ExpectedVector"
      match p.types[idx.typeId]? with
      | some (.bits _) | some (.fin _) => pure ()
      | _ => throw "IndexTypeMismatch"
    | .unary t mode value =>
      check value
      if t != value.typeId || mode > 2 then throw "UnaryTypeMismatch"
      if mode == 0 then
        if ty != .bool then throw "UnaryBoolRequired"
      else match ty with | .bits _ => pure () | _ => throw "UnaryBitsRequired"
    | .compare _ mode a b =>
      check a; check b
      if ty != .bool || a.typeId != b.typeId || mode > 5 then throw "CompareTypeMismatch"
      if mode > 1 then match p.types[a.typeId]? with | some (.bits _) => pure () | _ => throw "CompareBitsRequired"
    | .convert _ mode value =>
      check value
      let .bits width := ty | throw "ConvertBitsRequired"
      let some (.bits sourceWidth) := p.types[value.typeId]? | throw "ConvertBitsRequired"
      if mode > 2 || (mode == 0 && width < sourceWidth) || (mode != 0 && width > sourceWidth) then throw "ConvertWidthMismatch"
    | .variantTag _ value =>
      check value
      if ty != .bits 64 then throw "VariantTagType"
      match p.types[value.typeId]? with | some (.variant _) => pure () | _ => throw "ExpectedVariant"
    | .variantGet t value tag index =>
      check value
      let some (.variant cases) := p.types[value.typeId]? | throw "ExpectedVariant"
      if ((cases[tag]?).bind (fun fields => fields[index]?)) != some t then throw "VariantFieldMismatch"
    | .makeVec _ fields =>
      let (element,bound,exact) ← match ty with
        | .vec element bound => pure (element,bound,true)
        | .boundedVec element bound => pure (element,bound,false)
        | _ => throw "ExpectedVector"
      if (exact && fields.length != bound) || fields.length > bound || !fields.all (fun f => f.typeId == element) then throw "VectorShape"
      for f in fields do check f
    | .vecSet t vec index value =>
      check vec; check index; check value
      if t != vec.typeId then throw "VectorTypeMismatch"
      let element ← match ty with | .vec e _ | .boundedVec e _ => pure e | _ => throw "ExpectedVector"
      if element != value.typeId then throw "ElementTypeMismatch"
      match p.types[index.typeId]? with | some (.bits _) => pure () | _ => throw "IndexTypeMismatch"
    | .callPure t id args =>
      let some callee := p.handlers.find? (fun h => h.id == id) | throw "UnknownPureFunction"
      if callee.context != .pureFunction || callee.declaredResultTypes != some [t] || callee.parameters.map LocalBinderIR.typeId != args.map Expr.typeId then throw "PureCallSignatureMismatch"
      for arg in args do check arg    | .makeRecord _ fields =>
      let .record ts := ty | throw "ExpectedRecord"
      if ts != fields.map Expr.typeId then throw "RecordFieldMismatch"
      for f in fields do check f
    | .makeVariant _ tag fields =>
      let .variant cs := ty | throw "ExpectedVariant"
      if cs[tag]? != some (fields.map Expr.typeId) then throw "VariantFieldMismatch"
      for f in fields do check f

def checkStmts (p : Project) : Nat → List (Nat × TypeId) → List Stmt → Except String (List (Nat × TypeId))
  | 0, _, _ => .error "StatementBudget"
  | _+1, locals, [] => .ok locals
  | fuel+1, locals, s::ss => do
    let ce := checkExpr p locals p.profile.instructionFuel
    let mut next := locals
    match s with
    | .readNow binder =>
      if p.types[binder.typeId]? != some (.bits 64) || (lookupTy locals binder.id).isSome then throw "ReadNowBinder"
      next := (binder.id,binder.typeId)::locals
    | .serviceCall dst id args =>
      let some service := p.services.find? (fun s => s.id == id) | throw "UnknownService"
      if service.inputTypes != args.map Expr.typeId || service.resultTypes != dst.toList.map LocalBinderIR.typeId then throw "ServiceSignatureMismatch"
      for arg in args do ce arg
      if let some binder := dst then
        if (lookupTy locals binder.id).isSome then throw "DuplicateLocal"
        next := (binder.id,binder.typeId)::locals
    | .await wait binder resultType =>
      ce wait
      if p.types[wait.typeId]? != some (.handle .wait) then throw "AwaitRequiresWaitHandle"
      if p.types[resultType]?.isNone then throw "UnknownWaitOutcomeType"
      if (lookupTy locals binder).isSome then throw "DuplicateLocal"
      next := (binder,resultType)::locals
    | .letVal id e =>
      ce e
      if (lookupTy locals id).isSome then throw "DuplicateLocal"
      next := (id,e.typeId)::locals
    | .writeState id e =>
      ce e
      if (p.states.find? (fun s => s.id == id)).map StateSlot.typeId != some e.typeId then throw "StateTypeMismatch"
    | .check c _ =>
      ce c
      if p.types[c.typeId]? != some .bool then throw "ExpectedBool"
    | .branch c yes no =>
      ce c
      if p.types[c.typeId]? != some .bool then throw "ExpectedBool"
      let _ ← checkStmts p fuel locals yes
      let _ ← checkStmts p fuel locals no
    | .repeat bound body =>
      if bound > p.profile.instructionFuel then throw "LoopBoundTooLarge"
      let _ ← checkStmts p fuel locals body
    | .emit _ es | .ret es => for e in es do ce e
    | .transportReturn e => ce e
    | .fail _ => pure ()
    | .unsupported feature => throw ("UnsupportedFeature: " ++ feature)
    checkStmts p fuel next ss

private def independentExpr : Nat → Expr → Bool
  | 0, _ => false
  | fuel+1, e => match e with
    | .state _ _ => false
    | .literal _ _ | .local _ _ => true
    | .select _ c y n => independentExpr fuel c && independentExpr fuel y && independentExpr fuel n
    | .binary _ _ a b | .index _ a b | .compare _ _ a b => independentExpr fuel a && independentExpr fuel b
    | .field _ e _ | .unary _ _ e | .convert _ _ e | .variantTag _ e | .variantGet _ e _ _ => independentExpr fuel e
    | .vecSet _ a b c => independentExpr fuel a && independentExpr fuel b && independentExpr fuel c
    | .makeRecord _ es | .makeVariant _ _ es | .makeVec _ es | .callPure _ _ es => es.all (independentExpr fuel)
private def checkContext (p : Project) (ctx : ContextKind) : Nat → List Stmt → Except String Unit
  | 0, _ => .error "ContextCheckBudget"
  | fuel+1, stmts => do
    let timed := ctx == .timedHandler || ctx == .process
    let expression := fun e => if timed || independentExpr fuel e then Except.ok () else Except.error "StateReadForbiddenInContext"
    for stmt in stmts do
      match stmt with
      | .readNow _ => if !timed && ctx != .transportEntry then throw "ReadNowForbiddenInContext"
      | .serviceCall _ id args =>
        let some service := p.services.find? (fun s => s.id == id) | throw "UnknownService"
        let contextBit := match ctx with | .timedHandler => 1 | .process => 2 | .transportEntry => 4 | .debugEntry => 8 | .dmiEntry => 16 | _ => 0
        if contextBit == 0 || Nat.land service.contextMask contextBit == 0 then throw "ServiceForbiddenInContext"
        for e in args do expression e
      | .await wait _ _ =>
        if ctx != .process then throw "AwaitForbiddenInContext"
        expression wait
      | .writeState _ e =>
        if !timed then throw "StateWriteForbiddenInContext"
        expression e
      | .letVal _ e | .check e _ => expression e
      | .emit _ es =>
        if !timed then throw "PublicationForbiddenInContext"
        for e in es do expression e
      | .ret es =>
        if ctx == .transportEntry then throw "UseExplicitTransportReturn"
        for e in es do expression e
      | .transportReturn e =>
        if ctx != .transportEntry then throw "TransportReturnForbiddenInContext"
        expression e
      | .branch c yes no =>
        expression c
        checkContext p ctx fuel yes
        checkContext p ctx fuel no
      | .repeat _ body => checkContext p ctx fuel body
      | .fail _ => pure ()
      | .unsupported feature => throw ("UnsupportedFeature: " ++ feature)
private def checkReturnShape (expected : List TypeId) : Nat → List Stmt → Except String Unit
  | 0, _ => .error "ReturnCheckBudget"
  | fuel+1, body => do
    for stmt in body do
      match stmt with
      | .ret values => if values.map Expr.typeId != expected then throw "DeclaredReturnTypeMismatch"
      | .transportReturn value => if [value.typeId] != expected then throw "DeclaredReturnTypeMismatch"
      | .branch _ yes no => checkReturnShape expected fuel yes; checkReturnShape expected fuel no
      | .repeat _ body => checkReturnShape expected fuel body
      | _ => pure ()
private def pureExprCalls : Nat → Expr → List Nat
  | 0,_ => []
  | fuel+1,expr => match expr with
    | .callPure _ id args => id::args.flatMap (pureExprCalls fuel)
    | .select _ a b c | .vecSet _ a b c => pureExprCalls fuel a ++ pureExprCalls fuel b ++ pureExprCalls fuel c
    | .binary _ _ a b | .compare _ _ a b | .index _ a b => pureExprCalls fuel a ++ pureExprCalls fuel b
    | .field _ a _ | .unary _ _ a | .convert _ _ a | .variantTag _ a | .variantGet _ a _ _ => pureExprCalls fuel a
    | .makeRecord _ args | .makeVariant _ _ args | .makeVec _ args => args.flatMap (pureExprCalls fuel)
    | _ => []
private def pureStmtCalls : Nat → List Stmt → List Nat
  | 0,_ => []
  | fuel+1,body => body.flatMap fun stmt => match stmt with
    | .letVal _ expr | .writeState _ expr | .check expr _ | .await expr _ _ | .transportReturn expr => pureExprCalls fuel expr
    | .serviceCall _ _ args | .emit _ args | .ret args => args.flatMap (pureExprCalls fuel)
    | .branch expr yes no => pureExprCalls fuel expr ++ pureStmtCalls fuel yes ++ pureStmtCalls fuel no
    | .repeat _ body => pureStmtCalls fuel body
    | _ => []
private def checkPureCallPath (project : Project) : Nat → List Nat → Nat → Except String Unit
  | 0,_,_ => throw "PureCallGraphBudget"
  | fuel+1,path,id => do
    if path.contains id then throw "RecursivePureCall"
    let some handler := project.handlers.find? (fun h => h.id == id) | throw "UnknownPureFunction"
    if handler.context != .pureFunction then throw "ImpureCall"
    for child in pureStmtCalls project.profile.instructionFuel handler.body do checkPureCallPath project fuel (id::path) child
private def checkPureCalls (project : Project) : Except String Unit := do
  for handler in project.handlers do
    if handler.context == .pureFunction then checkPureCallPath project (project.handlers.length+1) [] handler.id
private def checkHandlerShape (p : Project) (h : Handler) : Except String Unit := do
  if h.source.file.isEmpty || h.source.line == 0 then throw "MissingSource"
  if let some results := h.declaredResultTypes then checkReturnShape results p.profile.instructionFuel h.body
  if (h.parameters.map LocalBinderIR.id).eraseDups.length != h.parameters.length then throw "DuplicateHandlerParameter"
  for param in h.parameters do
    if p.types[param.typeId]?.isNone then throw "UnknownHandlerParameterType"
  if h.declaredResultTypes.any (fun ts => !(ts.all (fun t => t < p.types.length))) then throw "UnknownHandlerResultType"
  if h.context == .process then
    let some capacity := h.processCapacity | throw "MissingProcessCapacity"
    if capacity.maxInstances == 0 || capacity.frameBytesLimit == 0 || capacity.resultCapacity == 0 then throw "InvalidProcessCapacity"
    match capacity.overflow with
    | .reject => pure ()
    | .awaitSlot n | .queue n => if n == 0 then throw "InvalidOverflowCapacity"
  else if h.processCapacity.isSome then throw "CapacityOnNonProcess"
private def checkStateTable (p : Project) (states : List StateSlot) : Except String Unit := do
  if (states.map StateSlot.id).eraseDups.length != states.length then throw "DuplicateState"
  for s in states do
    let some t := p.types[s.typeId]? | throw "UnknownStateType"
    if !conforms p.types (p.types.length+1) t s.initial then throw "InvalidInitialValue"
def validateMetadata (p : Project) : Except String Unit := do
  validateTopology p
  if (p.services.map ServiceIR.id).eraseDups.length != p.services.length then throw "DuplicateService"
  for service in p.services do
    let some (contexts,effects) := serviceContract service.opcode | throw "UnknownServiceOpcode"
    if service.id ≥ 2^32 || service.resultTypes.length > 1 || !(service.inputTypes ++ service.resultTypes).all (fun t => t < p.types.length) then throw "InvalidServiceSignature"
    if service.contextMask == 0 || Nat.land service.contextMask contexts != service.contextMask || service.effectMask ≥ 2048 || Nat.land service.effectMask effects != effects then throw "InvalidServiceEffects"
    if service.providerKey.isEmpty || service.providerVersion.isEmpty || service.abiHash.length != 32 || service.extraFuel ≥ 2^64 then throw "InvalidServiceBinding"
  if (p.externalContracts.map ExternalContractIR.id).eraseDups.length != p.externalContracts.length then throw "DuplicateExternalContract"
  for contract in p.externalContracts do
    if contract.cppType.isEmpty || contract.header.isEmpty || contract.source.file.isEmpty || contract.source.line == 0 then throw "IncompleteExternalContract"
  let knownCapabilities := ["ext","standalone-protocol","extern-pure","vendor-protocol-adapter","managed-access","raw-dmi"]
  if !(p.enabledCapabilities.all (knownCapabilities.contains ·)) then throw "UnknownCapability"
  if (p.components.map ComponentIR.id).eraseDups.length != p.components.length then throw "DuplicateDefinition"
  if (p.systems.map SystemIR.id).eraseDups.length != p.systems.length then throw "DuplicateSystem"
  if p.topSystemId.any (fun id => !(p.systems.any (fun s => s.id == id))) then throw "UnknownTopSystem"
  for c in p.components do
    checkStateTable p c.states
    if (c.parameters.map ParamDeclIR.id).eraseDups.length != c.parameters.length then throw "DuplicateComponentParameter"
    for param in c.parameters do
      let some ty := p.types[param.typeId]? | throw "UnknownComponentParameterType"
      if param.defaultValue.any (fun v => !conforms p.types (p.types.length+1) ty v) then throw "InvalidParameterDefault"
    if c.source.file.isEmpty || c.source.line == 0 then throw "MissingSource"
    if (c.endpoints.map EndpointIR.id).eraseDups.length != c.endpoints.length then throw "DuplicateEndpoint"
    if (c.processes.map ProcessIR.id).eraseDups.length != c.processes.length then throw "DuplicateProcess"
    if (c.sidebands.map PortDeclIR.id).eraseDups.length != c.sidebands.length then throw "DuplicatePort"
    for port in c.sidebands do
      let some ty := p.types[port.typeId]? | throw "UnknownPortType"
      if port.initial.any (fun v => !conforms p.types (p.types.length+1) ty v) then throw "InvalidPortInitial"
    for endpoint in c.endpoints do
      if endpoint.busWidth == 0 || endpoint.busWidth % 8 != 0 || endpoint.maxBindings == 0 || endpoint.maxOutstanding == 0 then throw "InvalidEndpointCapacity"
      if endpoint.protocolRef != "tlm.base.v1" then throw "UnknownProtocolReference"
    let localProject := {p with states := c.states, handlers := c.handlers, components := [], systems := []}
    checkPureCalls localProject
    for h in c.handlers do
      checkHandlerShape p h
      checkContext p h.context p.profile.instructionFuel h.body
      if h.endpoint.any (fun id => !(c.endpoints.any (fun ep => ep.id == id))) then throw "UnknownHandlerEndpoint"
      if h.context == .transportEntry && h.endpoint.isNone then throw "MissingHandlerEndpoint"
      let _ ← checkStmts localProject p.profile.instructionFuel (h.parameters.map (fun b => (b.id,b.typeId))) h.body
    for process in c.processes do
      if p.types[process.resultType]?.isNone then throw "UnknownProcessResultType"
      checkReturnShape [process.resultType] 1000000 process.body
      if process.capacity.maxInstances == 0 || process.capacity.resultCapacity == 0 || process.instructionFuel == 0 then throw "InvalidProcessCapacity"
      if process.ownerPolicy.isEmpty || process.resultLifetimePolicy.isEmpty then throw "MissingOwnershipPolicy"
      match process.capacity.overflow with
      | .reject => pure ()
      | .awaitSlot n | .queue n => if n == 0 then throw "InvalidOverflowCapacity"
      for param in process.params do
        if p.types[param.typeId]?.isNone then throw "UnknownParameterType"
      if (process.params.map LocalBinderIR.id).eraseDups.length != process.params.length then throw "DuplicateParameter"
      checkContext p .process 1000000 process.body
      let _ ← checkStmts localProject 1000000 (process.params.map (fun b => (b.id,b.typeId))) process.body
  for system in p.systems do
    if (system.instances.map InstanceIR.id).eraseDups.length != system.instances.length then throw "DuplicateInstance"
    if (system.bindings.map BindingIR.id).eraseDups.length != system.bindings.length then throw "DuplicateConnection"
    for inst in system.instances do
      if !(p.components.any (fun c => c.id == inst.definition)) then throw "UnknownDefinition"
    let endpoint := fun ref => do
      let some inst := system.instances.find? (fun i => i.id == ref.instanceId) | throw "UnknownInstance"
      let some component := p.components.find? (fun c => c.id == inst.definition) | throw "UnknownDefinition"
      let some endpoint := component.endpoints.find? (fun e => e.id == ref.endpoint) | throw "UnknownEndpoint"
      if ref.bindingIndex ≥ endpoint.maxBindings then throw "BindingIndexOutOfRange"
      pure endpoint
    for binding in system.bindings do
      let sourceEndpoint ← endpoint binding.sourceEndpoint
      let targetEndpoint ← endpoint binding.sinkEndpoint
      if sourceEndpoint.role != .initiator || targetEndpoint.role != .target then throw "EndpointRoleMismatch"
      if sourceEndpoint.busWidth != targetEndpoint.busWidth || sourceEndpoint.protocolRef != targetEndpoint.protocolRef then throw "EndpointContractMismatch"
    if (system.bindings.map BindingIR.sinkEndpoint).eraseDups.length != system.bindings.length then throw "DuplicateSocketSink"
    for map in system.addressMaps do
      if map.size == 0 || map.sourceStart + (map.size-1) ≥ 2^64 || map.targetStart + (map.size-1) ≥ 2^64 then throw "AddressRangeOverflow"
      let _ ← endpoint ⟨map.decoder,map.outputEndpoint,map.bindingIndex⟩
  pure ()
def checkSchema (p : Project) : Except String Unit := do
  validateMetadata p
  if p.version != schemaVersion then throw "SchemaVersionMismatch"
  if p.profile.instructionFuel == 0 || p.profile.eventCapacity == 0 || p.profile.maxEventsPerTick == 0 then throw "InvalidBudget"
  checkTypes p.types
  if (p.states.map StateSlot.id).eraseDups.length != p.states.length then throw "DuplicateState"
  if (p.handlers.map Handler.id).eraseDups.length != p.handlers.length then throw "DuplicateHandler"
  for s in p.states do
    let some t := p.types[s.typeId]? | throw "UnknownStateType"
    if !conforms p.types (p.types.length+1) t s.initial then throw "InvalidInitialValue"
  checkPureCalls p
  for h in p.handlers do
    checkHandlerShape p h
    checkContext p h.context p.profile.instructionFuel h.body
    if h.endpoint.isSome then throw "FlatHandlerHasNoEndpointOwner"
    if h.source.file.isEmpty || h.source.line == 0 then throw "MissingSource"
    let _ ← checkStmts p p.profile.instructionFuel (h.parameters.map (fun b => (b.id,b.typeId))) h.body
  pure ()

/-- A kernel-checkable certificate binds the checked wrapper to this exact project. -/
structure SchemaCheckedProject where
  project : Project
  certificate : checkSchema project = .ok ()

abbrev ValidatedProject := SchemaCheckedProject

def validateSchema (p : Project) : Except String SchemaCheckedProject :=
  match h : checkSchema p with
  | .ok () => .ok ⟨p,h⟩
  | .error e => .error e
end LeanAT.ModelIR















