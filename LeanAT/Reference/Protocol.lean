import LeanAT.Reference.Storage

namespace LeanAT.Reference.Protocol
open ExecIR
private def u (n : Nat) : Value := .bits 64 n
private def nat : Value → Except String Nat
  | .bits 64 n => if n < 2^64 then pure n else throw "ProtocolIntegerRange"
  | _ => throw "ProtocolIntegerCodec"
private def bool : Value → Except String Bool
  | .bool b => pure b | _ => throw "ProtocolBooleanCodec"
private def envNat (ctx : Context) (key : String) (fallback : Nat) : Except String Nat :=
  match ctx.environment.lookup key with | none => pure fallback | some value => nat value

structure Admission where
  inTime : Nat := 0
  sequence : Nat := 0
  originalAddress : Nat := 0
  localAddress : Nat := 0
  upstream : Nat := 0
  downstream : Nat := 0
  wireTerminal : Bool := false
  semanticTerminal : Bool := false
  pending : Bool := false
  servicing : Bool := false
  resetDeferred : Bool := false
  deriving Repr, BEq
def Admission.encode (v : Admission) : Value := .record [u v.inTime,u v.sequence,u v.originalAddress,u v.localAddress,u v.upstream,u v.downstream,.bool v.wireTerminal,.bool v.semanticTerminal,.bool v.pending,.bool v.servicing,.bool v.resetDeferred]
def Admission.decode : Value → Except String Admission
  | .record [a,b,c,d,e,f,g,h,i,j,k] => do return ⟨← nat a,← nat b,← nat c,← nat d,← nat e,← nat f,← bool g,← bool h,← bool i,← bool j,← bool k⟩
  | _ => throw "ProtocolAdmissionCodec"

structure Ledger where
  phase : Nat := 0
  lastTiming : Nat := 0
  faulted : Bool := false
  pending : Bool := false
  callOrdinal : Nat := 0
  protocolState : Nat := 0
  requestOwned : Bool := false
  responseOwned : Bool := false
  deriving Repr, BEq
def Ledger.encode (v : Ledger) : Value := .record [u v.phase,u v.lastTiming,.bool v.faulted,.bool v.pending,u v.callOrdinal,u v.protocolState,.bool v.requestOwned,.bool v.responseOwned]
def Ledger.decode : Value → Except String Ledger
  | .record [a,b,c,d,e,f,g,h] => do return ⟨← nat a,← nat b,← bool c,← bool d,← nat e,← nat f,← bool g,← bool h⟩
  | _ => throw "ProtocolLedgerCodec"

structure Drain where
  registered : Bool := true
  epoch : Nat := 0
  hopPresent : Bool := true
  wireTerminal : Bool := false
  timingConsumed : Bool := false
  cleanupReturned : Bool := false
  callPin : Bool := false
  localFinished : Bool := false
  cancelled : Bool := false
  receipt : Nat := 0
  receiptReleased : Bool := false
  reason : Nat := 0
  deriving Repr, BEq
def Drain.encode (v : Drain) : Value := .record [.bool v.registered,u v.epoch,.bool v.hopPresent,.bool v.wireTerminal,.bool v.timingConsumed,.bool v.cleanupReturned,.bool v.callPin,.bool v.localFinished,.bool v.cancelled,u v.receipt,.bool v.receiptReleased,u v.reason]
def Drain.decode : Value → Except String Drain
  | .record [a,b,c,d,e,f,g,h,i,j,k,l] => do return ⟨← bool a,← nat b,← bool c,← bool d,← bool e,← bool f,← bool g,← bool h,← bool i,← nat j,← bool k,← nat l⟩
  | _ => throw "ProtocolDrainCodec"

structure Ack where
  present : Bool := false
  value : Bool := false
  version : Nat := 0
  epoch : Nat := 0
  stableBytes : Bool := false
  deriving Repr, BEq
def Ack.encode (v : Ack) : Value := .record [.bool v.present,.bool v.value,u v.version,u v.epoch,.bool v.stableBytes]
def Ack.decode : Value → Except String Ack
  | .record [a,b,c,d,e] => do return ⟨← bool a,← bool b,← nat c,← nat d,← bool e⟩
  | _ => throw "ProtocolAckCodec"

structure TransactionData where
  connection : Nat
  transport : Nat
  generation : Nat
  hop : HandleIdentity
  payload : HandleIdentity
  phase : Nat := 0
  acknowledged : Bool := false
  cancelled : Bool := false
  admission : Admission := {}
  ledger : Ledger := {}
  drain : Drain := {}
  ack : Ack := {}
  deriving Repr, BEq
def TransactionData.encode (v : TransactionData) : Value := .record [u v.connection,u v.transport,u v.generation,.handle v.hop,.handle v.payload,u v.phase,.bool v.acknowledged,.bool v.cancelled,v.admission.encode,v.ledger.encode,v.drain.encode,v.ack.encode]
def TransactionData.decode : Value → Except String TransactionData
  | .record [a,b,c,.handle h,.handle p,d,e,f,g,i,j,k] => do
    let v : TransactionData := ⟨← nat a,← nat b,← nat c,h,p,← nat d,← bool e,← bool f,← Admission.decode g,← Ledger.decode i,← Drain.decode j,← Ack.decode k⟩
    if v.connection ≥ 2^32 || v.generation == 0 || v.phase > 4 || h.kind != .hop then throw "ProtocolTransactionRange"
    pure v
  | _ => throw "ProtocolTransactionCodec"
def getTransaction (state : State) (ctx : Context) (txn : HandleIdentity) : Except String TransactionData := do
  if txn.kind != .transaction then throw "TransactionKind"
  TransactionData.decode (← getObject state ctx txn "runtime.transaction").value

structure Intent where
  transaction : HandleIdentity
  connection : Nat
  transport : Nat
  phase : Nat
  notBefore : Nat
  callId : Nat
  payload : Value
  deriving Repr, BEq
def Intent.encode (v : Intent) : Value := .record [.handle v.transaction,u v.connection,u v.transport,u v.phase,u v.notBefore,u v.callId,v.payload]
def Intent.decode : Value → Except String Intent
  | .record [.handle t,a,b,c,d,e,p] => do return ⟨t,← nat a,← nat b,← nat c,← nat d,← nat e,p⟩
  | _ => throw "ProtocolIntentCodec"
structure Control where
  nextAdmission : Nat := 1
  nextSequence : Nat := 0
  nextCall : Nat := 1
  nextReceipt : Nat := 1
  nextReset : Nat := 1
  transactions : Nat := 16
  hops : Nat := 16
  services : Nat := 4
  gates : Nat := 16
  returns : Nat := 16
  ledgers : Nat := 128
  drains : Nat := 128
  maxPayload : Nat := 4096
  wakeGeneration : Nat := 1
  intents : List Intent := []
  revision : Nat := 0
  wireBusy : List (Nat × Bool) := []
  nextProtocol : Nat := 1
  intentCapacity : Nat := 128
  callCapacity : Nat := 256
  callsPerLedger : Nat := 64
  drainHopLimit : Nat := 8
  responseOrder : Nat := 0
  deriving Repr, BEq
def Control.encode (v : Control) : Value := .record [u v.nextAdmission,u v.nextSequence,u v.nextCall,u v.nextReceipt,u v.nextReset,u v.transactions,u v.hops,u v.services,u v.gates,u v.returns,u v.ledgers,u v.drains,u v.maxPayload,u v.wakeGeneration,.vec (v.intents.map Intent.encode),u v.revision,.vec (v.wireBusy.map (fun (c,b) => .record [u c,.bool b])),u v.nextProtocol,u v.intentCapacity,u v.callCapacity,u v.callsPerLedger,u v.drainHopLimit,u v.responseOrder]
def Control.decode : Value → Except String Control
  | .record [a,b,c,d,e,f,g,h,i,j,k,l,m,n,.vec intents,r,.vec lanes,p,ic,cc,cl,hl,ro] => do
    let lanes ← lanes.mapM (fun value => match value with
      | .record [connection,busy] => do return (← nat connection,← bool busy)
      | _ => throw "ProtocolLaneCodec")
    return ⟨← nat a,← nat b,← nat c,← nat d,← nat e,← nat f,← nat g,← nat h,← nat i,← nat j,← nat k,← nat l,← nat m,← nat n,← intents.mapM Intent.decode,← nat r,lanes,← nat p,← nat ic,← nat cc,← nat cl,← nat hl,← nat ro⟩
  | _ => throw "ProtocolControlCodec"
private def controlEntry (state : State) : Except String ObjectEntry :=
  match state.objects.find? (fun e => e.alive && e.tag == "protocol.control") with
  | some e => pure e | none => throw "MissingProtocolControl"
def getControl (state : State) : Except String Control := do Control.decode (← controlEntry state).value
private def setControl (state : State) (ctx : Context) (value : Control) : Except String State := do
  putObject state ctx (← controlEntry state).identity "protocol.control" value.encode
def seedProtocol (ctx : Context) (state : State) : Except String State := do
  if state.objects.any (fun e => e.alive && e.tag == "protocol.control") then return state
  let control : Control := {
    maxPayload := ← envNat ctx "runtime.maxPayloadBytes" 4096,
    ledgers := ← envNat ctx "protocol.ledgerCapacity" 128,drains := ← envNat ctx "protocol.drainCapacity" 128}
  let mut state := state
  for (kind,store,capacity) in [(.transaction,12,control.transactions),(.hop,13,control.hops),(.gateTicket,12,control.gates)] do
    state ← configureAllocator state {kind,store,group := "protocol.admission",allowMax := false,capacity := some capacity}
  pure (← allocate state ctx .access 14 "protocol.control" control.encode).2

/-- Retain only allocator burns after rollback; semantic state comes exclusively from before. -/
def preserveBurned (before draft : State) : State :=
  match controlEntry before, getControl before, getControl draft with
  | .ok entry,.ok old,.ok next =>
    let c := {old with
      nextAdmission := max old.nextAdmission next.nextAdmission,
      nextCall := max old.nextCall next.nextCall,nextReceipt := max old.nextReceipt next.nextReceipt,
      nextReset := max old.nextReset next.nextReset,nextProtocol := max old.nextProtocol next.nextProtocol}
    {before with objects := before.objects.map (fun o => if o.identity == entry.identity then {o with value := c.encode} else o)}
  | _,_,_ => before

def supports (s : ServiceSignature) : Bool := [.newTransaction,.stagePhase,.ackResponse,.cancelLocal].contains s.op
private def fromHex (s : String) : ByteArray :=
  let ns := s.toList.map fun c => if c ≤ '9' then c.toNat - '0'.toNat else c.toNat - 'a'.toNat + 10
  ⟨(List.range (ns.length/2)).toArray.map (fun i => UInt8.ofNat (ns[2*i]! * 16 + ns[2*i+1]!))⟩
def validate : ProviderValidate := fun types s => do
  if !supports s then throw "UnsupportedProtocolProvider"
  let key := if s.op == .cancelLocal then "leanat.core.transaction.cancel" else "leanat.protocol"
  let hash := if s.op == .cancelLocal then fromHex "fd95191dfcd83497a02c4edcca1089d64b98ba014a288bf1800bc12df81a6956"
    else ABI.sha256 ("leanat.protocol.v1:" ++ opcodeNames[s.op.tag]!).toUTF8
  if s.providerKey != key || s.providerVersion != "1" || s.abiHash != hash || s.extraFuel != 0 || s.contextMask == 0 || Nat.land s.contextMask 3 != s.contextMask || s.effectMask != 8 then throw "ProtocolProviderABI"
  let ins := s.inputTypes.map (fun n => types[n]?)
  let outs := s.resultTypes.map (fun n => types[n]?)
  let valid := match s.op with
    | .newTransaction => outs == [some (.handle .transaction)] && match ins with
      | [some (.bits 64),some (.bits 64),some (.bits 64),some (.bits 64),some (.bits 64),some (.bytes _)] => true
      | _ => false
    | .stagePhase => ins == [some (.handle .transaction),some (.bits 64),some (.bits 64),some (.bits 64)] && outs == [some .unit]
    | .ackResponse => ins == [some (.handle .transaction),some (.bits 64),some (.bits 64)] && outs == [some .unit]
    | .cancelLocal => ins == [some (.handle .transaction),some (.bits 64)] && outs == [some .unit]
    | _ => false
  if !valid then throw "ProtocolProviderTypeABI"

private instance : MonadLift (Except String) (EStateM String State) where
  monadLift value := fun state => match value with
    | .ok value => .ok value state
    | .error error => .error error state
private def keepState (value : Except String State) : EStateM String State State := do
  let next ← value
  set next
  pure next
private def keepAllocation (value : Except String (HandleIdentity × State)) : EStateM String State (HandleIdentity × State) := do
  let next ← value
  set next.2
  pure next
private def execute (types : TypeEnvironment) (ctx : Context) (service : ServiceSignature)
    (args : List Value) : EStateM String State (List Value) := do
  let state ← get
  validate types service
  checkContext ctx
  if ctx.kind > 1 then throw "ProtocolContext"
  let control ← getControl state
  let operation := if service.op == .ackResponse then Op.stagePhase else service.op
  let args := if service.op == .ackResponse then match args with
    | [a,b,c] => [a,b,u 4,c] | other => other else args
  match operation,args with
  | .newTransaction,[.bits 64 connection,.bits 64 transport,.bits 64 generation,.bits 64 command,.bits 64 address,.bytes bytes] =>
    if connection ≥ 2^32 || generation == 0 || command > 2 || bytes.length > control.maxPayload then throw "TransactionOperands"
    let transactions := state.objects.filter (fun o => o.alive && o.tag == "runtime.transaction")
    for entry in transactions do
      let record ← TransactionData.decode entry.value
      if record.connection == connection && record.transport == transport then throw "DuplicateTransport"
    if transactions.length ≥ control.transactions || transactions.length ≥ control.hops then throw "TransactionCapacity"
    if control.nextAdmission ≥ 2^64-1 then throw "ProtocolCounterExhausted"
    let (txn,next) ← keepAllocation (allocate state ctx .transaction 12 "runtime.transaction" .unit)
    let next ← keepState (setControl next ctx {control with nextAdmission := control.nextAdmission+1})
    if control.nextAdmission+1 ≥ 2^64-1 then throw "ProtocolCounterExhausted"
    let (hop,next) ← keepAllocation (allocate next ctx .hop 13 "runtime.hop" (.record [.handle txn,u ctx.instanceId,u connection,u transport,u generation,u 0]))
    let next ← keepState (setControl next ctx {control with nextAdmission := control.nextAdmission+2})
    if transactions.length ≥ control.ledgers then throw "LedgerCapacity"
    if transactions.length ≥ control.drains then throw "DrainCapacity"
    let (payload,next) ← keepAllocation (Storage.createPayload ctx next {
      transaction := txn,hop,instanceId := ctx.instanceId,
      localSide := ctx.instanceId,target := false,writable := false,command,address,streamingWidth := max 1 bytes.length,
      baseline := bytes,data := bytes,maxBytes := control.maxPayload,connection})
    let record : TransactionData := {
      connection,transport,generation,hop,payload,
      admission := {inTime := ctx.now,sequence := control.nextSequence,originalAddress := address,localAddress := address}}
    let next ← keepState (putObject next ctx txn "runtime.transaction" record.encode)
    let _ ← keepState (setControl next ctx {control with nextAdmission := control.nextAdmission+2,nextSequence := control.nextSequence+1,revision := control.revision+1})
    pure [.handle txn]
  | .stagePhase,[.handle txn,.bits 64 connection,.bits 64 phase,.bits 64 time] =>
    let record ← getTransaction state ctx txn
    if record.connection != connection || phase < 1 || phase > 4 || time < ctx.now then throw "PhaseOperands"
    let payload ← Storage.PayloadRecord.decode (← getObject state ctx record.payload Storage.payloadTag).value
    if phase == 3 then throw "MissingResponseSnapshotProvider"
    if phase == 1 && (record.cancelled || control.intents.any (fun e => e.phase == 1 && e.connection == connection)) then throw "RequestGateBusy"
    let mut next := state
    if phase == 1 then
      let busy := state.objects.any (fun entry => entry.alive && entry.tag == "runtime.transaction" &&
        (match TransactionData.decode entry.value with | .ok t => t.connection == connection && t.ledger.requestOwned | .error _ => false))
      if busy then throw "RequestGateBusy"
      let gates := state.objects.filter (fun entry => entry.alive && entry.tag == "protocol.gate")
      if gates.length ≥ control.gates then throw "RequestGateCapacity"
      next ← keepState (setControl next ctx {control with nextAdmission := control.nextAdmission+1})
      let (_,updated) ← keepAllocation (allocate next ctx .gateTicket 12 "protocol.gate"
        (.record [.handle txn,u connection,u 2,u ctx.now,u ctx.turn,u control.nextSequence]))
      next := updated
    if phase == 4 && (record.ledger.phase != 3 || record.ledger.faulted || record.ack.value) then throw "ResponseNotAckable"
    if control.nextCall ≥ 2^64-1 then throw "CallIdentityExhausted"
    if control.intents.length ≥ control.callCapacity then throw "CallCapacity"
    let ack := if phase == 4 then {record.ack with present := true,value := true,version := record.ack.version+1} else record.ack
    next ← keepState (putObject next ctx txn "runtime.transaction" {record with acknowledged := ack.value,ack}.encode)
    let intent : Intent := ⟨txn,connection,record.transport,phase,time,control.nextCall,payload.encode⟩
    next ← keepState (setControl next ctx {control with
      nextCall := control.nextCall+1,
      nextAdmission := control.nextAdmission+(if phase == 1 then 1 else 0)})
    if control.intents.length ≥ control.intentCapacity then throw "IntentCapacity"
    let _ ← keepState (setControl next ctx {control with
      nextCall := control.nextCall+1,
      nextAdmission := control.nextAdmission+(if phase == 1 then 1 else 0),
      nextSequence := control.nextSequence+(if phase == 1 then 1 else 0),intents := control.intents ++ [intent],
      revision := control.revision+(if phase == 1 then 5 else 0),
      wireBusy := if phase == 1 then (control.wireBusy.filter (fun p => p.1 != connection)) ++ [(connection,false)] else control.wireBusy})
    pure [.unit]
  | .cancelLocal,[.handle txn,.bits 64 reason] =>
    if reason > 3 then throw "CancelReason"
    let record ← getTransaction state ctx txn
    if !record.drain.registered then throw "StaleResponsibility"
    let unsent := record.ledger.phase == 0 && record.ledger.callOrdinal == 0 && !record.ledger.pending
    let hasGate := state.objects.any (fun entry => entry.alive && entry.tag == "protocol.gate" &&
      (match entry.value with | .record (.handle gateTxn :: _) => gateTxn == txn | _ => false))
    let hasIntent := control.intents.any (fun intent => intent.transaction == txn)
    let retireUnsent := unsent && !hasGate && !hasIntent
    let receipt := if record.drain.receipt > 0 then record.drain.receipt else if unsent then 0 else control.nextReceipt
    if !unsent && receipt ≥ 2^64-1 then throw "DrainReceiptExhausted"
    let drain := {record.drain with registered := !retireUnsent,hopPresent := !unsent,localFinished := true,cancelled := true,receipt,receiptReleased := receipt > 0,reason}
    let next ← keepState (putObject state ctx txn "runtime.transaction" {record with cancelled := true,drain}.encode)
    let _ ← keepState (setControl next ctx {control with nextReceipt := if !unsent && record.drain.receipt == 0 then control.nextReceipt+1 else control.nextReceipt})
    if retireUnsent then
      let current ← get
      let retired := {current with objects := current.objects.map (fun entry =>
        if entry.identity == txn || entry.identity == record.hop || entry.identity == record.payload then {entry with alive := false} else entry)}
      let _ ← keepState (setControl retired ctx {control with revision := control.revision+1})
    pure [.unit]
  | _,_ => throw "ProtocolOperands"

/-- Preserve the actual partial draft on a failing service, including earlier allocator burns. -/
def attempt (types : TypeEnvironment) (ctx : Context) (service : ServiceSignature)
    (args : List Value) (state : State) : Except String (List Value) × State :=
  match (execute types ctx service args).run state with
  | .ok values next => (.ok values,next)
  | .error error draft => (.error error,draft)
def invoke : ProviderInvoke := fun types ctx service args state =>
  match attempt types ctx service args state with
  | (.ok values,next) => .ok (values,next)
  | (.error error,_) => .error error

/-- Host fixture setup uses direct admission/binding, which advances the real binding allocator. -/
def seedInitiator (types : TypeEnvironment) (ctx : Context) (signature : ServiceSignature)
    (args : List Value) (state : State) : Except String (HandleIdentity × State) := do
  let (values,next) ← invoke types ctx signature args state
  let [.handle txn] := values | throw "ProtocolSeedReturn"
  let control ← getControl next
  let next ← setControl next ctx {control with nextProtocol := max control.nextProtocol control.nextAdmission}
  pure (txn,next)

/-- An actual BEGIN_REQ/return observation seeds response or terminal state, independent of VM execution. -/
def observeInitialExchange (ctx : Context) (state : State) (txn : HandleIdentity) (terminal : Bool) : Except String State := do
  let record ← getTransaction state ctx txn
  if record.ledger.phase != 0 || record.ledger.callOrdinal != 0 then throw "InitialExchangeAlreadyStarted"
  let control ← getControl state
  let phase := if terminal then 4 else 3
  let ledger : Ledger := {phase,lastTiming := ctx.now,callOrdinal := 1,responseOwned := !terminal}
  let drain := {record.drain with wireTerminal := terminal,timingConsumed := terminal}
  let ack : Ack := {present := !terminal}
  let next ← putObject state ctx txn "runtime.transaction" {record with phase,ledger,drain,ack}.encode
  let next ← putObject next ctx record.hop "runtime.hop" (.record [.handle txn,u ctx.instanceId,u record.connection,u record.transport,u record.generation,u phase])
  let next ← setControl next ctx {control with nextCall := control.nextCall+1,nextProtocol := control.nextProtocol+1}
  let payload ← Storage.PayloadRecord.decode (← getObject next ctx record.payload Storage.payloadTag).value
  if !payload.extensions.isEmpty then throw "InitialExchangeExtensionsUnsupported"
  let request : Value := .record [u payload.command,u payload.address,.bytes payload.baseline,
    u payload.streamingWidth,.bytes payload.byteEnable,u payload.status,.bool payload.dmiHint,.vec []]
  let response : Value := .record [u 1,.bytes payload.baseline,.bool false,.vec []]
  let call : Value := .record [u control.nextCall,u record.connection,u record.transport,u 0,u 1,
    u ctx.now,u 0,request]
  let returned : Value := .record [u (if terminal then 2 else 1),
    (if terminal then .unit else u 3),u 0,response]
  let milestones : List Value := [.record [u 0,u ctx.now,.bool true,.unit],
    .record [u 1,u ctx.now,.bool true,response]] ++
    (if terminal then [.record [u 2,u ctx.now,.bool false,.unit]] else [])
  let exchange : Value := .record [u control.nextCall,.handle record.hop,u phase,.vec milestones,
    .vec [],.bool (!terminal),.bool terminal,.bool false]
  observe next ⟨"protocol.setup.exchange","","",ctx.now,ctx.turn,[call,returned,exchange]⟩

/-- Commit-time host scheduling is distinct from publishing a wire intent. -/
def committed (ctx : Context) (state : State) : Except String State := do
  if !(state.objects.any (fun e => e.alive && e.tag == "protocol.control")) then return state
  let control ← getControl state
  if control.wakeGeneration ≥ 2^64-1 then throw "WakeGenerationExhausted"
  let next ← setControl state ctx {control with wakeGeneration := control.wakeGeneration+1}
  let earliest := control.intents.foldl (fun time intent => min time (max ctx.now intent.notBefore)) (2^64)
  if earliest == 2^64 then return next
  observe next ⟨"protocol.host.arm","","",ctx.now,ctx.turn,[u earliest,u 0,u (control.wakeGeneration+1)]⟩
/-- Public owned drain receipt, independent from a Unit-returning cancellation service. -/
structure DrainReceipt where
  transaction : HandleIdentity
  reason : Nat
  state : Nat
  hops : List (HandleIdentity × Drain)
  deriving Repr, BEq

def DrainReceipt.encode (r : DrainReceipt) : Value := .record [
  .handle r.transaction,u r.reason,u r.state,
  .vec (r.hops.map (fun (hop,status) => .record [.handle hop,status.encode]))]

def DrainReceipt.decode : Value → Except String DrainReceipt
  | .record [.handle transaction,reason,state,.vec hops] => do
    let reason ← nat reason
    let state ← nat state
    if reason > 3 || state > 2 then throw "DrainReceiptRange"
    let hops ← hops.mapM (fun value => match value with
      | .record [.handle hop,status] => do return (hop,← Drain.decode status)
      | _ => throw "DrainReceiptHopCodec")
    pure ⟨transaction,reason,state,hops⟩
  | _ => throw "DrainReceiptCodec"

/-- Seed through the same cancellation transition as a real owned receipt API.
    A wire-started responsibility is mandatory; local-only cancellation has no receipt. -/
def seedOwnedDrain (ctx : Context) (state : State) (transaction : HandleIdentity)
    (reason : Nat := 1) : Except String (HandleIdentity × State) := do
  let record ← getTransaction state ctx transaction
  let control ← getControl state
  if reason > 3 then throw "CancelReason"
  if !record.drain.registered || record.ledger.callOrdinal == 0 then throw "OwnedDrainRequiresStartedHop"
  if record.drain.receipt != 0 then throw "DrainReceiptAlreadyOwned"
  if control.nextReceipt ≥ 2^64-1 then throw "DrainReceiptExhausted"
  let status : Drain := { record.drain with
    localFinished := true
    cancelled := true
    receipt := control.nextReceipt
    receiptReleased := false
    reason := reason }
  let receipt : DrainReceipt := ⟨transaction,reason,
    if status.wireTerminal && status.timingConsumed && status.cleanupReturned && !status.callPin then 1 else 0,
    [(record.hop,status)]⟩
  let rule : AllocationRule := {
    kind := .drain
    store := 15
    group := "protocol.receipt"
    allowMax := false
    capacity := some control.drains }
  let next ← configureAllocator state rule
  let (handle,next) ← allocate next ctx .drain 15 "protocol.receipt" receipt.encode
  let next ← putObject next ctx transaction "runtime.transaction" {record with cancelled := true,drain := status}.encode
  let payload ← Storage.PayloadRecord.decode (← getObject next ctx record.payload Storage.payloadTag).value
  let cleanupIntent : Intent := ⟨transaction,record.connection,record.transport,4,
    max ctx.now record.ledger.lastTiming,control.nextCall,payload.encode⟩
  if record.ledger.phase == 3 && (control.nextCall ≥ 2^64-1 || control.intents.length ≥ control.intentCapacity || control.intents.length ≥ control.callCapacity) then throw "CleanupIntentCapacity"
  let nextControl : Control := { control with
    nextReceipt := control.nextReceipt+1
    nextCall := control.nextCall+(if record.ledger.phase == 3 then 1 else 0)
    intents := control.intents ++ (if record.ledger.phase == 3 then [cleanupIntent] else []) }
  let next ← setControl next ctx nextControl
  let next ← committed ctx next
  pure (handle,next)
end LeanAT.Reference.Protocol

