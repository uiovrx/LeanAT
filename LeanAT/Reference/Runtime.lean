import LeanAT.Reference.ABI
import LeanAT.Reference.Storage
namespace LeanAT.Reference.Runtime
structure ProcessData where
  program : Nat
  status : Nat := 0
  wait : Option HandleIdentity := none
  result : Option Value := none
  scope : Option HandleIdentity := none
  ordinal : Nat := 0
  instanceId : Nat := 0
  deriving Repr, BEq
structure WaitData where
  process : HandleIdentity
  kind : Nat
  deadline : Nat
  source : Option HandleIdentity := none
  readyKey : Option (Nat × Nat) := none
  outcome : Option Value := none
  claimed : Bool := false
  deriving Repr, BEq
private def encHandle (h : Option HandleIdentity) : Value := match h with | none => .variant 0 [] | some h => .variant 1 [.handle h]
private def decHandle : Value → Except String (Option HandleIdentity)
  | .variant 0 [] => pure none | .variant 1 [.handle h] => pure (some h) | _ => throw "OptionalHandleCodec"
private def encValue (v : Option Value) : Value := match v with | none => .variant 0 [] | some v => .variant 1 [v]
private def decValue : Value → Except String (Option Value)
  | .variant 0 [] => pure none | .variant 1 [v] => pure (some v) | _ => throw "OptionalValueCodec"
def ProcessData.encode (p : ProcessData) : Value := .record [.bits 64 p.program,.bits 64 p.status,encHandle p.wait,encValue p.result,encHandle p.scope,.bits 64 p.ordinal,.bits 64 p.instanceId]
def ProcessData.decode : Value → Except String ProcessData
  | .record [.bits 64 program,.bits 64 status,wait,result,scope,.bits 64 ordinal,.bits 64 instanceId] => do
    if program ≥ 2^32 || status > 3 || ordinal ≥ 2^64 || instanceId ≥ 2^32 then throw "ProcessRecordRange"
    pure ⟨program,status,← decHandle wait,← decValue result,← decHandle scope,ordinal,instanceId⟩
  | _ => throw "ProcessRecordCodec"
def WaitData.encode (w : WaitData) : Value := .record [.handle w.process,.bits 64 w.kind,.bits 64 w.deadline,encHandle w.source,
  (match w.readyKey with | none => .variant 0 [] | some (time,turn) => .variant 1 [.record [.bits 64 time,.bits 64 turn]]),encValue w.outcome,.bool w.claimed]
def WaitData.decode : Value → Except String WaitData
  | .record [.handle process,.bits 64 kind,.bits 64 deadline,source,key,outcome,.bool claimed] => do
    if process.kind != .process || kind ≥ 2^64 || deadline ≥ 2^64 then throw "WaitRecordRange"
    let readyKey ← match key with
      | .variant 0 [] => pure none
      | .variant 1 [.record [.bits 64 time,.bits 64 turn]] => if time < 2^64 && turn < 2^64 then pure (some (time,turn)) else throw "WaitReadyKey"
      | _ => throw "WaitReadyKey"
    pure ⟨process,kind,deadline,← decHandle source,readyKey,← decValue outcome,claimed⟩
  | _ => throw "WaitRecordCodec"
def getProcess (state : State) (ctx : Context) (handle : HandleIdentity) : Except String ProcessData := do
  if handle.kind != .process then throw "ProcessHandleKind"
  let data ← ProcessData.decode (← getObject state ctx handle "reference.process").value
  if data.instanceId != ctx.instanceId then throw "ProcessInstanceMismatch"
  pure data
def putProcess (state : State) (ctx : Context) (handle : HandleIdentity) (data : ProcessData) : Except String State := do
  let old ← getProcess state ctx handle
  if data.instanceId != old.instanceId then throw "ProcessInstanceMutation"
  putObject state ctx handle "reference.process" data.encode
def getWait (state : State) (ctx : Context) (handle : HandleIdentity) : Except String WaitData := do
  if handle.kind != .wait then throw "WaitHandleKind"
  WaitData.decode (← getObject state ctx handle "reference.wait").value
def putWait (state : State) (ctx : Context) (handle : HandleIdentity) (data : WaitData) : Except String State :=
  putObject state ctx handle "reference.wait" data.encode
def createProcess (state : State) (ctx : Context) (program : Nat) (scope : Option HandleIdentity := none) : Except String (HandleIdentity × State) :=
  allocate state ctx .process 10 "reference.process" (ProcessData.encode {program,scope,instanceId := ctx.instanceId})
/-- An explicitly supplied authoritative scheduler identity seeds the reference live registry. -/
def seedProcess (state : State) (ctx : Context) (identity : HandleIdentity) (program : Nat) : Except String State := do
  checkContext ctx
  if identity.kind != .process || identity.generation == 0 || identity.domain.toNat != ctx.domain || identity.owner.toNat != ctx.owner then throw "ProcessSeedIdentity"
  if state.objects.any (fun entry => entry.identity == identity) then throw "DuplicateProcessSeed"
  let next := {state with objects := state.objects ++ [⟨identity,"reference.process",(ProcessData.encode {program,instanceId := ctx.instanceId}),true⟩],nextGeneration := max state.nextGeneration (identity.generation.toNat+1)}
  checkCapacity next
  pure next

def newWait (state : State) (ctx : Context) (process : HandleIdentity) (kind deadline : Nat) (source : Option HandleIdentity) (readyKey : Option (Nat × Nat)) (outcome : Option Value) : Except String (HandleIdentity × State) := do
  let data ← getProcess state ctx process
  if data.status != 0 || data.wait.isSome then throw "ProcessAlreadyWaiting"
  if deadline ≥ 2^64 || readyKey.isSome != outcome.isSome then throw "InvalidWaitRegistration"
  let (handle,next) ← allocate state ctx .wait 11 "reference.wait" (WaitData.encode ⟨process,kind,deadline,source,readyKey,outcome,false⟩)
  let next ← putProcess next ctx process {data with wait := some handle}
  pure (handle,next)
def publishWait (state : State) (ctx : Context) (wait : HandleIdentity) (readyKey : Nat × Nat) (outcome : Value) : Except String State := do
  let record ← getWait state ctx wait
  if record.claimed || record.outcome.isSome then throw "WaitAlreadyPublished"
  let data ← getProcess state ctx record.process
  let next ← putWait state ctx wait {record with readyKey := some readyKey,outcome := some outcome}
  if data.status == 1 then
    let (_,next) ← enqueue next ctx (max ctx.now readyKey.1) 5 0 "runtime.resume" [.bits 64 0x4c415452,.handle record.process,.handle wait,.bits 64 data.ordinal]
    pure next
  else pure next

def suspendProcess (state : State) (ctx : Context) (process wait : HandleIdentity) : Except String State := do
  let data ← getProcess state ctx process
  let record ← getWait state ctx wait
  if data.wait != some wait || record.process != process || record.claimed || data.status != 0 then throw "InvalidAtomicSuspension"
  if data.ordinal + 1 ≥ 2^64 then throw "SuspensionOrdinalOverflow"
  let ordinal := data.ordinal + 1
  let next ← putProcess state ctx process {data with status := 1,ordinal}
  if record.outcome.isSome then
    let (_,next) ← enqueue next ctx (max ctx.now (record.readyKey.map Prod.fst |>.getD ctx.now)) 5 0 "runtime.resume" [.bits 64 0x4c415452,.handle process,.handle wait,.bits 64 ordinal]
    pure next
  else if record.kind == 3 || record.kind == 4 then
    let (_,next) ← enqueue next ctx (max ctx.now record.deadline) 5 0 "runtime.resume" [.bits 64 0x4c415452,.handle process,.handle wait,.bits 64 ordinal]
    pure next
  else pure next

def claimWait (state : State) (ctx : Context) (process wait : HandleIdentity) : Except String (Value × State) := do
  let data ← getProcess state ctx process
  let record ← getWait state ctx wait
  if data.status != 1 || data.wait != some wait || record.process != process || record.claimed then throw "StaleSuspensionToken"
  let some outcome := record.outcome | throw "WaitNotReady"
  let some ready := record.readyKey | throw "WaitNotReady"
  if ready.1 > ctx.now || (ready.1 == ctx.now && ready.2 > ctx.turn) then throw "PrematureWaitClaim"
  let next ← releaseObject state ctx wait "reference.wait"
  let next ← putProcess next ctx process {data with status := 0,wait := none}
  pure (outcome,next)
def createProcessAttempt (ctx : Context) (program : Nat) (scope : Option HandleIdentity := none) : Attempt HandleIdentity :=
  allocateAttempt ctx .process 10 "reference.process" (ProcessData.encode {program,scope,instanceId := ctx.instanceId})

def newWaitAttempt (ctx : Context) (process : HandleIdentity) (kind deadline : Nat) (source : Option HandleIdentity) (readyKey : Option (Nat × Nat)) (outcome : Option Value) : Attempt HandleIdentity := do
  let data ← fromExcept (getProcess (← get) ctx process)
  if data.status != 0 || data.wait.isSome then throw "ProcessAlreadyWaiting"
  if deadline ≥ 2^64 || readyKey.isSome != outcome.isSome then throw "InvalidWaitRegistration"
  let handle ← allocateAttempt ctx .wait 11 "reference.wait" (WaitData.encode ⟨process,kind,deadline,source,readyKey,outcome,false⟩)
  updateChecked (fun state => putProcess state ctx process {data with wait := some handle})
  pure handle

def publishWaitAttempt (ctx : Context) (wait : HandleIdentity) (readyKey : Nat × Nat) (outcome : Value) : Attempt Unit := do
  let record ← fromExcept (getWait (← get) ctx wait)
  if record.claimed || record.outcome.isSome then throw "WaitAlreadyPublished"
  let data ← fromExcept (getProcess (← get) ctx record.process)
  updateChecked (fun state => putWait state ctx wait {record with readyKey := some readyKey,outcome := some outcome})
  if data.status == 1 then
    let _ ← enqueueAttempt ctx (max ctx.now readyKey.1) 5 0 "runtime.resume" [.bits 64 0x4c415452,.handle record.process,.handle wait,.bits 64 data.ordinal]
    pure ()

def suspendProcessAttempt (ctx : Context) (process wait : HandleIdentity) : Attempt Unit := do
  let data ← fromExcept (getProcess (← get) ctx process)
  let record ← fromExcept (getWait (← get) ctx wait)
  if data.wait != some wait || record.process != process || record.claimed || data.status != 0 then throw "InvalidAtomicSuspension"
  if data.ordinal+1 ≥ 2^64 then throw "SuspensionOrdinalOverflow"
  let ordinal := data.ordinal+1
  updateChecked (fun state => putProcess state ctx process {data with status := 1,ordinal})
  if record.outcome.isSome || record.kind == 3 || record.kind == 4 then
    let time := if record.outcome.isSome then record.readyKey.map Prod.fst |>.getD ctx.now else record.deadline
    let _ ← enqueueAttempt ctx (max ctx.now time) 5 0 "runtime.resume" [.bits 64 0x4c415452,.handle process,.handle wait,.bits 64 ordinal]
    pure ()

def completeProcess (state : State) (ctx : Context) (process : HandleIdentity) (values : List Value) : Except String State := do
  let data ← getProcess state ctx process
  if data.wait.isSome || data.status != 0 then throw "RegisteredWaitWithoutSuspension"
  let next ← releaseObject state ctx process "reference.process"
  observe next ⟨"process.completed","return","",ctx.now,ctx.turn,[.handle process,.bits 64 data.program,.bits 64 data.ordinal,.record values]⟩
private def typeAt (types : TypeEnvironment) (id : Nat) := types[id]?
private def signatureIdentity (op : ExecIR.Op) (key : String) : Except String (String × String × Nat) := do
  match op with
  | .getContextField => pure ("leanat.core.context.process","f36277e80b561e7f2fccbe5e1fde0aa9b6599153f09130bd1f1c96f12133a0da",1)
  | .loadInput => pure ("leanat.core.input.read","e084d863eaf370dd7b8004942c1720fac8f02977fb9efa8d01ba85183012d30f",0)
  | .bufferOutputWrite => pure ("leanat.core.output.write","87d06714cd584408dbba0e953b91928c11867e65713f710a4256be0632bebdbc",1)
  | .registerWait =>
    if key == "leanat.core.wait.response" then pure (key,"7f0dfc014f1a8c07de8d55c2e40a52b85fc7d534be8df44f48d31c2962db1a04",1)
    else pure ("leanat.core.wait.timer","783dda9c9ea7bd22ee2b2b075230e110219632fb463886aed1579583d2aea36f",1)
  | .setTransportReturn => pure ("leanat.core.transport.return","eb4a92463bf8fc5c11181b492b8ad37f91671b8f703bdc5370604ef49ace0ffe",1)
  | .readWaitResult => pure ("leanat.core.wait.result","24ad58653d80da7b299d174f9f6f1c9fbb9f6b14159ca7846741b8214f981184",0)
  | .cancelLocal => pure ("leanat.core.transaction.cancel","fd95191dfcd83497a02c4edcca1089d64b98ba014a288bf1800bc12df81a6956",0)
  | .newTransaction | .stagePhase | .ackResponse => pure ("leanat.protocol",ABI.hex (ABI.sha256 ("leanat.protocol.v1:" ++ (ExecIR.opcodeNames[op.tag]!)).toUTF8),0)
  | _ => throw "UnsupportedRuntimeProvider"
def supports (s : ExecIR.ServiceSignature) : Bool :=
  [.getContextField,.loadInput,.bufferOutputWrite,.registerWait,.setTransportReturn,.readWaitResult].contains s.op
private def transportShape (types : TypeEnvironment) (id : Nat) : Bool := Id.run do
  let some (.record [sync,phase,delay,response]) := types[id]? | return false
  let some (.variant [[],[phaseType]]) := types[phase]? | return false
  let some (.variant [[],[responseType]]) := types[response]? | return false
  let some (.record [status,bytes,dmi]) := types[responseType]? | return false
  return [sync,delay,phaseType,status].all (fun id => types[id]? == some (.bits 64)) && types[dmi]? == some .bool && (types[bytes]?).any (fun type => match type with | .bytes _ => true | _ => false)
def validate : ProviderValidate := fun types s => do
  let (key,hash,cost) ← signatureIdentity s.op s.providerKey
  if s.providerKey != key || s.providerVersion != "1" || ABI.hex s.abiHash != hash || s.extraFuel != cost then throw "RuntimeProviderABI"
  let mask := if s.op == .setTransportReturn then 4 else if s.op == .registerWait || s.op == .readWaitResult then 2 else 3
  let effect := if s.op == .bufferOutputWrite then 4 else if s.op == .registerWait || s.op == .readWaitResult then 16 else if [.setTransportReturn,.cancelLocal,.newTransaction,.stagePhase,.ackResponse].contains s.op then 8 else 0
  if s.contextMask == 0 || Nat.land s.contextMask mask != s.contextMask || s.effectMask != effect then throw "RuntimeProviderEffects"
  let ins := s.inputTypes.map (typeAt types)
  let outs := s.resultTypes.map (typeAt types)
  let scalar := fun type => match type with | some .bool | some (.bits _) => true | _ => false
  let shape := match s.op with
    | .getContextField => ins.isEmpty && outs == [some (.handle .process)]
    | .loadInput => ins == [some (.bits 64)] && outs.length == 1 && outs.all scalar
    | .bufferOutputWrite => outs.isEmpty && match ins with | [some (.bits 64),value] => scalar value | _ => false
    | .registerWait => outs == [some (.handle .wait)] && (if key == "leanat.core.wait.response" then ins == [some (.handle .process),some (.handle .hop)] else ins == [some (.handle .process),some (.bits 64),some (.bits 64)])
    | .setTransportReturn => outs.isEmpty && s.inputTypes.length == 1 && s.inputTypes.all (transportShape types)
    | .readWaitResult => match ins,outs with | [some (.variant cases)],[some (.record fields)] => cases.head? == some fields | _,_ => false
    | .cancelLocal => ins == [some (.handle .transaction),some (.bits 64)] && outs == [some .unit]
    | .newTransaction => outs == [some (.handle .transaction)] && match ins with | [some (.bits 64),some (.bits 64),some (.bits 64),some (.bits 64),some (.bits 64),some (.bytes _)] => true | _ => false
    | .stagePhase => ins == [some (.handle .transaction),some (.bits 64),some (.bits 64),some (.bits 64)] && outs == [some .unit]
    | .ackResponse => ins == [some (.handle .transaction),some (.bits 64),some (.bits 64)] && outs == [some .unit]
    | _ => false
  if !shape then throw "RuntimeProviderTypeABI"

structure TransactionData where
  connection : Nat
  transport : Nat
  generation : Nat
  hop : HandleIdentity
  payload : HandleIdentity
  phase : Nat := 0
  acknowledged : Bool := false
  cancelled : Bool := false
  deriving Repr, BEq
def TransactionData.encode (t : TransactionData) : Value := .record [.bits 64 t.connection,.bits 64 t.transport,.bits 64 t.generation,.handle t.hop,.handle t.payload,.bits 64 t.phase,.bool t.acknowledged,.bool t.cancelled]
def TransactionData.decode : Value → Except String TransactionData
  | .record [.bits 64 connection,.bits 64 transport,.bits 64 generation,.handle hop,.handle payload,.bits 64 phase,.bool acknowledged,.bool cancelled] =>
    if connection < 2^32 && generation > 0 && phase ≤ 4 && hop.kind == .hop then pure ⟨connection,transport,generation,hop,payload,phase,acknowledged,cancelled⟩ else throw "TransactionRecordRange"
  | _ => throw "TransactionRecordCodec"
def getTransaction (state : State) (ctx : Context) (txn : HandleIdentity) : Except String TransactionData := do
  if txn.kind != .transaction then throw "TransactionKind"
  TransactionData.decode (← getObject state ctx txn "runtime.transaction").value
private def envNat (ctx : Context) (key : String) (fallback : Nat) : Except String Nat :=
  match ctx.environment.lookup key with | none => pure fallback | some (.bits 64 n) => if n < 2^64 then pure n else throw "RuntimeConfigRange" | _ => throw "RuntimeConfigType"
private def ownedPayload (state : State) (ctx : Context) (id : HandleIdentity) : Except String Storage.PayloadRecord := do
  Storage.PayloadRecord.decode (← getObject state ctx id Storage.payloadTag).value

def invoke : ProviderInvoke := fun types ctx service args state => do
  validate types service
  checkContext ctx
  if Nat.land service.contextMask (2^ctx.kind) == 0 then throw "RuntimeServiceContext"
  let args := if service.op == .ackResponse then match args with | [txn,connection,time] => [txn,connection,.bits 64 4,time] | _ => args else args
  let operation := if service.op == .ackResponse then ExecIR.Op.stagePhase else service.op
  match operation,args with
  | .getContextField,[] =>
    let some process := ctx.processIdentity | throw "MissingProcessContext"
    let _ ← getProcess state ctx process
    pure ([.handle process],state)
  | .loadInput,[.bits 64 port] =>
    let some (.vec ports) := ctx.environment.lookup "runtime.inputPorts" | throw "InputMetadataMissing"
    let some (.vec owners) := ctx.environment.lookup "runtime.owners" | throw "InputOwnerMetadataMissing"
    if !owners.contains (.record [.bits 64 ctx.instanceId,.bits 64 ctx.owner]) then throw "InputOwnerMismatch"
    let some typeId := service.resultTypes.head? | throw "InputResultType"
    if !ports.contains (.record [.bits 64 ctx.instanceId,.bits 64 port,.bits 64 typeId]) then throw "UnknownInputPort"
    let some (.vec values) := ctx.environment.lookup "runtime.inputs" | throw "InputNotReady"
    let some value := values.findSome? (fun row => match row with | .record [.bits 64 instanceId,.bits 64 id,value] => if instanceId == ctx.instanceId && id == port then some value else none | _ => none) | throw "InputNotReady"
    if !(types[typeId]?).any (fun type => conforms types (types.length+1) type value) then throw "InputValueType"
    pure ([value],state)
  | .bufferOutputWrite,[.bits 64 port,value] =>
    if port ≥ 2^32 then throw "OutputPortRange"
    let (_,next) ← enqueue state ctx ctx.now 6 0 "runtime.output" [.bits 64 0x4c41544f,.bits 64 port,value]
    pure ([],next)
  | .registerWait,[.handle process,.bits 64 kind,.bits 64 time] =>
    if ctx.processIdentity != some process then throw "TimerProcessContext"
    if kind != 3 && kind != 4 then throw "TimerKind"
    let deadline := if kind == 3 then ctx.now+time else time
    if deadline ≥ 2^64 then throw "TimeOverflow"
    let ready := if deadline ≤ ctx.now then some (ctx.now,ctx.turn) else none
    let (wait,next) ← newWait state ctx process kind deadline none ready (ready.map (fun _ => Value.unit))
    pure ([.handle wait],next)
  | .registerWait,[.handle process,.handle hop] =>
    if ctx.processIdentity != some process || hop.kind != .hop then throw "ResponseWaitContext"
    let _ ← getObject state ctx hop "runtime.hop"
    let latch := state.objects.find? (fun entry => entry.alive && entry.tag == "runtime.responseLatch" && (match entry.value with | .record [.handle source,_,_,_] => source == hop | _ => false))
    let ready := latch.bind (fun entry => match entry.value with | .record [.handle source,.bits 64 time,.bits 64 turn,value] => if source == hop then some ((time,turn),value) else none | _ => none)
    let (wait,next) ← newWait state ctx process 5 ctx.now (some hop) (ready.map Prod.fst) (ready.map Prod.snd)
    pure ([.handle wait],next)
  | .readWaitResult,[.variant 0 fields] => pure ([.record fields],state)
  | .readWaitResult,[.variant _ _] => throw "WaitOutcomeNotReady"
  | .setTransportReturn,[value] =>
    let .record [.bits 64 sync,phase,.bits 64 _,response] := value | throw "TransportReturnRecord"
    if sync > 2 then throw "TransportSync"
    match phase with | .variant 0 [] => pure () | .variant 1 [.bits 64 p] => if p ≥ 2^32 then throw "TransportPhase" | _ => throw "TransportPhase"
    match response with | .variant 0 [] => pure () | .variant 1 [.record [.bits 64 status,.bytes _,.bool _]] => if status > 6 then throw "TransportStatus" | _ => throw "TransportResponse"
    let next ← observe state ⟨"transport.prepared","setTransportReturn","",ctx.now,ctx.turn,[value]⟩
    pure ([],next)
  | .newTransaction,[.bits 64 connection,.bits 64 transport,.bits 64 generation,.bits 64 command,.bits 64 address,.bytes bytes] =>
    let capacity ← envNat ctx "runtime.maxTransactions" 16
    let maxPayload ← envNat ctx "runtime.maxPayloadBytes" 65536
    if connection ≥ 2^32 || generation == 0 || command > 2 || bytes.length > maxPayload then throw "TransactionOperands"
    if (state.objects.filter (fun entry => entry.alive && entry.tag == "runtime.transaction")).length ≥ capacity then throw "TransactionCapacity"
    let (txn,next) ← allocate state ctx .transaction 12 "runtime.transaction" .unit
    let (hop,next) ← allocate next ctx .hop 13 "runtime.hop" (.record [.handle txn,.bits 64 ctx.instanceId,.bits 64 connection,.bits 64 transport,.bits 64 generation,.bits 64 0])
    let (payload,next) ← Storage.createPayload ctx next {transaction := txn,hop,instanceId := ctx.instanceId,localSide := ctx.instanceId,target := false,writable := false,command,address,streamingWidth := max 1 bytes.length,baseline := bytes,data := bytes,maxBytes := maxPayload}
    let next ← putObject next ctx txn "runtime.transaction" (TransactionData.encode ⟨connection,transport,generation,hop,payload,0,false,false⟩)
    pure ([.handle txn],next)
  | .stagePhase,[.handle txn,.bits 64 connection,.bits 64 phase,.bits 64 time] =>
    let phase := if service.op == .ackResponse then 4 else phase
    let record ← getTransaction state ctx txn
    if record.connection != connection || phase < 1 || phase > 4 || time < ctx.now then throw "PhaseOperands"
    if record.cancelled && phase == 1 then throw "CancelledTransaction"
    if phase == 1 && state.events.any (fun event => event.kind == "runtime.send" && event.connection == connection && event.values[1]? == some (.bits 64 1)) then throw "RequestGateBusy"
    if phase == 4 && (record.phase != 3 || record.acknowledged) then throw "ResponseNotAckable"
    let payload ← ownedPayload state ctx record.payload
    if phase == 3 && payload.status == 0 then throw "ResponseNotPrepared"
    let next ← putObject state ctx txn "runtime.transaction" {record with acknowledged := record.acknowledged || phase == 4}.encode
    let (_,next) ← enqueue next ctx time 6 connection "runtime.send" [.handle txn,.bits 64 phase,.bits 64 record.transport,payload.encode]
    pure ([.unit],next)
  | .cancelLocal,[.handle txn,.bits 64 reason] =>
    if reason > 3 then throw "CancelReason"
    let record ← getTransaction state ctx txn
    let next ← putObject state ctx txn "runtime.transaction" {record with cancelled := true}.encode
    let next ← observe next ⟨"runtime.cancel","cancelLocal","",ctx.now,ctx.turn,[.handle txn,.bits 64 reason,.bool (record.phase != 0 && record.phase != 4)]⟩
    pure ([.unit],next)
  | _,_ => throw "RuntimeServiceOperands"

def invokeAttempt : ProviderAttempt := fun types ctx service args => do
  fromExcept (validate types service)
  fromExcept (checkContext ctx)
  if Nat.land service.contextMask (2^ctx.kind) == 0 then throw "RuntimeServiceContext"
  match service.op,args with
  | .bufferOutputWrite,[.bits 64 port,value] =>
    if port ≥ 2^32 then throw "OutputPortRange"
    let _ ← enqueueAttempt ctx ctx.now 6 0 "runtime.output" [.bits 64 0x4c41544f,.bits 64 port,value]
    pure []
  | .registerWait,[.handle process,.bits 64 kind,.bits 64 time] =>
    if ctx.processIdentity != some process then throw "TimerProcessContext"
    if kind != 3 && kind != 4 then throw "TimerKind"
    let deadline := if kind == 3 then ctx.now+time else time
    if deadline ≥ 2^64 then throw "TimeOverflow"
    let ready := if deadline ≤ ctx.now then some (ctx.now,ctx.turn) else none
    let wait ← newWaitAttempt ctx process kind deadline none ready (ready.map (fun _ => Value.unit))
    pure [.handle wait]
  | .registerWait,[.handle process,.handle hop] =>
    if ctx.processIdentity != some process || hop.kind != .hop then throw "ResponseWaitContext"
    let state ← get
    let _ ← fromExcept (getObject state ctx hop "runtime.hop")
    let latch := state.objects.find? (fun entry => entry.alive && entry.tag == "runtime.responseLatch" && (match entry.value with | .record [.handle source,_,_,_] => source == hop | _ => false))
    let ready := latch.bind (fun entry => match entry.value with | .record [.handle source,.bits 64 time,.bits 64 turn,value] => if source == hop then some ((time,turn),value) else none | _ => none)
    let wait ← newWaitAttempt ctx process 5 ctx.now (some hop) (ready.map Prod.fst) (ready.map Prod.snd)
    pure [.handle wait]
  | _,_ => modifyChecked (fun state => invoke types ctx service args state)
def deliver (ctx : Context) (state : State) (event : Event) : Except String State := do
  if event.time != ctx.now || event.turn != ctx.turn then throw "RuntimeEventReadyKey"
  if !state.events.contains event || event.instanceId != ctx.instanceId || event.identity.owner.toNat != ctx.owner then throw "RuntimeEventNotQueued"
  let next ← releaseObject state ctx event.identity "reference.event"
  let state := {next with events := next.events.filter (fun queued => queued.identity != event.identity)}
  if event.cancelled then return state
  match event.kind,event.values with
  | "runtime.resume",[.bits 64 0x4c415452,.handle process,.handle wait,.bits 64 ordinal] =>
    let data ← getProcess state ctx process
    let record ← getWait state ctx wait
    if data.status != 1 || data.wait != some wait || record.process != process || record.claimed || data.ordinal != ordinal then throw "StaleSuspensionToken"
    if record.outcome.isSome then return state
    if (record.kind != 3 && record.kind != 4) || record.deadline > ctx.now then throw "ResumeSourceNotReady"
    putWait state ctx wait {record with readyKey := some (ctx.now,ctx.turn),outcome := some .unit}
  | "runtime.output",[.bits 64 0x4c41544f,.bits 64 port,value] => observe state ⟨"output.published","bufferOutputWrite",event.source,ctx.now,ctx.turn,[.bits 64 ctx.instanceId,.bits 64 port,value]⟩
  | _,_ => throw "RuntimeEventNeedsEnvironment"
end LeanAT.Reference.Runtime







