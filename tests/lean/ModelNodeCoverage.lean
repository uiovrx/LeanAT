import LeanAT.Compiler.Main

open LeanAT LeanAT.Compiler

private def nodeModel : ModelIR.Project := {
  types := [.bool,.bits 64,.record [1,0],.vec 1 2,.variant [[1]]]
  states := [⟨0,1,.bits 64 0⟩]
  handlers := [{id := 0,body := [
    .letVal 0 (.makeRecord 2 [.literal 1 (.bits 64 7),.literal 0 (.bool true)]),
    .letVal 1 (.makeVariant 4 0 [.literal 1 (.bits 64 8)]),
    .letVal 2 (.literal 3 (.vec [.bits 64 5,.bits 64 6])),
    .branch (.literal 0 (.bool true)) [.writeState 0 (.literal 1 (.bits 64 9))] [.fail "wrong yes"],
    .branch (.literal 0 (.bool false)) [.fail "wrong no"] [.emit "no" []],
    .repeat 2 [.check (.literal 0 (.bool true)) "loop"],
    .ret [.state 1 0,.field 1 (.local 2 0) 0,.index 1 (.local 3 2) (.literal 1 (.bits 64 0)),
      .select 1 (.literal 0 (.bool false)) (.literal 1 (.bits 64 111)) (.binary 1 .addWrap (.literal 1 (.bits 64 1)) (.literal 1 (.bits 64 2))),
      .local 4 1]]}]
}

def main : IO Unit := do
  let check := fun b label => unless b do throw (IO.userError label)
  let .ok compilation := compileWithProvenance nodeModel | throw (IO.userError "all scalar ModelIR forms compile")
  let some handler := nodeModel.handlers.head? | throw (IO.userError "handler")
  let .ok high := ModelIR.runSegment nodeModel {values := [(0,.bits 64 0)]} handler.body | throw (IO.userError "model execution")
  let .ok low := ExecIR.evalExecSegment compilation.validated 0 [] {state := [.bits 64 0]} 1000 | throw (IO.userError "exec execution")
  check (high.returned == some low.returned && low.returned == [.bits 64 9,.bits 64 7,.bits 64 5,.bits 64 3,.variant 0 [.bits 64 8]]) "both IR observable values"
  check (ExecIR.commit low.txn == [.bits 64 9] && high.values.lookup 0 == some (.bits 64 9)) "both IR state"
  let .ok _ := buildSourceMap compilation | throw (IO.userError "all source nodes map")
  let failModel := {nodeModel with handlers := [{id := 0,body := [.fail "deliberate"]}]}
  let .ok failure := compile failModel | throw (IO.userError "fail lowering")
  match ModelIR.runSegment failModel {} [.fail "deliberate"] with
  | .error "deliberate" => pure () | _ => throw (IO.userError "ModelIR fail execution")
  match ExecIR.evalExecSegment failure 0 [] {state := [.bits 64 0]} 1 with
  | .error "deliberate" => pure () | _ => throw (IO.userError "fail execution")
  let unsupported := {nodeModel with handlers := [{id := 0,body := [.unsupported "not-in-language"]}]}
  match compile unsupported with
  | .error _ => pure () | .ok _ => throw (IO.userError "unsupported source accepted")
  IO.println "Every scalar ModelIR Expr and Stmt: real two-IR execution or explicit unsupported rejection, source mapping passed"
