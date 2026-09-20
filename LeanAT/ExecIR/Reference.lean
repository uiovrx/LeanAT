import LeanAT.ExecIR.Validation
import LeanAT.Pure.Numeric
import LeanAT.Reference.Dispatch

namespace LeanAT.ExecIR

structure ReferenceWait where
  identity : HandleIdentity
  deadline : Nat
  deriving Repr, BEq
structure ReferenceState where
  nextWaitGeneration : Nat := 1
  registered : Option ReferenceWait := none
  deriving Repr, BEq
structure ReferenceOutput where
  time : Nat
  turn : Nat
  instanceId : Nat
  port : Nat
  value : Value
  deriving Repr, BEq
structure EventTxn where
  state : List Value
  writes : List (Nat × Value) := []
  trace : List (String × List Value) := []
  reference : ReferenceState := {}
  outputs : List ReferenceOutput := []
  outputCapacity : Nat := 1024
  preparedTransport : Option Value := none
  world : Option LeanAT.Reference.State := none
  deriving Repr, BEq
inductive SegmentExit where
  | returned
  | transport (value : Value)
  | suspended (wait : Value) (resumeBlock : Nat) (live : List Value) (outcomeType : Nat)
  deriving Repr, BEq
structure SegmentResult where
  txn : EventTxn
  returned : List Value
  remainingFuel : Nat
  exit : SegmentExit := .returned
  deriving Repr, BEq

structure ExecutionContext where
  kind : Nat := 0
  now : Nat := 0
  domain : Nat := 0
  instanceId : Nat := 0
  processIdentity : Option HandleIdentity := none
  owner : Option Nat := none
  schedulerFrontier : Option (Nat × Nat) := none
  referenceContext : Option LeanAT.Reference.Context := none
  deriving Repr, BEq

private def getReg (regs : List (Nat × Value)) (r : VReg) : Except String Value :=
  match regs.lookup r.id with | some v => .ok v | none => .error "UndefinedRegister"
private def setReg (regs : List (Nat × Value)) (r : VReg) (v : Value) :=
  (r.id,v) :: regs.filter (fun x => x.1 != r.id)
private def indexValue (xs : List Value) (n : Nat) : Except String Value :=
  match xs[n]? with | some v => .ok v | none => .error "IndexOutOfRange"

private def transportTypeABI (p : ExecProject) (id : Nat) : Bool := Id.run do
  let some (.record [sync,phase,delay,response]) := p.types[id]? | return false
  if p.types[sync]? != some (.bits 64) || p.types[delay]? != some (.bits 64) then return false
  let some (.variant [[],[phaseValue]]) := p.types[phase]? | return false
  if p.types[phaseValue]? != some (.bits 64) then return false
  let some (.variant [[],[responseValue]]) := p.types[response]? | return false
  let some (.record [status,bytes,dmi]) := p.types[responseValue]? | return false
  return p.types[status]? == some (.bits 64) && p.types[dmi]? == some .bool && (p.types[bytes]?).any (fun type => match type with | .bytes _ => true | _ => false)
/-- The bounded reference registry is independent of the path that later executes. -/
def validateReferenceRegistry (p : ExecProject) : Except String Unit := do
  for service in p.services do
    let (key,hash) : String × ByteArray ← match service.op with
      | .getContextField => pure ("leanat.core.context.process",⟨#[243,98,119,232,11,86,30,127,47,204,190,94,31,222,10,169,182,89,145,83,240,145,48,189,31,28,150,241,33,51,160,218]⟩)
      | .registerWait => pure ("leanat.core.wait.timer",⟨#[120,61,218,156,158,167,189,34,238,43,43,7,82,48,225,16,33,150,50,251,70,56,134,174,209,87,149,131,210,174,163,111]⟩)
      | .bufferOutputWrite => pure ("leanat.core.output.write",⟨#[135,208,103,20,205,88,68,8,219,186,14,149,59,145,146,140,17,134,126,101,113,63,113,10,66,86,190,6,50,190,189,188]⟩)
      | .setTransportReturn => pure ("leanat.core.transport.return",⟨#[235,74,146,70,59,248,252,92,17,24,27,73,43,138,211,127,145,103,27,143,112,59,220,83,112,96,78,244,154,206,15,254]⟩)
      | _ => throw "UnsupportedReferenceProvider"
    if service.providerKey != key || service.providerVersion != "1" || service.abiHash != hash then throw "ReferenceProviderABIMismatch"
    if service.extraFuel != 1 || service.effectMask != requiredEffect service.op then throw "ReferenceProviderSignatureMismatch"
    if service.contextMask == 0 || Nat.land service.contextMask (allowedContexts service.op) != service.contextMask then throw "ReferenceProviderContextMismatch"
    let inputs := service.inputTypes.map (fun id => p.types[id]?)
    let outputs := service.resultTypes.map (fun id => p.types[id]?)
    let valid := match service.op with
      | .getContextField => inputs.isEmpty && outputs == [some (.handle .process)]
      | .registerWait => inputs == [some (.handle .process),some (.bits 64),some (.bits 64)] && outputs == [some (.handle .wait)]
      | .bufferOutputWrite => outputs.isEmpty && match inputs with
        | [some (.bits 64),some .bool] | [some (.bits 64),some (.bits _)] => true
        | _ => false
      | .setTransportReturn => outputs.isEmpty && match service.inputTypes with
        | [id] => transportTypeABI p id
        | _ => false
      | _ => false
    if !valid then throw "ReferenceProviderTypeABI"
private def referenceService (p : ExecProject) (context : ExecutionContext) (i : Instruction) (args : List Value) (txn : EventTxn) : Except String (Option Value × EventTxn) := do
  let some service := p.services.find? (fun s => s.id == i.immediate) | throw "UnknownReferenceService"
  if let some world := txn.world then
    let some ctx := context.referenceContext | throw "MissingReferenceContext"
    let (values,world) ← LeanAT.Reference.Dispatch.invoke p.types ctx service args world
    if values.length > 1 then throw "ReferenceServiceResultArity"
    let preparedTransport := if i.op == .setTransportReturn then args.head? else txn.preparedTransport
    return (values.head?,{txn with world := some world,preparedTransport})
  if service.providerVersion != "1" then throw "ReferenceProviderVersionMismatch"
  if service.op != i.op || service.extraFuel != 1 || service.effectMask != requiredEffect i.op then throw "ReferenceProviderSignatureMismatch"
  if service.contextMask == 0 || Nat.land service.contextMask (allowedContexts i.op) != service.contextMask || Nat.land service.contextMask (2^context.kind) == 0 then throw "ReferenceProviderContextMismatch"
  if args.length != service.inputTypes.length then throw "ReferenceProviderArity"
  for (typeId,value) in service.inputTypes.zip args do
    if !(p.types[typeId]?).any (fun type => conforms p.types (p.types.length+1) type value) then throw "ReferenceProviderInputType"
  let inputTypes := service.inputTypes.map (fun id => p.types[id]?)
  let resultTypes := service.resultTypes.map (fun id => p.types[id]?)
  if i.op == .bufferOutputWrite then
    if service.providerKey != "leanat.core.output.write" || service.abiHash != ⟨#[135,208,103,20,205,88,68,8,219,186,14,149,59,145,146,140,17,134,126,101,113,63,113,10,66,86,190,6,50,190,189,188]⟩ then throw "ReferenceProviderABIMismatch"
    let [.bits 64 port,value] := args | throw "OutputArgumentType"
    if inputTypes.head? != some (some (.bits 64)) || !resultTypes.isEmpty then throw "ReferenceProviderTypeABI"
    if !(inputTypes[1]?).any (fun type => match type with | some .bool | some (.bits _) => true | _ => false) then throw "ReferenceProviderTypeABI"
    if port ≥ 2^32 then throw "OutputPortRange"
    if txn.outputs.length ≥ txn.outputCapacity then throw "OutputCapacity"
    if context.schedulerFrontier.any (fun frontier => context.now < frontier.1) then throw "OutputTimeRegression"
    let turn := match context.schedulerFrontier with | some (time,turn) => if time == context.now then turn+1 else 0 | none => 0
    if turn ≥ 2^64 then throw "OutputTurnOverflow"
    return (none,{txn with outputs := txn.outputs ++ [⟨context.now,turn,context.instanceId,port,value⟩]})
  if i.op == .setTransportReturn then
    if service.providerKey != "leanat.core.transport.return" || service.abiHash != ⟨#[235,74,146,70,59,248,252,92,17,24,27,73,43,138,211,127,145,103,27,143,112,59,220,83,112,96,78,244,154,206,15,254]⟩ then throw "ReferenceProviderABIMismatch"
    if context.kind != 2 || !resultTypes.isEmpty then throw "ReferenceTransportContext"
    let [typeId] := service.inputTypes | throw "ReferenceProviderTypeABI"
    if !transportTypeABI p typeId then throw "ReferenceProviderTypeABI"
    let [value] := args | throw "TransportArgumentArity"
    let .record [.bits 64 sync,phase,.bits 64 _,response] := value | throw "TransportReturnRecord"
    if sync > 2 then throw "TransportSync"
    match phase with
    | .variant 0 [] => pure ()
    | .variant 1 [.bits 64 phase] => if phase ≥ 2^32 then throw "TransportPhase"
    | _ => throw "TransportPhase"
    match response with
    | .variant 0 [] => pure ()
    | .variant 1 [.record [.bits 64 status,.bytes _,.bool _]] => if status > 6 then throw "TransportResponseStatus"
    | _ => throw "TransportResponse"
    return (none,{txn with preparedTransport := some value})
  let contextHash : ByteArray := ⟨#[243,98,119,232,11,86,30,127,47,204,190,94,31,222,10,169,182,89,145,83,240,145,48,189,31,28,150,241,33,51,160,218]⟩
  let timerHash : ByteArray := ⟨#[120,61,218,156,158,167,189,34,238,43,43,7,82,48,225,16,33,150,50,251,70,56,134,174,209,87,149,131,210,174,163,111]⟩
  let some process := context.processIdentity | throw "MissingProcessContext"
  if context.kind != 1 || process.kind != .process || process.domain.toNat != context.domain || context.owner.any (fun owner => owner != process.owner.toNat) then throw "ReferenceProcessContextMismatch"
  if i.op == .getContextField && service.providerKey == "leanat.core.context.process" && args.isEmpty then
    if service.abiHash != contextHash then throw "ReferenceProviderABIMismatch"
    if !inputTypes.isEmpty || resultTypes != [some (.handle .process)] then throw "ReferenceProviderTypeABI"
    return (some (.handle process),txn)
  if i.op != .registerWait || service.providerKey != "leanat.core.wait.timer" then throw "UnsupportedReferenceService"
  if service.abiHash != timerHash then throw "ReferenceProviderABIMismatch"
  if inputTypes != [some (.handle .process),some (.bits 64),some (.bits 64)] || resultTypes != [some (.handle .wait)] then throw "ReferenceProviderTypeABI"
  let [.handle owner,.bits 64 kind,.bits 64 tick] := args | throw "TimerArgumentType"
  if owner != process then throw "TimerOwnerMismatch"
  if txn.reference.registered.isSome then throw "WaitAlreadyRegistered"
  if txn.reference.nextWaitGeneration ≥ 2^64 then throw "WaitGenerationExhausted"
  let deadline ← if kind == 4 then pure (max context.now tick) else if kind == 3 then do
      if context.now+tick ≥ 2^64 then throw "TimeOverflow"
      pure (context.now+tick)
    else throw "UnsupportedTimerKind"
  let identity : HandleIdentity := ⟨.wait,process.domain,process.store,process.slot,UInt64.ofNat txn.reference.nextWaitGeneration,process.owner⟩
  pure (some (.handle identity),{txn with reference := {registered := some ⟨identity,deadline⟩,nextWaitGeneration := txn.reference.nextWaitGeneration+1}})

private def execute (p : ExecProject) (context : ExecutionContext) (i : Instruction) (args : List Value) (txn : EventTxn) : Except String (Option Value × EventTxn) := do
  let value ← match i.op, args with
  | .const, [] => pure i.value
  | .move, [v] => pure v
  | .binary, [a,b] => match Pure.evalBinary i.operator a b with | .ok v => pure v | .error e => throw (reprStr e)
  | .unary, [a] => Numeric.evalUnary i.immediate a
  | .compare, [a,b] => Numeric.evalCompare i.immediate a b
  | .convert, [a] => do
    let some r := i.dest | throw "MissingConvertDestination"
    let some (.bits width) := p.types[r.typeId]? | throw "InvalidConvertType"
    Numeric.evalConvert i.immediate width a
  | .makeRecord, vs => pure (.record vs)
  | .getField, [.record vs] => indexValue vs i.immediate
  | .makeVariant, vs => pure (.variant i.immediate vs)
  | .variantTag, [.variant tag _] => pure (.bits 64 tag)
  | .variantGet, [.variant tag vs] => do
      if tag != i.immediate / 65536 then throw "VariantTagMismatch"
      indexValue vs (i.immediate % 65536)
  | .makeVec, vs => pure (.vec vs)
  | .vecGet, [.vec vs,.bits _ n] => indexValue vs n
  | .vecSet, [.vec vs,.bits _ n,v] => do
      if n ≥ vs.length then throw "IndexOutOfRange"
      pure (.vec (vs.set n v))
  | .selectValue, [.bool c,a,b] => pure (if c then a else b)
  | .loadState, [] => match txn.writes.lookup i.immediate with | some v => pure v | none => indexValue txn.state i.immediate
  | .getNow, [] => pure (.bits 64 context.now)
  | .bufferStateWrite, [v] => return (none, {txn with writes := (i.immediate,v)::txn.writes.filter (fun x => x.1 != i.immediate)})
  | .check, [.bool c] => if c then return (none,txn) else throw i.text
  | .trace, values => return (none, {txn with trace := txn.trace ++ [(i.text,values)]})
  | op, _ => if op.tag ≥ 21 then return ← referenceService p context i args txn else throw "InvalidDynamicOperands"
  pure (some value,txn)

/-- One independently decoded ExecIR instruction; enclosing evaluator owns shared fuel. -/
def evalReferenceInstruction (p : ExecProject) (context : ExecutionContext) (i : Instruction) (args : List Value) (txn : EventTxn) : Except String (Option Value × EventTxn) :=
  execute p context i args txn

def evalReferenceInstructionAttempt (p : ExecProject) (context : ExecutionContext) (i : Instruction) (args : List Value) (txn : EventTxn) : EStateM.Result String EventTxn (Option Value) := Id.run do
  if i.op.tag ≥ 21 then
    if let some world := txn.world then
      let some service := p.services.find? (fun service => service.id == i.immediate) | return .error "UnknownReferenceService" txn
      let some ctx := context.referenceContext | return .error "MissingReferenceContext" txn
      match LeanAT.Reference.Dispatch.attempt p.types ctx service args world with
      | .error error draft => return .error error {txn with world := some draft}
      | .ok values next =>
        let txn := {txn with world := some next}
        if values.length > 1 then return .error "ReferenceServiceResultArity" txn
        let preparedTransport := if i.op == .setTransportReturn then args.head? else txn.preparedTransport
        return .ok values.head? {txn with preparedTransport}
  if i.op == .trace then
    if let some world := txn.world then
      let some ctx := context.referenceContext | return .error "MissingReferenceContext" txn
      match LeanAT.Reference.observe world ⟨"trace",i.text,i.source,ctx.now,ctx.turn,args⟩ with
      | .error error => return .error error txn
      | .ok next => return .ok none {txn with world := some next,trace := txn.trace ++ [(i.text,args)]}
  match execute p context i args txn with
  | .ok (value,next) => return .ok value next
  | .error error => return .error error txn
private def bindInputs (program : Program) (values : List Value) : Except String (List (Nat × Value)) := do
  let some entry := program.blocks.find? (fun b => b.id == program.entry) | throw "MissingPureEntry"
  if entry.parameters.length != values.length then throw "PureInputArity"
  pure ((entry.parameters.zip values).map (fun (r,v) => (r.id,v)))

/-- Pure calls execute their delivered CFG and share the caller's remaining fuel. -/
private def evalPureProgram (p : ExecProject) (context : ExecutionContext) :
    Nat → Program → Nat → Nat → List (Nat × Value) → Except String (List Value × Nat)
  | 0, _, _, _, _ => throw "FuelExhausted"
  | fuel+1, program, blockId, offset, regs => do
    let some block := program.blocks.find? (fun b => b.id == blockId) | throw "MissingPureBlock"
    if let some i := block.instructions[offset]? then
      let args ← i.args.mapM (getReg regs)
      let (values,left) ← if i.op == .callPure then do
          let some callee := p.programs.find? (fun f => f.id == i.immediate) | throw "MissingPureProgram"
          evalPureProgram p context fuel callee callee.entry 0 (← bindInputs callee args)
        else do
          let (value,_) ← execute p context i args {state := []}
          pure (value.toList,fuel)
      let regs ← match i.dest,values with
        | some r,[v] => pure (setReg regs r v)
        | none,[] => pure regs
        | _,_ => throw "PureResultArity"
      evalPureProgram p context (min left fuel) program blockId (offset+1) regs
    else
      let edge ← match block.terminator with
        | .ret values => return (← values.mapM (getReg regs),fuel)
        | .fail error => throw error
        | .jump edge => pure edge
        | .branch c yes no => do
          let .bool b ← getReg regs c | throw "ExpectedBool"
          pure (if b then yes else no)
        | .switch r cases default => do
          let .bits _ tag ← getReg regs r | throw "ExpectedTag"
          pure ((cases.lookup tag).getD default)
        | _ => throw "ImpureTerminator"
      let some target := program.blocks.find? (fun b => b.id == edge.target) | throw "MissingPureTarget"
      let values ← edge.args.mapM (getReg regs)
      let regs := (target.parameters.zip values).foldl (fun rs (r,v) => setReg rs r v) regs
      evalPureProgram p context fuel program edge.target 0 regs

private def evalInstructions (p : ExecProject) (context : ExecutionContext) : Nat → List Instruction → List (Nat × Value) → EventTxn → Except String (Nat × List (Nat × Value) × EventTxn)
  | fuel, [], regs, txn => .ok (fuel,regs,txn)
  | 0, _::_, _, _ => .error "FuelExhausted"
  | fuel+1, i::rest, regs, txn => do
      let extra := if i.op.tag ≥ 21 then ((p.services.find? (fun s => s.id == i.immediate)).map ServiceSignature.extraFuel).getD 0 else 0
      if fuel < extra then throw "FuelExhausted"
      let available := fuel-extra
      let args ← i.args.mapM (getReg regs)
      let (v,txn,left) ← if i.op == .callPure then do
          let some callee := p.programs.find? (fun f => f.id == i.immediate) | throw "MissingPureProgram"
          let (values,left) ← evalPureProgram p context available callee callee.entry 0 (← bindInputs callee args)
          if values.length > 1 then throw "PureResultArity"
          pure (values.head?,txn,left)
        else do
          let (v,txn) ← execute p context i args txn
          pure (v,txn,available)
      let regs ← match i.dest,v with
      | some r,some v => do
        let some t := p.types[r.typeId]? | throw "MissingType"
        if !conforms p.types (p.types.length+1) t v then throw "ResultTypeMismatch"
        pure (setReg regs r v)
      | none,none => pure regs
      | _,_ => throw "ResultArityMismatch"
      evalInstructions p context (min left fuel) rest regs txn

private def runBlocks (p : ExecProject) (program : Program) (context : ExecutionContext) : Nat → Nat → List (Nat × Value) → EventTxn → Except String SegmentResult
  | 0, _, _, _ => .error "FuelExhausted"
  | fuel+1, blockId, regs, txn => do
    let some block := program.blocks.find? (fun b => b.id == blockId) | throw "MissingBlock"
    let (remaining,regs,txn) ← evalInstructions p context (fuel+1) block.instructions regs txn
    if remaining == 0 then throw "FuelExhausted"
    let left := remaining-1
    let edge ← match block.terminator with
    | .ret vs => return ⟨txn, ← vs.mapM (getReg regs), left,.returned⟩
    | .transportReturn r =>
      let v ← getReg regs r
      if txn.preparedTransport != some v then throw "UnpreparedTransportReturn"
      return ⟨txn,[v],left,.transport v⟩
    | .suspend wait resume live =>
      let some target := program.blocks.find? (fun b => b.id == resume) | throw "MissingResume"
      let some outcome := target.parameters.head? | throw "MissingWaitOutcome"
      return ⟨txn,[],left,.suspended (← getReg regs wait) resume (← live.mapM (getReg regs)) outcome.typeId⟩
    | .fail error => throw error
    | .jump e => pure e
    | .branch c y n => do
      let .bool b ← getReg regs c | throw "ExpectedBool"
      pure (if b then y else n)
    | .switch r cases fallback => do
      let .bits _ tag ← getReg regs r | throw "ExpectedTag"
      pure ((cases.lookup tag).getD fallback)
    let some target := program.blocks.find? (fun b => b.id == edge.target) | throw "MissingTarget"
    let values ← edge.args.mapM (getReg regs)
    let nextRegs := (target.parameters.zip values).foldl (fun rs (r,v) => setReg rs r v) regs
    -- Recursion is fuel bounded; execution never replenishes the segment budget.
    runBlocks p program context (min left fuel) edge.target nextRegs txn

def evalExecSegment (validated : ValidatedProject) (programId : Nat) (inputs : List Value)
    (txn : EventTxn) (fuel : Nat) (context : ExecutionContext := {}) : Except String SegmentResult := do
  let p := validated.project
  match context.referenceContext with
  | some ctx => LeanAT.Reference.Dispatch.validate p.types ctx p.services
  | none => validateReferenceRegistry p
  let some program := p.programs.find? (fun p => p.id == programId) | throw "UnknownProgram"
  if context.now ≥ 2^64 || (p.schemaMajor ≥ 2 && context.kind != program.context) then throw "ExecutionContextMismatch"
  if let some system := p.systemMetadata then
    if context.domain != system.runtimeDomain then throw "ExecutionDomainMismatch"
    let some owner := p.instances.find? (fun inst => inst.handlers.any (fun h => h.programId == programId)) | throw "ExecutionProgramOwnerMissing"
    if context.instanceId != owner.id then throw "ExecutionInstanceMismatch"
  let some entry := program.blocks.find? (fun b => b.id == program.entry) | throw "MissingEntry"
  if inputs.length != program.inputTypes.length then throw "InputArity"
  for (t,v) in program.inputTypes.zip inputs do
    if !(p.types[t]?).any (fun s => conforms p.types (p.types.length+1) s v) then throw "InputType"
  if txn.state.length != p.stateTypes.length then throw "StateArity"
  for (t,v) in p.stateTypes.zip txn.state do
    if !(p.types[t]?).any (fun s => conforms p.types (p.types.length+1) s v) then throw "StateType"
  for (id,v) in txn.writes do
    if !((p.stateTypes[id]?).bind (fun t => p.types[t]?)).any (fun s => conforms p.types (p.types.length+1) s v) then throw "WriteType"
  let capped := if program.instructionFuel == 0 then fuel else min fuel program.instructionFuel
  let result ← runBlocks p program context capped program.entry ((entry.parameters.zip inputs).map (fun (r,v) => (r.id,v))) txn
  pure {result with remainingFuel := fuel-capped+result.remainingFuel}

def commit (txn : EventTxn) : List Value :=
  txn.state.zipIdx |>.map (fun (v,id) => (txn.writes.lookup id).getD v)

/-- Data-level continuation reference. The scheduler must authenticate and claim its token first. -/
def evalPreparedResume (validated : ValidatedProject) (programId : Nat) (prepared : SegmentResult)
    (outcome : Value) (txn : EventTxn) (fuel : Nat) (context : ExecutionContext) : Except String SegmentResult := do
  let p := validated.project
  match context.referenceContext with
  | some ctx => LeanAT.Reference.Dispatch.validate p.types ctx p.services
  | none => validateReferenceRegistry p
  let some program := p.programs.find? (fun p => p.id == programId) | throw "UnknownProgram"
  if context.kind != 1 || program.context != 1 || context.now ≥ 2^64 then throw "ResumeContext"
  if let some system := p.systemMetadata then
    if context.domain != system.runtimeDomain || !p.instances.any (fun inst => inst.id == context.instanceId && inst.handlers.any (fun h => h.programId == programId)) then throw "ResumeOwnership"
  let .suspended _ blockId live outcomeType := prepared.exit | throw "NotSuspended"
  if !(p.types[outcomeType]?).any (fun t => conforms p.types 64 t outcome) then throw "WaitOutcomeType"
  let some block := program.blocks.find? (fun b => b.id == blockId) | throw "MissingResume"
  let values := outcome::live
  if block.parameters.length != values.length then throw "ResumeArity"
  for (r,v) in block.parameters.zip values do
    if !(p.types[r.typeId]?).any (fun t => conforms p.types 64 t v) then throw "ResumeValueType"
  let capped := if program.instructionFuel == 0 then fuel else min fuel program.instructionFuel
  let result ← runBlocks p program context capped blockId ((block.parameters.zip values).map (fun (r,v) => (r.id,v))) txn
  pure {result with remainingFuel := fuel-capped+result.remainingFuel}

end LeanAT.ExecIR




