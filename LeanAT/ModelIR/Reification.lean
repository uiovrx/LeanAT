import Lean
import LeanAT.Pure
namespace LeanAT.Pure
open Lean Meta
initialize pureAttribute : TagAttribute ← registerTagAttribute `leanat_pure "Request finite LeanAT pure reification; this attribute is not a proof."
structure ReifiedFunction where
  reference : Name
  program : Program
  inputType : TypeSchema
  outputType : TypeSchema
  sourceIdentity : String
  proofStatus : String := "checked-not-proved"
  deriving Repr
private def boolType : Expr := mkConst ``Bool
/-- Deliberately bounded whitelist: unsupported specialization is an error, never an opaque executable. -/
private def reifyBoolExpr : Nat → List Name → Expr → MetaM (Except String Program)
  | 0, _, _ => pure (.error "ReificationBudget")
  | fuel+1, path, expr => do
    let recurse := reifyBoolExpr fuel path
    match expr with
    | .bvar 0 => return .ok .input
    | .mdata _ e => recurse e
    | .letE _ _ value body _ => recurse (body.instantiate1 value)
    | .const ``Bool.true _ => return .ok (.literal (.bool true))
    | .const ``Bool.false _ => return .ok (.literal (.bool false))
    | _ =>
      let fn := expr.getAppFn
      let args := expr.getAppArgs
      if fn.isLambda then return ← recurse (fn.beta args)
      let .const name _ := fn | return .error "UnsupportedTerm"
      if name == ``Bool.not && args.size == 1 then
        return (← recurse args[0]!).map (fun p => .select p (.literal (.bool false)) (.literal (.bool true)))
      if (name == ``Bool.and || name == ``Bool.or) && args.size == 2 then
        let .ok a ← recurse args[0]! | return .error "UnsupportedBooleanOperand"
        let .ok b ← recurse args[1]! | return .error "UnsupportedBooleanOperand"
        return .ok (if name == ``Bool.and then .select a b (.literal (.bool false)) else .select a (.literal (.bool true)) b)
      if name == ``Bool.rec && args.size == 4 then
        let .ok c ← recurse args[3]! | return .error "UnsupportedBooleanScrutinee"
        let .ok n ← recurse args[1]! | return .error "UnsupportedBooleanBranch"
        let .ok y ← recurse args[2]! | return .error "UnsupportedBooleanBranch"
        return .ok (.select c y n)
      if path.contains name then return .error "RecursionNotBounded"
      let some (.defnInfo info) := (← getEnv).find? name | return .error "MissingExecutableReference"
      if info.safety != .safe then return .error "UnsafeOrPartialReference"
      if !info.levelParams.isEmpty then return .error "UnclosedSpecialization"
      reifyBoolExpr fuel (name::path) (info.value.beta args)

def reifyPure (name : Name) (budget : Nat := 1024) : MetaM (Except String ReifiedFunction) := do
  let some (.defnInfo info) := (← getEnv).find? name | return .error "MissingExecutableReference"
  if info.safety != .safe then return .error "UnsafeOrPartialReference"
  if !info.levelParams.isEmpty then return .error "UnclosedSpecialization"
  let .forallE _ domain codomain _ := info.type | return .error "ExpectedUnaryFunction"
  if codomain.hasLooseBVars then return .error "UnclosedSpecialization"
  if !(← isDefEq domain boolType) then return .error "UnsupportedInputType"
  if !(← isDefEq codomain boolType) then
    return .error "UnsupportedSpecialization: current reifier accepts Bool to Bool"
  let .lam _ _ body _ := info.value | return .error "ExpectedFunctionBody"
  return (← reifyBoolExpr budget [name] body).map (fun program =>
    ⟨name,program,.bool,.bool,toString name ++ ":Bool->Bool:" ++ toString info.value,"checked-not-proved"⟩)
end LeanAT.Pure

