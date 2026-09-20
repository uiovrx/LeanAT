import LeanAT.ModelIR.Reification
open Lean LeanAT LeanAT.Pure
namespace LeanAT.ModelIR.ReificationTests
@[leanat_pure] def negate (x : Bool) := !x
@[leanat_pure] def nested (x : Bool) := Bool.and (negate x) true
unsafe def unsafeIdentity (x : Bool) := x
def polymorphic {α : Type} (x : α) := x
run_meta do
  let .ok result ← reifyPure ``negate | throwError "Bool negation reification failed"
  match eval result.program (.bool true) with
  | .ok (.bool false) => pure ()
  | _ => throwError "Reified Bool semantics mismatch"
  if result.proofStatus != "checked-not-proved" then throwError "Unchecked correctness escalation"
run_meta do
  let .ok result ← reifyPure ``nested | throwError "Safe helper reification failed"
  match eval result.program (.bool false) with
  | .ok (.bool true) => pure ()
  | _ => throwError "Helper closure semantics mismatch"
run_meta do
  let .error _ ← reifyPure ``unsafeIdentity | throwError "Unsafe reference accepted"
  let .error _ ← reifyPure ``polymorphic | throwError "Unclosed specialization accepted"
  let .error _ ← reifyPure ``negate 0 | throwError "Zero reification budget accepted"
end LeanAT.ModelIR.ReificationTests
