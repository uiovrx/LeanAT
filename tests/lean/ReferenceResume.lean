import LeanAT.Reference.Runner
import LeanAT.Compiler.Main
import LeanAT.Frontend.Builtins
open LeanAT LeanAT.Reference
private def must (value : Except String α) : IO α := match value with | .ok value => pure value | .error error => throw (IO.userError error)
private def check (condition : Bool) (error : String) : IO Unit := unless condition do throw (IO.userError error)
private def u (n : Nat) : Value := .bits 64 n
private def project : ModelIR.Project := {
  types := Frontend.processTypes
  states := [⟨0,5,u 0⟩]
  services := Frontend.timerServices
  handlers := [{id := 0,context := .process,processCapacity := some {frameBytesLimit := 128,resultCapacity := 1},declaredResultTypes := some [],body := [
    .serviceCall (some ⟨0,6,{}⟩) 0 [],
    .serviceCall (some ⟨1,7,{}⟩) 1 [.local 6 0,.literal 5 (u 4),.literal 5 (u 2)],
    .await (.local 7 1) 2 0,
    .writeState 0 (.literal 5 (u 42)),
    .ret []]}]}
private def process : HandleIdentity := ⟨.process,1,10,0,1,9⟩
private def context : Context := {kind := 1,domain := 1,owner := 9,processIdentity := some process}
def main : IO Unit := do
  let model ← must (ModelIR.validateSchema project)
  let compiled ← must (Compiler.compile project)
  let world ← must (Runtime.seedProcess {} context process 0)
  let some handler := project.handlers.head? | throw (IO.userError "handler")
  let source : Source.Machine := {values := [(0,u 0)],world,context,remainingFuel := 100,program := 0}
  let source ← match Source.run project handler.body source with
    | .ok _ saved => pure saved | .error error _ => throw (IO.userError error)
  let some sourceEvent := source.world.events.head? | throw (IO.userError "source event")
  let forged := resumeModel model 0 source [u 0] source.world {context with now := 2} {sourceEvent with sequence := sourceEvent.sequence+1} 100
  check (!forged.outcome.ok && forged.outcome.world == source.world) "forged queued source event accepted"
  let resumed := resumeModel model 0 source [u 0] source.world {context with now := 2} sourceEvent 100
  check (resumed.outcome.ok && resumed.outcome.committed == [u 42] && resumed.continuation.isNone) "source queued resume failed"
  check ((Runtime.getProcess resumed.outcome.world context process).toOption.isNone) "completed process remained live"
  check (resumed.outcome.world.observations.any (fun o => o.kind == "process.completed")) "completion ownership observation missing"
  let replay := resumeModel model 0 source [u 42] resumed.outcome.world {context with now := 2} sourceEvent 100
  check (!replay.outcome.ok) "old source suspension replayed"
  let some program := compiled.project.programs.head? | throw (IO.userError "program")
  let execution : ExecIR.ExecutionContext := {kind := 1,domain := 1,owner := some 9,processIdentity := some process,referenceContext := some context}
  let exec : Exec.Machine := {txn := {state := [u 0],world := some world},remainingFuel := 100}
  let exec ← match Exec.run compiled.project execution program [] exec with
    | .ok _ saved => pure saved | .error error _ => throw (IO.userError error)
  let execWorld := exec.txn.world.getD {}
  let some execEvent := execWorld.events.head? | throw (IO.userError "exec event")
  let some (.handle wait) := exec.wait | throw (IO.userError "saved wait")
  let readyWorld ← must (Runtime.publishWait execWorld context wait (2,0) .unit)
  let cancelledWorld ← must (cancelEvent readyWorld context execEvent.identity)
  let some cancelledEvent := cancelledWorld.events.find? (fun e => e.identity == execEvent.identity)
    | throw (IO.userError "cancelled event")
  for outcome in [
    (resumeModel model 0 source [u 0] cancelledWorld {context with now := 2} cancelledEvent 100).outcome,
    (resumeExec compiled 0 exec [u 0] cancelledWorld {context with now := 2} cancelledEvent 100).outcome] do
    check (!outcome.ok && outcome.error == "CancelledResumeEvent" && outcome.world == cancelledWorld &&
      outcome.committed == [u 0] && outcome.trace.isEmpty && outcome.remainingFuel == 100)
      "cancelled ready event executed or claimed its continuation"
  let skipped ← must (Runtime.deliver {context with now := 2} cancelledWorld cancelledEvent)
  let unclaimed ← must (Runtime.getWait skipped context wait)
  check (!unclaimed.claimed && unclaimed.outcome == some .unit &&
    !(skipped.events.any (fun e => e.identity == cancelledEvent.identity)))
    "scheduler skip claimed the cancelled wait"
  let tampered := {cancelledEvent with values := [.bits 64 0x4c415452,.handle process,.handle wait,u 999]}
  let tamperedWorld := {cancelledWorld with events := cancelledWorld.events.map (fun e =>
    if e.identity == tampered.identity then tampered else e)}
  let rejected := resumeExec compiled 0 exec [u 0] tamperedWorld {context with now := 2} tampered 100
  check (!rejected.outcome.ok && rejected.outcome.error == "StaleSuspensionToken" && rejected.outcome.world == tamperedWorld)
    "cancelled event bypassed ordinal authentication"
  let protocolWorld ← must (Protocol.seedProtocol context execWorld)
  let control ← must (Protocol.getControl protocolWorld)
  let exhaustedControl := {control with wakeGeneration := 2^64-1}
  let failingWorld := {protocolWorld with objects := protocolWorld.objects.map (fun entry =>
    if entry.tag == "protocol.control" then {entry with value := exhaustedControl.encode} else entry)}
  let deliveredBoundary ← must (Runtime.deliver {context with now := 2} failingWorld execEvent)
  for outcome in [
    (resumeModel model 0 source [u 0] failingWorld {context with now := 2} execEvent 100).outcome,
    (resumeExec compiled 0 exec [u 0] failingWorld {context with now := 2} execEvent 100).outcome] do
    check (!outcome.ok && outcome.error == "WakeGenerationExhausted" && outcome.committed == [u 0] &&
      !outcome.trace.isEmpty && outcome.remainingFuel < 100 && outcome.world == deliveredBoundary)
      "resume commit failure lost its cause, prefix or rollback boundary"
  let resumedExec := resumeExec compiled 0 exec [u 0] execWorld {context with now := 2} execEvent 100
  check (resumedExec.outcome.ok && resumedExec.outcome.committed == [u 42] && resumedExec.outcome.world == resumed.outcome.world) "independent resumed worlds differ"
  let (_,again) ← must (Runtime.createProcess resumed.outcome.world context 0)
  check (again.objects.length == resumed.outcome.world.objects.length) "process slot not reused after completion"
  let exhausted : State := {nextGeneration := 2^64}
  let (unchanged,nonce) ← must (reserveSegment exhausted)
  check (finishSegment unchanged nonce == exhausted) "no-op segment burned identities"
  IO.println "ReferenceResume: authenticated queue, independent continuation, completion retirement, replay and no-op allocator PASS"
