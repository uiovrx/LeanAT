import LeanAT.Compiler.Main
import LeanAT.Reference.Runner
import LeanAT.Compiler.OpcodeCases.Protocol
open LeanAT
private def must {α : Type} : Except String α → IO α
  | .ok value => pure value
  | .error error => throw (IO.userError error)
def main : IO Unit := do
  let cases ← must Compiler.OpcodeCases.Protocol.cases
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
  let some fixture := cases.find? (fun c => c.id == "newTransaction" && c.variant == "success")
    | throw (IO.userError "missing NewTransaction boundary fixture")
  let source ← must (ModelIR.validateSchema fixture.model)
  let x := fixture.input
  let first := Reference.runModel source fixture.handlerId x.inputs x.committed x.world x.context x.fuel
  unless first.ok do throw (IO.userError "boundary setup failed")
  for ledgerBound in [false,true] do
    let control ← must (Reference.Protocol.getControl first.world)
    let bounded := if ledgerBound then {control with ledgers := 1} else {control with drains := 1}
    let world := {first.world with objects := first.world.objects.map (fun o =>
      if o.tag == "protocol.control" then {o with value := bounded.encode} else o)}
    let inputs := x.inputs.zipIdx.map (fun (v,n) => if n == 1 then Value.bits 64 2 else v)
    let failed := Reference.runModel source fixture.handlerId inputs [] world x.context x.fuel
    let after ← must (Reference.Protocol.getControl failed.world)
    unless !failed.ok && after.nextAdmission == control.nextAdmission+2 do
      throw (IO.userError "downstream capacity rejection lost two admission identity burns")
    unless failed.world.objects.filter (fun o => o.tag != "protocol.control") == world.objects.filter (fun o => o.tag != "protocol.control") do
      throw (IO.userError "capacity failure published semantic draft")
  let some (.handle txn) := first.returned.head? | throw (IO.userError "missing created transaction")
  let some stageCase := cases.find? (fun c => c.id == "stagePhase" && c.variant == "success")
    | throw (IO.userError "missing StagePhase fixture")
  let some cancelCase := cases.find? (fun c => c.id == "cancelLocal" && c.variant == "success")
    | throw (IO.userError "missing CancelLocal fixture")
  let stageSource ← must (ModelIR.validateSchema stageCase.model)
  let cancelSource ← must (ModelIR.validateSchema cancelCase.model)
  let staged := Reference.runModel stageSource stageCase.handlerId
    [.handle txn,.bits 64 1,.bits 64 1,.bits 64 100] [] first.world x.context x.fuel
  unless staged.ok do throw (IO.userError "future request setup failed")
  let cancelled := Reference.runModel cancelSource cancelCase.handlerId
    [.handle txn,.bits 64 1] [] staged.world x.context x.fuel
  let retained ← must (Reference.Protocol.getTransaction cancelled.world x.context txn)
  let retainedControl ← must (Reference.Protocol.getControl cancelled.world)
  unless cancelled.ok && retained.cancelled && retained.drain.registered && !retained.drain.hopPresent && retainedControl.intents.length == 1 do
    throw (IO.userError "unsent gate-owned cancellation prematurely retired external send ownership")
  let control ← must (Reference.Protocol.getControl x.world)
  let exhausted := {control with nextAdmission := 2^64-2}
  let counter : Reference.AllocationCounter := {
    group := "protocol.admission"
    domain := x.context.domain
    nextGeneration := 2^64-2 }
  let world := {x.world with
    nextGeneration := 2^64-2
    allocationCounters := x.world.allocationCounters ++ [counter]
    objects := x.world.objects.map (fun o => if o.tag == "protocol.control" then {o with value := exhausted.encode} else o) }
  let failed := Reference.runModel source fixture.handlerId x.inputs [] world x.context x.fuel
  let after ← must (Reference.Protocol.getControl failed.world)
  unless !failed.ok && after.nextAdmission == 2^64-1 do
    throw (IO.userError "hop exhaustion must retain the preceding transaction identity burn")
  IO.println s!"OpcodeProtocol: {cases.length} actual source cases compile, contain required opcodes, and match source/Exec outcomes and full state"
