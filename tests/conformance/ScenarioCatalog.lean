import tests.conformance.models.WireModels
import tests.conformance.models.SystemCModels
import tests.conformance.TopologyModels

def main (args : List String) : IO Unit := do
  let [output] := args | throw (IO.userError "expected output directory")
  LeanAT.Compiler.writeScenarioCatalog (LeanAT.Conformance.WireModels.scenarios ++
    LeanAT.Conformance.MemoryModels.scenarios ++ LeanAT.Conformance.RuntimeModels.scenarios ++
    LeanAT.Conformance.SystemCModels.scenarios ++ LeanAT.Conformance.TopologyModels.scenarios) output
  IO.println "Fresh Core/Ext scenario catalog emitted from canonical source models"
