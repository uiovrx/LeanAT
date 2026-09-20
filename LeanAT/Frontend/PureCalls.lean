import LeanAT.ModelIR.Reification
import LeanAT.ModelIR.Schema

namespace LeanAT.Frontend
open Lean Elab Command

def boolProgramExpr : Pure.Program → ModelIR.Expr
  | .input => .local 1 0
  | .literal value => .literal 1 value
  | .binary op left right => .binary 1 op (boolProgramExpr left) (boolProgramExpr right)
  | .select c y n => .select 1 (boolProgramExpr c) (boolProgramExpr y) (boolProgramExpr n)
  | .field value index => .field 1 (boolProgramExpr value) index

private def programTerm : Pure.Program → CommandElabM (TSyntax `term)
  | .input => `(LeanAT.Pure.Program.input)
  | .literal (.bool true) => `(LeanAT.Pure.Program.literal (.bool true))
  | .literal (.bool false) => `(LeanAT.Pure.Program.literal (.bool false))
  | .select c y n => do
    let c ← programTerm c
    let y ← programTerm y
    let n ← programTerm n
    `(LeanAT.Pure.Program.select $c $y $n)
  | _ => throwError "UnsupportedReifiedBooleanProgram"

/-- Resolve a real Lean definition and run the bounded reifier. No opaque call is accepted. -/
def reifiedHandlerTerm (reference : TSyntax `ident) (id : Nat) : CommandElabM (TSyntax `term) := do
  let name ← resolveGlobalConstNoOverload reference
  let result ← liftTermElabM (Pure.reifyPure name)
  let .ok reified := result | throwErrorAt reference "PureReificationFailed: {repr result}"
  let program ← programTerm reified.program
  `(({id := $(quote id), context := .pureFunction, parameters := [{id := 0, typeId := 1}],
      declaredResultTypes := some [1], trigger := $(quote name.toString),
      body := [.ret [LeanAT.Frontend.boolProgramExpr $program]]} : LeanAT.ModelIR.Handler))

end LeanAT.Frontend
