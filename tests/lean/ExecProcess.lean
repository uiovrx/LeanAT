import LeanAT.Frontend.BlockSyntax
import LeanAT.Compiler.Lowering
import LeanAT.ExecIR.TimerReference
import LeanAT.ModelIR.Process
set_option maxRecDepth 16384
set_option maxHeartbeats 4000000
open LeanAT

at_component IndependentTimer where
  state cell : UInt64 := 0
  process worker : Unit maxInstances 1 frameBytes 1024 results 1 do
    let saved : UInt64 := 41
    await until 5
    set cell := saved
    await after 2
    set cell := 42
    return

def main : IO Unit := do
  let model ← match ModelIR.validateSchema IndependentTimer.model with | .ok p => pure p | .error e => throw (IO.userError e)
  let executable ← match Compiler.compile IndependentTimer.model with | .ok p => pure p | .error e => throw (IO.userError e)
  let identity : HandleIdentity := ⟨.process,1,1,0,1,1⟩
  let high ← match ModelIR.Process.start model 0 identity [] with | .ok p => pure p | .error e => throw (IO.userError e)
  let low ← match ExecIR.TimerReference.start executable 0 identity [] with | .ok p => pure p | .error e => throw (IO.userError e)
  let some oldWait := low.reference.registered | throw (IO.userError "first wait missing")
  let highPaused := ModelIR.Process.runFrom model (some 5) 100 high
  let lowPaused := ExecIR.TimerReference.runFrom executable (some 5) 1000 100 low
  if lowPaused.frame.context.now != highPaused.frame.time || lowPaused.frame.state[0]? != highPaused.frame.committed.values.lookup 0 then throw (IO.userError "first resume mismatch")
  if lowPaused.frame.state[0]? != some (.bits 64 41) then throw (IO.userError "saved register lost")
  match ExecIR.TimerReference.resume executable lowPaused.frame oldWait.identity with
  | .ok _ => throw (IO.userError "old notification accepted")
  | .error _ => pure ()
  let highDone := ModelIR.Process.runFrom model none 100 highPaused.frame
  let lowDone := ExecIR.TimerReference.runFrom executable none 1000 100 lowPaused.frame
  if lowDone.reason != .quiescent || highDone.reason != .quiescent then throw (IO.userError "process did not complete")
  if lowDone.frame.context.now != 7 || lowDone.frame.state[0]? != some (.bits 64 42) || lowDone.frame.context.now != highDone.frame.time then throw (IO.userError "second resume mismatch")
  if lowDone.frame.reference.nextWaitGeneration != highDone.frame.nextWaitGeneration then throw (IO.userError "wait generation mismatch")
  IO.println "Independent ModelIR/ExecIR native two-await timer reference: time7 state42, stale notification rejected"
