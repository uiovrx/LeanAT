import LeanAT.Compiler.Main
import LeanAT.Reference.Runner
import LeanAT.Compiler.OpcodeCases.Storage
open LeanAT
private def must {α : Type} : Except String α → IO α
  | .ok value => pure value
  | .error error => throw (IO.userError error)
def main : IO Unit := do
  let cases ← must Compiler.OpcodeCases.Storage.cases
  for fixture in cases do
    let validated ← must (Compiler.compile fixture.model)
    let tags := validated.project.programs.flatMap (fun p => p.blocks.flatMap (fun b => b.instructions.map (fun i => i.op.tag)))
    unless fixture.expectedOpcodeTags.all tags.contains do
      throw (IO.userError (fixture.id ++ ": missing actual lowered opcode"))
    unless (Reference.validateState fixture.input.world).toOption.isSome do
      throw (IO.userError (fixture.id ++ ": invalid seed world"))
    let source ← must (ModelIR.validateSchema fixture.model)
    let x := fixture.input
    let modelResult := Reference.runModel source fixture.handlerId x.inputs x.committed x.world x.context x.fuel
    let execResult := Reference.runExec validated fixture.programId x.inputs x.committed x.world x.context x.fuel
    unless modelResult.ok == (fixture.expectedOutcome == "success") && execResult.ok == modelResult.ok do
      throw (IO.userError (fixture.id ++ "/" ++ fixture.variant ++ ": outcome mismatch: " ++ modelResult.error ++ " / " ++ execResult.error))
    unless modelResult.world == execResult.world && modelResult.returned == execResult.returned && modelResult.committed == execResult.committed do
      throw (IO.userError (fixture.id ++ ": full source/exec state mismatch"))
    unless fixture.input.context.environment.lookup "storage.payload.record" |>.isSome do
      throw (IO.userError "missing owned native fixture seed")
  IO.println s!"OpcodeStorage: {cases.length} actual source cases compile, contain required opcodes, and match source/Exec outcomes and full state"


