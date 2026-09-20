import LeanAT.Frontend.BlockSyntax
import LeanAT.ModelIR.Process
import LeanAT.Compiler.Main
set_option maxRecDepth 16384
set_option maxHeartbeats 4000000
open LeanAT

at_component TimedProcess where
  state cell : UInt64 := 0
  process worker : Unit maxInstances 1 frameBytes 1024 results 1 do
    let saved : UInt64 := 41
    await until 5
    set cell := saved
    await after 2
    set cell := 42
    return

def processReference : Except String (Nat × Option Value × Nat) := do
  let checked ← ModelIR.validateSchema TimedProcess.model
  let frame ← ModelIR.Process.start checked 0 ⟨.process,1,1,0,1,1⟩ []
  let paused := ModelIR.Process.runFrom checked (some 5) 100 frame
  if paused.frame.time != 5 then throw "FirstAwaitDidNotResumeAtFive"
  if paused.frame.committed.values.lookup 0 != some (.bits 64 41) then throw "LiveLocalLostAcrossAwait"
  let done := ModelIR.Process.runFrom checked none 100 paused.frame
  match done.reason with
  | .quiescent => pure (done.frame.time,done.frame.committed.values.lookup 0,done.frame.nextWaitGeneration)
  | _ => throw "SecondAwaitDidNotComplete"

def main (args : List String) : IO UInt32 := do
  match args with
  | ["emit", path] =>
    match Compiler.compile TimedProcess.model with
    | .error e => IO.eprintln e; pure 1
    | .ok executable => IO.FS.writeBinFile path (Compiler.serialize executable); pure 0
  | [] | ["run"] =>
    match processReference with
    | .error e => IO.eprintln e; pure 1
    | .ok value => IO.println (reprStr value); pure 0
  | _ => Compiler.runCLI TimedProcess.model args TimedProcess.sourceBundle
