import LeanAT.Compiler.Lowering
import LeanAT.Compiler.Load
import LeanAT.Compiler.SourceMap
import LeanAT.ExecIR.Reference
open LeanAT LeanAT.Compiler

def policyProcess (fuel : Nat) : ModelIR.ProcessIR := {
  id := 0
  params := []
  resultType := 0
  body := [.ret [.literal 0 (.bits 64 42)]]
  capacity := {frameBytesLimit := 64,resultCapacity := 1}
  instructionFuel := fuel
  ownerPolicy := "caller"
  resultLifetimePolicy := "until-release"
}

def policyProject (fuel : Nat := 2) : ModelIR.Project := {
  types := [.bits 64]
  components := [{id := ⟨1⟩,processes := [policyProcess fuel]}]
  systems := [{id := 0,runtimeDomain := 1,instances := [{id := ⟨1⟩,definition := ⟨1⟩}],bindings := []}]
  topSystemId := some 0
}
def main : IO Unit := do
  let checkProcessReturn := fun (body : List ModelIR.Stmt) => do
    let process := {policyProcess 20 with body}
    let project := {policyProject 20 with
      types := [.bits 64,.bool]
      components := [{id := ⟨1⟩,processes := [process]}]}
    match ModelIR.validateSchema project with
    | .error "DeclaredReturnTypeMismatch" => pure ()
    | _ => throw (IO.userError "ProcessIR declared return shape not enforced")
  checkProcessReturn [.ret [.literal 1 (.bool true)]]
  checkProcessReturn [.ret []]
  checkProcessReturn [.branch (.literal 1 (.bool true))
    [.ret [.literal 0 (.bits 64 42)]] [.ret [.literal 1 (.bool false)]]]
  let .ok compilation := compileWithProvenance (policyProject 2) | throw (IO.userError "policy lowering")
  let some original := compilation.nodes.find? (fun n => n.path == "component/1/process/0") | throw (IO.userError "missing original ProcessIR node")
  if original.kind != "process" || original.text != reprStr (policyProcess 2) then throw (IO.userError "synthetic ProcessIR parent")
  if !(compilation.origins.any (fun o => o.kind == "program" && o.node == original.path)) then throw (IO.userError "process program mapping")
  if !(compilation.origins.any (fun o => o.kind == "instruction" && o.node == "component/1/process/0/body/0/expr/0")) then throw (IO.userError "process body mapping")
  let project := compilation.validated
  if project.project.schemaMajor != 5 then throw (IO.userError "missing v5")
  let .ok decoded := deserialize (serialize project) | throw (IO.userError "v5 roundtrip")
  if serialize decoded != serialize project then throw (IO.userError "noncanonical v5")
  let some decodedProgram := decoded.project.programs.head? | throw (IO.userError "missing decoded process")
  if decodedProgram.instructionFuel != 2 || decodedProgram.ownerPolicy != "caller" || decodedProgram.resultLifetimePolicy != "until-release" then throw (IO.userError "decoded policy fields lost")
  let .ok _ := buildSourceMap compilation | throw (IO.userError "process source map")
  let .ok result := ExecIR.evalExecSegment decoded 0 [] {state := []} 100 {kind := 1,domain := 1,instanceId := 1}
    | throw (IO.userError "exact policy fuel")
  if result.remainingFuel != 98 then throw (IO.userError "caller fuel accounting")
  match ExecIR.evalExecSegment decoded 0 [] {state := []} 1 {kind := 1,domain := 1,instanceId := 1} with
  | .error "FuelExhausted" => pure ()
  | _ => throw (IO.userError "caller budget not enforced")
  for (owner,lifetime) in [("custom-owner","until-release"),("caller","custom-lifetime")] do
    let changed := {decoded.project with programs := [{decodedProgram with ownerPolicy := owner,resultLifetimePolicy := lifetime}]} 
    match ExecIR.validateExec changed with
    | .error "UnsupportedProcessPolicy" => pure ()
    | _ => throw (IO.userError "unknown persisted process policy accepted")
  let .ok low := compile (policyProject 1) | throw (IO.userError "low policy")
  match ExecIR.evalExecSegment low 0 [] {state := []} 100 {kind := 1,domain := 1,instanceId := 1} with
  | .error "FuelExhausted" => pure ()
  | _ => throw (IO.userError "source cap not enforced")
  let sharedNamespace := {(policyProject 2) with components := [{
    id := ⟨1⟩
    handlers := [{id := 0,body := [.ret [.literal 0 (.bits 64 7)]]}]
    processes := [policyProcess 2]
  }]}
  let .ok mixed := compileWithProvenance sharedNamespace | throw (IO.userError "handler/process shared local ID")
  let some timed := mixed.validated.project.programs[0]? | throw (IO.userError "timed program")
  let some process := mixed.validated.project.programs[1]? | throw (IO.userError "process program")
  if timed.context != 0 || timed.instructionFuel != 0 || !timed.ownerPolicy.isEmpty || process.context != 1 || process.instructionFuel != 2 then
    throw (IO.userError "process policy attached by ambiguous local ID")
  if timed.source != "component/1/handler/0" || process.source != "component/1/process/0" then throw (IO.userError "origin classification lost")
  let .ok _ := buildSourceMap mixed | throw (IO.userError "shared ID source coverage")
  let .ok timedResult := ExecIR.evalExecSegment mixed.validated 0 [] {state := []} 2 {domain := 1,instanceId := 1}
    | throw (IO.userError "timed shared ID execution")
  let .ok processResult := ExecIR.evalExecSegment mixed.validated 1 [] {state := []} 2 {kind := 1,domain := 1,instanceId := 1}
    | throw (IO.userError "process shared ID execution")
  if timedResult.returned != [.bits 64 7] || processResult.returned != [.bits 64 42] then throw (IO.userError "shared ID program selection")
  IO.FS.createDirAll "build/compiler-fixtures"
  IO.FS.writeBinFile "build/compiler-fixtures/process-policy-v5.execir.bin" (serialize project)
  IO.FS.writeBinFile "build/compiler-fixtures/process-policy-shared-v5.execir.bin" (serialize mixed.validated)
  IO.println "ProcessIR v5 policy preservation, exact fuel cap, canonical codec and source mapping passed"
