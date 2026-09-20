import LeanAT.Compiler.Main

open LeanAT LeanAT.Compiler

def hierarchyExample : ModelIR.Project := {
  types := [.bits 64]
  components := [{id := ⟨3⟩,states := [⟨0,0,.bits 64 0⟩],handlers := [{id := 7,body := [.writeState 0 (.literal 0 (.bits 64 42)),.ret []]}]}]
  systems := [{id := 9,runtimeDomain := 7,instances := [{id := ⟨10⟩,definition := ⟨3⟩},{id := ⟨20⟩,definition := ⟨3⟩}],bindings := []}]
  topSystemId := some 9
}

def main : IO Unit := do
  let project ← match compile hierarchyExample with | .ok p => pure p | .error e => throw (IO.userError e)
  let .ok decoded := deserialize (serialize project) | throw (IO.userError "hierarchy decode")
  if serialize decoded != serialize project then throw (IO.userError "hierarchy canonical roundtrip")
  let .ok result := ExecIR.evalExecSegment project 1 [] {state := project.project.initialState} 3 {domain := 7,instanceId := 20} | throw (IO.userError "instance execute")
  if ExecIR.commit result.txn != [.bits 64 0,.bits 64 42] then throw (IO.userError "instance state isolation")
  let .ok artifacts := emitSystemC project | throw (IO.userError "hierarchy facade emit")
  IO.FS.createDirAll "build/compiler-fixtures"
  IO.FS.writeBinFile "build/compiler-fixtures/hierarchy.execir.bin" (serialize project)
  if !(← (System.FilePath.mk "build/generated-hierarchy").pathExists) then writeArtifacts artifacts "build/generated-hierarchy"
  IO.println "Hierarchy v4 canonical metadata, instance state isolation, shared-domain facade emitted"

