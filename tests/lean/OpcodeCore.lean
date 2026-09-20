import LeanAT.Compiler.OpcodeCases.Core
import LeanAT.Compiler.Codegen

def main : IO Unit := do
  let cases ← match LeanAT.Compiler.OpcodeCases.Core.cases with
    | .ok cases => pure cases
    | .error e => throw (IO.userError e)
  for candidate in cases do
    match LeanAT.Compiler.compileForProfile candidate.model candidate.profile with
    | .error e => throw (IO.userError (candidate.id ++ ": " ++ e))
    | .ok project =>
      let tags := project.project.programs.flatMap fun p => p.blocks.flatMap fun b => b.instructions.map fun i => i.op.tag
      if !candidate.expectedOpcodeTags.all tags.contains then
        throw (IO.userError (candidate.id ++ ": missing actual opcode"))
  IO.println s!"{cases.length} core opcode sources compiled"
