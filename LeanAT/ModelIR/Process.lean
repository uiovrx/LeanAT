import LeanAT.ModelIR.Semantics
import LeanAT.ModelIR.Layout
namespace LeanAT.ModelIR.Process
inductive Work where
  | statement (stmt : Stmt) | loop (remaining : Nat) (body : List Stmt)
  deriving Repr
structure WaitRecord where
  identity : HandleIdentity
  deadline : Nat
  deriving Repr
inductive Status where
  | running | suspended (wait : WaitRecord) (binder : Nat) (typeId : TypeId)
  | done (values : List Value) | failed (error : String)
  deriving Repr
structure Frame where
  processIdentity : HandleIdentity
  handlerId : Nat
  localTypes : List (Nat × TypeId) := []
  committed : RuntimeState
  working : RuntimeState
  work : List Work
  status : Status := .running
  registered : Option WaitRecord := none
  nextWaitGeneration : Nat := 1
  time : Nat := 0
  turn : Nat := 0
  segmentFuel : Nat
  deriving Repr
structure RunResult where
  frame : Frame
  reason : ModelIR.StopReason
  deriving Repr
private def putLocal (s : RuntimeState) (id : Nat) (value : Value) : RuntimeState :=
  {s with locals := (id,value)::s.locals.filter (fun x => x.1 != id)}
private def fail (frame : Frame) (error : String) : Frame :=
  {frame with working := frame.committed, registered := none, status := .failed error}
/-- Starts one already allocated logical process identity. The caller represents the scheduler/store. -/
def start (p : ValidatedProject) (handlerId : Nat) (processIdentity : HandleIdentity) (args : List Value) (time : Nat := 0) : Except String Frame := do
  let some h := p.project.handlers.find? (fun h => h.id == handlerId) | throw "UnknownProcess"
  if h.context != .process || processIdentity.kind != .process then throw "ExpectedProcessIdentity"
  if time ≥ 2^64 then throw "TimeOverflow"
  if h.parameters.length != args.length then throw "ProcessArgumentArity"
  for (param,value) in h.parameters.zip args do
    let some ty := p.project.types[param.typeId]? | throw "UnknownParameterType"
    if !conforms p.project.types (p.project.types.length+1) ty value then throw "ProcessArgumentType"
  let runtime : RuntimeState := {values := p.project.states.map (fun s => (s.id,s.initial)),locals := (h.parameters.zip args).map (fun (b,v) => (b.id,v))}
  pure {processIdentity,handlerId,localTypes := h.parameters.map (fun b => (b.id,b.typeId)),committed := runtime,working := runtime,work := h.body.map Work.statement,time,segmentFuel := p.project.profile.instructionFuel}
private def contextHash : List UInt8 := [243,98,119,232,11,86,30,127,47,204,190,94,31,222,10,169,182,89,145,83,240,145,48,189,31,28,150,241,33,51,160,218]
private def timerHash : List UInt8 := [120,61,218,156,158,167,189,34,238,43,43,7,82,48,225,16,33,150,50,251,70,56,134,174,209,87,149,131,210,174,163,111]
private def putType (types : List (Nat × TypeId)) (id : Nat) (typeId : TypeId) :=
  (id,typeId)::types.filter (fun x => x.1 != id)
private def runService (p : Project) (frame : Frame) (id : Nat) (args : List Value) : Except String (Frame × Value) := do
  let some service := p.services.find? (fun s => s.id == id) | throw "UnknownService"
  if service.opcode == "getContextField" && service.providerKey == "leanat.core.context.process" && service.providerVersion == "1" && args.isEmpty then
    if service.abiHash != contextHash then throw "ProviderABIMismatch"
    return (frame,.handle frame.processIdentity)
  if service.opcode != "registerWait" || service.providerKey != "leanat.core.wait.timer" || service.providerVersion != "1" then throw "UnsupportedRuntimeServiceReference"
  if service.abiHash != timerHash then throw "ProviderABIMismatch"
  let [.handle owner,.bits 64 kind,.bits 64 value] := args | throw "TimerWaitArgumentType"
  if owner != frame.processIdentity then throw "InvalidProcessOwner"
  if frame.registered.isSome then throw "ProcessAlreadyHasRegisteredWait"
  if frame.nextWaitGeneration ≥ 2^64 then throw "WaitGenerationExhausted"
  let deadline ← if kind == 4 then pure (max frame.time value) else if kind == 3 then
    if frame.time + value < 2^64 then pure (frame.time+value) else throw "TimeOverflow"
    else throw "UnsupportedWaitKind"
  let identity : HandleIdentity := ⟨.wait,owner.domain,owner.store,owner.slot,UInt64.ofNat frame.nextWaitGeneration,owner.owner⟩
  let record := WaitRecord.mk identity deadline
  pure ({frame with registered := some record,nextWaitGeneration := frame.nextWaitGeneration+1},.handle identity)
private def execute (p : Project) (frame : Frame) (stmt : Stmt) : Except String Frame := do
  let ev := fun expr => evalExpr frame.working p.profile.instructionFuel expr p
  match stmt with
  | .readNow binder => return {frame with working := putLocal frame.working binder.id (.bits 64 frame.time),localTypes := putType frame.localTypes binder.id binder.typeId}
  | .letVal id e => return {frame with working := putLocal frame.working id (← ev e),localTypes := putType frame.localTypes id e.typeId}
  | .writeState id e =>
    let value ← ev e
    return {frame with working := {frame.working with values := (id,value)::frame.working.values.filter (fun x => x.1 != id)}}
  | .emit tag args =>
    let values ← args.mapM ev
    return {frame with working := {frame.working with trace := frame.working.trace ++ [⟨tag,values⟩]}}
  | .check condition error =>
    let .bool condition ← ev condition | throw "ExpectedBool"
    if condition then pure frame else throw error
  | .fail error => throw error
  | .transportReturn _ => throw "TransportReturnForbiddenInProcess"
  | .ret args =>
    if frame.registered.isSome then throw "UnconsumedWaitRegistration"
    let values ← args.mapM ev
    return {frame with committed := frame.working,status := .done values}
  | .serviceCall destination id args =>
    let (next,value) ← runService p frame id (← args.mapM ev)
    match destination with
    | none => throw "TimerServiceRequiresDestination"
    | some binder =>
      let some schema := p.types[binder.typeId]? | throw "UnknownServiceResultType"
      if !conforms p.types (p.types.length+1) schema value then throw "ServiceResultTypeMismatch"
      return {next with working := putLocal next.working binder.id value,localTypes := putType next.localTypes binder.id binder.typeId}
  | .await handle binder typeId =>
    let .handle identity ← ev handle | throw "ExpectedWaitIdentity"
    let some record := frame.registered | throw "UnknownOrConsumedWait"
    if identity != record.identity then throw "StaleSuspensionToken"
    if p.types[typeId]? != some .unit then throw "UnsupportedTimerOutcomeLayout"
    let some handler := p.handlers.find? (fun h => h.id == frame.handlerId) | throw "UnknownProcess"
    let mut bytes := 0
    for (_,type) in frame.localTypes do
      let layout ← (deriveLayout type p.types {maxBytes := handler.frameBytesLimit}).mapError (fun _ => "FrameCapacityExceeded")
      bytes := bytes + layout.size
    if bytes > handler.frameBytesLimit then throw "FrameCapacityExceeded"
    return {frame with committed := frame.working,registered := none,status := .suspended record binder typeId}
  | .branch c yes no =>
    let .bool condition ← ev c | throw "ExpectedBool"
    return {frame with work := (if condition then yes else no).map Work.statement ++ frame.work}
  | .repeat count body => return {frame with work := .loop count body :: frame.work}
  | .unsupported feature => throw ("UnsupportedFeature: " ++ feature)

/-- Exact timer reference runner; semantic segment fuel is not replenished by host-budget continuation. -/
def runFrom (p : ValidatedProject) (horizon : Option Nat) : Nat → Frame → RunResult
  | fuel, frame =>
    match frame.status with
    | .done _ => ⟨frame,.quiescent⟩
    | .failed error => ⟨frame,.failed error⟩
    | .running =>
      match frame.work with
      | [] => if frame.registered.isSome then ⟨fail frame "UnconsumedWaitRegistration",.failed "UnconsumedWaitRegistration"⟩ else ⟨{frame with committed := frame.working,status := .done []},.quiescent⟩
      | work::rest =>
        match fuel with
        | 0 => ⟨frame,.runBudgetReached⟩
        | fuel+1 =>
          let extra := match work with
            | .statement (.serviceCall _ id _) => (p.project.services.find? (fun s => s.id == id)).map ServiceIR.extraFuel |>.getD 0
            | _ => 0
          if frame.segmentFuel < 1+extra then
            let next := fail frame "FuelExhausted"
            ⟨next,.failed "FuelExhausted"⟩
          else
            let frame := {frame with work := rest,segmentFuel := frame.segmentFuel-(1+extra)}
            match work with
            | .loop 0 _ => runFrom p horizon fuel frame
            | .loop (n+1) body => runFrom p horizon fuel {frame with work := body.map Work.statement ++ [.loop n body] ++ rest}
            | .statement stmt =>
              match execute p.project frame stmt with
              | .error error => let next := fail frame error; ⟨next,.failed error⟩
              | .ok next => runFrom p horizon fuel next
    | .suspended wait binder typeId =>
      if horizon.any (fun h => wait.deadline > h) then ⟨frame,.horizonReached⟩ else
      match fuel with
      | 0 => ⟨frame,.runBudgetReached⟩
      | fuel+1 =>
        let turn := if wait.deadline == frame.time then frame.turn+1 else 0
        if turn ≥ p.project.profile.maxEventsPerTick then
          let next := fail frame "ZenoDetected"
          ⟨next,.failed "ZenoDetected"⟩
        else
          let runtime := putLocal frame.committed binder .unit
          runFrom p horizon fuel {frame with working := runtime,committed := runtime,status := .running,localTypes := putType frame.localTypes binder typeId,time := wait.deadline,turn,segmentFuel := p.project.profile.instructionFuel}
end LeanAT.ModelIR.Process





