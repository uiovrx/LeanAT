import LeanAT.ModelIR.Process
import LeanAT.Frontend.Builtins
namespace LeanAT.ModelIR.Process.Tests
private def self : HandleIdentity := ⟨.process,1,0,3,1,3⟩
private def body : List Stmt := [
  .serviceCall (some ⟨0,6,{}⟩) 0 [],
  .letVal 5 (.literal 4 (.bits 32 42)),
  .serviceCall (some ⟨1,7,{}⟩) 1 [.local 6 0,.literal 5 (.bits 64 4),.literal 5 (.bits 64 10)],
  .await (.local 7 1) 2 0,
  .emit "first" [.local 4 5],
  .serviceCall (some ⟨3,7,{}⟩) 1 [.local 6 0,.literal 5 (.bits 64 3),.literal 5 (.bits 64 5)],
  .await (.local 7 3) 4 0,
  .emit "second" [.local 4 5],
  .ret []]
private def project : Project := {
  types := Frontend.processTypes
  services := Frontend.timerServices
  handlers := [{id := 0,body,context := .process,processCapacity := some {maxInstances := 1,frameBytesLimit := 128,resultCapacity := 1},declaredResultTypes := some []}]
}
private def checkedRun (project : Project) (budget : Nat) (horizon : Option Nat) : Except String RunResult := do
  let p ← validateSchema project
  let frame ← start p 0 self []
  pure (runFrom p horizon budget frame)
#guard match checkedRun project 11 none with
  | .error _ => false
  | .ok r => r.reason == .quiescent && r.frame.time == 15 && r.frame.committed.trace == [⟨"first",[.bits 32 42]⟩,⟨"second",[.bits 32 42]⟩]
#guard match checkedRun project 50 (some 9) with
  | .error _ => false
  | .ok r => r.reason == .horizonReached && r.frame.time == 0 && r.frame.committed.trace.isEmpty
#guard match validateSchema project with
  | .error _ => false
  | .ok p => match start p 0 self [] with
    | .error _ => false
    | .ok f =>
      let first := runFrom p none 4 f
      let second := runFrom p none 7 first.frame
      first.reason == .runBudgetReached && second.reason == .quiescent && second.frame.time == 15
#guard match checkedRun {project with handlers := project.handlers.map (fun h => {h with processCapacity := some {maxInstances := 1,frameBytesLimit := 16,resultCapacity := 1}})} 50 none with
  | .error _ => false
  | .ok r => r.reason == .failed "FrameCapacityExceeded" && r.frame.committed.trace.isEmpty
#guard match validateSchema project with
  | .error _ => false
  | .ok p => match start p 0 self [] with
    | .error _ => false
    | .ok frame => let r := runFrom p none 50 {frame with segmentFuel := 3}
                   r.reason == .failed "FuelExhausted" && r.frame.committed.trace.isEmpty#guard match checkedRun {project with handlers := project.handlers.map (fun h => {h with body := body.take 6 ++ [.await (.local 7 1) 4 0]})} 50 none with
  | .error _ => false
  | .ok r => r.reason == .failed "StaleSuspensionToken" && r.frame.committed.trace.isEmpty
#guard match checkedRun {project with services := project.services.map (fun s => {s with abiHash := List.replicate 32 0})} 50 none with
  | .error _ => false
  | .ok r => r.reason == .failed "ProviderABIMismatch"
#guard match checkedRun {project with handlers := project.handlers.map (fun h => {h with body := body.take 3 ++ [.ret []]})} 50 none with
  | .error _ => false
  | .ok r => r.reason == .failed "UnconsumedWaitRegistration"
end LeanAT.ModelIR.Process.Tests


