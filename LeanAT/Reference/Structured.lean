import LeanAT.Reference.Storage
import LeanAT.Reference.Runtime
import LeanAT.ExecIR.Validation

namespace LeanAT.Reference.Structured
open ExecIR

private def u := Value.bits 64
private def nat : Value → Except String Nat
  | .bits 64 n => if n < 2^64 then .ok n else .error "ValueRange"
  | _ => .error "StructuredNatABI"
private def handle : Value → Except String HandleIdentity
  | .handle h => .ok h
  | _ => .error "StructuredHandleABI"
private def handles : Value → Except String (List HandleIdentity)
  | .vec xs => xs.mapM handle
  | _ => .error "StructuredHandlesABI"
private def own (ctx : Context) (h : HandleIdentity) : Context := {ctx with owner := h.owner.toNat}
private def object (s : State) (ctx : Context) (h : HandleIdentity) (tag : String) :=
  getObject s (own ctx h) h tag
private def put (s : State) (ctx : Context) (h : HandleIdentity) (tag : String) (v : Value) :=
  putObject s (own ctx h) h tag v
private def retire (s : State) (ctx : Context) (h : HandleIdentity) (tag : String) :=
  releaseObject s (own ctx h) h tag
private def liveIdentity (s : State) (h : HandleIdentity) := s.objects.any (fun o => o.alive && o.identity == h)
private def present (s : State) (tag : String) :=
  (s.objects.filter (fun o => o.alive && o.tag == tag)).mergeSort (fun a b => a.identity.slot.toNat ≤ b.identity.slot.toNat)

structure PoolConfig where
  frames : Nat := 1
  tasks : Nat := 64
  results : Nat := 64
  queue : Nat := 0
  waiters : Nat := 0
  argsBytes : Nat := 4096
  resultBytes : Nat := 4096
  overflow : Nat := 0 -- Reject, AwaitSlot, Queue
  resultType : Nat := 0
  deriving Repr, BEq
def encodePool (p : PoolConfig) : Value := .record ([p.frames,p.tasks,p.results,p.queue,p.waiters,p.argsBytes,p.resultBytes,p.overflow,p.resultType].map u)
private def pool (ctx : Context) : Except String PoolConfig := do
  let some (.record vs) := ctx.environment.lookup "structured.pool" | throw "MissingStructuredPool"
  let [a,b,c,d,e,f,g,h,i] ← vs.mapM nat | throw "StructuredPoolABI"
  if a == 0 || b == 0 || c == 0 || f == 0 || g == 0 || h > 2 || (h == 1 && e == 0) || (h == 2 && d == 0) then throw "StructuredPoolBounds"
  pure ⟨a,b,c,d,e,f,g,h,i⟩

structure ScopeAction where
  id : Nat
  child : HandleIdentity
  kind : Nat
  applied : Bool := false
  deriving Repr, BEq
private def encodeAction (a : ScopeAction) : Value := .record [u a.id,.handle a.child,u a.kind,.bool a.applied]
private def decodeAction : Value → Except String ScopeAction
  | .record [i,.handle h,k,.bool a] => return ⟨← nat i,h,← nat k,a⟩
  | _ => .error "ScopeActionABI"
private def ownedKind (h : HandleIdentity) : Nat := match h.kind with
  | .scope => 0 | .task => 1 | .wait => 2 | .event => 3 | .transaction => 4
  | .spawnTicket => 5 | .gateTicket => 6 | .access => 7 | .lease => 8 | .consumer => 9 | _ => 10
structure ScopeRecord where
  parent : Option HandleIdentity := none
  status : Nat := 0 -- Open, Cancelling, Cancelled, Closed
  owned : List HandleIdentity := []
  reason : Option (List UInt8) := none
  actions : List ScopeAction := []
  serial : Nat := 0
  deriving Repr, BEq
def encodeScope (r : ScopeRecord) : Value := .record [match r.parent with | none => .variant 0 [] | some h => .variant 1 [.handle h],u r.status,.vec (r.owned.map .handle),match r.reason with | none => .variant 0 [] | some bytes => .variant 1 [.bytes bytes],.vec (r.actions.map encodeAction),u r.serial]
def decodeScope : Value → Except String ScopeRecord
  | .record [p,status,owned,reason,.vec actions,serial] => do
    let parent ← match p with | .variant 0 [] => pure none | .variant 1 [.handle h] => pure (some h) | _ => throw "ScopeParentABI"
    let reason ← match reason with | .variant 0 [] => pure none | .variant 1 [.bytes r] => pure (some r) | _ => throw "ScopeReasonABI"
    pure ⟨parent,← nat status,← handles owned,reason,← actions.mapM decodeAction,← nat serial⟩
  | _ => .error "ScopeABI"
private def scope (s : State) (ctx : Context) (h : HandleIdentity) : Except String ScopeRecord := do
  if h.kind != .scope then throw "ScopeKind"
  decodeScope (← object s ctx h "structured.scope").value
private def ensureOpen (s : State) (ctx : Context) (h : HandleIdentity) : Except String Unit := do
  let mut current := some h
  for _ in List.range (s.maxObjects+1) do
    match current with
    | none => return ()
    | some h =>
      let r ← scope s ctx h
      if r.status != 0 then throw "Cancelled"
      current := r.parent
  throw "ScopeCycle"
def seedRoot (ctx : Context) (s : State) : Except String (HandleIdentity × State) := do
  let s ← configureAllocator s {kind := .scope,store := 130,group := "structured.scope",perSlot := false,persistent := true,allowMax := false,capacity := some 16}
  allocate s {ctx with owner := 0} .scope 130 "structured.scope" (encodeScope {})
private def attach (s : State) (ctx : Context) (h child : HandleIdentity) : Except String State := do
  ensureOpen s ctx h
  let r ← scope s ctx h
  if (present s "structured.scope").any (fun o => match decodeScope o.value with | .ok r => r.owned.contains child | _ => false) then throw "DuplicateOwner"
  if r.owned.length ≥ 64 then throw "Capacity"
  put s ctx h "structured.scope" (encodeScope {r with owned := r.owned ++ [child]})
private def detach (s : State) (ctx : Context) (h child : HandleIdentity) : Except String State := do
  let r ← scope s ctx h
  if !r.owned.contains child then throw "WrongOwner"
  put s ctx h "structured.scope" (encodeScope {r with owned := r.owned.filter (· != child)})
private def detachOwned (s : State) (ctx : Context) (child : HandleIdentity) : Except String State := do
  for o in present s "structured.scope" do
    let r ← decodeScope o.value
    if r.owned.contains child then return ← detach s ctx o.identity child
  throw "WrongOwner"

structure TaskRecord where
  scope : HandleIdentity
  status : Nat -- 0 queued,1 runnable,2 waiting,3 complete,4 error,5 cancelled
  args : List Value
  result : HandleIdentity
  consumer : HandleIdentity
  poolId : Nat
  start : Option HandleIdentity := none
  process : Option HandleIdentity := none
  deriving Repr, BEq
def encodeTask (r : TaskRecord) : Value := .record [.handle r.scope,u r.status,.record r.args,.handle r.result,.handle r.consumer,u r.poolId,match r.start with | none => .variant 0 [] | some h => .variant 1 [.handle h],match r.process with | none => .variant 0 [] | some h => .variant 1 [.handle h]]
def decodeTask : Value → Except String TaskRecord
  | .record [.handle scope,status,.record args,.handle result,.handle consumer,poolId,start,process] => do
    let start ← match start with | .variant 0 [] => pure none | .variant 1 [.handle h] => pure (some h) | _ => throw "TaskStartABI"
    let process ← match process with | .variant 0 [] => pure none | .variant 1 [.handle h] => pure (some h) | _ => throw "TaskProcessABI"
    pure ⟨scope,← nat status,args,result,consumer,← nat poolId,start,process⟩
  | _ => .error "TaskABI"
private def task (s : State) (ctx : Context) (h : HandleIdentity) : Except String TaskRecord := do
  if h.kind != .task then throw "TaskKind"
  decodeTask (← object s ctx h "structured.task").value
private def taskRecords (s : State) (ctx : Context) : List (HandleIdentity × TaskRecord) :=
  (present s "structured.task").filterMap fun o => match decodeTask o.value with | .ok t => if t.poolId == ctx.instanceId then some (o.identity,t) else none | _ => none
private def frameCount (s : State) (ctx : Context) := ((taskRecords s ctx).filter (fun t => t.2.status == 1 || t.2.status == 2)).length
private def reapTasks (s : State) (ctx : Context) : Except String State := do
  let mut next := s
  for (h,t) in taskRecords s ctx do
    if t.status ≥ 3 && !liveIdentity next t.result then next ← retire next ctx h "structured.task"
  pure next
private def scheduleTask (s : State) (ctx : Context) (h : HandleIdentity) (r : TaskRecord) : Except String State := do
  let program := match ctx.environment.lookup "structured.program" with | some (.bits 64 n) => n | _ => 0
  let (process,s) ← Runtime.createProcess s (own ctx h) program (some r.scope)
  let (event,s) ← enqueue s (own ctx h) ctx.now 3 ctx.connection "structured.task.start" [.handle h,.handle process] "structured"
  put s ctx h "structured.task" (encodeTask {r with status := 1,start := some event,process := some process})

structure TicketRecord where
  process : HandleIdentity
  scope : HandleIdentity
  status : Nat := 0 -- pending/granted/consumed/cancelled
  poolId : Nat
  reservation : Option (HandleIdentity × HandleIdentity × HandleIdentity) := none
  deriving Repr, BEq
def encodeTicket (r : TicketRecord) : Value := .record [.handle r.process,.handle r.scope,u r.status,u r.poolId,match r.reservation with | none => .variant 0 [] | some (t,a,b) => .variant 1 [.handle t,.handle a,.handle b]]
def decodeTicket : Value → Except String TicketRecord
  | .record [.handle process,.handle scope,status,poolId,reservation] => do
    let reservation ← match reservation with | .variant 0 [] => pure none | .variant 1 [.handle t,.handle a,.handle b] => pure (some (t,a,b)) | _ => throw "TicketReservationABI"
    pure ⟨process,scope,← nat status,← nat poolId,reservation⟩
  | _ => .error "TicketABI"
private def tickets (s : State) (ctx : Context) : List (HandleIdentity × TicketRecord) :=
  (present s "structured.ticket").filterMap fun o => match decodeTicket o.value with | .ok t => if t.poolId == ctx.instanceId then some (o.identity,t) else none | _ => none
private def grants (s : State) (ctx : Context) := ((tickets s ctx).filter (fun t => t.2.status == 1)).length
private def grantSlot (types : TypeEnvironment) (ctx : Context) (s : State) (h : HandleIdentity) (ticket : TicketRecord) : Except String State := do
  let config ← pool ctx
  let (reserved,s) ← allocate s (own ctx h) .task 131 "structured.reservation" .unit
  let (result,consumer,s) ← Storage.reserveResult types (own ctx h) s reserved config.resultType config.resultBytes h.owner.toNat
  let s ← attach s ctx ticket.scope consumer
  put s ctx h "structured.ticket" (encodeTicket {ticket with status := 1,reservation := some (reserved,result,consumer)})
private def reserveTask (types : TypeEnvironment) (s : State) (ctx : Context) (ownerScope : HandleIdentity) (args : List Value) (status : Nat) : Except String (HandleIdentity × State) := do
  let config ← pool ctx
  let s ← reapTasks s ctx
  ensureOpen s ctx ownerScope
  if (← chargeValue ctx (.record args)) > config.argsBytes then throw "TaskArgsCapacity"
  if (taskRecords s ctx).length + grants s ctx ≥ min config.tasks config.results then throw "Capacity"
  let ownerCtx := {ctx with owner := ownerScope.slot.toNat+1}
  let (h,s) ← allocate s ownerCtx .task 131 "structured.task" .unit
  let (result,consumer,s) ← Storage.reserveResult types ownerCtx s h config.resultType config.resultBytes ownerCtx.owner
  let r : TaskRecord := ⟨ownerScope,status,args,result,consumer,ctx.instanceId,none,none⟩
  let s ← put s ctx h "structured.task" (encodeTask r)
  let s ← attach s ctx ownerScope h
  let s ← attach s ctx ownerScope consumer
  pure (h, ← if status == 1 then scheduleTask s ctx h r else pure s)

def submit (types : TypeEnvironment) (s : State) (ctx : Context) (ownerScope : HandleIdentity) (args : List Value) (allowQueue : Bool) : Except String (HandleIdentity × State) := do
  let config ← pool ctx
  ensureOpen s ctx ownerScope
  let full := frameCount s ctx + grants s ctx ≥ config.frames
  if full && (!allowQueue || config.overflow != 2) then throw "Capacity"
  if full && ((taskRecords s ctx).filter (fun t => t.2.status == 0)).length ≥ config.queue then throw "Capacity"
  reserveTask types s ctx ownerScope args (if full then 0 else 1)

private def promote (types : TypeEnvironment) (s : State) (ctx : Context) : Except String State := do
  let config ← pool ctx
  let s ← reapTasks s ctx
  let queued := ((taskRecords s ctx).filter (fun t => t.2.status == 0)).mergeSort (fun a b => a.1.generation.toNat ≤ b.1.generation.toNat)
  let mut s := s
  let pending := ((tickets s ctx).filter (fun t => t.2.status == 0)).mergeSort (fun a b => a.1.generation.toNat ≤ b.1.generation.toNat)
  for (h,t) in pending do
    if frameCount s ctx + grants s ctx < config.frames && (taskRecords s ctx).length + grants s ctx < min config.tasks config.results then
      match ensureOpen s ctx t.scope with
      | .ok _ => s ← grantSlot types ctx s h t
      | .error _ => pure ()
  for (h,r) in queued do
    if frameCount s ctx + grants s ctx < config.frames then
      match ensureOpen s ctx r.scope with
      | .ok _ => s ← scheduleTask s ctx h r
      | .error _ => pure ()
  pure s

private def retireTaskProcess (s : State) (ctx : Context) (process : HandleIdentity) : Except String State := do
  let _ ← Runtime.getProcess s (own ctx process) process
  let mut next := s
  for entry in present s "reference.wait" do
    let record ← Runtime.WaitData.decode entry.value
    if record.process == process then
      next ← retire next ctx entry.identity "reference.wait"
  retire next ctx process "reference.process"

def completeTask (types : TypeEnvironment) (ctx : Context) (s : State) (h : HandleIdentity) (outcome : Value) : Except String State := do
  let r ← task s ctx h
  let .record [kind,_] := outcome | throw "TaskOutcomeABI"
  let kind ← nat kind
  if kind > 2 then throw "TaskOutcomeABI"
  if r.status ≥ 3 then throw "DuplicateTerminal"
  if r.status == 0 && kind != 2 then throw "QueuedCannotExecute"
  let s ← Storage.publishResult types (own ctx r.result) s r.result outcome
  let s ← Storage.releaseProducer (own ctx r.result) s r.result
  let s ← match r.start with
    | some event => if s.events.any (fun e => e.identity == event) then cancelEvent s (own ctx event) event else pure s
    | none => pure s
  let s ← match r.process with | some p => retireTaskProcess s ctx p | none => pure s
  let s ← put s ctx h "structured.task" (encodeTask {r with status := 3+kind,args := [],start := none,process := none})
  let s ← detachOwned s ctx h
  promote types s ctx

def requestSlot (types : TypeEnvironment) (ctx : Context) (s : State) (process ownerScope : HandleIdentity) : Except String (HandleIdentity × State) := do
  let config ← pool ctx
  if ctx.kind != 1 || config.overflow != 1 then throw "AwaitSlotContext"
  if process.kind != .process || process.domain.toNat != ctx.domain || process.owner.toNat != ctx.owner || !liveIdentity s process then throw "SlotProcess"
  ensureOpen s ctx ownerScope
  if (tickets s ctx).length ≥ config.waiters then throw "Capacity"
  let ticket : TicketRecord := ⟨process,ownerScope,0,ctx.instanceId,none⟩
  let (h,s) ← allocate s {ctx with owner := ownerScope.slot.toNat+1} .spawnTicket 133 "structured.ticket" (encodeTicket ticket)
  let s ← attach s ctx ownerScope h
  if frameCount s ctx + grants s ctx ≥ config.frames then return (h,s)
  if (taskRecords s ctx).length + grants s ctx ≥ min config.tasks config.results then return (h,s)
  pure (h, ← grantSlot types ctx s h ticket)

structure Branch where
  ordinal : Nat
  priority : Nat
  source : HandleIdentity
  result : HandleIdentity
  consumer : HandleIdentity
  ready : Option (Nat × Nat) := none
  outcome : Option Value := none
  failed : Bool := false
  pinned : Bool := false
  released : Bool := false
  deriving Repr, BEq
def encodeBranch (b : Branch) : Value := .record [u b.ordinal,u b.priority,.handle b.source,.handle b.result,.handle b.consumer,
  match b.ready with | none => .variant 0 [] | some (t,r) => .variant 1 [.record [u t,u r]],
  match b.outcome with | none => .variant 0 [] | some v => .variant 1 [v],.bool b.failed,.bool b.pinned,.bool b.released]
def decodeBranch : Value → Except String Branch
  | .record [a,b,.handle c,.handle d,.handle e,key,value,.bool failed,.bool pinned,.bool released] => do
    let ready ← match key with | .variant 0 [] => pure none | .variant 1 [.record [t,r]] => pure (some (← nat t,← nat r)) | _ => throw "BranchReadyABI"
    let outcome ← match value with | .variant 0 [] => pure none | .variant 1 [v] => pure (some v) | _ => throw "BranchOutcomeABI"
    pure ⟨← nat a,← nat b,c,d,e,ready,outcome,failed,pinned,released⟩
  | _ => .error "BranchABI"
structure WaitRecord where
  process : HandleIdentity
  scope : HandleIdentity
  mode : Nat
  count : Nat
  policy : Nat
  result : HandleIdentity
  consumer : HandleIdentity
  external : HandleIdentity
  branches : List Branch := []
  resolved : Bool := false
  deriving Repr, BEq
def encodeWait (w : WaitRecord) : Value := .record [.handle w.process,.handle w.scope,u w.mode,u w.count,u w.policy,.handle w.result,.handle w.consumer,.handle w.external,.vec (w.branches.map encodeBranch),.bool w.resolved]
def decodeWait : Value → Except String WaitRecord
  | .record [.handle p,.handle s,m,c,f,.handle r,.handle o,.handle e,.vec bs,.bool done] => return ⟨p,s,← nat m,← nat c,← nat f,r,o,e,← bs.mapM decodeBranch,done⟩
  | _ => .error "WaitGroupABI"
private def findWait (s : State) (ctx : Context) (external : HandleIdentity) : Except String (HandleIdentity × WaitRecord) := do
  if external.kind != .wait || external.domain.toNat != ctx.domain || external.owner.toNat != ctx.owner then throw "WaitOwner"
  for o in present s "structured.wait" do
    let w ← decodeWait o.value
    if w.external == external then return (o.identity,w)
  throw "StaleWaitGroup"
def createWait (types : TypeEnvironment) (ctx : Context) (s : State) (process ownerScope : HandleIdentity) (mode count policy typeId maxBytes : Nat) : Except String (HandleIdentity × State) := do
  if ctx.kind != 1 || mode > 1 || policy > 1 || count > 64 || (count == 0 && mode == 0) then throw "WaitCreateRange"
  if process.owner.toNat != ctx.owner || process.kind != .process then throw "WaitProcessOwner"
  ensureOpen s ctx ownerScope
  for o in present s "structured.wait" do
    let w ← decodeWait o.value
    if w.process == process && !w.resolved then throw "DuplicateProcessWait"
  let ownerCtx := {ctx with owner := ownerScope.slot.toNat+1}
  let (group,s) ← allocate s ownerCtx .wait 132 "structured.wait" .unit
  let (result,consumer,s) ← Storage.reserveResult types ownerCtx s group typeId maxBytes ownerCtx.owner
  let (external,s) ← Runtime.newWait s ctx process 100 0 (some group) none none
  let w : WaitRecord := ⟨process,ownerScope,mode,count,policy,result,consumer,external,[],false⟩
  let s ← put s ctx group "structured.wait" (encodeWait w)
  let s ← attach s ctx ownerScope group
  let s ← attach s ctx ownerScope consumer
  pure (external,s)
def armWait (ctx : Context) (s : State) (external : HandleIdentity) (ordinal priority : Nat) (source sourceScope : HandleIdentity) : Except String State := do
  let (group,w) ← findWait s ctx external
  if w.resolved || w.branches.length == w.count || ordinal ≥ w.count || priority ≥ 2^31 then throw "WaitArmRange"
  if w.branches.any (fun b => b.ordinal == ordinal) then throw "DuplicateWaitArm"
  let t ← task s ctx source
  if t.scope != sourceScope then throw "WrongOwner"
  let metadata ← Storage.readResult ctx s t.result
  if metadata.source != source then throw "WaitSourceIdentity"
  let (consumer,s) ← Storage.retainResult (own ctx t.consumer) s t.result t.consumer ctx.owner
  let mut branch : Branch := {ordinal,priority,source,result := t.result,consumer}
  if metadata.published && (metadata.readyTime < ctx.now || (metadata.readyTime == ctx.now && metadata.readyTurn ≤ ctx.turn)) then
    let value ← Storage.getResult (own ctx consumer) s t.result consumer
    branch := {branch with
      ready := some (metadata.readyTime,metadata.readyTurn)
      outcome := some value
      failed := match value with | .record [.bits 64 tag,_] => tag != 0 | _ => false}
  put s ctx group "structured.wait" (encodeWait {w with branches := w.branches ++ [branch]})

structure Candidate where
  branch : Branch
  time : Nat
  turn : Nat
  value : Value
  failed : Bool
private def earlier (a b : Candidate) : Bool :=
  a.time < b.time || (a.time == b.time && (a.turn < b.turn || (a.turn == b.turn &&
    (a.branch.priority < b.branch.priority || (a.branch.priority == b.branch.priority && a.branch.ordinal ≤ b.branch.ordinal)))))
private def candidate (ctx : Context) (s : State) (b : Branch) : Except String (Option Candidate) := do
  if let some key := b.ready then
    let some value := b.outcome | throw "BranchCandidateIntegrity"
    return some ⟨b,key.1,key.2,value,b.failed⟩
  let metadata ← Storage.readResult ctx s b.result
  if !metadata.published || ctx.now < metadata.readyTime || (ctx.now == metadata.readyTime && ctx.turn < metadata.readyTurn) then return none
  let value ← Storage.getResult (own ctx b.consumer) s b.result b.consumer
  let failed := match value with | .record [.bits 64 tag,_] => tag != 0 | _ => false
  pure (some ⟨b,metadata.readyTime,metadata.readyTurn,value,failed⟩)
private def aggregate (w : WaitRecord) (cs : List Candidate) : Option (Value × Nat × Nat) := Id.run do
  let ordered := cs.mergeSort earlier
  if w.mode == 0 then
    return ordered.head?.map fun c => (.record [u c.branch.ordinal,c.value],c.time,c.turn)
  let byOrdinal := cs.mergeSort (fun a b => a.branch.ordinal ≤ b.branch.ordinal)
  if w.policy == 1 then
    if let some failed := ordered.find? Candidate.failed then
      let options := (List.range w.count).map fun i => match byOrdinal.find? (fun c => c.branch.ordinal == i) with
        | some c => Value.record [.bool true,c.value]
        | none => .record [.bool false]
      return some (.record [u 1,u failed.branch.ordinal,failed.value,.record options],failed.time,failed.turn)
  if cs.length != w.count then return none
  let last := ordered.getLast?
  return some (.record [u 0,.record (byOrdinal.map Candidate.value)],last.map Candidate.time |>.getD 0,last.map Candidate.turn |>.getD 0)

/-- Explicit closed-batch epilogue: historical source keys survive publication order. -/
def closeBatch (types : TypeEnvironment) (ctx : Context) (s : State) : Except String State := do
  let mut next := s
  for o in present s "structured.wait" do
    let mut w ← decodeWait o.value
    if !w.resolved && w.branches.length == w.count then
      let mut branches := []
      for b in w.branches do
        if b.outcome.isSome then branches := branches ++ [b]
        else
          match ← candidate ctx next b with
          | none => branches := branches ++ [b]
          | some c =>
            next ← Storage.pinResult (own ctx b.consumer) next b.result b.consumer
            branches := branches ++ [{b with ready := some (c.time,c.turn),outcome := some c.value,failed := c.failed,pinned := true}]
      w := {w with branches}
      next ← put next ctx o.identity "structured.wait" (encodeWait w)
      let cs ← branches.mapM (candidate ctx next)
      if let some (value,time,turn) := aggregate w (cs.filterMap id) then
        let keyCtx := {ctx with now := if w.count == 0 then ctx.now else time,turn := if w.count == 0 then ctx.turn else turn}
        next ← Storage.publishResult types (own keyCtx w.result) next w.result value
        next ← Storage.releaseProducer (own ctx w.result) next w.result
        for branch in w.branches do
          if branch.pinned then next ← Storage.unpinResult (own ctx branch.consumer) next branch.result
          next ← Storage.releaseResult (own ctx branch.consumer) next branch.result branch.consumer
        next ← put next ctx o.identity "structured.wait" (encodeWait {w with resolved := true,branches := w.branches.map (fun b => {b with pinned := false,released := true})})
        next ← Runtime.publishWait next (own ctx w.external) w.external (keyCtx.now,keyCtx.turn) value
  pure next

private def releaseTask (ctx : Context) (s : State) (h ownerScope : HandleIdentity) : Except String State := do
  let t ← task s ctx h
  if t.scope != ownerScope then throw "WrongOwner"
  let s ← Storage.releaseResult (own ctx t.consumer) s t.result t.consumer
  let s ← detachOwned s ctx t.consumer
  if liveIdentity s t.result then pure s else retire s ctx h "structured.task"
private def releaseWait (ctx : Context) (s : State) (external ownerScope : HandleIdentity)
    (cancelling : Bool := false) : Except String State := do
  let (h,w) ← findWait s ctx external
  if w.scope != ownerScope then throw "WrongOwner"
  let mut next := s
  if liveIdentity next w.process then
    let process ← Runtime.getProcess next (own ctx w.process) w.process
    if process.wait == some external then
      if cancelling then
        if process.status == 1 then
          next ← retire next ctx w.process "reference.process"
        else
          next ← Runtime.putProcess next (own ctx w.process) w.process {process with wait := none}
      else if process.status == 1 && !w.resolved then
        throw "WaitNotReady"
  if cancelling && liveIdentity next external then
    let registered ← Runtime.getWait next (own ctx external) external
    if registered.process != w.process then throw "WrongOwner"
    next ← retire next ctx external "reference.wait"
  if !w.resolved then
    for b in w.branches do
      if b.pinned then next ← Storage.unpinResult (own ctx b.consumer) next b.result
      next ← Storage.releaseResult (own ctx b.consumer) next b.result b.consumer
    next ← Storage.releaseProducer (own ctx w.result) next w.result
  next ← Storage.releaseResult (own ctx w.consumer) next w.result w.consumer
  next ← detach next ctx ownerScope w.consumer
  next ← detach next ctx ownerScope h
  retire next ctx h "structured.wait"

private def inClosure (s : State) (ctx : Context) (root h : HandleIdentity) : Bool := Id.run do
  let mut current := some h
  for _ in List.range (s.maxObjects+1) do
    match current with
    | none => return false
    | some x =>
      if x == root then return true
      match scope s ctx x with | .ok r => current := r.parent | .error _ => return false
  return false
def cancelScope (types : TypeEnvironment) (ctx : Context) (s : State) (root : HandleIdentity) (reason : List UInt8) : Except String State := do
  if reason.length > 256 then throw "CancellationReasonCapacity"
  let r ← scope s ctx root
  if r.status == 3 then throw "ScopeClosed"
  if r.status == 2 then return s
  let closure := (present s "structured.scope").filter (fun o => inClosure s ctx root o.identity)
  let mut actionCount := 0
  for o in closure do
    let r ← decodeScope o.value
    if r.status == 0 then actionCount := actionCount+r.owned.length
  if actionCount > 256 then throw "ScopeActionCapacity"
  let mut next := s
  let some rootRecord := (present s "structured.scope").find? (fun o => match decodeScope o.value with | .ok r => r.parent.isNone | _ => false) | throw "MissingRuntimeScope"
  let serialRecord ← decodeScope rootRecord.value
  let mut serial := serialRecord.serial
  for o in closure do
    let r ← decodeScope o.value
    if r.status != 0 then continue
    let mut actions := []
    for child in r.owned do
      if serial+1 ≥ 2^64 then throw "ScopeActionOverflow"
      serial := serial+1
      actions := actions ++ [{id := serial,child,kind := ownedKind child : ScopeAction}]
    next ← put next ctx o.identity "structured.scope" (encodeScope {r with status := 1,reason := some reason,actions})
  let currentRoot ← scope next ctx rootRecord.identity
  next ← put next ctx rootRecord.identity "structured.scope" (encodeScope {currentRoot with serial})
  for o in closure do
    let r ← scope next ctx o.identity
    for child in r.owned do
      if liveIdentity next child then
        match child.kind with
        | .scope => pure () -- Descendant plan is executed by the bounded closure pass.
        | .task =>
          let t ← task next ctx child
          if t.status < 3 then next ← completeTask types ctx next child (.record [u 2,.bytes reason])
        | .consumer =>
          let c ← Storage.ConsumerRecord.decode (← object next ctx child Storage.consumerTag).value
          next ← Storage.releaseResult (own ctx child) next c.result child
        | .wait =>
          let w ← decodeWait (← object next ctx child "structured.wait").value
          next ← releaseWait (own ctx w.external) next w.external w.scope true
        | .spawnTicket =>
          let t ← decodeTicket (← object next ctx child "structured.ticket").value
          if let some (reserved,result,consumer) := t.reservation then
            if liveIdentity next consumer then next ← Storage.releaseResult (own ctx consumer) next result consumer
            next ← Storage.releaseProducer (own ctx result) next result
            next ← retire next ctx reserved "structured.reservation"
          next ← put next ctx child "structured.ticket" (encodeTicket {t with status := 3,reservation := none})
        | _ => throw "UnsupportedScopeResource"
    let r ← scope next ctx o.identity
    next ← put next ctx o.identity "structured.scope" (encodeScope {r with owned := [],status := 2,actions := r.actions.map (fun a => {a with applied := true})})
  pure next

def supports (sig : ServiceSignature) : Bool :=
  [.spawnProcess,.trySpawnProcess,.waitGroupNew,.waitArm,.waitResultGet,.waitGroupRelease,
   .scopeNew,.scopeTransfer,.scopeCancel,.scopeClose,.submitTask,.cancelTask,.taskResultGet,
   .taskResultRelease,.requestTaskSlot].contains sig.op
def providerKey := "leanat.ext.structured"
def canonicalHash (types : TypeEnvironment) (op : Op) (ins outs : List Nat) : ByteArray :=
  ABI.sha256 (ABI.str providerKey ++ ABI.str "1" ++ ABI.le op.tag 1 ++ ABI.vector ABI.encodeType types ++ ABI.vector ABI.word ins ++ ABI.vector ABI.word outs)
private def allowsTimed (op : Op) : Bool := [.spawnProcess,.trySpawnProcess,.submitTask].contains op
private def contextMask (op : Op) : Nat := if allowsTimed op then 3 else 2

def signature (types : TypeEnvironment) (id : Nat) (op : Op) (ins outs : List Nat) : ServiceSignature :=
  {id,op,inputTypes := ins,resultTypes := outs,contextMask := contextMask op,effectMask := requiredEffect op,extraFuel := 1,providerKey,providerVersion := "1",abiHash := canonicalHash types op ins outs}
private def typeAt (types : TypeEnvironment) (n : Nat) : Except String TypeSchema :=
  match types[n]? with | some t => .ok t | none => .error "StructuredTypeId"
private def isRecord : TypeSchema → Bool | .record _ => true | _ => false
private def isBytes : TypeSchema → Bool | .bytes _ => true | _ => false
private def exceptHandle (types : TypeEnvironment) (schema : TypeSchema) (kind : HandleKind) : Bool :=
  match schema with
  | .variant [[ok],[error]] => types[ok]? == some (.handle kind) && types[error]? == some (.bits 64)
  | _ => false
def validate : ProviderValidate := fun types sig => do
  if !supports sig then throw "UnsupportedStructuredProvider"
  if sig.providerKey != providerKey || sig.providerVersion != "1" || sig.abiHash != canonicalHash types sig.op sig.inputTypes sig.resultTypes ||
      sig.contextMask != contextMask sig.op || sig.effectMask != requiredEffect sig.op || sig.extraFuel != 1 then throw "StructuredProviderABI"
  let ins ← sig.inputTypes.mapM (typeAt types)
  let outs ← sig.resultTypes.mapM (typeAt types)
  let valid := match sig.op,ins,outs with
    | .scopeNew,[.handle .scope],[.handle .scope] => true
    | .scopeClose,[.handle .scope],[.unit] => true
    | .scopeTransfer,[.handle _,.handle .scope,.handle .scope],[.unit] => true
    | .scopeCancel,[.handle .scope,reason],[.unit] => isBytes reason
    | .spawnProcess,[.handle .scope,args],[.handle .task] => isRecord args
    | .trySpawnProcess,[.handle .scope,args],[out]
    | .submitTask,[.handle .scope,args],[out] => isRecord args && exceptHandle types out .task
    | .requestTaskSlot,[.handle .process,.handle .scope],[out] => exceptHandle types out .spawnTicket
    | .cancelTask,[.handle .task,reason],[.unit] => isBytes reason
    | .taskResultGet,[.handle .task,.handle .scope],[out] => isRecord out
    | .taskResultRelease,[.handle .task,.handle .scope],[.unit] => true
    | .waitGroupNew,[.handle .process,.handle .scope,.bits 64,.bits 64,.bits 64,.bits 64,.bits 64],[.handle .wait] => true
    | .waitArm,[.handle .wait,.bits 64,.bits 64,.handle .task,.handle .scope],[.unit] => true
    | .waitResultGet,[.handle .wait,.handle .scope],[out] => isRecord out
    | .waitGroupRelease,[.handle .wait,.handle .scope],[.unit] => true
    | _,_,_ => false
  if !valid then throw "StructuredProviderTypeABI"

private def scopeTransfer (ctx : Context) (s : State) (child source target : HandleIdentity) : Except String State := do
  ensureOpen s ctx source
  ensureOpen s ctx target
  for o in present s "structured.wait" do
    let w ← decodeWait o.value
    if child == o.identity || child == w.external || child == w.consumer then
      throw "WaitOwnershipPairedTransfer"
  if child.kind == .scope then
    let r ← scope s ctx child
    if r.parent != some source then throw "WrongOwner"
    if inClosure s ctx child target then throw "ScopeCycle"
    for o in present s "structured.wait" do
      let w ← decodeWait o.value
      if !w.resolved && inClosure s ctx child w.scope then throw "ActiveScopeObserver"
    let s ← detach s ctx source child
    let s ← attach s ctx target child
    put s ctx child "structured.scope" (encodeScope {r with parent := some target})
  else
    let r ← scope s ctx source
    if !r.owned.contains child then throw "WrongOwner"
    let s ← detach s ctx source child
    attach s ctx target child
private def closeScope (ctx : Context) (s : State) (h : HandleIdentity) : Except String State := do
  let r ← scope s ctx h
  if r.parent.isNone || r.status == 1 || !r.owned.isEmpty then throw "ScopeNotQuiescent"
  if (present s "structured.scope").any (fun o => match decodeScope o.value with | .ok c => c.parent == some h | _ => false) then throw "ScopeHasChildren"
  let mut next := s
  if let some parent := r.parent then
    let parentRecord ← scope next ctx parent
    if parentRecord.owned.contains h then next ← detach next ctx parent h
  retire next ctx h "structured.scope"

private def rawInvoke (types : TypeEnvironment) (ctx : Context) (op : Op) (args : List Value) (s : State) : Except String (List Value × State) := do
  match op,args with
  | .scopeNew,[.handle parent] =>
    ensureOpen s ctx parent
    let (h,s) ← allocate s {ctx with owner := 0} .scope 130 "structured.scope" (encodeScope {parent := some parent})
    pure ([.handle h],← attach s ctx parent h)
  | .scopeTransfer,[.handle child,.handle source,.handle target] => return ([.unit],← scopeTransfer ctx s child source target)
  | .scopeCancel,[.handle h,.bytes reason] => return ([.unit],← cancelScope types ctx s h reason)
  | .scopeClose,[.handle h] => return ([.unit],← closeScope ctx s h)
  | .spawnProcess,[.handle ownerScope,.record args]
  | .trySpawnProcess,[.handle ownerScope,.record args]
  | .submitTask,[.handle ownerScope,.record args] =>
    let (h,s) ← submit types s ctx ownerScope args (op == .submitTask)
    pure ([if op == .spawnProcess then .handle h else .variant 0 [.handle h]],s)
  | .requestTaskSlot,[.handle process,.handle ownerScope] =>
    let (h,s) ← requestSlot types ctx s process ownerScope
    pure ([.variant 0 [.handle h]],s)
  | .cancelTask,[.handle h,.bytes reason] => return ([.unit],← completeTask types ctx s h (.record [u 2,.bytes reason]))
  | .taskResultGet,[.handle h,.handle ownerScope] =>
    let t ← task s ctx h
    if t.scope != ownerScope then throw "WrongOwner"
    pure ([← Storage.getResult (own ctx t.consumer) s t.result t.consumer],s)
  | .taskResultRelease,[.handle h,.handle ownerScope] => return ([.unit],← releaseTask ctx s h ownerScope)
  | .waitGroupNew,[.handle process,.handle ownerScope,m,c,f,t,b] =>
    let (h,s) ← createWait types ctx s process ownerScope (← nat m) (← nat c) (← nat f) (← nat t) (← nat b)
    pure ([.handle h],s)
  | .waitArm,[.handle external,a,b,.handle source,.handle ownerScope] => return ([.unit],← armWait ctx s external (← nat a) (← nat b) source ownerScope)
  | .waitResultGet,[.handle external,.handle ownerScope] =>
    let (_,w) ← findWait s ctx external
    if w.scope != ownerScope then throw "WrongOwner"
    pure ([← Storage.getResult (own ctx w.consumer) s w.result w.consumer],s)
  | .waitGroupRelease,[.handle external,.handle ownerScope] => return ([.unit],← releaseWait ctx s external ownerScope)
  | _,_ => throw "StructuredArgumentABI"

/-- Pure prepare/commit boundary: every failure returns no replacement state. -/
def invoke : ProviderInvoke := fun types ctx sig args s => do
  validate types sig
  checkContext ctx
  if ctx.kind != 1 && !(ctx.kind == 0 && allowsTimed sig.op) then throw "StructuredContext"
  if args.length != sig.inputTypes.length || !(sig.inputTypes.zip args).all (fun (t,v) => (types[t]?).any (fun schema => conforms types (types.length+1) schema v)) then throw "StructuredArguments"
  let result := rawInvoke types ctx sig.op args s
  let (values,next) ← match result with
    | .ok r => pure r
    | .error error =>
      if [.trySpawnProcess,.submitTask,.requestTaskSlot].contains sig.op && ["Capacity","Cancelled","TaskArgsCapacity"].contains error then
        pure ([.variant 1 [u (if error == "Capacity" then 2 else if error == "Cancelled" then 20 else 0)]],s)
      else throw error
  if values.length != sig.resultTypes.length || !(sig.resultTypes.zip values).all (fun (t,v) => (types[t]?).any (fun schema => conforms types (types.length+1) schema v)) then throw "StructuredResults"
  checkCapacity next
  pure (values,next)

private def configureStructured (ctx : Context) : Attempt Unit := do
  let config ← fromExcept (pool ctx)
  for (kind,store,group) in [(.scope,130,"structured.scope"),(.task,131,"structured.task"),
      (.spawnTicket,133,"structured.task"),(.wait,132,"structured.wait")] do
    let capacity := if store == 131 then config.tasks else if store == 133 then config.waiters else 16
    updateChecked (fun s => configureAllocator s {kind,store,group,perSlot := false,persistent := true,allowMax := false,capacity := some capacity})

private def bootstrapCounter (ctx : Context) (group : String) : Attempt Unit := do
  let s ← get
  if !(s.allocationCounters.any (fun c => c.group == group && c.domain == ctx.domain)) then
    let imported := s.objects.filter (fun o =>
      o.identity.domain.toNat == ctx.domain &&
      ((group == "structured.scope" && o.identity.kind == .scope && o.identity.store.toNat == 130) ||
       (group == "structured.task" && ((o.identity.kind == .task && o.identity.store.toNat == 131) ||
         (o.identity.kind == .spawnTicket && o.identity.store.toNat == 133))) ||
       (group == "structured.wait" && o.identity.kind == .wait && o.identity.store.toNat == 132)))
    let next := (imported.map (fun o => o.identity.generation.toNat+1)).foldl max 2
    let counter : AllocationCounter := {group,domain := ctx.domain,nextGeneration := next,persistent := true}
    set {s with allocationCounters := s.allocationCounters ++ [counter]}

def initializeState (ctx : Context) (state : State) : Except String State := do
  let action : Attempt Unit := do
    configureStructured ctx
    for group in ["structured.scope","structured.task","structured.wait"] do bootstrapCounter ctx group
  let (_,next) ← attemptExcept (action.run state)
  pure next

private def scheduleTaskAttempt (ctx : Context) (h : HandleIdentity) (r : TaskRecord) : Attempt Unit := do
  let program := match ctx.environment.lookup "structured.program" with | some (.bits 64 n) => n | _ => 0
  let process ← Runtime.createProcessAttempt (own ctx h) program (some r.scope)
  let event ← enqueueAttempt (own ctx h) ctx.now 3 ctx.connection "structured.task.start" [.handle h,.handle process] "structured"
  updateChecked (fun s => put s ctx h "structured.task" (encodeTask {r with status := 1,start := some event,process := some process}))

private def grantSlotAttempt (types : TypeEnvironment) (ctx : Context) (h : HandleIdentity) (ticket : TicketRecord) : Attempt Unit := do
  let config ← fromExcept (pool ctx)
  let reserved ← allocateAttempt (own ctx h) .task 131 "structured.reservation" .unit
  let (result,consumer) ← Storage.reserveResultAttempt types (own ctx h) reserved config.resultType config.resultBytes h.owner.toNat
  updateChecked (fun s => attach s ctx ticket.scope consumer)
  updateChecked (fun s => put s ctx h "structured.ticket" (encodeTicket {ticket with status := 1,reservation := some (reserved,result,consumer)}))

def submitAttempt (types : TypeEnvironment) (ctx : Context) (ownerScope : HandleIdentity) (args : List Value) (allowQueue : Bool) : Attempt HandleIdentity := do
  let config ← fromExcept (pool ctx)
  let s ← get
  fromExcept (ensureOpen s ctx ownerScope)
  let full := frameCount s ctx + grants s ctx ≥ config.frames
  if full && (!allowQueue || config.overflow != 2) then throw "Capacity"
  if full && ((taskRecords s ctx).filter (fun t => t.2.status == 0)).length ≥ config.queue then throw "Capacity"
  updateChecked (fun s => reapTasks s ctx)
  let s ← get
  if (← fromExcept (chargeValue ctx (.record args))) > config.argsBytes then throw "TaskArgsCapacity"
  if (taskRecords s ctx).length + grants s ctx ≥ min config.tasks config.results then throw "Capacity"
  let ownerCtx := {ctx with owner := ownerScope.slot.toNat+1}
  let h ← allocateAttempt ownerCtx .task 131 "structured.task" .unit
  let (result,consumer) ← Storage.reserveResultAttempt types ownerCtx h config.resultType config.resultBytes ownerCtx.owner
  let r : TaskRecord := ⟨ownerScope,if full then 0 else 1,args,result,consumer,ctx.instanceId,none,none⟩
  updateChecked (fun s => put s ctx h "structured.task" (encodeTask r))
  updateChecked (fun s => attach s ctx ownerScope h)
  updateChecked (fun s => attach s ctx ownerScope consumer)
  if !full then scheduleTaskAttempt ctx h r
  pure h

private def promoteAttempt (types : TypeEnvironment) (ctx : Context) : Attempt Unit := do
  let config ← fromExcept (pool ctx)
  updateChecked (fun s => reapTasks s ctx)
  let initial ← get
  let queued := ((taskRecords initial ctx).filter (fun t => t.2.status == 0)).mergeSort (fun a b => a.1.generation.toNat ≤ b.1.generation.toNat)
  let pending := ((tickets initial ctx).filter (fun t => t.2.status == 0)).mergeSort (fun a b => a.1.generation.toNat ≤ b.1.generation.toNat)
  for (h,t) in pending do
    let s ← get
    if frameCount s ctx + grants s ctx < config.frames && (taskRecords s ctx).length + grants s ctx < min config.tasks config.results then
      match ensureOpen s ctx t.scope with
      | .ok _ => grantSlotAttempt types ctx h t
      | .error _ => pure ()
  for (h,r) in queued do
    let s ← get
    if frameCount s ctx + grants s ctx < config.frames then
      match ensureOpen s ctx r.scope with
      | .ok _ => scheduleTaskAttempt ctx h r
      | .error _ => pure ()

def completeTaskAttempt (types : TypeEnvironment) (ctx : Context) (h : HandleIdentity) (outcome : Value) : Attempt Unit := do
  let r ← fromExcept (task (← get) ctx h)
  let .record [kind,_] := outcome | throw "TaskOutcomeABI"
  let kind ← fromExcept (nat kind)
  if kind > 2 then throw "TaskOutcomeABI"
  if r.status ≥ 3 then throw "DuplicateTerminal"
  if r.status == 0 && kind != 2 then throw "QueuedCannotExecute"
  Storage.publishResultAttempt types (own ctx r.result) r.result outcome
  Storage.releaseProducerAttempt (own ctx r.result) r.result
  if let some event := r.start then
    if (← get).events.any (fun e => e.identity == event) then
      updateChecked (fun s => cancelEvent s (own ctx event) event)
  if let some process := r.process then
    updateChecked (fun s => retireTaskProcess s ctx process)
  updateChecked (fun s => put s ctx h "structured.task" (encodeTask {r with status := 3+kind,args := [],start := none,process := none}))
  updateChecked (fun s => detachOwned s ctx h)
  promoteAttempt types ctx

private def requestSlotAttempt (types : TypeEnvironment) (ctx : Context) (process ownerScope : HandleIdentity) : Attempt HandleIdentity := do
  let config ← fromExcept (pool ctx)
  let s ← get
  if ctx.kind != 1 || config.overflow != 1 then throw "AwaitSlotContext"
  if process.kind != .process || process.domain.toNat != ctx.domain || process.owner.toNat != ctx.owner || !liveIdentity s process then throw "SlotProcess"
  fromExcept (ensureOpen s ctx ownerScope)
  if (tickets s ctx).length ≥ config.waiters then throw "Capacity"
  let ticket : TicketRecord := ⟨process,ownerScope,0,ctx.instanceId,none⟩
  let h ← allocateAttempt {ctx with owner := ownerScope.slot.toNat+1} .spawnTicket 133 "structured.ticket" (encodeTicket ticket)
  updateChecked (fun s => attach s ctx ownerScope h)
  let s ← get
  if frameCount s ctx + grants s ctx < config.frames && (taskRecords s ctx).length + grants s ctx < min config.tasks config.results then
    grantSlotAttempt types ctx h ticket
  pure h

private def createWaitAttempt (types : TypeEnvironment) (ctx : Context) (process ownerScope : HandleIdentity)
    (mode count policy typeId maxBytes : Nat) : Attempt HandleIdentity := do
  let s ← get
  if ctx.kind != 1 || mode > 1 || policy > 1 || count > 64 || (count == 0 && mode == 0) then throw "WaitCreateRange"
  if process.owner.toNat != ctx.owner || process.kind != .process then throw "WaitProcessOwner"
  fromExcept (ensureOpen s ctx ownerScope)
  for o in present s "structured.wait" do
    let w ← fromExcept (decodeWait o.value)
    if w.process == process && !w.resolved then throw "DuplicateProcessWait"
  let ownerCtx := {ctx with owner := ownerScope.slot.toNat+1}
  let group ← allocateAttempt ownerCtx .wait 132 "structured.wait" .unit
  let (result,consumer) ← Storage.reserveResultAttempt types ownerCtx group typeId maxBytes ownerCtx.owner
  let external ← Runtime.newWaitAttempt ctx process 100 0 (some group) none none
  let w : WaitRecord := ⟨process,ownerScope,mode,count,policy,result,consumer,external,[],false⟩
  updateChecked (fun s => put s ctx group "structured.wait" (encodeWait w))
  updateChecked (fun s => attach s ctx ownerScope group)
  updateChecked (fun s => attach s ctx ownerScope consumer)
  pure external

private def armWaitAttempt (ctx : Context) (external : HandleIdentity) (ordinal priority : Nat) (source sourceScope : HandleIdentity) : Attempt Unit := do
  let s ← get
  let (group,w) ← fromExcept (findWait s ctx external)
  if w.resolved || w.branches.length == w.count || ordinal ≥ w.count || priority ≥ 2^31 then throw "WaitArmRange"
  if w.branches.any (fun b => b.ordinal == ordinal) then throw "DuplicateWaitArm"
  let t ← fromExcept (task s ctx source)
  if t.scope != sourceScope then throw "WrongOwner"
  let metadata ← fromExcept (Storage.readResult ctx s t.result)
  if metadata.source != source then throw "WaitSourceIdentity"
  let consumer ← Storage.retainResultAttempt (own ctx t.consumer) t.result t.consumer ctx.owner
  let mut branch : Branch := {ordinal,priority,source,result := t.result,consumer}
  if metadata.published && (metadata.readyTime < ctx.now || (metadata.readyTime == ctx.now && metadata.readyTurn ≤ ctx.turn)) then
    let value ← fromExcept (Storage.getResult (own ctx consumer) (← get) t.result consumer)
    branch := {branch with
      ready := some (metadata.readyTime,metadata.readyTurn)
      outcome := some value
      failed := match value with | .record [.bits 64 tag,_] => tag != 0 | _ => false}
  updateChecked (fun s => put s ctx group "structured.wait" (encodeWait {w with branches := w.branches ++ [branch]}))

def closeBatchAttempt (types : TypeEnvironment) (ctx : Context) : Attempt Unit := do
  let initial ← get
  for o in present initial "structured.wait" do
    let mut w ← fromExcept (decodeWait o.value)
    if !w.resolved && w.branches.length == w.count then
      let mut branches := []
      for b in w.branches do
        if b.outcome.isSome then branches := branches ++ [b]
        else
          match ← fromExcept (candidate ctx (← get) b) with
          | none => branches := branches ++ [b]
          | some c =>
            updateChecked (fun s => Storage.pinResult (own ctx b.consumer) s b.result b.consumer)
            branches := branches ++ [{b with ready := some (c.time,c.turn),outcome := some c.value,failed := c.failed,pinned := true}]
      w := {w with branches}
      updateChecked (fun s => put s ctx o.identity "structured.wait" (encodeWait w))
      let cs ← fromExcept (branches.mapM (candidate ctx (← get)))
      if let some (value,time,turn) := aggregate w (cs.filterMap id) then
        let keyCtx := {ctx with now := if w.count == 0 then ctx.now else time,turn := if w.count == 0 then ctx.turn else turn}
        Storage.publishResultAttempt types (own keyCtx w.result) w.result value
        Storage.releaseProducerAttempt (own ctx w.result) w.result
        for branch in w.branches do
          if branch.pinned then updateChecked (fun s => Storage.unpinResult (own ctx branch.consumer) s branch.result)
          Storage.releaseResultAttempt (own ctx branch.consumer) branch.result branch.consumer
        updateChecked (fun s => put s ctx o.identity "structured.wait" (encodeWait {w with resolved := true,branches := w.branches.map (fun b => {b with pinned := false,released := true})}))
        Runtime.publishWaitAttempt (own ctx w.external) w.external (keyCtx.now,keyCtx.turn) value

private def cancelScopeAttempt (types : TypeEnvironment) (ctx : Context) (root : HandleIdentity) (reason : List UInt8) : Attempt Unit := do
  if reason.length > 256 then throw "CancellationReasonCapacity"
  let s ← get
  let r ← fromExcept (scope s ctx root)
  if r.status == 3 then throw "ScopeClosed"
  if r.status == 2 then return ()
  let closure := (present s "structured.scope").filter (fun o => inClosure s ctx root o.identity)
  let mut actionCount := 0
  for o in closure do
    let r ← fromExcept (decodeScope o.value)
    if r.status == 0 then actionCount := actionCount+r.owned.length
  if actionCount > 256 then throw "ScopeActionCapacity"
  let some rootRecord := (present s "structured.scope").find? (fun o => match decodeScope o.value with | .ok r => r.parent.isNone | _ => false) | throw "MissingRuntimeScope"
  let serialRecord ← fromExcept (decodeScope rootRecord.value)
  let mut serial := serialRecord.serial
  for o in closure do
    let r ← fromExcept (decodeScope o.value)
    if r.status != 0 then continue
    let mut actions := []
    for child in r.owned do
      if serial+1 ≥ 2^64 then throw "ScopeActionOverflow"
      serial := serial+1
      actions := actions ++ [{id := serial,child,kind := ownedKind child : ScopeAction}]
    updateChecked (fun s => put s ctx o.identity "structured.scope" (encodeScope {r with status := 1,reason := some reason,actions}))
  let currentRoot ← fromExcept (scope (← get) ctx rootRecord.identity)
  updateChecked (fun s => put s ctx rootRecord.identity "structured.scope" (encodeScope {currentRoot with serial}))
  for o in closure do
    let r ← fromExcept (scope (← get) ctx o.identity)
    for child in r.owned do
      if liveIdentity (← get) child then
        match child.kind with
        | .scope => pure ()
        | .task =>
          let t ← fromExcept (task (← get) ctx child)
          if t.status < 3 then completeTaskAttempt types ctx child (.record [u 2,.bytes reason])
        | .consumer =>
          let c ← fromExcept (Storage.ConsumerRecord.decode (← fromExcept (object (← get) ctx child Storage.consumerTag)).value)
          Storage.releaseResultAttempt (own ctx child) c.result child
        | .wait =>
          let w ← fromExcept (decodeWait (← fromExcept (object (← get) ctx child "structured.wait")).value)
          updateChecked (fun s => releaseWait (own ctx w.external) s w.external w.scope true)
        | .spawnTicket =>
          let t ← fromExcept (decodeTicket (← fromExcept (object (← get) ctx child "structured.ticket")).value)
          if let some (reserved,result,consumer) := t.reservation then
            if liveIdentity (← get) consumer then Storage.releaseResultAttempt (own ctx consumer) result consumer
            Storage.releaseProducerAttempt (own ctx result) result
            updateChecked (fun s => retire s ctx reserved "structured.reservation")
          updateChecked (fun s => put s ctx child "structured.ticket" (encodeTicket {t with status := 3,reservation := none}))
        | _ => throw "UnsupportedScopeResource"
    let r ← fromExcept (scope (← get) ctx o.identity)
    updateChecked (fun s => put s ctx o.identity "structured.scope" (encodeScope {r with owned := [],status := 2,actions := r.actions.map (fun a => {a with applied := true})}))

private def rawInvokeAttempt (types : TypeEnvironment) (ctx : Context) (op : Op) (args : List Value) : Attempt (List Value) := do
  match op,args with
  | .scopeNew,[.handle parent] =>
    fromExcept (ensureOpen (← get) ctx parent)
    let h ← allocateAttempt {ctx with owner := 0} .scope 130 "structured.scope" (encodeScope {parent := some parent})
    updateChecked (fun s => attach s ctx parent h)
    pure [.handle h]
  | .scopeCancel,[.handle h,.bytes reason] =>
    cancelScopeAttempt types ctx h reason
    pure [.unit]
  | .spawnProcess,[.handle ownerScope,.record args]
  | .trySpawnProcess,[.handle ownerScope,.record args]
  | .submitTask,[.handle ownerScope,.record args] =>
    let h ← submitAttempt types ctx ownerScope args (op == .submitTask)
    pure [if op == .spawnProcess then .handle h else .variant 0 [.handle h]]
  | .requestTaskSlot,[.handle process,.handle ownerScope] =>
    let h ← requestSlotAttempt types ctx process ownerScope
    pure [.variant 0 [.handle h]]
  | .cancelTask,[.handle h,.bytes reason] =>
    completeTaskAttempt types ctx h (.record [u 2,.bytes reason])
    pure [.unit]
  | .waitGroupNew,[.handle process,.handle ownerScope,m,c,f,t,b] =>
    let h ← createWaitAttempt types ctx process ownerScope (← fromExcept (nat m)) (← fromExcept (nat c))
      (← fromExcept (nat f)) (← fromExcept (nat t)) (← fromExcept (nat b))
    pure [.handle h]
  | .waitArm,[.handle external,a,b,.handle source,.handle ownerScope] =>
    armWaitAttempt ctx external (← fromExcept (nat a)) (← fromExcept (nat b)) source ownerScope
    pure [.unit]
  | _,_ => modifyChecked (rawInvoke types ctx op args)

/-- Each failed provider rolls back visible effects while retaining actual persistent allocation burns. -/
def invokeAttempt (types : TypeEnvironment) (ctx : Context) (sig : ServiceSignature) (args : List Value) : Attempt (List Value) := fun initial =>
  let action : Attempt (List Value) := do
    fromExcept (validate types sig)
    fromExcept (checkContext ctx)
    if ctx.kind != 1 && !(ctx.kind == 0 && allowsTimed sig.op) then throw "StructuredContext"
    if args.length != sig.inputTypes.length || !(sig.inputTypes.zip args).all (fun (t,v) => (types[t]?).any (fun schema => conforms types (types.length+1) schema v)) then throw "StructuredArguments"
    configureStructured ctx
    for group in ["structured.scope","structured.task","structured.wait"] do bootstrapCounter ctx group
    let values ← rawInvokeAttempt types ctx sig.op args
    if values.length != sig.resultTypes.length || !(sig.resultTypes.zip values).all (fun (t,v) => (types[t]?).any (fun schema => conforms types (types.length+1) schema v)) then throw "StructuredResults"
    fromExcept (checkCapacity (← get))
    pure values
  match action.run initial with
  | .ok values next => .ok values next
  | .error error draft =>
    let next := rollbackAllocations initial draft
    if [.trySpawnProcess,.submitTask,.requestTaskSlot].contains sig.op && ["Capacity","Cancelled","TaskArgsCapacity"].contains error then
      .ok [.variant 1 [u (if error == "Capacity" then 2 else if error == "Cancelled" then 20 else 0)]] next
    else .error error next

structure ChildLifecycle where
  world : State
  trace : List ExecutionTrace
  committed : List Value
  remainingFuel : Nat
  ok : Bool := true
  error : String := ""

/-- An explicit bounded host fixture dispatches one authenticated scheduled process;
    the evaluator callback supplies its computed return value, never an expected answer. -/
def runScheduledChild (types : TypeEnvironment) (ctx : Context) (world : State)
    (execute : Context → Nat → List Value → State → Outcome) : Except String ChildLifecycle := do
  let pending := world.events.filter (fun e => e.kind == "structured.task.start" && !e.cancelled)
  let [event] := pending | throw "ScheduledChildRequiresOneStart"
  let [.handle taskHandle,.handle process] := event.values | throw "ScheduledChildEventABI"
  let r ← task world ctx taskHandle
  if r.start != some event.identity || r.process != some process || r.status != 1 ||
      event.stage != 3 || event.identity.owner != taskHandle.owner then throw "ScheduledChildIdentity"
  let processData ← Runtime.getProcess world (own ctx process) process
  let childCtx : Context := {ctx with
    kind := 1
    instanceId := event.instanceId
    connection := event.connection
    owner := process.owner.toNat
    now := event.time
    turn := event.turn
    processIdentity := some process
    environment := ctx.environment.filter (fun entry => entry.1 != "structured.runChild")}
  let world ← retire world ctx event.identity "reference.event"
  let world := {world with events := world.events.filter (fun e => e.identity != event.identity)}
  let deliveredWorld := world
  let world ← put world ctx taskHandle "structured.task" (encodeTask {r with start := none})
  let computed := execute childCtx processData.program r.args world
  let failure := fun (error : String) (state : State) =>
    ({world := state,trace := computed.trace,committed := computed.committed,remainingFuel := computed.remainingFuel,ok := false,error} : ChildLifecycle)
  if !computed.ok || computed.exit != "returned" then
    return failure (if computed.ok then "ScheduledChildDidNotReturn" else computed.error) (rollbackAllocations deliveredWorld computed.world)
  let [returned] := computed.returned | return failure "ScheduledChildReturnArity" (rollbackAllocations deliveredWorld computed.world)
  let proof := computed.world.observations.any (fun observation =>
    observation.kind == "process.completed" && observation.time == childCtx.now && observation.turn == childCtx.turn &&
    match observation.values with
    | [.handle identity,.bits 64 program,.bits 64 _,.record values] =>
      identity == process && program == processData.program && values == computed.returned
    | _ => false)
  if liveIdentity computed.world process || !proof then return failure "ScheduledChildCompletionProof" computed.world
  let some runtimeFuel := computed.runtimeFuelRemaining | return failure "ScheduledChildMissingRuntimeFuel" computed.world
  let action : Attempt Unit := do
    let current ← fromExcept (task (← get) childCtx taskHandle)
    updateChecked (fun s => put s childCtx taskHandle "structured.task" (encodeTask {current with process := none}))
    completeTaskAttempt types childCtx taskHandle (.record [u 0,returned])
    for phase in ["structured.child.completed","structured.child.retained"] do
      let value ← fromExcept (Storage.getResult (own childCtx r.consumer) (← get) r.result r.consumer)
      let s ← get
      updateChecked (fun state => observe state ⟨phase,"","",childCtx.now,childCtx.turn,
        [.handle taskHandle,.handle r.result,.handle r.consumer,value,u (frameCount s childCtx),
         .bool (liveIdentity s r.result),.bool (liveIdentity s r.consumer),u runtimeFuel]⟩)
    let owning ← fromExcept (Storage.getResult (own childCtx r.consumer) (← get) r.result r.consumer)
    updateChecked (fun s => releaseTask childCtx s taskHandle r.scope)
    let s ← get
    updateChecked (fun state => observe state ⟨"structured.child.released","","",childCtx.now,childCtx.turn,
      [.handle taskHandle,.handle r.result,.handle r.consumer,owning,u (frameCount s childCtx),
       .bool (liveIdentity s r.result),.bool (liveIdentity s r.consumer),u runtimeFuel]⟩)
  match action.run computed.world with
  | .error error world => pure (failure error world)
  | .ok _ world => pure {world,trace := computed.trace,committed := computed.committed,remainingFuel := computed.remainingFuel}

end LeanAT.Reference.Structured
