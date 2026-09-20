import LeanAT.Compiler.OpcodeCase
import LeanAT.Frontend.CatalogExample

namespace LeanAT.Compiler.OpcodeCases.Pure
open LeanAT ModelIR ExecIR

private def types : TypeEnvironment :=
  [.unit,.bool,.bits 8,.bits 16,.bits 32,.bits 64,.record [5,1],.variant [[],[5]],.vec 5 2,.bytes 4]
private def u (n : Nat) : Value := .bits 64 n
private def b (n : Nat) : Value := .bits 8 n
private def lit (n : Nat) : ModelIR.Expr := .literal 5 (u n)
private def param (id typeId : Nat) : LocalBinderIR := {id,typeId}

private def fixture (id variant : String) (body : List Stmt) (parameters : List LocalBinderIR)
    (inputs : List Value) (tags : List Nat) (failure : Bool := false) : OpcodeCase := {
  id,variant,providerFamily := "pure",
  model := {types,states := [⟨0,5,u 17⟩],handlers := [{id := 0,parameters,body}]},
  «input» := {inputs,committed := [u 17],context := {now := 37},fuel := 1000},
  expectedOpcodeTags := tags,expectedOutcome := if failure then "failure" else "success",
  sourceModule := "LeanAT.Compiler.OpcodeCases.Pure",
  sourceFiles := ["LeanAT/Compiler/OpcodeCases/Pure.lean"]}

/-- Negative operands are valid typed input values that fail during execution.
    A write before the operation makes transaction rollback observable. -/
private def expression (id variant : String) (expr : ModelIR.Expr) (parameters : List LocalBinderIR)
    (inputs : List Value) (tag : Nat) (failure : Bool := false) : OpcodeCase :=
  fixture id variant [.writeState 0 (lit 9),.letVal 100 expr,.writeState 0 (lit 99),
    .ret [.local expr.typeId 100]] parameters inputs [tag] failure

private def binaryCases : List OpcodeCase :=
  let bits := fun variant op a b failure => expression "binary" variant
    (.binary 2 op (.local 2 0) (.local 2 1)) [param 0 2,param 1 2] [.bits 8 a,.bits 8 b] 2 failure
  let bools := fun variant op a b => expression "binary" variant
    (.binary 1 op (.local 1 0) (.local 1 1)) [param 0 1,param 1 1] [.bool a,.bool b] 2
  [bits "add-wrap-zero" .addWrap 0 0 false,
   bits "add-wrap-boundary" .addWrap 255 1 false,
   bits "sub-wrap-underflow" .subWrap 0 1 false,
   bits "mul-wrap-boundary" .mulWrap 128 2 false,
   bits "add-checked-max" .addChecked 254 1 false,
   bits "add-checked-overflow" .addChecked 255 1 true,
   bits "divide-nonexact" .divChecked 17 3 false,
   bits "divide-zero" .divChecked 17 0 true,
   bools "and-true" .and true true,bools "and-false" .and true false,
   bools "or-true" .or false true,bools "or-false" .or false false] ++
  [expression "binary" "equal-bits" (.binary 1 .eq (.local 5 0) (.local 5 1))
      [param 0 5,param 1 5] [u 7,u 7] 2,
   expression "binary" "less-bits" (.binary 1 .lt (.local 5 0) (.local 5 1))
      [param 0 5,param 1 5] [u 7,u 8] 2]

private def unaryCases : List OpcodeCase :=
  [expression "unary" "logical-not-true" (.unary 1 0 (.local 1 0)) [param 0 1] [.bool true] 16,
   expression "unary" "logical-not-false" (.unary 1 0 (.local 1 0)) [param 0 1] [.bool false] 16] ++
  ([1,2] : List Nat).flatMap fun mode => ([0,1,255] : List Nat).map fun value =>
    expression "unary" s!"mode{mode}-value{value}" (.unary 2 mode (.local 2 0))
      [param 0 2] [b value] 16

private def compareCases : List OpcodeCase :=
  (List.range 6).flatMap fun mode =>
    ([(0,0),(0,2^64-1),(2^64-1,0)] : List (Nat × Nat)).map fun (a,b) =>
      expression "compare" s!"mode{mode}-{a}-{b}" (.compare 1 mode (.local 5 0) (.local 5 1))
        [param 0 5,param 1 5] [u a,u b] 17

private def structuralCases : List OpcodeCase :=
  ([0,1] : List Nat).flatMap fun mode =>
    ([(0,Value.unit,Value.unit),(1,.bool true,.bool false),
      (6,.record [u 42,.bool true],.record [u 42,.bool true]),
      (6,.record [u 42,.bool true],.record [u 43,.bool true]),
      (7,.variant 0 [],.variant 1 [u 42]),
      (7,.variant 1 [u 42],.variant 1 [u 42]),
      (8,.vec [u 1,u 2],.vec [u 1,u 3]),
      (9,.bytes [0,255],.bytes [0,255]),(9,.bytes [],.bytes [0])] : List (Nat × Value × Value)).zipIdx.map fun ((typ,a,b),index) =>
      expression "compare" s!"structural-mode{mode}-case{index}" (.compare 1 mode (.local typ 0) (.local typ 1))
        [param 0 typ,param 1 typ] [a,b] 17

private def convertCases : List OpcodeCase :=
  [expression "convert" "zero-extend-zero" (.convert 5 0 (.local 2 0)) [param 0 2] [b 0] 18,
   expression "convert" "zero-extend-max" (.convert 5 0 (.local 2 0)) [param 0 2] [b 255] 18,
   expression "convert" "truncate-high" (.convert 2 1 (.local 5 0)) [param 0 5] [u 511] 18,
   expression "convert" "truncate-zero" (.convert 2 1 (.local 5 0)) [param 0 5] [u 256] 18,
   expression "convert" "checked-max" (.convert 2 2 (.local 5 0)) [param 0 5] [u 255] 18,
   expression "convert" "checked-overflow" (.convert 2 2 (.local 5 0)) [param 0 5] [u 256] 18 true,
   expression "convert" "identity-width" (.convert 5 0 (.local 5 0)) [param 0 5] [u (2^64-1)] 18]

private def aggregateCases : List OpcodeCase :=
  let record : Value := .record [u 42,.bool true]
  let vector : Value := .vec [u 7,u 9]
  [expression "makeRecord" "mixed-fields" (.makeRecord 6 [lit 42,.literal 1 (.bool true)]) [] [] 3,
   expression "getField" "first" (.field 5 (.local 6 0) 0) [param 0 6] [record] 4,
   expression "getField" "second" (.field 1 (.local 6 0) 1) [param 0 6] [record] 4,
   expression "makeVariant" "none" (.makeVariant 7 0 []) [] [] 5,
   expression "makeVariant" "some" (.makeVariant 7 1 [lit 42]) [] [] 5,
   expression "variantTag" "none" (.variantTag 5 (.local 7 0)) [param 0 7] [.variant 0 []] 6,
   expression "variantTag" "some" (.variantTag 5 (.local 7 0)) [param 0 7] [.variant 1 [u 42]] 6,
   expression "variantGet" "matching-tag" (.variantGet 5 (.local 7 0) 1 0) [param 0 7] [.variant 1 [u 42]] 7,
   expression "variantGet" "wrong-tag" (.variantGet 5 (.local 7 0) 1 0) [param 0 7] [.variant 0 []] 7 true,
   expression "makeVec" "two-elements" (.makeVec 8 [lit 7,lit 9]) [] [] 8,
   expression "vecGet" "first" (.index 5 (.local 8 0) (.local 5 1)) [param 0 8,param 1 5] [vector,u 0] 9,
   expression "vecGet" "last" (.index 5 (.local 8 0) (.local 5 1)) [param 0 8,param 1 5] [vector,u 1] 9,
   expression "vecGet" "out-of-range" (.index 5 (.local 8 0) (.local 5 1)) [param 0 8,param 1 5] [vector,u 2] 9 true,
   expression "vecSet" "first" (.vecSet 8 (.local 8 0) (.local 5 1) (lit 42)) [param 0 8,param 1 5] [vector,u 0] 10,
   expression "vecSet" "last" (.vecSet 8 (.local 8 0) (.local 5 1) (lit 42)) [param 0 8,param 1 5] [vector,u 1] 10,
   expression "vecSet" "out-of-range" (.vecSet 8 (.local 8 0) (.local 5 1) (lit 42)) [param 0 8,param 1 5] [vector,u 2] 10 true]

private def flowCases : List OpcodeCase :=
  let lazyExpr (condition : Bool) : ModelIR.Expr := .select 5 (.literal 1 (.bool condition))
    (lit 42) (.binary 5 .divChecked (lit 7) (lit 0))
  [expression "const" "literal-zero" (lit 0) [] [] 0,
   expression "const" "literal-max64" (lit (2^64-1)) [] [] 0,
   expression "const" "unit" (.literal 0 .unit) [] [] 0,
   expression "const" "bool" (.literal 1 (.bool true)) [] [] 0,
   expression "const" "record" (.literal 6 (.record [u 42,.bool false])) [] [] 0,
   expression "const" "variant" (.literal 7 (.variant 1 [u 42])) [] [] 0,
   expression "const" "vec" (.literal 8 (.vec [u 1,u 2])) [] [] 0,
   expression "const" "bytes" (.literal 9 (.bytes [0,255])) [] [] 0,
   expression "loadState" "read-committed" (.state 5 0) [] [] 12,
   fixture "move" "parameter-copy" [.letVal 1 (.local 5 0),.ret [.local 5 1]] [param 0 5] [u 42] [1],
   expression "selectValue" "true" (.select 5 (.local 1 0) (lit 7) (lit 9)) [param 0 1] [.bool true] 11,
   expression "selectValue" "false" (.select 5 (.local 1 0) (lit 7) (lit 9)) [param 0 1] [.bool false] 11,
   expression "if" "lazy-safe-arm" (lazyExpr true) [] [] 0,
   expression "if" "lazy-failing-arm" (lazyExpr false) [] [] 2 true,
   fixture "branch" "yes" [.branch (.local 1 0) [.writeState 0 (lit 41)] [.writeState 0 (lit 42)],.ret []]
     [param 0 1] [.bool true] [13],
   fixture "branch" "no" [.branch (.local 1 0) [.writeState 0 (lit 41)] [.writeState 0 (lit 42)],.ret []]
     [param 0 1] [.bool false] [13],
   fixture "repeat" "bounded-three" [.repeat 3 [.writeState 0 (.binary 5 .addWrap (.state 5 0) (lit 1))],.ret [.state 5 0]] [] [] [2,12,13],
   fixture "repeat" "zero" [.repeat 0 [.fail "unreachable"],.ret [lit 42]] [] [] [0],
   fixture "check" "true" [.check (.literal 1 (.bool true)) "Rejected",.ret []] [] [] [14],
   fixture "check" "false-rollback" [.writeState 0 (lit 99),.check (.literal 1 (.bool false)) "Rejected",.ret []] [] [] [14] true,
   fixture "trace" "owned-values" [.emit "event" [lit 42,.literal 1 (.bool true)],.ret []] [] [] [15],
   fixture "fail" "explicit-rollback" [.writeState 0 (lit 99),.fail "Deliberate"] [] [] [13] true,
   fixture "getNow" "nonzero-tick" [.readNow (param 0 5),.ret [.local 5 0]] [] [] [20],
   fixture "return" "empty" [.ret []] [] [] [],
   fixture "return" "multiple-values" [.ret [lit 42,.literal 1 (.bool true)]] [] [] [0]]

private def pureCallCase (failure : Bool) : OpcodeCase :=
  let base := expression "callPure" (if failure then "callee-check-failure" else "callee-return")
    (.callPure 5 1 [.local 5 0]) [param 0 5] [u (if failure then 0 else 41)] 19 failure
  {base with model := {base.model with handlers := base.model.handlers ++ [
    {id := 1,context := .pureFunction,parameters := [param 0 5],declaredResultTypes := some [5],body := [
      .check (.compare 1 1 (.local 5 0) (lit 0)) "ZeroArgument",
      .ret [.binary 5 .addWrap (.local 5 0) (lit 1)]]}]}}

def cases : Except String (List OpcodeCase) :=
  pure (binaryCases ++ unaryCases ++ compareCases ++ structuralCases ++ convertCases ++ aggregateCases ++ flowCases ++
    [pureCallCase false,pureCallCase true,
     {id := "native-source",variant := "reified-call-and-time",providerFamily := "pure",
      model := Frontend.CatalogExample.NativeScalar.model,
      «input» := {committed := [u 0],context := {now := 7},fuel := 1000},
      expectedOpcodeTags := [0,2,13,17,18,19,20],
      sourceBundle := Frontend.CatalogExample.NativeScalar.sourceBundle,
      sourceModule := "LeanAT.Compiler.OpcodeCases.Pure",
      sourceFiles := ["LeanAT/Frontend/CatalogExample.lean","LeanAT/Compiler/OpcodeCases/Pure.lean"]}])

end LeanAT.Compiler.OpcodeCases.Pure



