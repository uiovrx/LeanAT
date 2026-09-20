import LeanAT.Compiler.OpcodeCatalog
import LeanAT.Compiler.OpcodeCases.All

def main (args : List String) : IO Unit := do
  let [outputPath] := args | throw (IO.userError "expected output directory")
  let cases ← match LeanAT.Compiler.OpcodeCases.All.cases with
    | .ok cases => pure cases | .error e => throw (IO.userError e)
  LeanAT.Compiler.writeOpcodeCatalog cases outputPath
  IO.println "Actual source opcode catalog emitted"