import LeanAT.ExecIR.Schema

namespace LeanAT.Reference
structure Context where
  kind : Nat := 0
  now : Nat := 0
  turn : Nat := 0
  domain : Nat := 0
  instanceId : Nat := 0
  connection : Nat := 0
  owner : Nat := 0
  processIdentity : Option HandleIdentity := none
  inputs : List (Nat × Value) := []
  environment : List (String × Value) := []
  deriving Repr, BEq
structure ObjectEntry where
  identity : HandleIdentity
  tag : String
  value : Value
  alive : Bool := true
  deriving Repr, BEq
structure Event where
  identity : HandleIdentity
  time : Nat
  turn : Nat
  stage : Nat
  instanceId : Nat
  connection : Nat
  sequence : Nat
  kind : String
  values : List Value := []
  source : String := ""
  cancelled : Bool := false
  deriving Repr, BEq
structure Observation where
  kind : String
  opcode : String := ""
  source : String := ""
  time : Nat := 0
  turn : Nat := 0
  values : List Value := []
  deriving Repr, BEq
structure AllocationRule where
  kind : HandleKind
  store : Nat
  group : String
  perSlot : Bool := false
  persistent : Bool := true
  allowMax : Bool := true
  capacity : Option Nat := none
  deriving Repr, BEq
structure AllocationCounter where
  group : String
  domain : Nat
  slot : Option Nat := none
  nextGeneration : Nat := 1
  persistent : Bool := true
  retired : Bool := false
  deriving Repr, BEq
structure State where
  objects : List ObjectEntry := []
  events : List Event := []
  observations : List Observation := []
  allocationRules : List AllocationRule := []
  allocationCounters : List AllocationCounter := []
  nextGeneration : Nat := 1
  generationLimit : Nat := 2^64
  nextSequence : Nat := 1
  maxObjects : Nat := 1024
  maxPins : Nat := 256
  maxEvents : Nat := 1024
  maxBytes : Nat := 1048576
  maxObservations : Nat := 100000
  deriving Repr, BEq
abbrev Attempt := EStateM String State
def fromExcept (value : Except String α) : Attempt α :=
  match value with | .ok value => pure value | .error error => throw error
def modifyChecked (f : State → Except String (α × State)) : Attempt α := do
  let (value,next) ← fromExcept (f (← get))
  set next
  pure value
def updateChecked (f : State → Except String State) : Attempt Unit := do
  set (← fromExcept (f (← get)))
def attemptExcept (result : EStateM.Result String State α) : Except String (α × State) :=
  match result with | .ok value state => pure (value,state) | .error error _ => throw error

def allocationRule (state : State) (kind : HandleKind) (store : Nat) : AllocationRule :=
  (state.allocationRules.find? (fun r => r.kind == kind && r.store == store)).getD
    (if kind == .event && store == 1 then ⟨kind,store,"runtime.event",true,true,true,some state.maxEvents⟩
    else if (kind == .process && store == 10) || (kind == .wait && store == 11) then ⟨kind,store,"runtime.process",false,true,false,none⟩
    else if store == 20 then ⟨kind,store,"storage.result",false,true,true,some 128⟩
    else if store == 21 then ⟨kind,store,"storage.consumer",false,true,true,some 512⟩
    else if store == 401 then ⟨kind,store,"managed.lease",true,true,true,none⟩
    else if store == 402 then ⟨kind,store,"managed.access",true,true,true,none⟩
    else ⟨kind,store,"logical." ++ toString store ++ "." ++ reprStr kind,false,true,true,none⟩)
def configureAllocator (state : State) (rule : AllocationRule) : Except String State := do
  if rule.store ≥ 2^32 || rule.group.isEmpty then throw "AllocatorRule"
  if rule.capacity.any (fun capacity => capacity > 2^32) then throw "AllocatorCapacityRange"
  if let some old := state.allocationRules.find? (fun r => r.kind == rule.kind && r.store == rule.store) then
    if old != rule then throw "AllocatorRuleConflict"
    return state
  pure {state with allocationRules := state.allocationRules ++ [rule]}

def rollbackAllocations (before draft : State) : State := Id.run do
  let surviving := draft.allocationCounters.filter AllocationCounter.persistent
  let advanced := surviving.filter (fun counter =>
    match before.allocationCounters.find? (fun old => old.group == counter.group &&
        old.domain == counter.domain && old.slot == counter.slot) with
    | none => true
    | some old => counter.nextGeneration > old.nextGeneration)
  -- A first failed allocation may introduce its allocator's immutable policy.
  -- Keep only policies needed to interpret an irreversible surviving journal.
  let candidates := draft.allocationRules ++ draft.objects.map (fun item =>
    allocationRule draft item.identity.kind item.identity.store.toNat)
  let rules := candidates.foldl (fun rules rule =>
    if rule.persistent && surviving.any (fun counter => counter.group == rule.group) &&
        !before.objects.any (fun item => item.identity.kind == rule.kind && item.identity.store.toNat == rule.store) &&
        !rules.any (fun old => old.kind == rule.kind && old.store == rule.store)
    then rules ++ [rule] else rules) before.allocationRules
  return {before with
    nextGeneration := (advanced.map AllocationCounter.nextGeneration).foldl max before.nextGeneration
    nextSequence := max before.nextSequence draft.nextSequence
    allocationRules := rules
    allocationCounters := surviving ++
      before.allocationCounters.filter (fun counter => !counter.persistent)}
/-- Conservative owning-value storage charge, independent of native addresses. -/
def valueBytes : Value → Nat
  | .unit => 1 | .bool _ => 1 | .bits width _ => 8+(width+7)/8
  | .bytes bytes => 8+bytes.length | .handle _ => 29
  | .record fields | .vec fields => 8+(fields.map valueBytes).sum
  | .variant _ fields => 16+(fields.map valueBytes).sum

/-- Native compact materialization charge. The profile supplies measured sizeof(Value);
    adapters must separately expose spare capacity if they retain any. -/
def nativeValueBytes (nodeBytes : Nat) : Value → Nat
  | .bytes bytes => nodeBytes+bytes.length
  | .record fields | .vec fields => nodeBytes+(fields.map (nativeValueBytes nodeBytes)).sum
  | .variant _ fields => 3*nodeBytes+(fields.map (nativeValueBytes nodeBytes)).sum
  | _ => nodeBytes
def chargeValue (context : Context) (value : Value) : Except String Nat := do
  let some (.bits 64 nodeBytes) := context.environment.lookup "profile.valueNodeBytes" | throw "MissingValueABI"
  if nodeBytes == 0 || nodeBytes > 1024 then throw "InvalidValueABI"
  pure (nativeValueBytes nodeBytes value)
def ownedBytes (state : State) : Nat :=
  (state.allocationRules.map (fun rule => 32+rule.group.utf8ByteSize)).sum +
  (state.allocationCounters.map (fun counter => 40+counter.group.utf8ByteSize)).sum +
  (state.objects.map (fun item => 64+item.tag.utf8ByteSize+valueBytes item.value)).sum +
  (state.events.map (fun event => 96+event.kind.utf8ByteSize+event.source.utf8ByteSize+(event.values.map valueBytes).sum)).sum +
  (state.observations.map (fun observation => 32+observation.kind.utf8ByteSize+observation.opcode.utf8ByteSize+observation.source.utf8ByteSize+(observation.values.map valueBytes).sum)).sum
def checkCapacity (state : State) : Except String Unit := do
  if state.objects.length > state.maxObjects || state.events.length > state.maxEvents || state.observations.length > state.maxObservations || ownedBytes state > state.maxBytes then throw "ReferenceCapacity"

private def lexBefore : List Nat → List Nat → Bool
  | a::as, b::bs => a < b || (a == b && lexBefore as bs)
  | [], _::_ => true
  | _, _ => false
def eventBefore (a b : Event) : Bool :=
  lexBefore [a.time,a.turn,a.stage,a.instanceId,a.connection,a.sequence] [b.time,b.turn,b.stage,b.instanceId,b.connection,b.sequence]

/-- Independent namespace policy: imported rules cannot redefine native allocators
    or borrow a protected group's counter for an unrelated handle store. -/
private def protectedAllocatorRules (state : State) : List AllocationRule := [
  ⟨.event,1,"runtime.event",true,true,true,some state.maxEvents⟩,
  ⟨.process,10,"runtime.process",false,true,false,none⟩,
  ⟨.wait,11,"runtime.process",false,true,false,none⟩,
  ⟨.result,20,"storage.result",false,true,true,some 128⟩,
  ⟨.consumer,21,"storage.consumer",false,true,true,some 512⟩,
  ⟨.lease,401,"managed.lease",true,true,true,none⟩,
  ⟨.access,402,"managed.access",true,true,true,none⟩,
  ⟨.scope,130,"structured.scope",false,true,false,none⟩,
  ⟨.task,131,"structured.task",false,true,false,none⟩,
  ⟨.spawnTicket,133,"structured.task",false,true,false,none⟩,
  ⟨.wait,132,"structured.wait",false,true,false,none⟩]

private def sameAllocatorPolicy (a b : AllocationRule) : Bool :=
  a.group == b.group && a.perSlot == b.perSlot &&
    a.persistent == b.persistent && a.allowMax == b.allowMax

/-- Validate imported owned state before any service is executed. -/
def validateState (state : State) : Except String Unit := do
  checkCapacity state
  let counterKeys := state.allocationCounters.map (fun counter => (counter.group,counter.domain,counter.slot))
  if counterKeys.eraseDups.length != counterKeys.length then throw "DuplicateAllocatorCounter"
  if state.allocationCounters.any (fun counter => counter.group.isEmpty || counter.domain ≥ 2^32 || counter.slot.any (fun slot => slot ≥ 2^32) || counter.nextGeneration == 0 || counter.nextGeneration > 2^64 || (counter.retired && counter.nextGeneration != 2^64)) then throw "AllocatorCounterRange"
  let canonical := protectedAllocatorRules state
  let ruleKeys := state.allocationRules.map (fun rule => (rule.kind,rule.store))
  if ruleKeys.eraseDups.length != ruleKeys.length then throw "DuplicateAllocatorRule"
  if state.allocationRules.any (fun rule => rule.group.isEmpty || rule.store ≥ 2^32 || rule.capacity.any (fun capacity => capacity > 2^32)) then throw "AllocatorRuleRange"
  if state.nextGeneration == 0 || state.nextGeneration > state.generationLimit || state.generationLimit != 2^64 || state.nextSequence ≥ 2^64 then throw "ReferenceCounterRange"
  let ids := state.objects.map ObjectEntry.identity
  if ids.eraseDups.length != ids.length then throw "DuplicateReferenceIdentity"
  let slots := state.objects.map (fun item => (item.identity.kind,item.identity.domain,item.identity.store,item.identity.slot))
  if slots.eraseDups.length != slots.length then throw "DuplicateReferenceSlot"
  if state.objects.any (fun item => item.identity.generation == 0 || item.identity.generation.toNat ≥ state.nextGeneration) then throw "ReferenceGenerationHighWater"
  let effective := state.allocationRules ++ state.objects.map (fun item =>
    allocationRule state item.identity.kind item.identity.store.toNat)
  for rule in effective do
    if let some policy := canonical.find? (fun p => p.kind == rule.kind && p.store == rule.store) then
      if !sameAllocatorPolicy policy rule then throw "ProtectedAllocatorPolicy"
    if canonical.any (fun p => p.group == rule.group) &&
        !canonical.any (fun p => p.group == rule.group && p.kind == rule.kind && p.store == rule.store) then
      throw "ProtectedAllocatorNamespace"
    if effective.any (fun other => other.group == rule.group && !sameAllocatorPolicy other rule) then
      throw "AllocatorGroupPolicy"
  for counter in state.allocationCounters do
    let policies := effective.filter (fun rule => rule.group == counter.group)
    let policies := if policies.isEmpty then canonical.filter (fun rule => rule.group == counter.group) else policies
    if policies.isEmpty then throw "UnknownAllocatorCounter"
    for policy in policies do
      if counter.persistent != policy.persistent || counter.slot.isSome != policy.perSlot then
        throw "AllocatorCounterPolicy"
      if !policy.allowMax && counter.nextGeneration > 2^64-1 then throw "AllocatorCounterRange"
      if counter.slot.any (fun slot => policy.capacity.any (fun capacity => slot ≥ capacity)) then
        throw "AllocatorSlotCapacity"
    if counter.retired && counter.group != "runtime.event" then throw "AllocatorRetirementPolicy"
  for item in state.objects do
    let rule := allocationRule state item.identity.kind item.identity.store.toNat
    if rule.capacity.any (fun capacity => item.identity.slot.toNat ≥ capacity) then throw "AllocatorSlotCapacity"
    if !rule.allowMax && item.identity.generation.toNat ≥ 2^64-1 then throw "AllocatorIdentityRange"
    let slot := if rule.perSlot then some item.identity.slot.toNat else none
    if let some counter := state.allocationCounters.find? (fun counter => counter.group == rule.group && counter.domain == item.identity.domain.toNat && counter.slot == slot) then
      if counter.nextGeneration ≤ item.identity.generation.toNat || counter.persistent != rule.persistent || (counter.retired && item.alive) then throw "AllocatorCounterIdentity"
  if (state.events.map Event.identity).eraseDups.length != state.events.length || (state.events.map Event.sequence).eraseDups.length != state.events.length then throw "DuplicateReferenceEvent"
  if state.events.any (fun event => event.time ≥ 2^64 || event.turn ≥ 2^64 || event.stage > 6 || event.instanceId ≥ 2^32 || event.connection ≥ 2^32 || event.sequence == 0) then throw "ReferenceEventKeyRange"
  if !(state.events.zip (state.events.drop 1)).all (fun (a,b) => eventBefore a b) then throw "ReferenceEventOrder"
  if state.events.any (fun event => event.identity.kind != .event || event.sequence ≥ state.nextSequence || !state.objects.any (fun item => item.alive && item.identity == event.identity && item.tag == "reference.event" && item.value == .bits 64 event.sequence)) then throw "ReferenceEventIdentity"

/-- Entry validation does not allocate or reserve any identity. -/
def reserveSegment (state : State) : Except String (State × Nat) := do
  validateState state
  pure (state,state.nextGeneration)
def finishSegment (state : State) (highWater : Nat) : State :=
  {state with nextGeneration := max state.nextGeneration highWater,generationLimit := 2^64}
def checkContext (context : Context) : Except String Unit := do
  if context.kind > 4 || context.now ≥ 2^64 || context.turn ≥ 2^64 || context.domain ≥ 2^32 || context.instanceId ≥ 2^32 || context.connection ≥ 2^32 || context.owner ≥ 2^64 then throw "ReferenceContextRange"

def getObject (state : State) (context : Context) (identity : HandleIdentity) (tag : String) : Except String ObjectEntry := do
  if identity.domain.toNat != context.domain then throw "WrongDomain"
  if identity.owner.toNat != context.owner then throw "WrongOwner"
  let some item := state.objects.find? (fun item => item.identity == identity && item.alive) | throw "StaleReferenceHandle"
  if item.tag != tag then throw "ReferenceObjectKind"
  pure item

def putObject (state : State) (context : Context) (identity : HandleIdentity) (tag : String) (value : Value) : Except String State := do
  let _ ← getObject state context identity tag
  let next := {state with objects := state.objects.map (fun item => if item.identity == identity then {item with value} else item)}
  checkCapacity next
  pure next

def releaseObject (state : State) (context : Context) (identity : HandleIdentity) (tag : String) : Except String State := do
  let _ ← getObject state context identity tag
  pure {state with objects := state.objects.map (fun item => if item.identity == identity then {item with alive := false,value := .unit} else item)}

def previewAllocation (state : State) (context : Context) (kind : HandleKind) (store : Nat) : Except String HandleIdentity := do
  checkContext context
  if store ≥ 2^32 then throw "ReferenceIdentityExhausted"
  let rule := allocationRule state kind store
  let generationAt := fun slot =>
    let key := if rule.perSlot then some slot else none
    let imported := state.objects.filter (fun entry => entry.identity.domain.toNat == context.domain &&
      (allocationRule state entry.identity.kind entry.identity.store.toNat).group == rule.group &&
      (!rule.perSlot || entry.identity.slot.toNat == slot))
    let baseline := (imported.map (fun entry => entry.identity.generation.toNat+1)).foldl max 1
    ((state.allocationCounters.find? (fun counter => counter.group == rule.group && counter.domain == context.domain && counter.slot == key)).map AllocationCounter.nextGeneration).getD baseline
  let available := fun slot => !state.objects.any (fun entry => entry.alive && entry.identity.kind == kind && entry.identity.domain.toNat == context.domain && entry.identity.store.toNat == store && entry.identity.slot.toNat == slot) &&
    generationAt slot < (if rule.allowMax then 2^64 else 2^64-1)
  let capacity := min state.maxObjects (rule.capacity.getD state.maxObjects)
  let searchBound := min capacity (state.objects.length+state.allocationCounters.length+1)
  let some slot := (List.range searchBound).find? available | throw "ReferenceCapacity"
  if slot ≥ 2^32 then throw "ReferenceSlotExhausted"
  pure ⟨kind,UInt32.ofNat context.domain,UInt32.ofNat store,UInt32.ofNat slot,UInt64.ofNat (generationAt slot),UInt64.ofNat context.owner⟩

def commitAllocation (state : State) (context : Context) (identity : HandleIdentity) (tag : String) (value : Value) : Except String (HandleIdentity × State) := do
  if (← previewAllocation state context identity.kind identity.store.toNat) != identity then throw "AllocationPreviewStale"
  let rule := allocationRule state identity.kind identity.store.toNat
  let slot := if rule.perSlot then some identity.slot.toNat else none
  let counter : AllocationCounter := ⟨rule.group,context.domain,slot,identity.generation.toNat+1,rule.persistent,false⟩
  let counters := state.allocationCounters.filter (fun old => !(old.group == counter.group && old.domain == counter.domain && old.slot == counter.slot)) ++ [counter]
  let objects := state.objects.filter (fun old => !(old.identity.kind == identity.kind && old.identity.domain == identity.domain && old.identity.store == identity.store && old.identity.slot == identity.slot)) ++ [⟨identity,tag,value,true⟩]
  let next := {state with objects,allocationCounters := counters,nextGeneration := max state.nextGeneration (identity.generation.toNat+1)}
  checkCapacity next
  pure (identity,next)

def allocate (state : State) (context : Context) (kind : HandleKind) (store : Nat) (tag : String) (value : Value) : Except String (HandleIdentity × State) := do
  commitAllocation state context (← previewAllocation state context kind store) tag value

def allocateAttempt (context : Context) (kind : HandleKind) (store : Nat) (tag : String) (value : Value) : Attempt HandleIdentity :=
  modifyChecked (fun state => allocate state context kind store tag value)

def commitAllocationAttempt (context : Context) (identity : HandleIdentity) (tag : String) (value : Value) : Attempt HandleIdentity :=
  modifyChecked (fun state => commitAllocation state context identity tag value)
def observe (state : State) (observation : Observation) : Except String State := do
  let next := {state with observations := state.observations ++ [observation]}
  checkCapacity next
  pure next

def successor (context : Context) (time : Nat) : Except String (Nat × Nat) := do
  if time < context.now then throw "TimeRegression"
  let turn := if time == context.now then context.turn+1 else 0
  if time ≥ 2^64 || turn ≥ 2^64 then throw "TimeOverflow"
  pure (time,turn)

private def insertEvent (event : Event) : List Event → List Event
  | [] => [event]
  | head::tail => if eventBefore event head then event::head::tail else head::insertEvent event tail

def enqueue (state : State) (context : Context) (time stage connection : Nat) (kind : String) (values : List Value) (source : String := "") : Except String (HandleIdentity × State) := do
  let (time,turn) ← successor context time
  if stage > 6 || connection ≥ 2^32 || state.nextSequence ≥ 2^64-1 then throw "EventKeyRange"
  let (identity,next) ← allocate state context .event 1 "reference.event" (.bits 64 state.nextSequence)
  let event : Event := ⟨identity,time,turn,stage,context.instanceId,connection,state.nextSequence,kind,values,source,false⟩
  let next := {next with events := insertEvent event next.events,nextSequence := next.nextSequence+1}
  checkCapacity next
  pure (identity,next)

def cancelEvent (state : State) (context : Context) (identity : HandleIdentity) : Except String State := do
  let _ ← getObject state context identity "reference.event"
  if !state.events.any (fun event => event.identity == identity) then throw "EventNotQueued"
  pure {state with events := state.events.map (fun event => if event.identity == identity then {event with cancelled := true} else event)}

def enqueueAttempt (context : Context) (time stage connection : Nat) (kind : String) (values : List Value) (source : String := "") : Attempt HandleIdentity := do
  fromExcept (checkContext context)
  let (time,turn) ← fromExcept (successor context time)
  let initial ← get
  if stage > 6 || connection ≥ 2^32 || initial.nextSequence ≥ 2^64-1 then throw "EventKeyRange"
  let rule := allocationRule initial .event 1
  let capacity := min initial.maxEvents (rule.capacity.getD initial.maxEvents)
  -- Only visited free slots retire. A successful reservation stops the native scan.
  let bound := min capacity (initial.objects.length+initial.allocationCounters.length+1)
  for slot in List.range bound do
    let current ← get
    let occupied := current.objects.any (fun entry => entry.alive && entry.identity.kind == .event &&
      entry.identity.domain.toNat == context.domain && entry.identity.store == 1 && entry.identity.slot.toNat == slot)
    if occupied then continue
    let imported := (current.objects.filter (fun entry => entry.identity.kind == .event &&
      entry.identity.domain.toNat == context.domain && entry.identity.store == 1 && entry.identity.slot.toNat == slot)).map
      (fun entry => entry.identity.generation.toNat+1)
    let old := current.allocationCounters.find? (fun counter => counter.group == rule.group &&
      counter.domain == context.domain && counter.slot == some slot)
    let generation := (old.map AllocationCounter.nextGeneration).getD (imported.foldl max 1)
    if generation ≥ 2^64 then
      let retired : AllocationCounter := {
        group := rule.group
        domain := context.domain
        slot := some slot
        nextGeneration := 2^64
        persistent := true
        retired := true
      }
      let counters : List AllocationCounter := current.allocationCounters.filter (fun (counter : AllocationCounter) =>
        !(counter.group == rule.group && counter.domain == context.domain && counter.slot == some slot)) ++ [retired]
      set {current with allocationCounters := counters}
      continue
    let identity ← allocateAttempt context .event 1 "reference.event" (.bits 64 current.nextSequence)
    let reserved ← get
    set {reserved with nextSequence := current.nextSequence+1}
    let event : Event := ⟨identity,time,turn,stage,context.instanceId,connection,current.nextSequence,kind,values,source,false⟩
    let next ← get
    let next := {next with events := insertEvent event next.events}
    fromExcept (checkCapacity next)
    set next
    return identity
  throw "ReferenceCapacity"
def cancelEventAttempt (context : Context) (identity : HandleIdentity) : Attempt Unit :=
  updateChecked (fun state => cancelEvent state context identity)

structure ExecutionTrace where
  layer : String
  program : Nat
  location : String
  operation : String
  args : List Value := []
  results : List Value := []
  fuelBefore : Nat
  fuelAfter : Nat
  deriving Repr, BEq
structure Outcome where
  ok : Bool
  error : String := ""
  returned : List Value := []
  committed : List Value := []
  world : State := {}
  remainingFuel : Nat := 0
  runtimeFuelRemaining : Option Nat := none
  exit : String := "returned"
  wait : Option Value := none
  resumeBlock : Option Nat := none
  outcomeType : Option Nat := none
  live : List Value := []
  trace : List ExecutionTrace := []
  deriving Repr, BEq

abbrev ProviderValidate := TypeEnvironment → ExecIR.ServiceSignature → Except String Unit
abbrev ProviderInvoke := TypeEnvironment → Context → ExecIR.ServiceSignature → List Value → State → Except String (List Value × State)
abbrev ProviderAttempt := TypeEnvironment → Context → ExecIR.ServiceSignature → List Value → Attempt (List Value)
end LeanAT.Reference









