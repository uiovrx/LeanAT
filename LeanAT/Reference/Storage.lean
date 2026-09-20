import LeanAT.Reference.Contract
import LeanAT.Reference.ABI
namespace LeanAT.Reference.Storage
open ExecIR

def resultTag := "storage.result"
def consumerTag := "storage.consumer"
def payloadTag := "storage.payload"

/-- Explicit native ResultStore profile and imported allocator journal. Native snapshots
    report the last allocated generation; the reference counter stores the next one. -/
def seedResultStore (state : State) (ctx : Context) (resultCapacity consumerCapacity pinCapacity : Nat)
    (lastResultGeneration lastConsumerGeneration : Nat := 0) : Except String State := do
  checkContext ctx
  validateState state
  if resultCapacity == 0 || consumerCapacity == 0 || pinCapacity == 0 ||
      resultCapacity ≥ 2^32 || consumerCapacity ≥ 2^32 ||
      lastResultGeneration ≥ 2^64 || lastConsumerGeneration ≥ 2^64 then throw "ResultStoreProfile"
  let state ← configureAllocator state {kind := .result,store := 20,group := "storage.result",capacity := some resultCapacity}
  let state ← configureAllocator state {kind := .consumer,store := 21,group := "storage.consumer",capacity := some consumerCapacity}
  let mut counters := state.allocationCounters
  for (group,last) in [("storage.result",lastResultGeneration),("storage.consumer",lastConsumerGeneration)] do
    let imported := (state.objects.filter (fun entry => entry.identity.domain.toNat == ctx.domain &&
      (allocationRule state entry.identity.kind entry.identity.store.toNat).group == group)).map
      (fun entry => entry.identity.generation.toNat)
    if imported.any (fun generation => generation > last) then throw "ResultJournalRewind"
    if let some old := counters.find? (fun c => c.group == group && c.domain == ctx.domain && c.slot.isNone) then
      if old.nextGeneration > last+1 then throw "ResultJournalRewind"
    counters := counters.filter (fun c => !(c.group == group && c.domain == ctx.domain && c.slot.isNone)) ++
      [{group,domain := ctx.domain,nextGeneration := last+1}]
  let next := {state with
    allocationCounters := counters
    maxPins := pinCapacity
    nextGeneration := max state.nextGeneration (max (lastResultGeneration+1) (lastConsumerGeneration+1))}
  checkCapacity next
  validateState next
  pure next

private def ensureResultRules : Attempt Unit := do
  let state ← get
  let state ← if state.allocationRules.any (fun r => r.kind == .result && r.store == 20) then pure state
    else fromExcept (configureAllocator state {kind := .result,store := 20,group := "storage.result",capacity := some 128})
  let state ← if state.allocationRules.any (fun r => r.kind == .consumer && r.store == 21) then pure state
    else fromExcept (configureAllocator state {kind := .consumer,store := 21,group := "storage.consumer",capacity := some 256})
  set state
structure ResultRecord where
  source : HandleIdentity
  typeId : Nat
  maxBytes : Nat
  producerAlive : Bool := true
  published : Bool := false
  value : Value := .unit
  readyTime : Nat := 0
  readyTurn : Nat := 0
  pins : Nat := 0
  producerOwner : Nat := 0
  initialConsumerOwner : Nat := 0
  publishing : Bool := false
  deriving Repr, BEq
structure ConsumerRecord where
  result : HandleIdentity
  dropOnTerminal : Bool := false
  deriving Repr, BEq

def ResultRecord.encode (r : ResultRecord) : Value := .record [.handle r.source,.bits 64 r.typeId,.bits 64 r.maxBytes,.bool r.producerAlive,.bool r.published,r.value,.bits 64 r.readyTime,.bits 64 r.readyTurn,.bits 64 r.pins,.bits 64 r.producerOwner,.bits 64 r.initialConsumerOwner,.bool r.publishing]
def ResultRecord.decode : Value → Except String ResultRecord
  | .record [.handle source,.bits 64 typeId,.bits 64 maxBytes,.bool producerAlive,.bool published,value,.bits 64 readyTime,.bits 64 readyTurn,.bits 64 pins,.bits 64 producerOwner,.bits 64 initialConsumerOwner,.bool publishing] =>
    if [typeId,maxBytes,readyTime,readyTurn,pins,producerOwner,initialConsumerOwner].all (· < 2^64) && !(publishing && (published || !producerAlive)) then
      .ok {source,typeId,maxBytes,producerAlive,published,value,readyTime,readyTurn,pins,producerOwner,initialConsumerOwner,publishing}
    else .error "ResultRecordRange"
  | _ => .error "ResultRecordCodec"
def ConsumerRecord.encode (r : ConsumerRecord) : Value := .record [.handle r.result,.bool r.dropOnTerminal]
def ConsumerRecord.decode : Value → Except String ConsumerRecord
  | .record [.handle result,.bool dropOnTerminal] =>
    if result.kind == .result && !dropOnTerminal then .ok {result,dropOnTerminal} else .error "ConsumerResultKind"
  | _ => .error "ConsumerRecordCodec"

structure ResultSlotView where
  identity : HandleIdentity
  alive : Bool
  record : ResultRecord
  deriving Repr, BEq
structure ConsumerSlotView where
  identity : HandleIdentity
  active : Bool
  record : ConsumerRecord
  deriving Repr, BEq
structure ResultPoolView where
  resultCapacity : Nat
  consumerCapacity : Nat
  pinCapacity : Nat
  pinCount : Nat
  lastResultGeneration : Nat
  lastConsumerGeneration : Nat
  results : List ResultSlotView
  consumers : List ConsumerSlotView
  deriving Repr, BEq

/-- Owning slot state and independent native counters. Indices below capacity not
    listed are never-used slots, not invented live handles or erased tombstones. -/
def resultPoolSnapshot (ctx : Context) (state : State) : Except String ResultPoolView := do
  let resultRule := allocationRule state .result 20
  let consumerRule := allocationRule state .consumer 21
  let some resultCapacity := resultRule.capacity | throw "ResultPoolCapacityMissing"
  let some consumerCapacity := consumerRule.capacity | throw "ConsumerPoolCapacityMissing"
  let results ← (state.objects.filter (fun e => e.tag == resultTag && e.identity.domain.toNat == ctx.domain)).mapM fun e => do
    let record ← ResultRecord.decode e.value
    if e.identity.kind != .result || e.identity.store != 20 || e.identity.slot.toNat ≥ resultCapacity || record.producerOwner != e.identity.owner.toNat then throw "ResultPoolSlot"
    pure ({identity := e.identity,alive := e.alive,record} : ResultSlotView)
  let consumers ← (state.objects.filter (fun e => e.tag == consumerTag && e.identity.domain.toNat == ctx.domain)).mapM fun e => do
    let record ← ConsumerRecord.decode e.value
    if e.identity.kind != .consumer || e.identity.store != 21 || e.identity.slot.toNat ≥ consumerCapacity then throw "ConsumerPoolSlot"
    pure ({identity := e.identity,active := e.alive,record} : ConsumerSlotView)
  let last := fun (rule : AllocationRule) =>
    let imported := state.objects.filter (fun e => e.identity.domain.toNat == ctx.domain && (allocationRule state e.identity.kind e.identity.store.toNat).group == rule.group)
    let high := (imported.map (fun e => e.identity.generation.toNat)).foldl max 0
    ((state.allocationCounters.find? (fun c => c.group == rule.group && c.domain == ctx.domain && c.slot.isNone)).map (fun c => c.nextGeneration-1)).getD high
  pure {
    resultCapacity := resultCapacity
    consumerCapacity := consumerCapacity
    pinCapacity := state.maxPins
    pinCount := ((results.filter ResultSlotView.alive).map (fun e => e.record.pins)).sum
    lastResultGeneration := last resultRule
    lastConsumerGeneration := last consumerRule
    results := results
    consumers := consumers
  }

def readResult (ctx : Context) (state : State) (result : HandleIdentity) : Except String ResultRecord := do
  if result.kind != .result then throw "ResultHandleKind"
  let entry ← getObject state {ctx with owner := result.owner.toNat} result resultTag
  let record ← ResultRecord.decode entry.value
  if record.producerOwner != result.owner.toNat then throw "ResultRecordOwner"
  pure record
private def writeResult (ctx : Context) (state : State) (result : HandleIdentity) (r : ResultRecord) : Except String State :=
  putObject state {ctx with owner := result.owner.toNat} result resultTag r.encode

def readConsumer (ctx : Context) (state : State) (result consumer : HandleIdentity) : Except String ConsumerRecord := do
  if consumer.kind != .consumer then throw "ConsumerHandleKind"
  let c ← ConsumerRecord.decode (← getObject state ctx consumer consumerTag).value
  if c.result != result then throw "ConsumerResultMismatch"
  let _ ← readResult ctx state result
  pure c

/-- Roll back one provider operation, retaining only native persistent allocation burns. -/
def atomicAttempt (action : Attempt α) : Attempt α := fun before =>
  match action.run before with
  | .ok value next => .ok value next
  | .error error draft => .error error (rollbackAllocations before draft)

def reserveResultAttempt (types : TypeEnvironment) (ctx : Context) (source : HandleIdentity)
    (typeId maxBytes consumerOwner : Nat) : Attempt (HandleIdentity × HandleIdentity) := atomicAttempt do
  if (types[typeId]?).isNone || maxBytes ≥ 2^64 || consumerOwner ≥ 2^64 then throw "ResultCreateRange"
  if source.domain.toNat != ctx.domain || source.owner.toNat != ctx.owner then throw "ResultSourceOwner"
  ensureResultRules
  let r : ResultRecord := {source,typeId,maxBytes,producerOwner := ctx.owner,initialConsumerOwner := consumerOwner}
  let result ← allocateAttempt ctx .result 20 resultTag r.encode
  let consumer ← allocateAttempt {ctx with owner := consumerOwner} .consumer 21 consumerTag (ConsumerRecord.encode {result})
  pure (result,consumer)

def reserveResult (types : TypeEnvironment) (ctx : Context) (state : State) (source : HandleIdentity)
    (typeId maxBytes consumerOwner : Nat) : Except String (HandleIdentity × HandleIdentity × State) := do
  let ((result,consumer),next) ← attemptExcept ((reserveResultAttempt types ctx source typeId maxBytes consumerOwner).run state)
  pure (result,consumer,next)

def publishResult (types : TypeEnvironment) (ctx : Context) (state : State) (result : HandleIdentity) (value : Value) : Except String State := do
  checkContext ctx
  if result.owner.toNat != ctx.owner then throw "WrongOwner"
  let r ← readResult ctx state result
  if !r.producerAlive || r.published || r.publishing then throw "ResultPublicationState"
  if !(types[r.typeId]?).any (fun t => conforms types (types.length+1) t value) then throw "ResultPublicationType"
  if (← chargeValue ctx value) > r.maxBytes then throw "ResultPublicationCapacity"
  let next ← writeResult ctx state result {r with published := true,value,readyTime := ctx.now,readyTurn := ctx.turn}
  pure next

def getResult (ctx : Context) (state : State) (result consumer : HandleIdentity) : Except String Value := do
  let _ ← readConsumer ctx state result consumer
  let r ← readResult ctx state result
  if !r.published then throw "ResultNotReady"
  pure r.value

def publishResultAttempt (types : TypeEnvironment) (ctx : Context) (result : HandleIdentity) (value : Value) : Attempt Unit :=
  updateChecked (fun state => publishResult types ctx state result value)

def retainResultAttempt (ctx : Context) (result consumer : HandleIdentity) (newOwner : Nat) : Attempt HandleIdentity := atomicAttempt do
  let _ ← fromExcept (readConsumer ctx (← get) result consumer)
  ensureResultRules
  allocateAttempt {ctx with owner := newOwner} .consumer 21 consumerTag (ConsumerRecord.encode {result})

def retainResult (ctx : Context) (state : State) (result consumer : HandleIdentity) (newOwner : Nat) : Except String (HandleIdentity × State) := do
  attemptExcept ((retainResultAttempt ctx result consumer newOwner).run state)

private def collectResult (ctx : Context) (state : State) (result : HandleIdentity) : Except String State := do
  let r ← readResult ctx state result
  let retained := state.objects.any fun entry => entry.alive && entry.tag == consumerTag &&
    (match ConsumerRecord.decode entry.value with | .ok c => c.result == result | .error _ => false)
  if !r.producerAlive && r.pins == 0 && !retained then
    pure {state with objects := state.objects.map (fun e => if e.identity == result then {e with alive := false,value := {r with published := false,value := .unit,readyTime := 0,readyTurn := 0}.encode} else e)}
  else pure state

def releaseResult (ctx : Context) (state : State) (result consumer : HandleIdentity) : Except String State := do
  let _ ← readConsumer ctx state result consumer
  let next := {state with objects := state.objects.map (fun e => if e.identity == consumer then {e with alive := false} else e)}
  collectResult ctx next result

def releaseResultAttempt (ctx : Context) (result consumer : HandleIdentity) : Attempt Unit :=
  updateChecked (fun state => releaseResult ctx state result consumer)

def transferResultAttempt (ctx : Context) (result consumer : HandleIdentity) (newOwner : Nat) : Attempt HandleIdentity := atomicAttempt do
  let fresh ← retainResultAttempt ctx result consumer newOwner
  releaseResultAttempt ctx result consumer
  pure fresh

def transferResult (ctx : Context) (state : State) (result consumer : HandleIdentity) (newOwner : Nat) : Except String (HandleIdentity × State) := do
  attemptExcept ((transferResultAttempt ctx result consumer newOwner).run state)

def releaseProducer (ctx : Context) (state : State) (result : HandleIdentity) : Except String State := do
  if result.owner.toNat != ctx.owner then throw "WrongOwner"
  let r ← readResult ctx state result
  if !r.producerAlive then throw "ResultProducerReleased"
  collectResult ctx (← writeResult ctx state result {r with producerAlive := false}) result

def releaseProducerAttempt (ctx : Context) (result : HandleIdentity) : Attempt Unit :=
  updateChecked (fun state => releaseProducer ctx state result)

def pinResult (ctx : Context) (state : State) (result consumer : HandleIdentity) : Except String State := do
  let _ ← getResult ctx state result consumer
  let r ← readResult ctx state result
  let records ← (state.objects.filter (fun e => e.alive && e.tag == resultTag)).mapM (fun e => ResultRecord.decode e.value)
  if (records.map ResultRecord.pins).sum >= state.maxPins then throw "ResultPinCapacity"
  if r.pins+1 ≥ 2^64 then throw "ResultPinOverflow"
  writeResult ctx state result {r with pins := r.pins+1}

def unpinResult (ctx : Context) (state : State) (result : HandleIdentity) : Except String State := do
  let r ← readResult ctx state result
  if r.pins == 0 then throw "ResultPinUnderflow"
  collectResult ctx (← writeResult ctx state result {r with pins := r.pins-1}) result



structure ExtensionRecord where
  name : String
  maxBytes : Nat
  requestAllowed : Bool := true
  responseAllowed : Bool := true
  responseWritable : Bool := true
  value : Option (List UInt8) := none
  deriving Repr, BEq

def ExtensionRecord.encode (r : ExtensionRecord) : Value := .record [.bytes r.name.toUTF8.data.toList,.bits 64 r.maxBytes,.bool r.requestAllowed,.bool r.responseAllowed,.bool r.responseWritable,match r.value with | none => .unit | some b => .bytes b]
def ExtensionRecord.decode : Value → Except String ExtensionRecord
  | .record [.bytes name,.bits 64 maxBytes,.bool requestAllowed,.bool responseAllowed,.bool responseWritable,value] => do
    let some name := String.fromUTF8? ⟨name.toArray⟩ | throw "ExtensionNameUTF8"
    let bytes ← match value with | .unit => pure none | .bytes b => pure (some b) | _ => throw "ExtensionValueCodec"
    if name.isEmpty || maxBytes ≥ 2^64 || bytes.any (fun b => b.length > maxBytes) || (responseWritable && !responseAllowed) then throw "ExtensionInvariant"
    pure {name,maxBytes,requestAllowed,responseAllowed,responseWritable,value := bytes}
  | _ => .error "ExtensionRecordCodec"

structure PayloadRecord where
  transaction : HandleIdentity
  hop : HandleIdentity
  instanceId : Nat
  localSide : Nat
  target : Bool
  writable : Bool
  command : Nat
  address : Nat := 0
  streamingWidth : Nat := 1
  baseline : List UInt8 := []
  data : List UInt8 := []
  byteEnable : List UInt8 := []
  status : Nat := 0
  dmiHint : Bool := false
  maxBytes : Nat := 65536
  extensions : List ExtensionRecord := []
  connection : Nat := 0
  deriving Repr, BEq

def PayloadRecord.encode (p : PayloadRecord) : Value := .record [.handle p.transaction,.handle p.hop,.bits 64 p.instanceId,.bits 64 p.localSide,.bool p.target,.bool p.writable,.bits 64 p.command,.bits 64 p.address,.bits 64 p.streamingWidth,.bytes p.baseline,.bytes p.data,.bytes p.byteEnable,.bits 64 p.status,.bool p.dmiHint,.bits 64 p.maxBytes,.vec (p.extensions.map ExtensionRecord.encode),.bits 64 p.connection]
def PayloadRecord.decode : Value → Except String PayloadRecord
  | .record [.handle transaction,.handle hop,.bits 64 instanceId,.bits 64 localSide,.bool target,.bool writable,.bits 64 command,.bits 64 address,.bits 64 streamingWidth,.bytes baseline,.bytes data,.bytes byteEnable,.bits 64 status,.bool dmiHint,.bits 64 maxBytes,.vec extensions,.bits 64 connection] => do
    let extensions ← extensions.mapM ExtensionRecord.decode
    if transaction.kind != .transaction || hop.kind != .hop || instanceId ≥ 2^32 || localSide ≥ 2^32 || connection ≥ 2^32 || command > 2 || status > 6 || address ≥ 2^64 || streamingWidth == 0 || streamingWidth ≥ 2^64 || maxBytes ≥ 2^64 then throw "PayloadRecordRange"
    if data.length != baseline.length || data.length + byteEnable.length + (extensions.map (fun e => (e.value.map (fun bytes => e.name.utf8ByteSize + bytes.length)).getD 0)).sum > maxBytes then throw "PayloadRecordCapacity"
    if extensions.any (fun e => e.maxBytes > maxBytes || e.name.utf8ByteSize > maxBytes) then throw "PayloadExtensionSchemaCapacity"
    if (extensions.map ExtensionRecord.name).eraseDups.length != extensions.length then throw "DuplicateExtension"
    pure {transaction,hop,instanceId,localSide,target,writable,command,address,streamingWidth,baseline,data,byteEnable,status,dmiHint,maxBytes,extensions,connection}
  | _ => .error "PayloadRecordCodec"

structure PayloadAccess where
  hop : HandleIdentity
  localSide : Nat
  returned : Bool := false
  forward : Bool := true
  phase : Nat := 1
  sync : Nat := 0
  returnedPhase : Nat := 0
  responseWritePermit : Bool := false
  validated : Bool := true
  deriving Repr, BEq

def PayloadAccess.encode (a : PayloadAccess) : Value := .record [.handle a.hop,.bits 64 a.localSide,.bool a.returned,.bool a.forward,.bits 64 a.phase,.bits 64 a.sync,.bits 64 a.returnedPhase,.bool a.responseWritePermit,.bool a.validated]
def PayloadAccess.decode : Value → Except String PayloadAccess
  | .record [.handle hop,.bits 64 localSide,.bool returned,.bool forward,.bits 64 phase,.bits 64 sync,.bits 64 returnedPhase,.bool responseWritePermit,.bool validated] =>
    .ok {hop,localSide,returned,forward,phase,sync,returnedPhase,responseWritePermit,validated}
  | _ => .error "PayloadAccessCodec"

def createPayload (ctx : Context) (state : State) (p : PayloadRecord) : Except String (HandleIdentity × State) := do
  let p := {p with connection := ctx.connection}
  let _ ← PayloadRecord.decode p.encode
  if p.extensions.any (fun e => e.value.isSome && !e.requestAllowed) then throw "PayloadInitialExtensionPhase"
  if p.transaction.domain.toNat != ctx.domain || p.transaction.owner.toNat != ctx.owner || p.hop.domain.toNat != ctx.domain || p.hop.owner.toNat != ctx.owner || p.instanceId != ctx.instanceId then throw "PayloadOwner"
  let state ← configureAllocator state {kind := .hop,store := 22,group := "storage.payload-view",persistent := false,capacity := some 128}
  allocate state ctx .hop 22 payloadTag p.encode

def createPayloadAttempt (ctx : Context) (p : PayloadRecord) : Attempt HandleIdentity :=
  modifyChecked (fun state => createPayload ctx state p)

def bindPayload (ctx : Context) (view : HandleIdentity) (access : PayloadAccess) : Context :=
  {ctx with environment := [("storage.payload.current",.handle view),("storage.payload.access",access.encode)] ++ ctx.environment.filter (fun x => x.1 != "storage.payload.current" && x.1 != "storage.payload.access")}

private def currentPayload (ctx : Context) (state : State) (transaction : HandleIdentity) : Except String (HandleIdentity × PayloadRecord × PayloadAccess) := do
  let some (.handle identity) := ctx.environment.lookup "storage.payload.current" | throw "PayloadScopeNotBound"
  let some access := ctx.environment.lookup "storage.payload.access" | throw "PayloadAccessNotBound"
  let p ← PayloadRecord.decode (← getObject state ctx identity payloadTag).value
  let a ← PayloadAccess.decode access
  if transaction.kind != .transaction || transaction != p.transaction || transaction.domain.toNat != ctx.domain || transaction.owner.toNat != ctx.owner || p.instanceId != ctx.instanceId || p.connection != ctx.connection || a.hop != p.hop || a.localSide != p.localSide then throw "PayloadWrongLocalView"
  if !a.validated || a.phase < 1 || a.phase > 4 || a.forward != (a.phase == 1 || a.phase == 4) then throw "PayloadPhaseViolation"
  if a.returned && a.sync > 2 then throw "PayloadSyncViolation"
  if a.returned && a.phase == 1 && a.sync == 1 && a.returnedPhase != 2 && a.returnedPhase != 3 then throw "PayloadUpdatedPhase"
  pure (identity,p,a)

private def writablePayload (p : PayloadRecord) (a : PayloadAccess) : Except String Unit := do
  if !p.target || !p.writable || !a.responseWritePermit || (a.phase != 1 && a.phase != 3) then throw "PayloadWritePermission"

def readPayloadField (p : PayloadRecord) : String → Except String Value
  | "command" => .ok (.bits 64 p.command)
  | "address" => .ok (.bits 64 p.address)
  | "streaming_width" => .ok (.bits 64 p.streamingWidth)
  | "data" => .ok (.bytes p.data)
  | "byte_enable" => .ok (.bytes p.byteEnable)
  | "status" => .ok (.bits 64 p.status)
  | "dmi_hint" => .ok (.bool p.dmiHint)
  | _ => .error "UnknownPayloadField"

private def writePayloadField (p : PayloadRecord) (name : String) (value : Value) : Except String PayloadRecord := do
  match name,value with
  | "data",.bytes bytes =>
    if p.command != 0 then throw "PayloadWriteCommand"
    if bytes.length != p.baseline.length then throw "PayloadDataLength"
    let data := bytes.zipIdx |>.map fun (byte,index) =>
      if p.byteEnable.isEmpty || (p.byteEnable[index % p.byteEnable.length]!).toNat != 0 then byte else p.baseline[index]!
    pure {p with data}
  | "status",.bits 64 status =>
    if status > 6 then throw "PayloadStatusRange"
    pure {p with status}
  | "dmi_hint",.bool dmiHint => pure {p with dmiHint}
  | _,_ => throw "PayloadFrozenFieldOrType"

structure Operation where
  extension : Bool := false
  write : Bool := false
  name : String
  deriving Repr, BEq

def parsePayload (key : String) : Except String Operation := do
  let write := key.endsWith ".write"
  if !write && !key.endsWith ".get" then throw "PayloadProviderSuffix"
  let stem := String.ofList (key.toList.take (key.length - (if write then 6 else 4)))
  let extension := stem.startsWith "leanat.extension."
  if !extension && !stem.startsWith "leanat.payload." then throw "PayloadProviderPrefix"
  let name := String.ofList (stem.toList.drop (if extension then 17 else 15))
  if name.isEmpty then throw "PayloadProviderName"
  if !extension && !["command","address","data","streaming_width","byte_enable","status","dmi_hint"].contains name then throw "PayloadProviderField"
  if !extension && write && !["data","status","dmi_hint"].contains name then throw "PayloadProviderFrozenField"
  pure {extension,write,name}

private def shape (types : TypeEnvironment) (op : Operation) (id : Nat) : Except String String := do
  let some ty := types[id]? | throw "PayloadProviderType"
  if op.extension && !op.write then
    let .variant [[],[inner]] := ty | throw "PayloadOptionType"
    let some (.bytes bound) := types[inner]? | throw "PayloadOptionBytes"
    return s!"option(bytes:{bound})"
  if op.extension || op.name == "data" || op.name == "byte_enable" then
    let .bytes bound := ty | throw "PayloadBytesType"
    return s!"bytes:{bound}"
  if op.name == "dmi_hint" then
    if ty != .bool then throw "PayloadBoolType"
    return "bool"
  if ty != .bits 64 then throw "PayloadUInt64Type"
  pure "u64"

def payloadSignature (types : TypeEnvironment) (id : Nat) (key : String) (transactionType valueType unitType : Nat) : Except String ServiceSignature := do
  let op ← parsePayload key
  if types[transactionType]? != some (.handle .transaction) then throw "PayloadTransactionType"
  if op.write && types[unitType]? != some .unit then throw "PayloadWriteResultType"
  let valueShape ← shape types op valueType
  let canonical := "LeanAT.Payload.v1|" ++ key ++ "|txn" ++ (if op.write then "," ++ valueShape ++ "->unit" else "->" ++ valueShape)
  pure {id,op := if op.extension then (if op.write then .bufferExtensionWrite else .extensionGet) else (if op.write then .bufferPayloadWrite else .payloadGet), inputTypes := if op.write then [transactionType,valueType] else [transactionType],resultTypes := [if op.write then unitType else valueType],contextMask := 7,effectMask := 8,extraFuel := 1,providerKey := key,providerVersion := "1",abiHash := ABI.sha256 canonical.toUTF8}

private def fromHex (s : String) : ByteArray :=
  let ns := s.toList.map fun c => if c ≤ '9' then c.toNat - '0'.toNat else c.toNat - 'a'.toNat + 10
  ⟨(List.range (ns.length/2)).toArray.map (fun i => UInt8.ofNat (ns[2*i]! * 16 + ns[2*i+1]!))⟩

def coreIdentity : Op → Except String (String × ByteArray)
  | .scheduleEvent => .ok ("leanat.core.event.schedule",fromHex "bffe39a0efb9a6f7c75fbb789e564aac1286c4f9b6c00e6f2971d896c39b8705")
  | .cancelEvent => .ok ("leanat.core.event.cancel",fromHex "214aeb1ccb48bbaeabe889924cbb909b6f2049b64b7fd587fff0fb25e8ffc479")
  | .resultGet => .ok ("leanat.core.result.get",fromHex "f40fcd8a22c18448a9b8b9bef88c7eca9dc150e28444ab9e4b2465ee6de587d0")
  | .resultRelease => .ok ("leanat.core.result.release",fromHex "21c278f5ed404ee7e67528ae212eab663d65b69ea2c08440980a21a244d673e7")
  | _ => .error "UnsupportedStorageCore"

def supports (s : ServiceSignature) : Bool := [.payloadGet,.bufferPayloadWrite,.extensionGet,.bufferExtensionWrite,.resultGet,.resultRelease,.scheduleEvent,.cancelEvent].contains s.op

def validate : ProviderValidate := fun types s => do
  if !supports s then throw "UnsupportedStorageProvider"
  if [.payloadGet,.bufferPayloadWrite,.extensionGet,.bufferExtensionWrite].contains s.op then
    let op ← parsePayload s.providerKey
    if s.inputTypes.length != (if op.write then 2 else 1) || s.resultTypes.length != 1 then throw "PayloadProviderArity"
    let expected ← payloadSignature types s.id s.providerKey s.inputTypes[0]! (if op.write then s.inputTypes[1]! else s.resultTypes[0]!) s.resultTypes[0]!
    if s.op != expected.op || s.inputTypes != expected.inputTypes || s.resultTypes != expected.resultTypes || s.contextMask != expected.contextMask || s.effectMask != expected.effectMask || s.extraFuel != expected.extraFuel || s.providerVersion != expected.providerVersion || s.abiHash != expected.abiHash then throw "PayloadProviderABIMismatch"
  else
    let (key,hash) ← coreIdentity s.op
    if s.providerKey != key || s.providerVersion != "1" || s.abiHash != hash || s.extraFuel != 0 || s.contextMask == 0 || Nat.land s.contextMask 3 != s.contextMask || s.effectMask != (if s.op == .resultGet || s.op == .resultRelease then 64 else 8) then throw "StorageProviderABIMismatch"
    let inputs := s.inputTypes.map (fun i => types[i]?)
    let outputs := s.resultTypes.map (fun i => types[i]?)
    let valid := match s.op,inputs,outputs with
      | .scheduleEvent,[some (.bits 64),some _],[some (.handle .event)] => true
      | .cancelEvent,[some (.handle .event)],[some .bool] => true
      | .resultGet,[some (.handle .result),some (.handle .consumer)],[some _] => true
      | .resultRelease,[some (.handle .result),some (.handle .consumer)],[some .unit] => true
      | _,_,_ => false
    if !valid then throw "StorageProviderTypeABI"

private def invokePayload (types : TypeEnvironment) (ctx : Context) (s : ServiceSignature) (args : List Value) (state : State) : Except String (List Value × State) := do
  let op ← parsePayload s.providerKey
  let some (.handle transaction) := (args[0]? : Option Value) | throw "PayloadTransactionArgument"
  let (identity,p,a) ← currentPayload ctx state transaction
  if op.extension then
    let some ext := p.extensions.find? (fun e => e.name == op.name) | throw "UnregisteredExtension"
    let bound ← if op.write then do
      let some (.bytes bound) := types[s.inputTypes[1]!]? | throw "ExtensionSignatureBytes"
      pure bound
    else do
      let some (.variant [[],[inner]]) := types[s.resultTypes[0]!]? | throw "ExtensionSignatureOption"
      let some (.bytes bound) := types[inner]? | throw "ExtensionSignatureBytes"
      pure bound
    if bound != ext.maxBytes then throw "ExtensionSignatureBound"
  if op.write then
    writablePayload p a
    let some value := args[1]? | throw "PayloadWriteArgument"
    let next ← if op.extension then do
      let some ext := p.extensions.find? (fun e => e.name == op.name) | throw "UnregisteredExtension"
      if !ext.responseWritable then throw "ExtensionWritePermission"
      let .bytes bytes := value | throw "ExtensionWriteType"
      if bytes.length > ext.maxBytes then throw "ExtensionWriteCapacity"
      pure {p with extensions := p.extensions.map (fun e => if e.name == op.name then {e with value := some bytes} else e)}
    else writePayloadField p op.name value
    let _ ← PayloadRecord.decode next.encode
    pure ([.unit],← putObject state ctx identity payloadTag next.encode)
  else if op.extension then
    let some ext := p.extensions.find? (fun e => e.name == op.name) | throw "UnregisteredExtension"
    pure ([match ext.value with | none => .variant 0 [] | some bytes => .variant 1 [.bytes bytes]],state)
  else pure ([← readPayloadField p op.name],state)

private def invokeCore (ctx : Context) (s : ServiceSignature) (args : List Value) (state : State) : Except String (List Value × State) := do
  match s.op,args with
  | .resultGet,[.handle result,.handle consumer] => pure ([← getResult ctx state result consumer],state)
  | .resultRelease,[.handle result,.handle consumer] => pure ([.unit],← releaseResult ctx state result consumer)
  | .scheduleEvent,[.bits 64 time,value] =>
    let (identity,next) ← enqueue state ctx time 3 ctx.connection "storage.event" [value]
    pure ([.handle identity],next)
  | .cancelEvent,[.handle identity] =>
    if identity.kind != .event || identity.domain.toNat != ctx.domain || identity.owner.toNat != ctx.owner then throw "EventAuthority"
    let some item := state.objects.find? (fun e => e.identity == identity) | throw "StaleEventHandle"
    if item.tag != "reference.event" then throw "EventObjectKind"
    let queued := state.events.find? (fun e => e.identity == identity)
    if !item.alive || queued.any Event.cancelled then pure ([.bool false],state)
    else pure ([.bool true],← Reference.cancelEvent state ctx identity)
  | _,_ => throw "StorageProviderArguments"

def invokeAttempt (types : TypeEnvironment) (ctx : Context) (s : ServiceSignature) (args : List Value) : Attempt (List Value) := atomicAttempt do
  fromExcept (validate types s)
  fromExcept (checkContext ctx)
  fromExcept (checkCapacity (← get))
  if Nat.land s.contextMask (2^ctx.kind) == 0 then throw "StorageProviderContext"
  if args.length != s.inputTypes.length then throw "StorageProviderArity"
  for (typeId,value) in s.inputTypes.zip args do
    if !(types[typeId]?).any (fun t => conforms types (types.length+1) t value) then throw "StorageProviderArgumentType"
  let values ← if [.payloadGet,.bufferPayloadWrite,.extensionGet,.bufferExtensionWrite].contains s.op then
    modifyChecked (fun state => invokePayload types ctx s args state)
  else match s.op,args with
    | .scheduleEvent,[.bits 64 time,value] => do
      if (← fromExcept (chargeValue ctx value)) > 65536 then throw "EventValueCapacity"
      let event ← enqueueAttempt ctx time 3 ctx.connection "storage.event" [value]
      pure [.handle event]
    | .resultRelease,[.handle result,.handle consumer] => do
      releaseResultAttempt ctx result consumer
      pure [.unit]
    | _,_ => modifyChecked (fun state => invokeCore ctx s args state)
  if values.length != s.resultTypes.length then throw "StorageProviderResultArity"
  for (typeId,value) in s.resultTypes.zip values do
    if !(types[typeId]?).any (fun t => conforms types (types.length+1) t value) then throw "StorageProviderResultType"
  pure values

def invoke : ProviderInvoke := fun types ctx s args state =>
  attemptExcept ((invokeAttempt types ctx s args).run state)
end LeanAT.Reference.Storage








