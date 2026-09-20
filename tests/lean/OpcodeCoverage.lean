import LeanAT.Compiler.Main

open LeanAT LeanAT.ExecIR LeanAT.Compiler Lean

def main : IO Unit := do
  let check := fun b label => unless b do throw (IO.userError label)
  let instructions : List Instruction := [
    {op := .const,dest := some ⟨0,2⟩,value := .bits 64 7}, {op := .const,dest := some ⟨1,1⟩,value := .bool true}, {op := .const,dest := some ⟨2,3⟩,value := .bits 8 5}, {op := .const,dest := some ⟨3,2⟩,value := .bits 64 0}, {op := .move,args := [⟨0,2⟩],dest := some ⟨4,2⟩}, {op := .binary,args := [⟨0,2⟩,⟨0,2⟩],dest := some ⟨5,2⟩}, {op := .makeRecord,args := [⟨0,2⟩,⟨1,1⟩],dest := some ⟨6,4⟩}, {op := .getField,args := [⟨6,4⟩],dest := some ⟨7,2⟩}, {op := .makeVariant,args := [⟨0,2⟩],dest := some ⟨8,5⟩}, {op := .variantTag,args := [⟨8,5⟩],dest := some ⟨9,2⟩}, {op := .variantGet,args := [⟨8,5⟩],dest := some ⟨10,2⟩}, {op := .makeVec,args := [⟨0,2⟩,⟨5,2⟩],dest := some ⟨11,6⟩}, {op := .vecGet,args := [⟨11,6⟩,⟨3,2⟩],dest := some ⟨12,2⟩}, {op := .vecSet,args := [⟨11,6⟩,⟨3,2⟩,⟨5,2⟩],dest := some ⟨13,6⟩}, {op := .selectValue,args := [⟨1,1⟩,⟨0,2⟩,⟨5,2⟩],dest := some ⟨14,2⟩}, {op := .unary,args := [⟨2,3⟩],dest := some ⟨15,3⟩,immediate := 1}, {op := .compare,args := [⟨5,2⟩,⟨0,2⟩],dest := some ⟨16,1⟩,immediate := 4}, {op := .convert,args := [⟨2,3⟩],dest := some ⟨17,2⟩}, {op := .loadState,dest := some ⟨18,2⟩}, {op := .bufferStateWrite,args := [⟨5,2⟩]}, {op := .check,args := [⟨1,1⟩],text := "check"}, {op := .trace,args := [⟨14,2⟩],text := "observed"}, {op := .getNow,dest := some ⟨19,2⟩}]
  let returns : List VReg := [⟨4,2⟩,⟨5,2⟩,⟨7,2⟩,⟨9,2⟩,⟨10,2⟩,⟨12,2⟩,⟨13,6⟩,⟨14,2⟩,⟨15,3⟩,⟨16,1⟩,⟨17,2⟩,⟨18,2⟩,⟨19,2⟩]
  let project : ExecProject := {schemaMajor := 2,types := [.unit,.bool,.bits 64,.bits 8,.record [2,1],.variant [[2]],.vec 2 2], stateTypes := [2],initialState := [.bits 64 3],programs := [{id := 0,resultTypes := returns.map VReg.typeId,effectMask := 3, blocks := [⟨0,[],instructions,.ret returns⟩]}]}
  let .ok validated := validateExec project | throw (IO.userError "opcode matrix validation")
  let .ok result := evalExecSegment validated 0 [] {state := project.initialState} (instructions.length+1) {now := 55} | throw (IO.userError "opcode matrix execution")
  check (result.returned == [.bits 64 7,.bits 64 14,.bits 64 7,.bits 64 0,.bits 64 7,.bits 64 7, .vec [.bits 64 14,.bits 64 14],.bits 64 7,.bits 8 250,.bool true,.bits 64 5,.bits 64 3,.bits 64 55]) "opcode result matrix"
  check (commit result.txn == [.bits 64 14] && result.txn.trace == [("observed",[.bits 64 7])] && result.remainingFuel == 0) "state trace and exact fuel"
  let .ok _ := buildSourceMap (directExecCompilation validated) | throw (IO.userError "every executed opcode has source location")
  for op in allOps do
    let malformed := {project with programs := [{id := 0,blocks := [⟨0,[],[{op,args := [⟨999,2⟩]}],.ret []⟩]}]}
    match validateExec malformed with
    | .error _ => pure () | .ok _ => throw (IO.userError s!"undefined operands accepted for {op.tag}")
  IO.FS.createDirAll "build/compiler-fixtures"
  IO.FS.writeBinFile "build/compiler-fixtures/opcode-matrix.execir.bin" (serialize validated)
  IO.println "Executed opcode matrix: 0..18 and20; exact values/state/trace/fuel and location coverage passed (CallPure separate regression)"
