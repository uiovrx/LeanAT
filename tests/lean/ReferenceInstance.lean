import LeanAT.Reference.Runtime
import LeanAT.Frontend.Builtins
open LeanAT LeanAT.Reference
private def must (value : Except String α) : IO α := match value with | .ok value => pure value | .error error => throw (IO.userError error)
private def check (condition : Bool) (error : String) : IO Unit := unless condition do throw (IO.userError error)
private def signature (index : Nat) (op : ExecIR.Op) : Except String ExecIR.ServiceSignature := do
  let some source := Frontend.timerServices[index]? | throw "MissingBuiltin"
  pure ⟨source.id,op,source.inputTypes,source.resultTypes,source.contextMask,source.effectMask,source.extraFuel,source.providerKey,source.providerVersion,⟨source.abiHash.toArray⟩⟩
private def identity : HandleIdentity := ⟨.process,1,10,0,1,7⟩
private def context : Context := {kind := 1,domain := 1,owner := 7,instanceId := 11,processIdentity := some identity}
private def rejected (attempt : EStateM.Result String State α) (before : State) : IO Unit :=
  match attempt with
  | .error "ProcessInstanceMismatch" after => check (after == before) "cross-instance call mutated state or allocation journal"
  | _ => throw (IO.userError "cross-instance process authority accepted or wrong error")
def main : IO Unit := do
  let getContext ← must (signature 0 .getContextField)
  let timer ← must (signature 1 .registerWait)
  let world ← must (Runtime.seedProcess {} context identity 0)
  let process ← must (Runtime.getProcess world context identity)
  check (process.instanceId == 11) "seed lost bound instance"
  let decoded ← must (Runtime.ProcessData.decode process.encode)
  check (decoded == process) "process instance codec roundtrip"
  let .record fields := process.encode | throw (IO.userError "process codec")
  check ((Runtime.ProcessData.decode (.record (fields.take 6 ++ [.bits 64 (2^32)]))).toOption.isNone) "process instance UInt32 overflow accepted"
  let wrong := {context with instanceId := 12}
  rejected ((Runtime.invokeAttempt Frontend.processTypes wrong getContext []).run world) world
  rejected ((Runtime.invokeAttempt Frontend.processTypes wrong timer [.handle identity,.bits 64 4,.bits 64 8]).run world) world
  match (Runtime.invokeAttempt Frontend.processTypes context getContext []).run world with
  | .ok [.handle actual] after => check (actual == identity && after == world) "same-instance context control"
  | _ => throw (IO.userError "same-instance process rejected")
  let (wait,registered) ← must (Runtime.newWait world context identity 4 8 none none none)
  rejected ((Runtime.publishWaitAttempt wrong wait (8,0) .unit).run registered) registered
  match (Runtime.publishWaitAttempt context wait (8,0) .unit).run registered with
  | .ok _ published =>
    let ready ← must (Runtime.getWait published context wait)
    check (ready.outcome == some .unit && ready.readyKey == some (8,0)) "same-instance publication control"
  | .error error _ => throw (IO.userError error)
  check ((Runtime.putProcess world context identity {process with instanceId := 12}).toOption.isNone) "stored process instance mutable"
  let (fresh,created) ← must (Runtime.createProcess {} context 0)
  let createdProcess ← must (Runtime.getProcess created context fresh)
  check (createdProcess.instanceId == 11 && (Runtime.getProcess created wrong fresh).toOption.isNone) "create did not bind instance"
  IO.println "ReferenceInstance: process instance binding, canonical calls, publication, no-mutation rejection and codec bounds PASS"
