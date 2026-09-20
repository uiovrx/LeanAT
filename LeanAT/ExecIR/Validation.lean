import LeanAT.ExecIR.Schema
import LeanAT.ModelIR.Validation
import LeanAT.ExecIR.Metadata

namespace LeanAT.ExecIR

private def require (b : Bool) (error : String) : Except String Unit :=
  if b then .ok () else .error error

private def typeOf (p : ExecProject) (r : VReg) : Option TypeSchema := p.types[r.typeId]?
private def hasType (p : ExecProject) (r : VReg) (s : TypeSchema) : Bool := typeOf p r == some s
private def unique (xs : List Nat) : Bool := xs.eraseDups.length == xs.length
private def destType (i : Instruction) : Option Nat := i.dest.map VReg.typeId

def requiredEffect : Op → Nat
  | .loadState => 1 | .bufferStateWrite => 2 | .bufferOutputWrite => 4
  | .payloadGet | .bufferPayloadWrite | .extensionGet | .bufferExtensionWrite | .newTransaction
  | .stagePhase | .ackResponse | .cancelLocal | .scheduleEvent | .cancelEvent | .setTransportReturn => 8
  | .registerWait | .readWaitResult | .waitGroupNew | .waitArm | .waitResultGet | .waitGroupRelease => 16
  | .spawnProcess | .trySpawnProcess | .submitTask | .cancelTask | .requestTaskSlot | .scopeNew
  | .scopeTransfer | .scopeCancel | .scopeClose => 32
  | .resultGet | .resultRelease | .taskResultGet | .taskResultRelease => 64
  | .objectCall | .debugTransfer => 128
  | .requestManaged | .beginManagedRead | .beginManagedWrite | .invalidateManaged | .releaseLease => 256
  | .callExternPure => 512
  | .grantRawDmi | .denyDmi | .invalidateRawDmi => 1024
  | _ => 0

def allowedContexts (op : Op) : Nat := match op with
  | .getNow | .payloadGet | .bufferPayloadWrite | .extensionGet | .bufferExtensionWrite => 7
  | .registerWait | .readWaitResult | .waitGroupNew | .waitArm | .waitResultGet | .waitGroupRelease | .requestTaskSlot => 2
  | .setTransportReturn => 4 | .debugTransfer => 8 | .grantRawDmi | .denyDmi => 16
  | _ => if op.tag < 12 || op == .check || op == .trace || (op.tag ≥ 16 && op.tag ≤ 19) then 31 else 3

/-- Iterative memoized layout validation: shared DAG nodes are sized once, with bounded work. -/
def typeLayouts (types : TypeEnvironment) (maxDepth : Nat := 64) (maxWork : Nat := 1000000) : Except String (List Nat) := do
  let mut cache : Array (Option (Nat × Nat)) := Array.replicate types.length none
  let mut work := 0
  let mut doneCount := 0
  for _ in [:types.length+1] do
    if doneCount == types.length then break
    let mut progress := false
    for (t,id) in types.zipIdx do
      if cache[id]!.isSome then continue
      let children := match t with
        | .record fs => fs | .variant cs => cs.flatten
        | .vec t _ | .boundedVec t _ => [t] | _ => []
      work := work+1+children.length
      require (work ≤ maxWork && children.all (· < types.length)) "TypeGraphBudgetOrReference"
      require (match t with | .bits n => n > 0 && n ≤ 64 | .fin n => n > 0 && n ≤ 2^64 | .variant cs => !cs.isEmpty | _ => true) "InvalidTypeBounds"
      if children.any (fun c => cache[c]!.isNone) then continue
      let sizeOf := fun c => (cache[c]!.getD (0,0)).1
      let height := 1 + (children.map (fun c => (cache[c]!.getD (0,0)).2)).foldl max 0
      let size := match t with
        | .record fs => 1+(fs.map sizeOf).foldl (· + ·) 0
        | .variant cs => (cs.map (fun fs => 3+(fs.map sizeOf).foldl (· + ·) 0)).foldl max 3
        | .vec t n | .boundedVec t n => max 1 ((1+sizeOf t)*n)
        | .bytes n => 1+n
        | _ => 1
      require (height ≤ maxDepth && size ≤ 1048576) "TypeDepthOrExpandedSizeLimit"
      cache := cache.set! id (some (size,height))
      doneCount := doneCount+1
      progress := true
    require progress "CyclicInlineType"
  require (doneCount == types.length) "TypeGraphBudget"
  pure (cache.toList.map (fun item => (item.getD (0,0)).1))

def validateInstruction (p : ExecProject) (i : Instruction) : Except String Unit := do
  require (i.immediate < 2^32 && i.text.toUTF8.size ≤ 65536 && i.source.toUTF8.size ≤ 65536) "EncodingLimit"
  require (i.op == .const || i.value == .unit) "NonCanonicalUnusedLiteral"
  require (i.args.all (fun r => r.typeId < p.types.length)) "OperandTypeMissing"
  require (i.dest.all (fun r => r.typeId < p.types.length)) "DestinationTypeMissing"
  let result := fun t => destType i == some t
  let noResult := i.dest.isNone
  let valid ← match i.op, i.args with
  | .const, [] => pure (i.dest.any (fun r => (typeOf p r).any (fun s => conforms p.types (p.types.length+1) s i.value)))
  | .move, [a] => pure (result a.typeId)
  | .binary, [a,b] => pure (a.typeId == b.typeId && match i.operator with
      | .eq => i.dest.any (fun r => hasType p r .bool)
      | .and | .or => hasType p a .bool && result a.typeId
      | .lt => (typeOf p a).any (fun s => match s with | .bits _ => true | .fin _ => p.schemaMajor ≥ 5 | _ => false) && i.dest.any (fun r => hasType p r .bool)
      | _ => (typeOf p a).any (fun s => match s with | .bits _ => true | _ => false) && result a.typeId)
  | .unary, [a] => pure (p.schemaMajor ≥ 2 && result a.typeId && (if i.immediate == 0 then hasType p a .bool else i.immediate ≤ 2 && (typeOf p a).any (fun t => match t with | .bits _ => true | _ => false)))
  | .compare, [a,b] => pure (p.schemaMajor ≥ 2 && a.typeId == b.typeId && i.dest.any (fun r => hasType p r .bool) && i.immediate ≤ 5 && (i.immediate ≤ 1 || (typeOf p a).any (fun t => match t with | .bits _ => true | _ => false)))
  | .convert, [a] => pure (p.schemaMajor ≥ 2 && i.dest.any (fun r => match typeOf p a,typeOf p r with
      | some (.bits inputWidth),some (.bits outputWidth) => if i.immediate == 0 then inputWidth ≤ outputWidth else i.immediate ≤ 2 && inputWidth ≥ outputWidth
      | _,_ => false))
  | .makeRecord, args => pure (i.dest.any (fun d => typeOf p d == some (.record (args.map VReg.typeId))))
  | .getField, [a] => pure (match typeOf p a with | some (.record ts) => (ts[i.immediate]?).any result | _ => false)
  | .makeVariant, args => pure (i.dest.any (fun d => match typeOf p d with | some (.variant cs) => cs[i.immediate]? == some (args.map VReg.typeId) | _ => false))
  | .variantTag, [a] => pure ((typeOf p a).any (fun s => match s with | .variant _ => true | _ => false) && i.dest.any (fun d => hasType p d (.bits 64)))
  | .variantGet, [a] => pure (match typeOf p a with | some (.variant cs) => (cs[i.immediate / 65536]?).any (fun ts => (ts[i.immediate % 65536]?).any result) | _ => false)
  | .makeVec, args => pure (i.dest.any (fun d => match typeOf p d with | some (.vec t n) => args.length == n && args.all (fun a => a.typeId == t) | some (.boundedVec t n) => args.length ≤ n && args.all (fun a => a.typeId == t) | _ => false))
  | .vecGet, [a,n] => pure ((hasType p n (.bits 64) || (p.schemaMajor ≥ 5 && (typeOf p n).any (fun t => match t with | .bits _ | .fin _ => true | _ => false))) && match typeOf p a with | some (.vec t _) | some (.boundedVec t _) => result t | _ => false)
  | .vecSet, [a,n,v] => pure ((hasType p n (.bits 64) || (p.schemaMajor ≥ 5 && (typeOf p n).any (fun t => match t with | .bits _ | .fin _ => true | _ => false))) && result a.typeId && match typeOf p a with | some (.vec t _) | some (.boundedVec t _) => v.typeId == t | _ => false)
  | .selectValue, [c,a,b] => pure (hasType p c .bool && a.typeId == b.typeId && result a.typeId)
  | .loadState, [] => pure ((p.stateTypes[i.immediate]?).any result)
  | .bufferStateWrite, [v] => pure (noResult && p.stateTypes[i.immediate]? == some v.typeId)
  | .check, [c] => pure (noResult && hasType p c .bool)
  | .trace, _ => pure noResult
  | .getNow, [] => pure (p.schemaMajor ≥ 2 && i.dest.any (fun d => hasType p d (.bits 64)))
  | .callPure, args => pure (p.schemaMajor ≥ 2 && p.programs.any (fun f => f.id == i.immediate && f.inputTypes == args.map VReg.typeId && f.resultTypes == i.dest.toList.map VReg.typeId))
  | op, args => pure (p.schemaMajor ≥ 2 && op.tag ≥ 21 && p.services.any (fun s => s.id == i.immediate && s.op == op && s.inputTypes == args.map VReg.typeId && s.resultTypes == i.dest.toList.map VReg.typeId))
  require valid "InvalidOpcodeSignature"
  require (i.op.tag < 46 || p.profile == "AT-Ext-1.1-draft") "ExtOpcodeRequiresExtProfile"

private def termArgs : Terminator → List VReg
  | .jump e => e.args
  | .branch c y n => c :: (y.args ++ n.args)
  | .switch v cs d => v :: ((cs.flatMap (fun c => c.2.args)) ++ d.args)
  | .ret vs => vs
  | .fail _ => []
  | .transportReturn r => [r]
  | .suspend wait _ live => wait::live
private def edges : Terminator → List Edge
  | .jump e => [e]
  | .branch _ y n => [y,n]
  | .switch _ cs d => cs.map Prod.snd ++ [d]
  | _ => []

private def dominators (program : Program) : List (Nat × List Nat) := Id.run do
  let ids := program.blocks.map Block.id
  let roots := program.entry :: program.blocks.filterMap (fun b => match b.terminator with | .suspend _ resume _ => some resume | _ => none)
  let mut dom := ids.map (fun id => (id, if roots.contains id then [id] else ids))
  for _ in [:ids.length + 1] do
    dom := dom.map fun (id, _) =>
      if roots.contains id then (id,[id]) else
      let predecessors := program.blocks.filter (fun b => (edges b.terminator).any (fun e => e.target == id))
      let common := ids.filter (fun d => predecessors.all (fun b => ((dom.lookup b.id).getD []).contains d))
      (id, id :: common.filter (· != id))
  return dom

def validateProgram (p : ExecProject) (program : Program) (layouts : List Nat := []) : Except String Unit := do
  require (program.instructionFuel < 2^64) "ProcessFuelEncodingLimit"
  if program.instructionFuel == 0 then
    require (program.ownerPolicy.isEmpty && program.resultLifetimePolicy.isEmpty) "UnexpectedProcessPolicy"
  else
    require (p.schemaMajor ≥ 5 && program.context == 1 && program.ownerPolicy == "caller" && program.resultLifetimePolicy == "until-release") "UnsupportedProcessPolicy"
  require (program.context < 5 && program.effectMask < 2048) "ProgramContextEffects"
  if p.schemaMajor == 1 then
    require (program.context == 0 && program.frame.isEmpty && program.frameBytes == 0 && program.effectMask == 0) "V1ProgramMetadata"
  require (program.frameBytes < 2^32 && program.frame.length ≤ 65536) "FrameLimit"
  if p.schemaMajor ≥ 3 then require (unique (program.frame.map FrameSlot.registerId)) "DuplicateFrameRegister"
  if p.schemaMajor < 3 then require (program.frame.all (fun s => s.registerId == 0)) "UnexpectedFrameRegisterMetadata"
  let mut frameEnd := 0
  for slot in program.frame do
    require (slot.typeId < p.types.length && slot.offset < 2^32 && [1,2,4,8,16,32,64].contains slot.alignment && slot.offset % slot.alignment == 0 && slot.offset ≥ frameEnd) "FrameLayout"
    let some width := layouts[slot.typeId]? | throw "MissingFrameTypeLayout"
    frameEnd := slot.offset+64*width
    require (frameEnd ≤ program.frameBytes) "FrameCapacity"
  require (!program.blocks.isEmpty && program.blocks.length ≤ 100000) "BlockLimit"
  require (unique (program.blocks.map Block.id)) "DuplicateBlock"
  let some entry := program.blocks.find? (fun b => b.id == program.entry) | throw "MissingEntry"
  require (entry.parameters.map VReg.typeId == program.inputTypes) "InputLayout"
  let definitions := program.blocks.flatMap (fun b => (b.parameters ++ b.instructions.filterMap Instruction.dest).map (fun r => (r.id,(r.typeId,b.id))))
  require (definitions.length ≤ 65536 && definitions.all (fun (id,(t,_)) => id < 65536 && t < p.types.length)) "RegisterEncodingLimit"
  if p.schemaMajor ≥ 3 then require (program.frame.all (fun s => definitions.any (fun (id,(t,_)) => id == s.registerId && t == s.typeId))) "FrameDefinitionMapping"
  require (program.id < 2^32 && program.blocks.all (fun b => b.id < 2^32 && b.instructions.length ≤ 65536) && program.source.toUTF8.size ≤ 65536) "ProgramEncodingLimit"
  require (unique (definitions.map Prod.fst)) "DuplicateRegister"
  let dom := dominators program
  let mut reached := [program.entry]
  for _ in [:program.blocks.length] do
    let next := (program.blocks.filter (fun (b : Block) => reached.contains b.id)).flatMap (fun (b : Block) =>
      (edges b.terminator).map Edge.target ++ (match b.terminator with | .suspend _ resume _ => [resume] | _ => []))
    reached := (reached ++ next).eraseDups
  require (program.blocks.all (fun b => reached.contains b.id)) "UnreachableBlock"
  require ((program.inputTypes ++ program.resultTypes).all (· < p.types.length)) "ProgramTypeReference"
  for b in program.blocks do
    let mut available := b.parameters
    let checkUse := fun (available : List VReg) (r : VReg) =>
      match definitions.lookup r.id with
      | none => false
      | some (t,owner) => t == r.typeId && (if owner == b.id then available.contains r else ((dom.lookup b.id).getD []).contains owner)
    for i in b.instructions do
      if p.schemaMajor ≥ 2 then
        require (Nat.land (allowedContexts i.op) (2^program.context) != 0 && Nat.land program.effectMask (requiredEffect i.op) == requiredEffect i.op) "OpcodeContextEffects"
        if i.op.tag ≥ 21 then
          let some signature := p.services.find? (fun s => s.id == i.immediate) | throw "MissingService"
          require (Nat.land signature.contextMask (2^program.context) != 0 && Nat.land program.effectMask signature.effectMask == signature.effectMask) "ServiceContextEffects"
      require (i.args.all (checkUse available)) s!"RegisterDominance program={program.id} block={b.id} source={i.source}"
      validateInstruction p i
      available := available ++ i.dest.toList
    require ((termArgs b.terminator).all (checkUse available)) "TerminatorRegisterDominance"
    for edge in edges b.terminator do
      let some target := program.blocks.find? (fun target => target.id == edge.target) | throw "MissingTarget"
      require (edge.args.map VReg.typeId == target.parameters.map VReg.typeId) "EdgeLayout"
    match b.terminator with
    | .branch c _ _ => require (hasType p c .bool) "BranchConditionType"
    | .switch v cases _ =>
      require (hasType p v (.bits 64)) "SwitchType"
      require (cases.all (fun c => c.1 < 2^64)) "SwitchEncodingLimit"
      require (unique (cases.map Prod.fst)) "DuplicateSwitchCase"
    | .ret vs => require (program.context != 2 && vs.map VReg.typeId == program.resultTypes) "ReturnLayout"
    | .fail error => require (error.toUTF8.size ≤ 65536) "ErrorStringLimit"
    | .transportReturn r =>
      require (p.schemaMajor ≥ 2 && program.context == 2 && program.resultTypes == [r.typeId]) "TransportReturnContext"
      require (b.instructions.getLast?.any (fun i => i.op == .setTransportReturn && i.args == [r])) "MissingPreparedTransportReturn"
    | .suspend wait resume live =>
      require (p.schemaMajor ≥ 2 && program.context == 1 && Nat.land program.effectMask 16 != 0 && hasType p wait (.handle .wait) && unique (live.map VReg.id)) "SuspendContextOrLive"
      let some target := program.blocks.find? (fun b => b.id == resume) | throw "MissingResume"
      require (!target.parameters.isEmpty && (target.parameters.drop 1).map VReg.typeId == live.map VReg.typeId) "ResumeLayout"
      if p.schemaMajor == 2 then require (program.frame.map FrameSlot.typeId == live.map VReg.typeId) "FrameLiveLayout"
      else require (live.all (fun r => program.frame.any (fun slot => slot.registerId == r.id && slot.typeId == r.typeId))) "FrameRegisterMapping"
    | _ => pure ()

private def validatePureCalls (p : ExecProject) : Except String Unit := do
  let instructions := p.programs.flatMap (fun program => program.blocks.flatMap Block.instructions)
  let called := (instructions.filter (fun i => i.op == .callPure)).map Instruction.immediate |>.eraseDups
  let mut heights : List (Nat × Nat) := []
  let mut work := 0
  for _ in [:called.length+1] do
    if heights.length == called.length then break
    let mut progress := false
    for id in called do
      if (heights.lookup id).isSome then continue
      let some f := p.programs.find? (fun f => f.id == id) | throw "UnknownPureProgram"
      require (f.frame.isEmpty && f.effectMask == 0 && f.context == 0 && f.resultTypes.length ≤ 1) "PureProgramMetadata"
      let body := f.blocks.flatMap Block.instructions
      work := work+body.length+1
      require (work ≤ 1000000) "PureValidationWork"
      require (body.all (fun i => i.op.tag < 12 || (i.op.tag ≥ 16 && i.op.tag ≤ 19))) "ImpureReferenceBody"
      require (f.blocks.all (fun b => match b.terminator with | .suspend .. | .transportReturn .. => false | _ => true)) "PureTerminator"
      let children := (body.filter (fun i => i.op == .callPure)).map Instruction.immediate
      if children.any (fun c => (heights.lookup c).isNone) then continue
      let height := 1+(children.map (fun c => (heights.lookup c).getD 0)).foldl max 0
      require (height ≤ 64) "PureCallDepth"
      heights := (id,height)::heights
      progress := true
    require progress "RecursivePureCall"

def validateRaw (p : ExecProject) : Except String Unit := do
  require (p.schemaMajor ≥ 1 && p.schemaMajor ≤ 5 && (p.profile == "AT-Core-1.1-draft" || (p.schemaMajor ≥ 2 && p.profile == "AT-Ext-1.1-draft"))) "UnsupportedSchemaOrProfile"
  require (p.types.length ≤ 10000 && p.programs.length ≤ 10000) "TableLimit"
  if p.schemaMajor == 1 then
    require (p.types.all (fun t => match t with | .bytes _ | .handle _ => false | _ => true) && p.services.isEmpty) "UnsupportedTypeEncoding: requires descriptor v2"
  require (p.services.length ≤ 65536 && unique (p.services.map ServiceSignature.id)) "ServiceTableLimit"
  for service in p.services do
    require (service.id < 2^32 && service.op.tag ≥ 21 && service.resultTypes.length ≤ 1 && (service.inputTypes ++ service.resultTypes).all (· < p.types.length)) "ServiceSignature"
    require (service.contextMask > 0 && Nat.land service.contextMask (allowedContexts service.op) == service.contextMask && service.effectMask < 2048 && Nat.land service.effectMask (requiredEffect service.op) == requiredEffect service.op) "ServiceEffectFloor"
    require (!service.providerKey.isEmpty && !service.providerVersion.isEmpty && service.providerKey.toUTF8.size ≤ 65536 && service.providerVersion.toUTF8.size ≤ 65536 && service.abiHash.size == 32 && service.extraFuel < 2^64) "ServiceBindingMetadata"
  require (p.types.all (fun t => match t with | .fin n => n < 2^64 || (p.schemaMajor ≥ 5 && n == 2^64) | .vec _ n | .boundedVec _ n => n ≤ 65536 | .record fs => fs.length ≤ 65536 | .variant cs => cs.length ≤ 65536 && cs.all (fun fs => fs.length ≤ 65536) | _ => true)) "TypeEncodingLimit"
  let layouts ← typeLayouts p.types
  require (p.stateTypes.length == p.initialState.length) "StateLayout"
  for (t,v) in p.stateTypes.zip p.initialState do
    require ((p.types[t]?).any (fun s => conforms p.types 64 s v)) "InvalidInitialValue"
  require (unique (p.programs.map Program.id)) "DuplicateProgram"
  validatePureCalls p
  for program in p.programs do validateProgram p program layouts
  validateProjectMetadata p

structure ValidatedProject where
  project : ExecProject
  valid : validateRaw project = .ok ()

def validateExec (p : ExecProject) : Except String ValidatedProject :=
  match h : validateRaw p with
  | .ok () => .ok ⟨p,h⟩
  | .error e => .error e

end LeanAT.ExecIR
