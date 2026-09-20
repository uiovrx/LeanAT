import LeanAT.Types
namespace LeanAT.Pure
inductive BinaryOp where
  | addWrap | subWrap | mulWrap | addChecked | divChecked | eq | lt | and | or
  deriving Repr, BEq, DecidableEq
inductive Error where
  | typeMismatch | arithmeticOverflow | divisionByZero | unsupportedTerm | indexOutOfRange
  deriving Repr, BEq, DecidableEq

def evalBinary (op : BinaryOp) (a b : Value) : Except Error Value :=
  match op, a, b with
  | .eq, a, b => .ok (.bool (a == b))
  | .and, .bool a, .bool b => .ok (.bool (a && b))
  | .or, .bool a, .bool b => .ok (.bool (a || b))
  | op, .bits w a, .bits v b =>
    if w != v || w == 0 || w > 64 || a ≥ 2^w || b ≥ 2^w then .error .typeMismatch else
    match op with
    | .addWrap => .ok (.bits w ((a+b) % 2^w))
    | .subWrap => .ok (.bits w ((a+2^w-b) % 2^w))
    | .mulWrap => .ok (.bits w ((a*b) % 2^w))
    | .addChecked => if a+b < 2^w then .ok (.bits w (a+b)) else .error .arithmeticOverflow
    | .divChecked => if b == 0 then .error .divisionByZero else .ok (.bits w (a/b))
    | .lt => .ok (.bool (a < b))
    | _ => .error .typeMismatch
  | _, _, _ => .error .typeMismatch
inductive Program where
  | input | literal (v : Value)
  | binary (op : BinaryOp) (a b : Program)
  | select (condition yes no : Program)
  | field (value : Program) (index : Nat)
  deriving Repr, BEq

def eval : Program → Value → Except Error Value
  | .input, input => .ok input
  | .literal v, _ => .ok v
  | .binary op a b, input => do evalBinary op (← eval a input) (← eval b input)
  | .select c y n, input => do
      match ← eval c input with
      | .bool true => eval y input
      | .bool false => eval n input
      | _ => .error .typeMismatch
  | .field v i, input => do
      match ← eval v input with
      | .record fields => match fields[i]? with | some x => .ok x | none => .error .indexOutOfRange
      | _ => .error .typeMismatch

def cost : Program → Nat
  | .input | .literal _ => 1
  | .binary _ a b => 1 + cost a + cost b
  | .select c y n => 1 + cost c + max (cost y) (cost n)
  | .field v _ => 1 + cost v
inductive RunResult where
  | done (result : Except Error Value) | limitReached
  deriving Repr
def evalBounded (p : Program) (v : Value) (budget : Nat) : RunResult :=
  if cost p ≤ budget then .done (eval p v) else .limitReached
structure ReifiedPure (α β : Type) [ATRepr α] [ATRepr β] where
  program : Program
  function : α → β
  correct : ∀ x, eval program (ATRepr.encode x) = .ok (ATRepr.encode (function x))
def identityBool : ReifiedPure Bool Bool := ⟨.input, id, fun _ => rfl⟩
theorem bounded_agrees (p : Program) (v : Value) (budget : Nat) (h : cost p ≤ budget) :
    evalBounded p v budget = .done (eval p v) := by simp [evalBounded, h]
end LeanAT.Pure


