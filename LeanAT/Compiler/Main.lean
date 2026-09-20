import LeanAT.Compiler.Codegen
import LeanAT.ModelIR.Semantics

namespace LeanAT.Compiler
open Lean

partial def valueJson : Value → Json
  | .unit => Json.mkObj [("kind",.str "unit"),("value",.null)]
  | .bool b => Json.mkObj [("kind",.str "bool"),("value",.bool b)]
  | .bits _ n => Json.mkObj [("kind",.str "u64"),("value",.str (toString n))]
  | .record vs | .vec vs => Json.mkObj [("kind",.str "array"),("value",.arr (vs.map valueJson).toArray)]
  | .variant tag vs => valueJson (.record [.bits 64 tag,.record vs])
  | .bytes bs => Json.mkObj [("kind",.str "bytes"),("value",toJson (bs.map UInt8.toNat))]
  | .handle h => Json.mkObj [("kind",.str "handle"),("value",.str (reprStr h))]

private def report (result state : List Value) (fuel : Nat) (error : String := "") : Json :=
  Json.mkObj [("scope",.str "segment"),("stopReason",.str (if error.isEmpty then "Completed" else if error == "FuelExhausted" then error else "Failed")),
    ("complete",.bool error.isEmpty),("fuelUsed",.str (toString fuel)),("result",.arr (result.map valueJson).toArray),
    ("state",.arr (state.map valueJson).toArray),("error",.str error)]
private def optionNat (args : List String) (flag : String) (fallback : Nat) :=
  ((args.dropWhile (· != flag)).drop 1).head?.bind String.toNat? |>.getD fallback

def runCLI (model : ModelIR.Project) (args : List String) (sourceBundle : SourceMapping.Bundle := {}) : IO UInt32 := do
  let stdout ← IO.getStdout
  let stderr ← IO.getStderr
  let fail := fun (error : String) => do stderr.putStrLn ((Json.mkObj [("error",.str error)]).compress); pure (3 : UInt32)
  let compilation ← match compileWithProvenance model with | .ok p => pure p | .error error => return ← fail error
  let validated := compilation.validated
  match args with
  | ["check"] => stdout.putStrLn "{\"status\":\"checked\",\"scope\":\"model-and-exec-schema\"}"; pure 0
  | ["dump-model-ir"] => stdout.putStrLn (reprStr model); pure 0
  | ["dump-exec-ir"] => stdout.putStrLn (reprStr validated.project); pure 0
  | ["dump-exec-ir","--table",table] =>
    let content ← match table with
      | "programs" => pure (reprStr validated.project.programs)
      | "types" => pure (reprStr validated.project.types)
      | "stateTypes" => pure (reprStr validated.project.stateTypes)
      | "initialState" => pure (reprStr validated.project.initialState)
      | _ => return ← fail "UnsupportedTable"
    stdout.putStrLn content
    pure 0
  | ["emit",output] =>
    let artifacts ← match emitSystemC validated (some compilation) sourceBundle with | .ok a => pure a | .error error => return ← fail error
    try
      writeArtifacts artifacts output
      stdout.putStrLn ((Json.mkObj [("status",.str "emitted"),("output",.str output)]).compress)
      pure 0
    catch e => fail e.toString
  | command::options =>
    if command != "run-model-segment" && command != "run-exec-segment" && command != "run-ref" then
      return ← fail "UnsupportedCommand"
    if options.contains "--transcript" then return ← fail "Segment runner does not implement transcript scheduling"
    let id := optionNat options "--program" ((model.handlers.head?.map ModelIR.Handler.id).getD 0)
    let fuel := optionNat options "--fuel" 10000
    let initial := validated.project.initialState
    let result : Except String (List Value × List Value × Nat) := do
      if command == "run-model-segment" then
        let some h := model.handlers.find? (fun h => h.id == id) | throw "UnknownProgram"
        let state : ModelIR.RuntimeState := {values := model.states.map (fun s => (s.id,s.initial))}
        let (state,left) ← ModelIR.execStmts model fuel state h.body
        pure (state.returned.getD [],model.states.map (fun s => (state.values.lookup s.id).getD s.initial),fuel-left)
      else
        let context : ExecIR.ExecutionContext := {
          kind := ((validated.project.programs.find? (fun p => p.id == id)).map ExecIR.Program.context).getD 0
          domain := (validated.project.systemMetadata.map ModelIR.SystemIR.runtimeDomain).getD 0
          instanceId := ((validated.project.instances.find? (fun inst => inst.handlers.any (fun h => h.programId == id))).map ExecIR.InstanceDesc.id).getD 0}
        let segment ← ExecIR.evalExecSegment validated id [] {state := validated.project.initialState} fuel context
        match segment.exit with | .suspended .. => throw "Suspended: scalar runner requires a scheduler" | _ => pure ()
        pure (segment.returned,ExecIR.commit segment.txn,fuel-segment.remainingFuel)
    match result with
    | .ok (values,state,used) => stdout.putStrLn ((report values state used).compress); pure 0
    | .error error => stdout.putStrLn ((report [] initial 0 error).compress); pure 4
  | _ => fail "usage: check | dump-model-ir | dump-exec-ir | emit OUTPUT | run-model-segment | run-exec-segment [--program ID --fuel N]"

/-- Small executable model used by the cross-language package smoke fixture. -/
def scalarExample : ModelIR.Project := {
  types := [.bool,.bits 64,.vec 1 0]
  states := [⟨0,1,.bits 64 0⟩]
  handlers := [{id := 0, source := ⟨"scalar-example",1,1⟩, body := [
    .letVal 0 (.select 1 (.literal 0 (.bool false))
      (.index 1 (.literal 2 (.vec [])) (.literal 1 (.bits 64 99)))
      (.literal 1 (.bits 64 42))),
    .writeState 0 (.local 1 0),.ret [.local 1 0]]}]
}

end LeanAT.Compiler

