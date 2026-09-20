import LeanAT.Reference.Runtime
import LeanAT.Frontend.Builtins
open LeanAT LeanAT.Reference
private def must {α : Type} (value : Except String α) : IO α := match value with | .ok value => pure value | .error error => throw (IO.userError error)
private def check (value : Bool) (error : String) : IO Unit := unless value do throw (IO.userError error)
private def ctx : Context := {kind := 1,now := 5,turn := 2,domain := 1,instanceId := 1,owner := 1}
private def proc : HandleIdentity := ⟨.process,1,10,0,1,1⟩
private def timer : Except String ExecIR.ServiceSignature := do
  let some s := Frontend.timerServices[1]? | throw "MissingTimerSignature"
  pure ⟨s.id,.registerWait,s.inputTypes,s.resultTypes,s.contextMask,s.effectMask,s.extraFuel,s.providerKey,s.providerVersion,⟨s.abiHash.toArray⟩⟩
def main : IO Unit := do
  let timer ← must timer
  let initial ← must (Runtime.seedProcess {maxObjects := 3} ctx proc 0)
  let (values,registered) ← must (Runtime.invoke Frontend.processTypes {ctx with processIdentity := some proc} timer [.handle proc,.bits 64 3,.bits 64 0] initial)
  check (registered.events.isEmpty) "registration scheduled before atomic suspend"
  let [.handle wait] := values | throw (IO.userError "wait result")
  let suspended ← must (Runtime.suspendProcess registered ctx proc wait)
  let some event := suspended.events.head? | throw (IO.userError "missing timer event")
  check (event.time == 5 && event.turn == 3 && event.stage == 5 && event.values == [.bits 64 0x4c415452,.handle proc,.handle wait,.bits 64 1]) "timer successor and raw resume identity"
  let published ← must (Runtime.deliver {ctx with turn := 3} suspended event)
  check ((Runtime.deliver {ctx with turn := 3} published event).toOption.isNone) "duplicate timer delivery"
  check (published.events.isEmpty) "timer inserted an extra resume event"
  let (outcome,doneState) ← must (Runtime.claimWait published {ctx with turn := 3} proc wait)
  check (outcome == .unit && (Runtime.getWait doneState ctx wait).toOption.isNone) "wait not retired"
  let (_,again) ← must (Runtime.newWait doneState {ctx with turn := 4} proc 3 5 none none none)
  check (again.objects.length == 3) "retired wait slot not reused"
  let (pastValues,past) ← must (Runtime.invoke Frontend.processTypes {ctx with processIdentity := some proc} timer [.handle proc,.bits 64 4,.bits 64 1] initial)
  let [.handle pastWait] := pastValues | throw (IO.userError "past wait result")
  let pastRecord ← must (Runtime.getWait past ctx pastWait)
  check (pastRecord.deadline == 1 && pastRecord.readyKey == some (5,2) && pastRecord.outcome == some .unit) "Until raw deadline or registration latch lost"
  let (futureValues,future) ← must (Runtime.invoke Frontend.processTypes {ctx with processIdentity := some proc} timer [.handle proc,.bits 64 4,.bits 64 8] initial)
  let [.handle futureWait] := futureValues | throw (IO.userError "future wait result")
  let future ← must (Runtime.suspendProcess future ctx proc futureWait)
  let some futureEvent := future.events.head? | throw (IO.userError "future resume missing")
  check (futureEvent.time == 8 && futureEvent.turn == 0 && futureEvent.stage == 5) "future timer key"
  check ((Runtime.claimWait future {ctx with now := 8,turn := 0} proc futureWait).toOption.isNone) "unobserved future timer claimed"
  let future ← must (Runtime.deliver {ctx with now := 8,turn := 0} future futureEvent)
  check ((Runtime.claimWait future {ctx with now := 8,turn := 0} proc futureWait).toOption.isSome) "future timer not ready on delivery"
  let (eventId,queued) ← must (enqueue {} ctx 8 3 0 "test" [])
  let cancelled ← must (cancelEvent queued ctx eventId)
  check (cancelled.events.length == 1 && cancelled.events.head?.any (fun event => event.cancelled)) "cancel freed reservation early"
  check ((validateState {queued with events := queued.events ++ queued.events}).toOption.isNone) "duplicate event accepted"
  check ((validateState {queued with events := queued.events.map (fun e => {e with turn := 2^64})}).toOption.isNone) "event range accepted"
  let oldA : HandleIdentity := ⟨.event,2,1,0,1,1⟩
  let liveB : HandleIdentity := ⟨.event,1,1,0,2,1⟩
  let foreign : State := {objects := [⟨oldA,"x",.unit,false⟩,⟨liveB,"x",.unit,true⟩],nextGeneration := 3}
  let (fresh,foreign) ← must (allocate foreign ctx .event 1 "x" .unit)
  check (fresh.slot == 1) "cross-domain retired slot collision"
  let _ ← must (validateState foreign)
  IO.println "Runtime reference: atomic timer registration/suspend, ready-turn, consumption, slot reuse and imported-state invariants passed"

