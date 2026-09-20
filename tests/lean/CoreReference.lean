import LeanAT.Compiler.Main
import LeanAT.ExecIR.TimerReference
import LeanAT.Frontend.BlockSyntax
open LeanAT LeanAT.Frontend
set_option maxRecDepth 16384
at_component RefSignals where
  output irq : Bool := false
  on internal.raise event do
    set output irq := true
    return
at_component RefTransport where
  target bus : TlmBase 64 capacity 2 payload 8 mask 8
  on bus.transport fw beginReq tx do
    returnTransport accepted
at_component RefProcessOutput where
  output irq : Bool := false
  process worker : Unit maxInstances 1 frameBytes 1024 results 1 do
    await after 0
    set output irq := true
    return
private def flat (model : ModelIR.Project) : ModelIR.Project :=
  {model with components := [],handlers := model.components.flatMap (fun c => c.handlers.map (fun h => {h with endpoint := none})), states := model.components.flatMap (fun c => c.states)}
private def must {α : Type} (value : Except String α) : IO α :=
  match value with | .ok value => pure value | .error error => throw (IO.userError error)
private def check (value : Bool) (error : String) : IO Unit := unless value do throw (IO.userError error)
private def cost (p : ExecIR.ExecProject) (program : ExecIR.Program) : Nat :=
  program.blocks.foldl (fun total block => total+1+block.instructions.foldl (fun n i => n+1+(if i.op.tag ≥ 21 then ((p.services.find? (fun s => s.id == i.immediate)).map ExecIR.ServiceSignature.extraFuel).getD 0 else 0)) 0) 0
private def rejected (value : Except String ExecIR.SegmentResult) : Bool := value.toOption.isNone
def main : IO Unit := do
  let signal ← must (Compiler.compile (flat RefSignals.model))
  let some program := signal.project.programs.head? | throw (IO.userError "missing signal program")
  let fuel := cost signal.project program
  let txn : ExecIR.EventTxn := {«state» := signal.project.initialState}
  let context : ExecIR.ExecutionContext := {now := 5,instanceId := 7,schedulerFrontier := some (5,3)}
  let result ← must (ExecIR.evalExecSegment signal program.id [] txn fuel context)
  check (result.remainingFuel == 0 && result.txn.outputs == [⟨5,4,7,0,.bool true⟩]) "output exact fuel/key/value"
  check (txn.outputs.isEmpty) "input transaction mutated"
  check (rejected (ExecIR.evalExecSegment signal program.id [] txn (fuel-1) context)) "insufficient fuel accepted"
  check (rejected (ExecIR.evalExecSegment signal program.id [] {txn with outputCapacity := 0} fuel context)) "output capacity accepted"
  check (rejected (ExecIR.evalExecSegment signal program.id [] txn fuel {context with kind := 2})) "output transport context accepted"
  for mutate in [fun s : ExecIR.ServiceSignature => {s with providerVersion := "2"},fun s => {s with abiHash := ⟨Array.replicate 32 0⟩},fun s => {s with extraFuel := 2}] do
    let changed ← must (ExecIR.validateExec {signal.project with services := signal.project.services.map mutate})
    check (rejected (ExecIR.evalExecSegment changed program.id [] txn (fuel+20) context)) "untrusted provider accepted"
  let failing ← must (ExecIR.validateExec {signal.project with programs := signal.project.programs.map (fun p => {p with blocks := p.blocks.map (fun b => {b with terminator := .fail "after output"})})})
  check (rejected (ExecIR.evalExecSegment failing program.id [] txn fuel context) && txn.outputs.isEmpty) "output rollback"
  let transport ← must (Compiler.compile (flat RefTransport.model))
  let some entry := transport.project.programs.head? | throw (IO.userError "missing transport program")
  let exact := cost transport.project entry
  let returned ← must (ExecIR.evalExecSegment transport entry.id [] {«state» := transport.project.initialState} exact {kind := 2})
  check (returned.remainingFuel == 0 && returned.exit == .transport acceptedTransport && returned.txn.preparedTransport == some acceptedTransport) "transport return observation"
  check (rejected (ExecIR.evalExecSegment transport entry.id [] {«state» := transport.project.initialState} (exact-1) {kind := 2})) "transport fuel"
  check (rejected (ExecIR.evalExecSegment transport entry.id [] {«state» := transport.project.initialState} exact {})) "transport wrong context"
  let failedTransport ← must (ExecIR.validateExec {transport.project with programs := transport.project.programs.map (fun p => {p with blocks := p.blocks.map (fun b => {b with terminator := .fail "after transport prepare"})})})
  let transportTxn : ExecIR.EventTxn := {«state» := transport.project.initialState}
  check (rejected (ExecIR.evalExecSegment failedTransport entry.id [] transportTxn exact {kind := 2}) && transportTxn.preparedTransport.isNone) "transport rollback"
  let idlePrograms := signal.project.programs.map (fun p => {p with blocks := [{id := p.entry,terminator := .ret []}]})
  let idle ← must (ExecIR.validateExec {signal.project with programs := idlePrograms})
  let _ ← must (ExecIR.evalExecSegment idle program.id [] txn 1 context)
  let unusedForged ← must (ExecIR.validateExec {idle.project with services := idle.project.services.map (fun s => {s with providerVersion := "forged unused"})})
  check (rejected (ExecIR.evalExecSegment unusedForged program.id [] txn 1 context)) "unused forged provider accepted"
  let processProject ← must (Compiler.compile (flat RefProcessOutput.model))
  let some proc := processProject.project.programs.head? | throw (IO.userError "process missing")
  let identity : HandleIdentity := ⟨.process,1,1,0,1,1⟩
  let waiting ← must (ExecIR.TimerReference.start processProject proc.id identity [] 1000 7 9)
  let some wait := waiting.reference.registered | throw (IO.userError "wait missing")
  let resumed ← must (ExecIR.TimerReference.resume processProject waiting wait.identity)
  check (resumed.turn == 1 && resumed.outputs == [⟨7,2,9,0,.bool true⟩]) "process output/frontier lost"
  let laterTurn ← must (ExecIR.TimerReference.start processProject proc.id identity [] 1000 7 9 1024 3)
  let laterOutput ← must (ExecIR.TimerReference.resume processProject laterTurn wait.identity)
  check (laterOutput.turn == 4 && laterOutput.outputs == [⟨7,5,9,0,.bool true⟩]) "initial scheduler frontier lost"
  let zeroCapacity ← must (ExecIR.TimerReference.start processProject proc.id identity [] 1000 7 9 0)
  check ((ExecIR.TimerReference.resume processProject zeroCapacity wait.identity).toOption.isNone && zeroCapacity.outputs.isEmpty) "failed output committed"
  let orphanPrograms := processProject.project.programs.map (fun p => {p with blocks := (p.blocks.filter (fun b => b.id == p.entry)).map (fun b => {b with terminator := match b.terminator with | .suspend _ _ _ => .ret [] | other => other})})
  let orphan ← must (ExecIR.validateExec {processProject.project with programs := orphanPrograms})
  check ((ExecIR.TimerReference.start orphan proc.id identity [] 1000 7 9).toOption.isNone) "registered wait without suspension committed"
  let withWrite ← must (ExecIR.validateExec {orphan.project with stateTypes := [1],initialState := [.bool false],programs := orphan.project.programs.map (fun p => {p with effectMask := Nat.lor p.effectMask 2,blocks := p.blocks.map (fun b => {b with instructions := b.instructions ++ [{op := .const,dest := some ⟨100,1⟩,value := .bool true},{op := .bufferStateWrite,args := [⟨100,1⟩],immediate := 0}]})})})
  check ((ExecIR.TimerReference.start withWrite proc.id identity [] 1000 7 9).toOption.isNone && withWrite.project.initialState == [.bool false]) "orphan wait state write committed"
  let forgedResume ← must (ExecIR.validateExec {processProject.project with services := processProject.project.services.map (fun s => if s.op == .getContextField then {s with abiHash := ⟨Array.replicate 32 0⟩} else s)})
  check ((ExecIR.TimerReference.resume forgedResume waiting wait.identity).toOption.isNone) "resume skipped unused registry preflight"
  IO.println "Core reference: generated signal and transport, exact fuel, staged rollback, registry preflight, orphan wait, process output turn, ABI/context rejection passed"









