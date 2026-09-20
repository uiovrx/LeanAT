import LeanAT.ExecIR.Reference
import LeanAT.Compiler.Provenance

namespace LeanAT.Compiler
open ExecIR

private structure Builder where
  nextReg : Nat := 0
  nextBlock : Nat := 1
  current : Nat := 0
  parameters : List VReg := []
  instructions : List Instruction := []
  blocks : List Block := []
  locals : List (Nat × VReg) := []
  closed : Bool := false
  source : String := ""
  stateIds : List Nat := []
  resultTypes : Option (List Nat) := none
  workRemaining : Nat := 10000
  services : List ServiceSignature := []
  context : Nat := 0
  pureFunction : Bool := false
  effectMask : Nat := 0
  frameRegisters : List VReg := []
  origins : List LoweredOrigin := []
  deriving Inhabited
private abbrev Build := StateT Builder (Except String)
private def consumeWork : Build Unit := do
  let s ← get
  if s.workRemaining == 0 then throw "LoweringWorkLimit"
  set {s with workRemaining := s.workRemaining-1}
private def freshReg (t : Nat) : Build VReg := do
  let s ← get
  if s.nextReg ≥ 2^32 then throw "RegisterLimit"
  set {s with nextReg := s.nextReg+1}
  pure ⟨s.nextReg,t⟩
private def freshBlock : Build Nat := do
  let s ← get
  if s.nextBlock ≥ 100000 then throw "BlockLimit"
  set {s with nextBlock := s.nextBlock+1}
  pure s.nextBlock
private def emit (i : Instruction) : Build Unit := modify fun s => {s with instructions := s.instructions ++ [{i with source := s.source}],effectMask := Nat.lor s.effectMask (requiredEffect i.op), origins := s.origins ++ [{kind := "instruction",block := some s.current,instruction := some s.instructions.length,node := s.source}]}
private def close (t : Terminator) : Build Unit := modify fun s =>
  {s with blocks := s.blocks ++ [⟨s.current,s.parameters,s.instructions,t⟩], closed := true, origins := s.origins ++ [{kind := "terminator",block := some s.current,node := s.source}]}
private def start (id : Nat) (ps : List VReg := []) : Build Unit := modify fun s =>
  {s with current := id, parameters := ps, instructions := [], closed := false, origins := s.origins ++ [{kind := "block",block := some id,node := s.source}]}
private def instruction (t : Nat) (op : Op) (args : List VReg := []) (value : Value := .unit)
    (immediate : Nat := 0) (operator : Pure.BinaryOp := .addWrap) : Build VReg := do
  let dest ← freshReg t
  emit {op,args,dest := some dest,value,immediate,operator}
  pure dest

private partial def lowerExpr (e : ModelIR.Expr) (path : String) : Build VReg := do
  consumeWork
  let outer := (← get).source
  modify fun s => {s with source := path}
  let child := fun e i => lowerExpr e s!"{path}/expr/{i}"
  let result ← match e with
  | .literal t v => instruction t .const [] v
  | .local _ id => match (← get).locals.lookup id with | some r => pure r | none => throw "UnknownLocal"
  | .state t id =>
    let some idx := (← get).stateIds.idxOf? id | throw "UnknownState"
    instruction t .loadState [] .unit idx
  | .binary t op a b => instruction t .binary [← child a (0 : Nat), ← child b 1] .unit 0 op
  | .field t obj idx => instruction t .getField [← child obj (0 : Nat)] .unit idx
  | .index t obj idx => instruction t .vecGet [← child obj (0 : Nat), ← child idx 1]
  | .makeRecord t es => instruction t .makeRecord (← es.zipIdx.mapM fun (e,i) => child e i)
  | .makeVariant t tag es => instruction t .makeVariant (← es.zipIdx.mapM fun (e,i) => child e i) .unit tag
  | .unary t mode value => instruction t .unary [← child value (0 : Nat)] .unit mode
  | .compare t mode left right => instruction t .compare [← child left (0 : Nat), ← child right 1] .unit mode
  | .convert t mode value => instruction t .convert [← child value (0 : Nat)] .unit mode
  | .variantTag t value => instruction t .variantTag [← child value (0 : Nat)]
  | .variantGet t value tag field => do
    if tag ≥ 65536 || field ≥ 65536 then throw "VariantProjectionEncodingLimit"
    instruction t .variantGet [← child value (0 : Nat)] .unit (tag*65536+field)
  | .makeVec t values => instruction t .makeVec (← values.zipIdx.mapM fun (e,i) => child e i)
  | .vecSet t vector index value => instruction t .vecSet [← child vector (0 : Nat), ← child index 1, ← child value 2]
  | .callPure t handlerId args => instruction t .callPure (← args.zipIdx.mapM fun (e,i) => child e i) .unit handlerId
  | .select t condition yes no =>
    let c ← child condition (0 : Nat)
    let totalLeaf := fun e => match e with | .literal .. | .local .. => true | _ => false
    if totalLeaf yes && totalLeaf no then
      instruction t .selectValue [c,← child yes (1 : Nat),← child no (2 : Nat)]
    else do
      let y ← freshBlock
      let n ← freshBlock
      let join ← freshBlock
      let result ← freshReg t
      close (.branch c ⟨y,[]⟩ ⟨n,[]⟩)
      start y
      let yv ← child yes (1 : Nat)
      close (.jump ⟨join,[yv]⟩)
      start n
      let nv ← child no (2 : Nat)
      close (.jump ⟨join,[nv]⟩)
      start join [result]
      pure result
  modify fun s => {s with source := outer}
  pure result

private partial def lowerStmts (stmts : List ModelIR.Stmt) (nodePrefix : String) : Build Unit := do
  for (stmt,index) in stmts.zipIdx do
    if (← get).closed then break
    consumeWork
    let path := s!"{nodePrefix}/{index}"
    modify fun s => {s with source := path}
    let expr := fun e i => lowerExpr e s!"{path}/expr/{i}"
    match stmt with
    | .readNow binder =>
      let r ← instruction binder.typeId .getNow
      modify fun s => {s with locals := (binder.id,r)::s.locals}
    | .letVal id e =>
      let raw ← expr e (0 : Nat)
      let r ← match e with | .local .. => instruction raw.typeId .move [raw] | _ => pure raw
      modify fun s => {s with locals := (id,r)::s.locals}
    | .writeState id e =>
      let r ← expr e (0 : Nat)
      let some idx := (← get).stateIds.idxOf? id | throw "UnknownState"
      emit {op := .bufferStateWrite, args := [r], immediate := idx}
    | .check c error =>
      let condition ← expr c (0 : Nat)
      if (← get).pureFunction then
        let success ← freshBlock
        let failure ← freshBlock
        close (.branch condition ⟨success,[]⟩ ⟨failure,[]⟩)
        start failure
        close (.fail error)
        start success
      else emit {op := .check,args := [condition],text := error}
    | .emit tag es => emit {op := .trace,args := ← es.zipIdx.mapM fun (e,i) => expr e i,text := tag}
    | .fail error => close (.fail error)
    | .ret es =>
      let rs ← es.zipIdx.mapM fun (e,i) => expr e i
      let ts := rs.map VReg.typeId
      let s ← get
      if s.resultTypes.any (· != ts) then throw "InconsistentReturnLayout"
      modify fun s => {s with resultTypes := some ts}
      if (← get).context == 2 then throw "OrdinaryReturnInTransport"
      close (.ret rs)
    | .transportReturn value =>
      let r ← expr value (0 : Nat)
      let s ← get
      if s.resultTypes.any (· != [r.typeId]) then throw "InconsistentTransportReturnLayout"
      modify fun s => {s with resultTypes := some [r.typeId]}
      if !s.instructions.getLast?.any (fun i => i.op == .setTransportReturn && i.args == [r]) then
        let some service := s.services.find? (fun service => service.op == .setTransportReturn && service.inputTypes == [r.typeId] && service.resultTypes.isEmpty) | throw "MissingTransportReturnService"
        emit {op := .setTransportReturn,args := [r],immediate := service.id}
      close (.transportReturn r)
    | .serviceCall destination serviceId args =>
      let some service := (← get).services.find? (fun s => s.id == serviceId) | throw "UnknownService"
      let args ← args.zipIdx.mapM fun (e,i) => expr e i
      let dest ← match destination with
        | none => pure none
        | some binder => do
          let r ← freshReg binder.typeId
          modify fun s => {s with locals := (binder.id,r)::s.locals}
          pure (some r)
      emit {op := service.op,args,dest,immediate := serviceId}
      modify fun s => {s with effectMask := Nat.lor s.effectMask service.effectMask}
    | .await wait outcomeBinder outcomeType =>
      let wait ← expr wait (0 : Nat)
      let locals := (← get).locals
      let live := (locals.reverse.map Prod.snd).eraseDups
      let copies ← live.mapM (fun r => freshReg r.typeId)
      let result ← freshReg outcomeType
      let resume ← freshBlock
      close (.suspend wait resume live)
      start resume (result::copies)
      let mapping := (live.zip copies).map (fun (old,new) => (old.id,new))
      modify fun s => {s with
        locals := (outcomeBinder,result)::locals.map (fun (name,r) => (name,(mapping.lookup r.id).getD r))
        frameRegisters := (s.frameRegisters ++ live).eraseDups
        effectMask := Nat.lor s.effectMask 16}
    | .unsupported feature => throw ("UnsupportedFeature: " ++ feature)
    | .repeat bound body =>
      if bound > (← get).workRemaining then throw "LoweringWorkLimit"
      let locals := (← get).locals
      for _ in [:bound] do
        if (← get).closed then break
        modify fun s => {s with locals}
        lowerStmts body (path ++ "/body")
      modify fun s => {s with locals}
    | .branch c yes no =>
      let r ← expr c (0 : Nat)
      let y ← freshBlock
      let n ← freshBlock
      let join ← freshBlock
      let locals := (← get).locals
      close (.branch r ⟨y,[]⟩ ⟨n,[]⟩)
      start y
      lowerStmts yes (path ++ "/yes")
      let yc := (← get).closed
      modify fun s => {s with source := path}
      if !yc then close (.jump ⟨join,[]⟩)
      modify fun s => {s with locals}
      start n
      lowerStmts no (path ++ "/no")
      let nc := (← get).closed
      modify fun s => {s with source := path}
      if !nc then close (.jump ⟨join,[]⟩)
      modify fun s => {s with locals}
      if !(yc && nc) then start join

private def lowerService (service : ModelIR.ServiceIR) : Except String ServiceSignature := do
  let some tag := opcodeNames.idxOf? service.opcode | throw "UnknownServiceOpcode"
  let some op := allOps[tag]? | throw "UnknownServiceOpcode"
  pure ⟨service.id,op,service.inputTypes,service.resultTypes,service.contextMask,service.effectMask,service.extraFuel,service.providerKey,service.providerVersion,⟨service.abiHash.toArray⟩⟩

def lowerHandlerWithOrigins (p : ModelIR.Project) (h : ModelIR.Handler) (path : String) (allowEndpoint : Bool := false) : Except String (Program × List LoweredOrigin) := do
  if h.endpoint.isSome && !allowEndpoint then throw "UnsupportedHandlerEndpoint: requires hierarchy lowering"
  let context ← match h.context with
    | .timedHandler | .pureFunction => pure 0 | .process => pure 1 | .transportEntry => pure 2
    | .debugEntry => pure 3 | .dmiEntry => pure 4 | _ => throw "UnsupportedHandlerContext"
  let services ← p.services.mapM lowerService
  let source := path
  let parameters := h.parameters.zipIdx |>.map (fun (p,id) => (⟨id,p.typeId⟩ : VReg))
  let locals := h.parameters.zip parameters |>.map (fun (p,r) => (p.id,r))
  let (_,s) ← (do
    lowerStmts h.body (path ++ "/body")
    modify fun s => {s with source := path}
    if !(← get).closed then close (.ret []) : Build Unit).run {source, stateIds := p.states.map ModelIR.StateSlot.id,services,context,parameters,locals,nextReg := parameters.length,resultTypes := h.declaredResultTypes,pureFunction := h.context == .pureFunction, origins := [{kind := "program",node := path},{kind := "block",block := some 0,node := path}]}
  let layouts ← typeLayouts p.types
  let mut frame : List FrameSlot := []
  let mut frameBytes := 0
  for r in s.frameRegisters do
    let some size := layouts[r.typeId]? | throw "MissingFrameType"
    frame := frame ++ [⟨r.typeId,frameBytes,8,r.id⟩]
    frameBytes := frameBytes+size*64
    if frameBytes ≥ 2^32 then throw "FrameSizeOverflow"
  if context == 1 && frameBytes > h.frameBytesLimit then throw "ProcessFrameCapacity"
  pure ({id := h.id, inputTypes := parameters.map VReg.typeId,blocks := s.blocks, resultTypes := s.resultTypes.getD [], source,context,frame,frameBytes,effectMask := s.effectMask},s.origins)

def lowerHandler (p : ModelIR.Project) (h : ModelIR.Handler) (allowEndpoint : Bool := false) : Except String Program :=
  (lowerHandlerWithOrigins p h s!"handler/{h.id}" allowEndpoint).map Prod.fst

private def sourceText (s : SourceSpan) := s!"{s.file}:{s.line}:{s.column}"

private def processHandler (process : ModelIR.ProcessIR) : ModelIR.Handler := {
  id := process.id,body := process.body,source := process.source,context := .process,
  trigger := "process",parameters := process.params,processCapacity := some process.capacity,
  declaredResultTypes := some [process.resultType]}

private def componentEntries (component : ModelIR.ComponentIR) : List (ModelIR.Handler × Option ModelIR.ProcessIR) :=
  component.handlers.map (fun h => (h,none)) ++ component.processes.map (fun p => (processHandler p,some p))

private def needsV5 (types : TypeEnvironment) (programs : List Program) : Bool :=
  types.any (fun t => match t with | .fin _ => true | _ => false) ||
  programs.any (fun p => p.instructionFuel > 0 || p.blocks.any (fun b => b.instructions.any (fun i =>
    (i.op == .vecGet || i.op == .vecSet) && i.args[1]?.any (fun r => types[r.typeId]? != some (.bits 64)))))
private def lowerHierarchy (p : ModelIR.Project) : Except String ExecProject := do
  let some topId := p.topSystemId | throw "MissingTopSystemSelection"
  let some selected := p.systems.find? (fun s => s.id == topId) | throw "UnknownTopSystem"
  let instances ← selected.instances.mapM (fun inst => do
    let some component := p.components.find? (fun c => c.id == inst.definition) | throw "UnknownInstanceDefinition"
    let resolvedConfig ← component.parameters.mapM (fun parameter => do
      let some value := (inst.resolvedConfig.lookup parameter.id).or parameter.defaultValue | throw "MissingResolvedParameter"
      pure (parameter.id,value))
    pure {inst with resolvedConfig})
  let system := {selected with instances}
  if !p.states.isEmpty || !p.handlers.isEmpty then throw "MixedFlatAndHierarchyProject"
  let services ← p.services.mapM lowerService
  let mut result : ExecProject := {
    types := p.types
    stateTypes := []
    initialState := []
    programs := []
    schemaMajor := if p.components.any (fun c => !c.processes.isEmpty) || p.types.any (fun t => match t with | .fin _ => true | _ => false) then 5 else 4
    services := services
    systemMetadata := some system
    externalContracts := p.externalContracts
    capabilities := p.enabledCapabilities
    profile := if !p.enabledCapabilities.isEmpty || services.any (fun s => s.op.tag ≥ 46) then "AT-Ext-1.1-draft" else "AT-Core-1.1-draft"}
  for component in p.components do
    for process in component.processes do
      if process.instructionFuel == 0 || process.instructionFuel ≥ 2^64 || process.ownerPolicy != "caller" || process.resultLifetimePolicy != "until-release" then throw "UnsupportedProcessPolicies"
    result := {result with components := result.components ++ [⟨component.id.value,component.parameters,component.states,component.endpoints,component.sidebands,component.resetPolicy,sourceText component.source⟩]}
  for inst in system.instances do
    let some component := p.components.find? (fun c => c.id == inst.definition) | throw "UnknownInstanceDefinition"
    let stateBase := result.initialState.length
    let mut bindings : List HandlerBinding := []
    let programBase := result.programs.length
    let entries := componentEntries component
    for (handler,policy) in entries do
      let sourceKind := if policy.isSome then "process" else "handler"
      let (program,_) ← lowerHandlerWithOrigins {p with states := component.states} handler s!"component/{component.id.value}/{sourceKind}/{handler.id}" true
      let programId := result.programs.length
      let blocks ← program.blocks.mapM (fun block => do
        let instructions ← block.instructions.mapM (fun i => do
          if i.op == .loadState || i.op == .bufferStateWrite then return {i with immediate := stateBase+i.immediate}
          if i.op == .callPure then
            let some offset := entries.findIdx? (fun entry => entry.1.id == i.immediate && entry.1.context == .pureFunction) | throw "UnknownLocalPureFunction"
            return {i with immediate := programBase+offset}
          pure i)
        pure {block with instructions})
      let program := {program with
        id := programId
        blocks := blocks
        instructionFuel := (policy.map (·.instructionFuel)).getD 0
        ownerPolicy := (policy.map (·.ownerPolicy)).getD ""
        resultLifetimePolicy := (policy.map (·.resultLifetimePolicy)).getD ""}
      result := {result with programs := result.programs ++ [program]}
      bindings := bindings ++ [⟨handler.id,programId,program.context,handler.trigger,handler.endpoint,handler.processCapacity,sourceText handler.source⟩]
    result := {result with
      stateTypes := result.stateTypes ++ component.states.map ModelIR.StateSlot.typeId
      initialState := result.initialState ++ component.states.map ModelIR.StateSlot.initial
      instances := result.instances ++ [⟨inst.id.value,component.id.value,stateBase,component.states.length,bindings,inst.resolvedConfig,sourceText inst.source⟩]}
  pure {result with schemaMajor := if needsV5 result.types result.programs then 5 else result.schemaMajor}

def lower (p : ModelIR.ValidatedProject) : Except String ExecProject := do
  if !p.project.components.isEmpty || !p.project.systems.isEmpty then return ← lowerHierarchy p.project
  if !p.project.externalContracts.isEmpty || !p.project.enabledCapabilities.isEmpty then throw "FlatProjectMetadataRequiresHierarchy"
  let programs ← p.project.handlers.mapM (lowerHandler p.project)
  let services ← p.project.services.mapM lowerService
  let extended := !services.isEmpty || programs.any (fun p => p.context != 0 || !p.frame.isEmpty || p.blocks.any (fun b => b.instructions.any (fun i => i.op.tag ≥ 16))) || p.project.types.any (fun t => match t with | .bytes _ | .handle _ => true | _ => false)
  pure {
    types := p.project.types
    stateTypes := p.project.states.map ModelIR.StateSlot.typeId
    initialState := p.project.states.map ModelIR.StateSlot.initial
    programs := if extended then programs else programs.map (fun p => {p with effectMask := 0})
    schemaMajor := if needsV5 p.project.types programs then 5 else if extended then 3 else 1
    services := services
    profile := if services.any (fun s => s.op.tag ≥ 46) then "AT-Ext-1.1-draft" else "AT-Core-1.1-draft"
  }

def compile (p : ModelIR.Project) : Except String ExecIR.ValidatedProject := do
  ExecIR.validateExec (← lower (← ModelIR.validateSchema p))

/-- Explicit profile selection still passes the complete descriptor validator. -/
def compileForProfile (p : ModelIR.Project) (profile : String) : Except String ExecIR.ValidatedProject := do
  if profile != "AT-Core-1.1-draft" && profile != "AT-Ext-1.1-draft" then throw "UnknownProfile"
  let project ← lower (← ModelIR.validateSchema p)
  if profile == "AT-Core-1.1-draft" && (!project.capabilities.isEmpty || project.programs.any (fun program =>
      program.blocks.any (fun block => block.instructions.any (fun i => i.op.tag ≥ 46)))) then
    throw "ExtFeatureInCoreProfile"
  let project := if profile == "AT-Ext-1.1-draft" && project.schemaMajor == 1 then
    {project with schemaMajor := 2,programs := project.programs.map (fun program => {program with
      effectMask := program.blocks.foldl (fun mask block => block.instructions.foldl (fun mask i => Nat.lor mask (ExecIR.requiredEffect i.op)) mask) 0})}
    else project
  ExecIR.validateExec {project with profile}

structure Compilation where
  validated : ExecIR.ValidatedProject
  origins : List LoweredOrigin
  nodes : List ModelNode

/-- Replays the same bounded lowering to obtain its emitted-location audit trail. -/
def compileWithProvenance (p : ModelIR.Project) : Except String Compilation := do
  let validated ← compile p
  let mut origins := []
  let mut nodes := []
  if let some system := validated.project.systemMetadata then
    for inst in system.instances do
      let some component := p.components.find? (fun c => c.id == inst.definition) | throw "UnknownInstanceDefinition"
      for ((h,process),ordinal) in (componentEntries component).zipIdx do
        let sourceKind := if process.isSome then "process" else "handler"
        let path := s!"component/{component.id.value}/{sourceKind}/{h.id}"
        let (_,mapping) ← lowerHandlerWithOrigins {p with states := component.states} h path true
        let some owner := validated.project.instances.find? (fun i => i.id == inst.id.value) | throw "MissingInstance"
        let some binding := owner.handlers[ordinal]? | throw "MissingHandler"
        if binding.localId != h.id then throw "HandlerOrdinalMismatch"
        origins := origins ++ mapping.map (fun m => {m with program := binding.programId})
        if !(nodes.any (fun n => n.path == path)) then
          nodes := nodes ++ (match process with
            | some process => SourceMapping.processNodes path process
            | none => handlerNodes path h)
  else
    for h in p.handlers do
      let path := s!"handler/{h.id}"
      let (_,mapping) ← lowerHandlerWithOrigins p h path
      origins := origins ++ mapping.map (fun m => {m with program := h.id})
      nodes := nodes ++ handlerNodes path h
  pure ⟨validated,origins,nodes⟩

end LeanAT.Compiler
