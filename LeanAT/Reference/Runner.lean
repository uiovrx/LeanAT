import LeanAT.ModelIR.Semantics
import LeanAT.Reference.Exec
namespace LeanAT.Reference
private def sourceServices (project : ModelIR.Project) : Except String (List ExecIR.ServiceSignature) :=
  project.services.mapM fun service => do
    let some op := ExecIR.allOps.find? (fun op => ExecIR.opcodeNames[op.tag]? == some service.opcode) | throw "UnknownServiceOpcode"
    pure ⟨service.id,op,service.inputTypes,service.resultTypes,service.contextMask,service.effectMask,service.extraFuel,service.providerKey,service.providerVersion,⟨service.abiHash.toArray⟩⟩
private def contextKind : ModelIR.ContextKind → Nat
  | .timedHandler => 0 | .process => 1 | .transportEntry => 2 | .debugEntry => 3 | .dmiEntry => 4 | _ => 5
private def checkValues (types : TypeEnvironment) (typeIds : List Nat) (values : List Value) : Except String Unit := do
  if typeIds.length != values.length then throw "ReferenceValueArity"
  for (typeId,value) in typeIds.zip values do
    if !(types[typeId]?).any (fun type => conforms types (types.length+1) type value) then throw "ReferenceValueType"
private def prepareContext (context : Context) (world : State) : Context :=
  {context with environment := ("objects.committed",.vec (world.objects.filter (fun entry => entry.alive && entry.tag.startsWith "reference.object.") |>.map (fun entry => .record [.handle entry.identity,entry.value])))::context.environment.filter (fun pair => pair.1 != "objects.committed")}
private def seed (world : State) (context : Context) (program : Nat) : Except String State := do
  if context.kind != 1 then return world
  let some process := context.processIdentity | throw "MissingProcessContext"
  if world.objects.any (fun entry => entry.identity == process) then
    let data ← Runtime.getProcess world context process
    if data.program != program then throw "ProcessProgramMismatch"
    return world
  Runtime.seedProcess world context process program
private def failed (error : String) (committed : List Value) (world : State) (fuel : Nat) : Outcome :=
  {ok := false,error,committed,world,remainingFuel := fuel,exit := "failed"}

private def commitWorld (context : Context) (world : State) : Except String State := do
  Protocol.committed context (← Objects.committed world)

structure SourceSelection where
  project : ModelIR.Project
  handler : ModelIR.Handler
  stateBase : Nat := 0
  allStateTypes : List Nat
  runtimeProgram : Nat
  sourceRoot : String
  instructionFuel : Nat := 0
  sourcePrograms : List (Nat × Nat) := []

private def processHandler (process : ModelIR.ProcessIR) : ModelIR.Handler := {
  id := process.id,body := process.body,source := process.source,context := .process,
  trigger := "process",parameters := process.params,processCapacity := some process.capacity,
  declaredResultTypes := some [process.resultType]}

def resolveSource (p : ModelIR.Project) (handlerId : Nat) (context : Context) : Except String SourceSelection := do
  if p.components.isEmpty && p.systems.isEmpty then
    let some handler := p.handlers.find? (fun h => h.id == handlerId) | throw "UnknownHandler"
    return ⟨p,handler,0,p.states.map ModelIR.StateSlot.typeId,handlerId,"handler/" ++ toString handlerId,0,p.handlers.map (fun h => (h.id,h.id))⟩
  let some topId := p.topSystemId | throw "MissingTopSystemSelection"
  let some system := p.systems.find? (fun system => system.id == topId) | throw "UnknownTopSystem"
  if context.domain != system.runtimeDomain then throw "ExecutionOwnership"
  let mut allStateTypes := []
  let mut stateBase := 0
  let mut programBase := 0
  let mut selected : Option SourceSelection := none
  for inst in system.instances do
    let some component := p.components.find? (fun component => component.id == inst.definition) | throw "UnknownInstanceDefinition"
    let entries := component.handlers.map (fun handler => (handler,none)) ++ component.processes.map (fun process => (processHandler process,some process))
    let handlers := entries.map Prod.fst
    if inst.id.value == context.instanceId then
      let some index := entries.findIdx? (fun (handler,_) => handler.id == handlerId && contextKind handler.context == context.kind) | throw "UnknownHandler"
      let some (handler,process) := entries[index]? | throw "UnknownHandler"
      if let some process := process then
        if process.ownerPolicy != "caller" || process.resultLifetimePolicy != "until-release" then throw "UnsupportedProcessPolicies"
      selected := some ⟨{p with states := component.states,handlers},handler,stateBase,[],programBase+index,
        "component/" ++ toString component.id.value ++ "/" ++ (if process.isSome then "process" else "handler") ++ "/" ++ toString handlerId,
        (process.map ModelIR.ProcessIR.instructionFuel).getD 0,(handlers.zipIdx).map (fun (h,index) => (h.id,programBase+index))⟩
    allStateTypes := allStateTypes ++ component.states.map ModelIR.StateSlot.typeId
    stateBase := stateBase+component.states.length
    programBase := programBase+handlers.length
  let some result := selected | throw "ExecutionOwnership"
  pure {result with allStateTypes}
def runModelSegment (validated : ModelIR.ValidatedProject) (handlerId : Nat) (inputs committed : List Value) (world : State) (context : Context) (fuel : Nat) : Outcome := Id.run do
  let resolved := resolveSource validated.project handlerId context
  let .ok selection := resolved | return failed (match resolved with | .error error => error | _ => "ReferenceSourceSelection") committed world fuel
  let p := selection.project
  let handler := selection.handler
  if contextKind handler.context != context.kind then return failed "ExecutionContextMismatch" committed world fuel
  let initialSeed := seed world context selection.runtimeProgram
  let .ok world := initialSeed | return failed (match initialSeed with | .error error => error | _ => "ReferenceProcessSeed") committed world fuel
  let setup := do
    checkValues p.types (handler.parameters.map ModelIR.LocalBinderIR.typeId) inputs
    checkValues p.types selection.allStateTypes committed
    let services ← sourceServices p
    Dispatch.validate p.types context services
    reserveSegment world
  let .ok (prepared,highWater) := setup | return failed (match setup with | .error error => error | _ => "ReferenceSetup") committed world fuel
  let context := prepareContext context world
  let machine : Source.Machine := {values := (p.states.zip (committed.drop selection.stateBase)).map (fun (slot,value) => (slot.id,value)),locals := (handler.parameters.zip inputs).map (fun (binder,value) => (binder.id,value)),localTypes := handler.parameters.map (fun binder => (binder.id,binder.typeId)),frameBytesLimit := handler.processCapacity.map ModelIR.ProcessCapacityIR.frameBytesLimit,world := prepared,context,remainingFuel := fuel,fuelCaps := if selection.instructionFuel == 0 then [] else [selection.instructionFuel],sourceRoot := some selection.sourceRoot,sourcePrograms := selection.sourcePrograms,program := selection.runtimeProgram}
  match ModelIR.runReferenceSegment p handler.body machine with
  | .error error partialState => return {failed error committed (finishSegment (Protocol.preserveBurned (rollbackAllocations world partialState.world) partialState.world) highWater) partialState.remainingFuel with runtimeFuelRemaining := partialState.fuelCaps.head?,trace := partialState.trace}
  | .ok _ finalState =>
    let localValues := p.states.map (fun slot => (finalState.values.lookup slot.id).getD slot.initial)
    let values := committed.take selection.stateBase ++ localValues ++ committed.drop (selection.stateBase+p.states.length)
    let committedWorld := commitWorld context finalState.world
    let .ok next := committedWorld | return {failed (match committedWorld with | .error error => error | _ => "ReferenceCommit") committed (Protocol.preserveBurned (rollbackAllocations world finalState.world) finalState.world) finalState.remainingFuel with runtimeFuelRemaining := finalState.fuelCaps.head?,trace := finalState.trace}
    return {ok := true,returned := finalState.returned.getD [],committed := values,world := finishSegment next highWater,remainingFuel := finalState.remainingFuel,runtimeFuelRemaining := finalState.fuelCaps.head?,exit := finalState.exit,wait := finalState.wait,outcomeType := finalState.outcomeType,live := if finalState.exit == "suspended" then finalState.locals.map Prod.snd else [],trace := finalState.trace}

def runExecSegment (validated : ExecIR.ValidatedProject) (programId : Nat) (inputs committed : List Value) (world : State) (context : Context) (fuel : Nat) : Outcome := Id.run do
  let p := validated.project
  let some program := p.programs.find? (fun p => p.id == programId) | return failed "UnknownProgram" committed world fuel
  if program.context != context.kind then return failed "ExecutionContextMismatch" committed world fuel
  let initialSeed := seed world context programId
  let .ok world := initialSeed | return failed (match initialSeed with | .error error => error | _ => "ReferenceProcessSeed") committed world fuel
  let setup := do
    checkValues p.types program.inputTypes inputs
    checkValues p.types p.stateTypes committed
    if let some system := p.systemMetadata then
      if context.domain != system.runtimeDomain || !p.instances.any (fun inst => inst.id == context.instanceId && inst.handlers.any (fun handler => handler.programId == programId)) then throw "ExecutionOwnership"
    Dispatch.validate p.types context p.services
    reserveSegment world
  let .ok (prepared,highWater) := setup | return failed (match setup with | .error error => error | _ => "ReferenceSetup") committed world fuel
  let context := prepareContext context world
  let execution : ExecIR.ExecutionContext := {kind := context.kind,now := context.now,domain := context.domain,instanceId := context.instanceId,processIdentity := context.processIdentity,owner := some context.owner,schedulerFrontier := some (context.now,context.turn),referenceContext := some context}
  let machine : Exec.Machine := {txn := {state := committed,world := some prepared},remainingFuel := fuel}
  match Exec.run p execution program inputs machine with
  | .error error partialState => return {failed error committed (finishSegment (Protocol.preserveBurned (rollbackAllocations world (partialState.txn.world.getD world)) (partialState.txn.world.getD world)) highWater) partialState.remainingFuel with runtimeFuelRemaining := partialState.fuelCaps.head?,trace := partialState.trace}
  | .ok values finalState =>
    let draft := finalState.txn.world.getD prepared
    let committedWorld := commitWorld context draft
    let .ok next := committedWorld | return {failed (match committedWorld with | .error error => error | _ => "ReferenceCommit") committed (Protocol.preserveBurned (rollbackAllocations world draft) draft) finalState.remainingFuel with runtimeFuelRemaining := finalState.fuelCaps.head?,trace := finalState.trace}
    return {ok := true,returned := values,committed := ExecIR.commit finalState.txn,world := finishSegment next highWater,remainingFuel := finalState.remainingFuel,runtimeFuelRemaining := finalState.fuelCaps.head?,exit := finalState.exit,wait := finalState.wait,resumeBlock := finalState.resumeBlock,outcomeType := finalState.outcomeType,live := finalState.live,trace := finalState.trace}
private def lifecycleEnabled (context : Context) : Bool :=
  context.environment.lookup "structured.runChild" == some (.bool true)

private def finishChild (parent : Outcome) (child : Except String Structured.ChildLifecycle) : Outcome :=
  match child with
  | .error error => {parent with ok := false,error := "ChildLifecycle:" ++ error,exit := "failed"}
  | .ok child => {parent with ok := child.ok,error := child.error,exit := if child.ok then parent.exit else "failed",world := child.world,committed := child.committed,trace := parent.trace ++ child.trace}

def runModel (validated : ModelIR.ValidatedProject) (handlerId : Nat) (inputs committed : List Value) (world : State) (context : Context) (fuel : Nat) : Outcome := Id.run do
  let parent := runModelSegment validated handlerId inputs committed world context fuel
  if !parent.ok || !lifecycleEnabled context then return parent
  let selection := resolveSource validated.project handlerId context
  let .ok selection := selection | return {parent with ok := false,error := "ChildSourceSelection",exit := "failed"}
  let execute := fun childContext target args childWorld => Id.run do
    let some localId := selection.sourcePrograms.findSome? (fun (localId,runtimeId) => if runtimeId == target then some localId else none)
      | return failed "UnknownChildSourceProgram" parent.committed childWorld 0
    let childSelection := resolveSource validated.project localId childContext
    let .ok childSelection := childSelection | return failed "ChildSourceSelection" parent.committed childWorld 0
    if childSelection.runtimeProgram != target || childSelection.instructionFuel == 0 then
      return failed "ChildProcessPolicy" parent.committed childWorld 0
    runModelSegment validated localId args parent.committed childWorld childContext parent.remainingFuel
  return finishChild parent (Structured.runScheduledChild validated.project.types context parent.world execute)

def runExec (validated : ExecIR.ValidatedProject) (programId : Nat) (inputs committed : List Value) (world : State) (context : Context) (fuel : Nat) : Outcome := Id.run do
  let parent := runExecSegment validated programId inputs committed world context fuel
  if !parent.ok || !lifecycleEnabled context then return parent
  let execute := fun childContext target args childWorld => Id.run do
    let some program := validated.project.programs.find? (fun program => program.id == target)
      | return failed "UnknownChildProgram" parent.committed childWorld 0
    if program.instructionFuel == 0 || program.ownerPolicy != "caller" || program.resultLifetimePolicy != "until-release" then
      return failed "ChildProcessPolicy" parent.committed childWorld 0
    runExecSegment validated target args parent.committed childWorld childContext parent.remainingFuel
  return finishChild parent (Structured.runScheduledChild validated.project.types context parent.world execute)
structure Resumed (α : Type) where
  outcome : Outcome
  continuation : Option α := none

private def prepareResume (world : State) (context : Context) (event : Event) (savedWait : Option Value) (program : Nat) : Except String (Value × State × State) := do
  checkContext context
  validateState world
  let some process := context.processIdentity | throw "MissingProcessContext"
  let some (.handle wait) := savedWait | throw "MissingSavedWait"
  let [.bits 64 0x4c415452,.handle eventProcess,.handle eventWait,.bits 64 ordinal] := event.values | throw "ResumeEventShape"
  if event.kind != "runtime.resume" || event.stage != 5 || eventProcess != process || eventWait != wait then throw "ResumeEventIdentity"
  if event.time != context.now || event.turn != context.turn then throw "RuntimeEventReadyKey"
  if !world.events.contains event || event.instanceId != context.instanceId || event.identity.owner.toNat != context.owner then throw "RuntimeEventNotQueued"
  let data ← Runtime.getProcess world context process
  if data.program != program then throw "ProcessProgramMismatch"
  if data.ordinal != ordinal || data.status != 1 || data.wait != some wait then throw "StaleSuspensionToken"
  if event.cancelled then throw "CancelledResumeEvent"
  let delivered ← Runtime.deliver context world event
  let (outcome,claimed) ← Runtime.claimWait delivered context process wait
  pure (outcome,delivered,claimed)

/-- Resume an owned source machine captured by Source.run. Serialized inputs cannot
    provide a source continuation; the scheduler retains the typed machine itself. -/
def resumeModel (validated : ModelIR.ValidatedProject) (handlerId : Nat) (saved : Source.Machine)
    (committed : List Value) (world : State) (context : Context) (event : Event) (fuel : Nat) : Resumed Source.Machine := Id.run do
  let resolution := resolveSource validated.project handlerId context
  let .ok selection := resolution | return ⟨failed "ReferenceSourceSelection" committed world fuel,none⟩
  if saved.exit != "suspended" || saved.program != selection.runtimeProgram || saved.context.domain != context.domain || saved.context.owner != context.owner || saved.context.instanceId != context.instanceId || saved.context.processIdentity != context.processIdentity then
    return ⟨failed "SavedSourceFrameIdentity" committed world fuel,none⟩
  let checks := do
    checkValues selection.project.types selection.allStateTypes committed
    Dispatch.validate selection.project.types context (← sourceServices selection.project)
    prepareResume world context event saved.wait selection.runtimeProgram
  let .ok (value,delivered,claimed) := checks | return ⟨failed (match checks with | .error error => error | _ => "ResumeSetup") committed world fuel,none⟩
  let machine := {saved with
    values := (selection.project.states.zip (committed.drop selection.stateBase)).map (fun (slot,value) => (slot.id,value))
    context := prepareContext context delivered
    world := claimed
    remainingFuel := fuel
    fuelCaps := if selection.instructionFuel == 0 then [] else [selection.instructionFuel]
    runtimeSpent := 0
    trace := []}
  match Source.resume selection.project value machine with
  | .error error draft =>
    let rolled := Protocol.preserveBurned (rollbackAllocations delivered draft.world) draft.world
    return ⟨{failed error committed rolled draft.remainingFuel with runtimeFuelRemaining := draft.fuelCaps.head?,trace := draft.trace},none⟩
  | .ok _ next =>
    let localValues := selection.project.states.map (fun slot => (next.values.lookup slot.id).getD slot.initial)
    let values := committed.take selection.stateBase ++ localValues ++ committed.drop (selection.stateBase+selection.project.states.length)
    let commitResult := commitWorld context next.world
    let .ok finalWorld := commitResult | return ⟨{failed (match commitResult with | .error error => error | _ => "ReferenceCommit") committed (Protocol.preserveBurned (rollbackAllocations delivered next.world) next.world) next.remainingFuel with runtimeFuelRemaining := next.fuelCaps.head?,trace := next.trace},none⟩
    let next := {next with world := finalWorld}
    let outcome : Outcome := {ok := true,returned := next.returned.getD [],committed := values,world := finalWorld,remainingFuel := next.remainingFuel,runtimeFuelRemaining := next.fuelCaps.head?,exit := next.exit,wait := next.wait,outcomeType := next.outcomeType,live := if next.exit == "suspended" then next.locals.map Prod.snd else [],trace := next.trace}
    return ⟨outcome,if next.exit == "suspended" then some next else none⟩

/-- Resume an owned CFG machine only after the actual queued token is consumed. -/
def resumeExec (validated : ExecIR.ValidatedProject) (programId : Nat) (saved : Exec.Machine)
    (committed : List Value) (world : State) (context : Context) (event : Event) (fuel : Nat) : Resumed Exec.Machine := Id.run do
  let p := validated.project
  let some program := p.programs.find? (fun p => p.id == programId) | return ⟨failed "UnknownProgram" committed world fuel,none⟩
  if saved.exit != "suspended" || program.context != 1 || context.kind != 1 then return ⟨failed "SavedExecFrameIdentity" committed world fuel,none⟩
  let checks := do
    checkValues p.types p.stateTypes committed
    Dispatch.validate p.types context p.services
    prepareResume world context event saved.wait programId
  let .ok (value,delivered,claimed) := checks | return ⟨failed (match checks with | .error error => error | _ => "ResumeSetup") committed world fuel,none⟩
  let context := prepareContext context delivered
  let execution : ExecIR.ExecutionContext := {kind := context.kind,now := context.now,domain := context.domain,instanceId := context.instanceId,processIdentity := context.processIdentity,owner := some context.owner,schedulerFrontier := some (context.now,context.turn),referenceContext := some context}
  let machine := {saved with txn := {state := committed,world := some claimed},remainingFuel := fuel,trace := []}
  match Exec.resume p execution program value machine with
  | .error error draft =>
    let world := draft.txn.world.getD claimed
    return ⟨{failed error committed (Protocol.preserveBurned (rollbackAllocations delivered world) world) draft.remainingFuel with runtimeFuelRemaining := draft.fuelCaps.head?,trace := draft.trace},none⟩
  | .ok values next =>
    let draftWorld := next.txn.world.getD claimed
    let commitResult := commitWorld context draftWorld
    let .ok finalWorld := commitResult | return ⟨{failed (match commitResult with | .error error => error | _ => "ReferenceCommit") committed (Protocol.preserveBurned (rollbackAllocations delivered draftWorld) draftWorld) next.remainingFuel with runtimeFuelRemaining := next.fuelCaps.head?,trace := next.trace},none⟩
    let next := {next with txn := {next.txn with world := some finalWorld}}
    let outcome : Outcome := {ok := true,returned := values,committed := ExecIR.commit next.txn,world := finalWorld,remainingFuel := next.remainingFuel,runtimeFuelRemaining := next.fuelCaps.head?,exit := next.exit,wait := next.wait,resumeBlock := next.resumeBlock,outcomeType := next.outcomeType,live := next.live,trace := next.trace}
    return ⟨outcome,if next.exit == "suspended" then some next else none⟩
end LeanAT.Reference


