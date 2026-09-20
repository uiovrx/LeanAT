import LeanAT.Reference.Objects
import LeanAT.Compiler.OpcodeCases.Objects
import LeanAT.Compiler.Lowering
import LeanAT.Reference.Source
open LeanAT LeanAT.Reference LeanAT.Reference.Objects LeanAT.ExecIR
set_option maxRecDepth 8192
namespace ReferenceObjectsTests
private def u (n : Nat) : Value := .bits 64 n
private def must {α : Type} : Except String α → IO α
  | .ok v => pure v | .error e => throw (IO.userError e)
private def check (b : Bool) (s : String) : IO Unit := unless b do throw (IO.userError s)
private def rejected {α : Type} (r : Except String α) : Bool := r.toOption.isNone
-- ID-stable service type catalog; variants have the actual VM representations.
def types : TypeEnvironment := [.bits 64,.bytes 16,.bool,.handle .transaction,.handle .resourceTicket,
  .record [0,0,0,4,0],.record [0,1],.handle .event,.record [5,3,7,0],.boundedVec 5 4,
  .handle .result,.handle .consumer,.record [10,11],.variant [[],[12]],.handle .scope,.handle .drain,
  .variant [[14],[15]],.variant [[],[16]],.variant [[],[0]],.record [18,13,17,0],.variant [[],[19]],.boundedVec 0 4]
def ctx : Context := {
  domain := 1,instanceId := 2,owner := 7,connection := 9
  environment := [("objects.valueNodeBytes",u 40),("objects.maxBytes",u 16),("objects.maxEntries",u 4),("objects.elementType",u 0)]}
def txn : HandleIdentity := ⟨.transaction,1,50,0,1,7⟩
def sig (kind obj mid : Nat) (ins outs : List Nat) : Except String ServiceSignature :=
  makeSignature types kind obj mid ins outs 16 4
private def committed (c : Context) (s : State) : Context :=
  {c with environment := c.environment ++ [("objects.committed",.vec (s.objects.map (fun e => .record [.handle e.identity,e.value]))),("objects.epoch",u 0)]}
private def update (s : State) (object : ObjectEntry) (value : Value) : State :=
  {s with objects := s.objects.map (fun e => if e.identity == object.identity then {e with value} else e)}
end ReferenceObjectsTests
open ReferenceObjectsTests

def main : IO Unit := do
  let catalog ← must LeanAT.Compiler.OpcodeCases.Objects.cases
  for fixture in catalog do
    let validated ← must (ModelIR.validateSchema fixture.model)
    let _ ← must (LeanAT.Compiler.lower validated)
    let some handler := fixture.model.handlers.head? | throw (IO.userError "catalog handler")
    let c := committed fixture.input.context fixture.input.world
    let machine : Source.Machine := {values := [],locals := (handler.parameters.zip fixture.input.inputs).map (fun (p,v) => (p.id,v)),world := fixture.input.world,context := c,remainingFuel := fixture.input.fuel,program := 0}
    match Source.run fixture.model handler.body machine with
    | .ok _ _ => check (fixture.expectedOutcome == "success") (fixture.id ++ ": expected failure")
    | .error error _ => check (fixture.expectedOutcome == "failure") (fixture.id ++ ": " ++ error)
  let mem := memoryEntry ctx 1 [1,2,3,4]
  let initial : State := stateWithObjects [mem]
  let write ← must (sig 0 1 3 [0,1,1] [])
  let read ← must (sig 0 1 2 [0,0] [1])
  let transfer ← must (sig 0 1 1 [0,0,1,0,1] [6])
  let debug ← must (sig 0 1 6 [0,0,1] [6])
  let (out,written) ← must (invoke types ctx write [u 1,.bytes [8,9],.bytes [255,0]] initial)
  let expected := update initial mem (.record [u 2,.bytes [1,8,3,4],.bytes [1,2,3,4],u 0,.bool true])
  check (out.isEmpty && written == expected) "memory masked write full state"
  let (out,same) ← must (invoke types ctx read [u 0,u 4] written)
  check (out == [.bytes [1,8,3,4]] && same == written) "memory read full state"
  let (out,same) ← must (invoke types ctx read [u (2^64-1),u 0] initial)
  check (out == [.bytes []] && same == initial) "empty read ignores address"
  check (rejected (invoke types {ctx with environment := []} read [u 0,u 1] initial)) "missing native ABI charge rejected"
  let (out,streamed) ← must (invoke types ctx transfer [u 1,u 0,.bytes [5,6,7],u 2,.bytes []] initial)
  check (out == [.record [u 1,.bytes [5,6,7]]] && streamed == update initial mem (.record [u 2,.bytes [7,6,3,4],.bytes [1,2,3,4],u 0,.bool true])) "memory cyclic streaming"
  let (out,same) ← must (invoke types ctx transfer [u 2,u (2^64-1),.bytes [],u 0,.bytes [2]] initial)
  check (out == [.record [u 1,.bytes []]] && same == initial) "ignore before unused validation"
  let (out,same) ← must (invoke types ctx transfer [u 1,u 3,.bytes [7,8],u 2,.bytes [255,0]] initial)
  check (out == [.record [u 3,.bytes [7,8]]] && same == initial) "disabled addresses still preflight"
  let (out,poked) ← must (invoke types {ctx with kind := 3} debug [u 1,u 3,.bytes [8,9]] initial)
  check (out == [.record [u 1,.bytes [8,9]]] && poked == update initial mem (.record [u 2,.bytes [1,2,3,8],.bytes [1,2,3,4],u 0,.bool true])) "debug bounded prefix"
  let reset ← must (sig 0 1 5 [] [])
  let (_,restored) ← must (invoke types ctx reset [] poked)
  check (restored == update initial mem (.record [u 2,.bytes [1,2,3,4],.bytes [1,2,3,4],u 0,.bool true])) "reset restores bytes and retains dirty backing"
  let (resetResult,resetCommitted) := atomic types ctx [(reset,[]),(reset,[])] initial
  check (!rejected resetResult && resetCommitted == update initial mem (.record [u 2,.bytes [1,2,3,4],.bytes [1,2,3,4],u 1,.bool false])) "same-byte resets increment backing version exactly once per commit"
  let (resetAgain,twiceCommitted) := atomic types ctx [(reset,[])] resetCommitted
  check (!rejected resetAgain && twiceCommitted == update initial mem (.record [u 2,.bytes [1,2,3,4],.bytes [1,2,3,4],u 2,.bool false])) "next segment reset increments backing version again"
  let exhausted := update initial mem (.record [u 2,.bytes [1,2,3,4],.bytes [1,2,3,4],u (2^64-1),.bool false])
  let (exhaustedResult,exhaustedRollback) := atomic types ctx [(reset,[])] exhausted
  check (rejected exhaustedResult && exhaustedRollback == exhausted) "version overflow aborts commit and restores bytes/dirty metadata"
  let clear ← must (sig 0 1 4 [] [])
  let (_,cleared) ← must (invoke types ctx clear [] initial)
  check (cleared == update initial mem (.record [u 2,.bytes [0,0,0,0],.bytes [1,2,3,4],u 0,.bool true])) "memory clear"
  for bad in [{write with abiHash := ByteArray.empty},{write with providerVersion := "1"},
              {write with inputTypes := [1,1,1]},{write with effectMask := 128}] do
    check (rejected (invoke types ctx bad [u 0,.bytes [9],.bytes []] initial)) "wrong ABI/type/effects rejected"
  for c in [{ctx with domain := 2},{ctx with instanceId := 3},{ctx with kind := 3}] do
    check (rejected (invoke types c write [u 0,.bytes [9],.bytes []] initial)) "object context rejection"
  check (rejected (invoke types ctx write [.bytes [],.bytes [9],.bytes []] initial)) "argument variant before provider"
  let (failed,rolledBack) := atomic types ctx [(write,[u 0,.bytes [9],.bytes []]),(write,[u 8,.bytes [1],.bytes []])] initial
  check (rejected failed && rolledBack == initial) "whole segment rollback"
  let bounded := {initial with maxBytes := ownedBytes initial}
  check (rejected (invoke types ctx transfer [u 0,u 0,.bytes (List.replicate 17 0),u 1,.bytes []] bounded)) "schema byte bound"

  -- Packed register: low nibble read-clear, high nibble write-one-clear.
  let reg : ObjectEntry := {mem with
    identity := {mem.identity with store := 301,slot := 2}
    tag := "reference.object.register"
    value := .record [u 2,.bytes [255],.bytes [255],.vec [.record [u 0,u 8,.vec [u 1],u 0,.bool false,.vec [.record [u 1,u 0,u 4,u 5],.record [u 2,u 4,u 4,u 3]]]]]}
  let regs : State := stateWithObjects [reg]
  let access ← must (sig 1 2 1 [0,0,1,0,1] [6])
  let (out,changed) ← must (invoke types ctx access [u 0,u 0,.bytes [0],u 1,.bytes []] regs)
  let .record rs := reg.value | throw (IO.userError "register fixture")
  check (out == [.record [u 1,.bytes [255]]] && changed == update regs reg (.record (rs.set 1 (.bytes [240])))) "RC read full state"
  let (_,changed) ← must (invoke types ctx access [u 1,u 0,.bytes [48],u 1,.bytes []] regs)
  check (changed == update regs reg (.record (rs.set 1 (.bytes [207])))) "W1C full state"
  let field ← must (sig 1 2 7 [0] [1])
  check (rejected (invoke types ctx field [u (2^32+1)] regs)) "field ID does not narrow"
  let poke ← must (sig 1 2 8 [0,1] [])
  let (_,updated) ← must (invoke types ctx poke [u 1,.bytes [3]] regs)
  check (updated == update regs reg (.record (rs.set 1 (.bytes [243])))) "field update ignores access side effects"
  check (rejected (invoke types ctx poke [u 1,.bytes [16]] regs)) "field high bits rejected"
  let big := {reg with value := .record [u 2,.bytes [171,18],.bytes [171,18],.vec [.record [u 0,u 16,.vec [u 2],u 0,.bool true,.vec [.record [u 1,u 0,u 8,u 2],.record [u 2,u 8,u 8,u 0]]]]]}
  let bigState : State := stateWithObjects [big]
  let (out,same) ← must (invoke types ctx access [u 0,u 0,.bytes [0,0],u 2,.bytes []] bigState)
  check (out == [.record [u 1,.bytes [171,0]]] && same == bigState) "big-endian RO/WO read"
  let (_,updated) ← must (invoke types ctx access [u 1,u 0,.bytes [255,52],u 2,.bytes []] bigState)
  let .record bigFields := big.value | throw (IO.userError "big register")
  check (updated == update bigState big (.record (bigFields.set 1 (.bytes [171,52])))) "big-endian RO preserved WO written"
  let setReg := {reg with value := .record [u 2,.bytes [1],.bytes [1],.vec [.record [u 0,u 8,.vec [u 1],u 0,.bool false,.vec [.record [u 1,u 0,u 8,u 4]]]]]}
  let setState : State := stateWithObjects [setReg]
  let (_,setBits) ← must (invoke types ctx access [u 1,u 0,.bytes [2],u 1,.bytes []] setState)
  let .record setFields := setReg.value | throw (IO.userError "set register")
  check (setBits == update setState setReg (.record (setFields.set 1 (.bytes [3])))) "W1S preserves prior bits"

  let resource := resourceEntry ctx 3 5 0 1 4
  let resources : State := stateWithObjects [resource]
  let reserve ← must (sig 2 3 10 [3,0] [5])
  let (out,reserved) ← must (invoke types ctx reserve [.handle txn,u 2] resources)
  let ticket : HandleIdentity := ⟨.resourceTicket,1,313,0,1,7⟩
  let g := .record [u 2,u 7,u 0,.handle ticket,u 0]
  let .record rf := resource.value | throw (IO.userError "resource fixture")
  let record := .record [.bool true,.handle ticket,u 2,u 7,u 0,u 0,.bool false]
  let expectedR := update resources resource (.record (rf.set 7 (u 2) |>.set 8 (u 2) |>.set 9 (u 0) |>.set 10 (.vec [u 7]) |>.set 11 (.vec [record])))
  check (out == [g] && reserved == expectedR) "resource reserve exact grant/full state"
  let (out,two) ← must (invoke types ctx reserve [.handle txn,u 1] reserved)
  check (out == [.record [u 7,u 12,u 6,.handle {ticket with slot := 1},u 0]]) "FCFS stable future start"
  let cancel ← must (sig 2 3 11 [4] [0])
  let (out,cancelled) ← must (invoke types (committed ctx resources) cancel [.handle ticket] reserved)
  let expectedCancel := update resources resource (.record (rf.set 11 (.vec [.record [.bool false,.handle ticket,u 2,u 7,u 0,u 0,.bool true]])))
  check (out == [u 0] && cancelled == expectedCancel) "unpublished cancellation restores scheduling"
  let (out,retained) ← must (invoke types (committed ctx reserved) cancel [.handle ticket] reserved)
  check (out == [u 1] && retained.events == reserved.events) "committed cancellation retains finish"
  let complete ← must (sig 2 3 12 [4,0] [2])
  check (rejected (invoke types {ctx with now := 6} complete [.handle ticket,u 6] retained)) "early complete"
  check (rejected (invoke types {ctx with owner := 8,now := 7} complete [.handle ticket,u 7] retained)) "ticket caller owner"
  check (rejected (invoke types {ctx with now := 7} complete [.handle {ticket with generation := 2},u 7] retained)) "ticket generation"
  let (out,finished) ← must (invoke types {ctx with now := 7} complete [.handle ticket,u 7] retained)
  check (out == [.bool false] && finished != retained) "cancelled finish cleanup"
  let full := {resources with maxBytes := ownedBytes resources}
  check (rejected (invoke types ctx reserve [.handle txn,u 0] full)) "resource capacity atomic"
  let (result,rollback) := atomic types ctx [(reserve,[.handle txn,u 0]),(complete,[.handle ticket,u 7])] resources
  check (rejected result && rollback == resources) "reservation later failure rollback"
  check (two != reserved) "second reservation installed"

  let q := queueEntry ctx 4 1 64 4 99
  let queues : State := stateWithObjects [q]
  let push ← must (sig 3 4 14 [0] [2]); let pop ← must (sig 3 4 15 [] [20])
  let (out,pushed) ← must (invoke types ctx push [u 42] queues)
  let .record qf := q.value | throw (IO.userError "queue fixture")
  let qrow := .record [u 1,.variant 1 [u 42],.variant 0 [],.bool false,.variant 0 []]
  check (out == [.bool true] && pushed == update queues q (.record (qf.set 5 (u 2) |>.set 6 (.vec [qrow])))) "queue push full state"
  let (out,same) ← must (invoke types ctx push [u 43] pushed)
  check (out == [.bool false] && same == pushed) "bounded queue no mutation"
  let (out,popped) ← must (invoke types ctx pop [] pushed)
  check (out == [.variant 1 [.record [.variant 1 [u 42],.variant 0 [],.variant 0 [],u 1]]] && popped == update queues q (.record (qf.set 5 (u 2)) )) "queue pop full state"
  let pushOwned ← must (sig 3 4 19 [10,11,0] [2])
  let fakeResult : HandleIdentity := ⟨.result,1,20,0,1,7⟩
  let fakeConsumer : HandleIdentity := ⟨.consumer,1,21,0,2,7⟩
  check (rejected (invoke types ctx pushOwned [.handle fakeResult,.handle fakeConsumer,u 0] pushed)) "full queue still rejects bad ownership policy"
  check (rejected (invoke types ctx pushOwned [.handle fakeResult,.handle {fakeConsumer with owner := 8},u 1] pushed)) "full queue still rejects consumer caller"
  let tinyQueue := queueEntry ctx 4 1 1 4 99
  let booleanPush ← must (sig 3 4 14 [2] [2])
  let boolContext := {ctx with environment := ("objects.elementType",u 2)::ctx.environment}
  check (rejected (invoke types boolContext booleanPush [.bool true] (stateWithObjects [tinyQueue]))) "native Value node contributes queue byte budget"
  let (result,consumer,withResult) ← must (Storage.reserveResult types ctx queues txn 0 128 ctx.owner)
  let (out,owned) ← must (invoke types ctx pushOwned [.handle result,.handle consumer,u 1] withResult)
  check (out == [.bool true] && rejected (Storage.readConsumer ctx owned result consumer)) "MoveOwned consumes source capability"
  let fresh : HandleIdentity := ⟨.consumer,1,21,1,2,99⟩
  let some ownedEntry := owned.objects.head? | throw (IO.userError "owned queue entry")
  let .record queueData := ownedEntry.value | throw (IO.userError "owned queue")
  let expectedOwnedRows := .vec [.record [u 1,.variant 0 [],.variant 0 [],.bool false,.variant 1 [.record [.handle result,.handle fresh]]]]
  check (queueData == (qf.set 5 (u 2) |>.set 6 expectedOwnedRows)) "owned queue exact capability record"
  check ((← must (Storage.readConsumer {ctx with owner := 99} owned result fresh)).result == result) "queue owns fresh consumer"
  let (out,returned) ← must (invoke types ctx pop [] owned)
  let returnedConsumer : HandleIdentity := ⟨.consumer,1,21,0,3,7⟩
  check (out == [.variant 1 [.record [.variant 0 [],.variant 1 [.record [.handle result,.handle returnedConsumer]],.variant 0 [],u 1]]]) "pop returns distinct consumer capability"
  check (rejected (Storage.readConsumer {ctx with owner := 99} returned result fresh) && returned.nextGeneration == 4) "pop revokes queue consumer without generation reuse"
  let (failed,rolled) := atomic types ctx [(pushOwned,[.handle result,.handle consumer,u 1]),(pushOwned,[.handle result,.handle consumer,u 0])] withResult
  check (rejected failed && rolled.objects == withResult.objects && rolled.events == withResult.events &&
    rolled.allocationCounters.any (fun counter => counter.group == "storage.consumer" && counter.nextGeneration == 3)) "owned transfer rolls back consumers but preserves allocated generation"
  let (scope,scopeState) ← must (Structured.seedRoot ctx queues)
  let qrow2 := .record [u 2,.variant 1 [u 43],.variant 0 [],.bool false,.variant 0 []]
  let crowdedQueue := .record (qf.set 1 (u 2) |>.set 3 (u 1) |>.set 5 (u 3) |>.set 6 (.vec [qrow,qrow2]))
  let crowded := update scopeState q crowdedQueue
  let drainTransfer ← must (sig 3 4 21 [14,15,0] [21])
  let invalidDrain : HandleIdentity := ⟨.drain,1,90,0,1,7⟩
  check (match invoke types ctx drainTransfer [.handle scope,.handle invalidDrain,u 2] crowded with
    | .error "QueueOwner" => true
    | _ => false) "drain authority precedes scan budget"

  let pipeline := resourceEntry ctx 5 0 0 1 4 true
  let pipelines : State := stateWithObjects [pipeline]
  let submit ← must (sig 4 5 17 [3,0] [8]); let ready ← must (sig 4 5 18 [8,7] [2])
  let (out,scheduled) ← must (invoke types (committed ctx pipelines) submit [.handle txn,u 0] pipelines)
  let [.record [pg,.handle po,.handle event,ep]] := out | throw (IO.userError "pipeline result")
  let some actual := scheduled.events.head? | throw (IO.userError "pipeline event")
  check (actual.time == 0 && actual.turn == 1 && actual.stage == 3 && actual.connection == ctx.connection && po == txn && ep == u 0 && scheduled.events.length == 1) "zero duration successor event"
  check (rejected (invoke types {ctx with turn := 1} ready [out.head!, .handle event] scheduled)) "ready requires authoritative event"
  let active := {ctx with turn := 1,environment := ctx.environment ++ [("objects.epoch",u 0),("objects.activeEvent",.handle event)]}
  let (out,done) ← must (invoke types active ready [.record [pg,.handle po,.handle event,ep],.handle event] scheduled)
  check (out == [.bool true] && done.events == scheduled.events && done.nextGeneration == scheduled.nextGeneration && done != scheduled) "pipeline completion preserves dispatch cursor state"
  check (rejected (invoke types (committed ctx {pipelines with maxEvents := 0}) submit [.handle txn,u 0] {pipelines with maxEvents := 0})) "pipeline event capacity atomic"
  IO.println s!"ReferenceObjects: {catalog.length} ModelIR source cases plus memory/register/resource/queue/pipeline effects and rejection/rollback fixtures passed"



