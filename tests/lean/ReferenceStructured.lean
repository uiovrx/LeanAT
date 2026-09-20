import LeanAT.Reference.Structured
open LeanAT LeanAT.Reference LeanAT.Reference.Structured LeanAT.ExecIR

private def types : TypeEnvironment := [
  .unit, .bits 64, .handle .scope, .handle .process, .handle .task, .handle .wait,
  .record [], .bytes 256, .record [1,7], .variant [[4],[1]], .handle .spawnTicket,
  .variant [[10],[1]], .record [1,8], .record [1,14], .record [8,8],
  .record [16,8], .bool, .record [16], .record [15,17], .record [1,1,8,18]]
private def u := Value.bits 64
private def success := Value.record [u 0,.bytes [42]]
private def failure := Value.record [u 1,.bytes [99]]
private def config : PoolConfig := {frames := 2,tasks := 8,results := 8,queue := 2,waiters := 4,overflow := 2,resultType := 8}
private def ctx : Context := {kind := 1,domain := 1,instanceId := 1,owner := 1,environment := [("profile.valueNodeBytes",u 40),("structured.pool",encodePool config)]}
private def check (b : Bool) (msg : String) : Except String Unit := if b then pure () else throw msg
private def failed {α} : Except String α → Bool | .error _ => true | _ => false
private def call (op : Op) (ins : List Nat) (out : Nat) (args : List Value) (s : State) (context : Context := ctx) : Except String (List Value × State) := do
  let sig := signature types 1 op ins [out]
  check (failed (invoke types {context with kind := 2} sig args s)) "context admitted"
  check (failed (invoke types context {sig with abiHash := ByteArray.empty} args s)) "ABI forgery admitted"
  if let .handle h :: rest := args then
    check (failed (invoke types context sig (.handle {h with domain := 99} :: rest) s)) ("foreign handle admitted " ++ toString op.tag)
  attemptExcept ((invokeAttempt types context sig args).run s)
private def resultHandle : List Value → Except String HandleIdentity
  | [.handle h] | [.variant 0 [.handle h]] => pure h
  | _ => throw "ExpectedHandle"
private def groupResult (s : State) (wait root : HandleIdentity) (type : Nat := 12) : Except String Value := do
  let (values,unchanged) ← call .waitResultGet [5,2] type [.handle wait,.handle root] s
  check (unchanged == s) "get mutated full state"
  match values with | [v] => pure v | _ => throw "result arity"
private def seeded : Except String (HandleIdentity × HandleIdentity × State) := do
  let (root,s) ← seedRoot ctx {}
  let s ← initializeState ctx s
  let (process,s) ← Runtime.createProcess s ctx 0
  pure (root,process,s)

private def testTasks : Except String Unit := do
  let (root,_,s) ← seeded
  let (a,s) ← call .spawnProcess [2,6] 4 [.handle root,.record []] s
  let a ← resultHandle a
  let (b,s) ← call .trySpawnProcess [2,6] 9 [.handle root,.record []] s
  let b ← resultHandle b
  let (c,s) ← call .submitTask [2,6] 9 [.handle root,.record []] s
  let c ← resultHandle c
  let cr ← decodeTask (← getObject s {ctx with owner := c.owner.toNat} c "structured.task").value
  check (cr.status == 0 && cr.process.isNone && (s.events.filter (fun e => !e.cancelled)).length == 2) "queued child allocated process"
  let (full,unchanged) ← call .trySpawnProcess [2,6] 9 [.handle root,.record []] s
  check (full == [.variant 1 [u 2]] && unchanged == s) "capacity error leaked state"
  check (failed (call .taskResultGet [4,2] 8 [.handle a,.handle root] s)) "unpublished task readable"
  let s ← completeTask types ctx s a success
  let promoted ← decodeTask (← getObject s {ctx with owner := c.owner.toNat} c "structured.task").value
  check (promoted.status == 1 && promoted.process.isSome && (s.events.filter (fun e => !e.cancelled)).length == 2) "queue failed FIFO promotion"
  let (value,unchanged) ← call .taskResultGet [4,2] 8 [.handle a,.handle root] s
  check (value == [success] && unchanged == s) "task result mismatch"
  let (_,s) ← call .cancelTask [4,7] 0 [.handle b,.bytes [7]] s
  check (failed (call .cancelTask [4,7] 0 [.handle b,.bytes [7]] s)) "duplicate terminal admitted"
  let (_,s) ← call .taskResultRelease [4,2] 0 [.handle a,.handle root] s
  check (failed (call .taskResultGet [4,2] 8 [.handle a,.handle root] s)) "released result readable"
  let limited := {s with maxBytes := ownedBytes s + 1}
  let attempt := invoke types ctx (signature types 1 .submitTask [2,6] [9]) [.handle root,.record []] limited
  check (failed attempt) "late result reserve capacity escaped"

private def testWaits : Except String Unit := do
  let (root,process,s) ← seeded
  let (a,s) ← submit types s ctx root [] false
  let (b,s) ← submit types s ctx root [] false
  let (values,s) ← call .waitGroupNew [3,2,1,1,1,1,1] 5 [.handle process,.handle root,u 0,u 2,u 0,u 12,u 4096] s
  let wait ← resultHandle values
  let (_,s) ← call .waitArm [5,1,1,4,2] 0 [.handle wait,u 1,u 9,.handle a,.handle root] s
  check (failed (call .waitArm [5,1,1,4,2] 0 [.handle wait,u 1,u 0,.handle b,.handle root] s)) "duplicate ordinal admitted"
  let (_,s) ← call .waitArm [5,1,1,4,2] 0 [.handle wait,u 0,u 3,.handle b,.handle root] s
  let s ← Runtime.suspendProcess s ctx process wait
  check (failed (call .waitGroupRelease [5,2] 0 [.handle wait,.handle root] s)) "pending suspended group released"
  let s ← completeTask types {ctx with now := 8,turn := 2} s a success
  let s ← completeTask types {ctx with now := 8,turn := 2} s b failure
  let s ← closeBatch types {ctx with now := 8,turn := 2} s
  check ((← groupResult s wait root) == .record [u 0,failure]) "Any priority/ordinal choice"
  let resumptions := s.events.filter (fun e => e.kind == "runtime.resume")
  check (resumptions.length == 1) "wait did not single-claim resume"
  check ((← closeBatch types {ctx with now := 9} s) == s) "second epilogue mutated resolved wait"
  let (value,s) ← Runtime.claimWait s {ctx with now := 9} process wait
  check (value == .record [u 0,failure]) "resume owning value"
  check (failed (Runtime.claimWait s {ctx with now := 9} process wait)) "duplicate resume"
  let (_,s) ← call .waitGroupRelease [5,2] 0 [.handle wait,.handle root] s
  check (failed (groupResult s wait root)) "released wait readable"
  let (_,s) ← call .taskResultRelease [4,2] 0 [.handle a,.handle root] s
  let (_,s) ← call .taskResultRelease [4,2] 0 [.handle b,.handle root] s
  check (!(s.objects.any (fun o => o.alive && o.tag == Storage.resultTag))) "result ownership leaked"

private def testOwnedWaitCancellation : Except String Unit := do
  for phase in [0,1,2] do
    let (root,process,s) ← seeded
    let (task,s) ← submit types s ctx root [] false
    let (created,s) ← call .waitGroupNew [3,2,1,1,1,1,1] 5 [.handle process,.handle root,u 0,u 1,u 0,u 12,u 4096] s
    let wait ← resultHandle created
    let (_,s) ← call .waitArm [5,1,1,4,2] 0 [.handle wait,u 0,u 0,.handle task,.handle root] s
    let s ← if phase > 0 then Runtime.suspendProcess s ctx process wait else pure s
    let s ←
      if phase == 2 then do
        let s ← completeTask types ctx s task success
        closeBatch types ctx s
      else pure s
    let (_,next) ← call .scopeCancel [2,7] 0 [.handle root,.bytes []] s
    check (failed (Runtime.getWait next ctx wait)) "cancelled scope retains registered wait"
    check (failed (groupResult next wait root)) "cancelled scope retains group"
    if phase == 0 then
      let p ← Runtime.getProcess next ctx process
      check (p.status == 0 && p.wait.isNone) "prepared cancellation destroyed executing frame"
    else
      check (failed (Runtime.getProcess next ctx process)) "suspended cancellation retains frame"
    check (!(next.objects.any (fun o => o.alive && o.tag == Storage.resultTag))) "scope cancellation leaked result"

private def testAll : Except String Unit := do
  let (root,process,s) ← seeded
  let (a,s) ← submit types s ctx root [] false
  let (b,s) ← submit types s ctx root [] false
  let (wait,s) ← createWait types ctx s process root 1 2 1 19 4096
  let s ← armWait ctx s wait 0 0 a root
  let s ← armWait ctx s wait 1 0 b root
  let s ← completeTask types {ctx with now := 3} s a failure
  let s ← closeBatch types {ctx with now := 3} s
  check ((← groupResult s wait root 19) == .record [u 1,u 0,failure,.record [.record [.bool true,failure],.record [.bool false]]]) "failFast missing observation fabricated"
  let (root,process,s) ← seeded
  let (a,s) ← submit types s ctx root [] false
  let (b,s) ← submit types s ctx root [] false
  let (wait,s) ← createWait types ctx s process root 1 2 0 13 4096
  let s ← armWait ctx s wait 1 0 b root
  let s ← armWait ctx s wait 0 0 a root
  let s ← completeTask types {ctx with now := 9} s b success
  let s ← completeTask types {ctx with now := 2} s a failure
  let s ← closeBatch types {ctx with now := 9} s
  check ((← groupResult s wait root 13) == .record [u 0,.record [failure,success]]) "All declaration order"

private def testScopes : Except String Unit := do
  let (root,_,s) ← seeded
  let (a,s) ← call .scopeNew [2] 2 [.handle root] s
  let a ← resultHandle a
  let (b,s) ← call .scopeNew [2] 2 [.handle root] s
  let b ← resultHandle b
  let (_,s) ← call .scopeTransfer [2,2,2] 0 [.handle a,.handle root,.handle b] s
  check (failed (call .scopeTransfer [2,2,2] 0 [.handle b,.handle root,.handle a] s)) "cycle admitted"
  let (_,s) ← call .scopeCancel [2,7] 0 [.handle b,.bytes []] s
  check (failed (call .scopeNew [2] 2 [.handle a] s)) "frozen descendant admitted child"
  check (failed (call .scopeClose [2] 0 [.handle b] s)) "closed parent with live child"
  let (_,s) ← call .scopeClose [2] 0 [.handle a] s
  let (_,s) ← call .scopeClose [2] 0 [.handle b] s
  check (failed (call .scopeClose [2] 0 [.handle b] s)) "duplicate close"

private def testTickets : Except String Unit := do
  let context := {ctx with environment := [("profile.valueNodeBytes",u 40),("structured.pool",encodePool {config with frames := 1,overflow := 1})]}
  let (root,process,s) ← seeded
  let (parent,s) ← submit types s context root [] false
  let (ticket,s) ← call .requestTaskSlot [3,2] 11 [.handle process,.handle root] s context
  let ticket ← resultHandle ticket
  let pending ← decodeTicket (← getObject s {context with owner := ticket.owner.toNat} ticket "structured.ticket").value
  check (pending.status == 0 && pending.reservation.isNone) "pending ticket fabricated reservation"
  let s ← completeTask types context s parent success
  let granted ← decodeTicket (← getObject s {context with owner := ticket.owner.toNat} ticket "structured.ticket").value
  check (granted.status == 1 && granted.reservation.isSome) "ticket no actual grant"
  let (_,s) ← call .scopeCancel [2,7] 0 [.handle root,.bytes []] s context
  let cancelled ← decodeTicket (← getObject s {context with owner := ticket.owner.toNat} ticket "structured.ticket").value
  check (cancelled.status == 3 && cancelled.reservation.isNone) "cancelled ticket retains capacity"

private def testAllocationJournal : Except String Unit := do
  let (root,process,s) ← seeded
  let sig := signature types 77 .waitGroupNew [3,2,1,1,1,1,1] [5]
  let forged := {process with generation := process.generation+1000}
  let args := [.handle forged,.handle root,u 0,u 1,u 0,u 12,u 4096]
  let next ← match (invokeAttempt types ctx sig args).run s with
    | .ok _ _ => throw "stale process admitted by allocation attempt"
    | .error _ state => pure state
  check (next.objects == s.objects && next.events == s.events) "failed prepare exposed objects/events"
  let counter := fun (state : State) (group : String) =>
    (state.allocationCounters.find? (fun c => c.group == group)).map (·.nextGeneration)
  check (counter next "structured.wait" == some 3) "group allocation burn lost"
  check (counter next "storage.result" == some 2 && counter next "storage.consumer" == some 2) "result/consumer allocation burns lost"
  let valid := [.handle process,.handle root,u 0,u 1,u 0,u 12,u 4096]
  let (_,next) ← attemptExcept ((invokeAttempt types ctx sig valid).run next)
  check (counter next "structured.wait" == some 4) "discarded group generation reused"

private def testDomainAndCancellation : Except String Unit := do
  let (root,_,s) ← seeded
  let foreignRoot := {root with domain := 99, generation := 100}
  let rootObject ← getObject s {ctx with owner := root.owner.toNat} root "structured.scope"
  let imported := {s with
    allocationCounters := s.allocationCounters.filter (fun c => c.group != "structured.scope") ++
      [{group := "structured.scope",domain := 99,nextGeneration := 101,persistent := true}]
    nextGeneration := 101
    objects := s.objects ++ [{rootObject with identity := foreignRoot}]}
  let imported ← initializeState ctx imported
  check ((imported.allocationCounters.find? (fun c => c.group == "structured.scope" && c.domain == ctx.domain)).map (·.nextGeneration) == some 2)
    "foreign domain polluted structured allocation journal"
  let context := {ctx with connection := 17}
  let (values,s) ← call .spawnProcess [2,6] 4 [.handle root,.record []] s context
  let task ← resultHandle values
  let record ← decodeTask (← getObject s {context with owner := task.owner.toNat} task "structured.task").value
  check (s.events.any (fun e => e.kind == "structured.task.start" && e.connection == 17)) "task start lost connection"
  let some process := record.process | throw "task lacks process"
  let processContext := {context with owner := process.owner.toNat}
  let (wait,s) ← Runtime.newWait s processContext process 3 50 none none none
  let s ← Runtime.suspendProcess s processContext process wait
  let (_,s) ← call .cancelTask [4,7] 0 [.handle task,.bytes []] s context
  check (!(s.objects.any (fun o => o.alive && (o.identity == process || o.identity == wait))))
    "cancelled suspended task retained frame or registered wait"
  let (values,s) ← call .spawnProcess [2,6] 4 [.handle root,.record []] s context
  let task ← resultHandle values
  let record ← decodeTask (← getObject s {context with owner := task.owner.toNat} task "structured.task").value
  let some process := record.process | throw "second task lacks process"
  let (wait,s) ← Runtime.newWait s {context with owner := process.owner.toNat} process 3 60 none none none
  let (_,s) ← call .cancelTask [4,7] 0 [.handle task,.bytes []] s context
  check (!(s.objects.any (fun o => o.alive && (o.identity == process || o.identity == wait))))
    "cancelled executing task retained its prepared wait"

private def testChildDispatchContext : Except String Unit := do
  let (root,_,s) ← seeded
  let context := {ctx with kind := 0,connection := 17}
  let (_,s) ← call .spawnProcess [2,6] 4 [.handle root,.record []] s context
  let dispatched ← runScheduledChild types {context with kind := 0,connection := 99} s
    (fun child _ _ state =>
      ({ok := false
        error := if child.kind == 1 && child.connection == 17 && child.processIdentity.isSome then "ProbeReachedProcess" else "WrongChildContext"
        world := state
        exit := "failed"} : Outcome))
  check (!dispatched.ok && dispatched.error == "ProbeReachedProcess") "child inherited timed scheduler context"
  check (!(dispatched.world.events.any (fun e => e.kind == "structured.task.start"))) "failed child probe did not consume delivered start"

def main : IO Unit :=
  match (do testTasks; testWaits; testOwnedWaitCancellation; testAll; testScopes; testTickets; testAllocationJournal; testDomainAndCancellation; testChildDispatchContext : Except String Unit) with
  | .ok _ => IO.println "ReferenceStructured: task/wait/scope/ticket lifecycle and all 15 typed providers passed"
  | .error error => throw (IO.userError error)
