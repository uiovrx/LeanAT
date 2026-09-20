import Lean
import LeanAT.ModelIR.Semantics
namespace LeanAT.Proofs
theorem uint64_decode_encode (x : UInt64) : ATRepr.decode (ATRepr.encode x) = Except.ok x := ATRepr.decode_encode x
theorem boundedBytes_decode_encode (capacity : Nat) (x : BoundedBytes capacity) :
    ATRepr.decode (ATRepr.encode x) = Except.ok x := ATRepr.decode_encode x
theorem finiteValue_decode_encode (env : TypeEnvironment) (schema : TypeSchema) (x : FiniteValue env schema) :
    ATRepr.decode (ATRepr.encode x) = Except.ok x := ATRepr.decode_encode x
theorem handle_identity_decode_encode (kind : HandleKind) (x : HandleValue kind) :
    ATRepr.decode (ATRepr.encode x) = Except.ok x := ATRepr.decode_encode x
/-- A safety property transports only after the concrete observation inclusion is supplied. -/
theorem transportSafety {Trace : Type} (model implementation : Trace → Prop) (safe : Trace → Prop)
    (refinement : ∀ t, implementation t → model t)
    (modelSafe : ∀ t, model t → safe t) : ∀ t, implementation t → safe t :=
  fun t h => modelSafe t (refinement t h)

theorem select_true (yes no : Pure.Program) (input : Value) :
    Pure.eval (.select (.literal (.bool true)) yes no) input = Pure.eval yes input := rfl

theorem select_false (yes no : Pure.Program) (input : Value) :
    Pure.eval (.select (.literal (.bool false)) yes no) input = Pure.eval no input := rfl

/-- An unsuccessful local segment exposes exactly the original committed state. -/
def commitSegment (p : ModelIR.Project) (s : ModelIR.RuntimeState) (body : List ModelIR.Stmt) : ModelIR.RuntimeState :=
  match ModelIR.runSegment p s body with | .ok next => next | .error _ => s

theorem segment_failure_atomic (p : ModelIR.Project) (s : ModelIR.RuntimeState) (body : List ModelIR.Stmt)
    (error : String) (h : ModelIR.runSegment p s body = .error error) : commitSegment p s body = s := by
  simp [commitSegment, h]

theorem event_sound (p : ModelIR.ValidatedProject) (s next : ModelIR.RuntimeState) (e : ModelIR.Event)
    (h : ModelIR.stepEvent p s e = .ok next) : ModelIR.Step p s e next := .event h

theorem exact_budget_quiescent (p : ModelIR.ValidatedProject) (s : ModelIR.RuntimeState) (horizon : Option Nat) :
    (ModelIR.runFrom p horizon 0 s []).reason = .quiescent := rfl

inductive Status where
  | planned | checked | proved
  deriving Repr, BEq
structure Obligation where
  propertyId : String
  scope : String
  assumptions : List String
  status : Status
  deriving Repr

def collectObligations : List Obligation :=
  [⟨"codec-supported-scalars","Lean kernel",[],.proved⟩,
   ⟨"pure-identity-bool","Lean kernel",[],.proved⟩,
   ⟨"local-segment-failure-atomicity","ModelIR finite segment",[],.proved⟩,
   ⟨"lowering-observable-refinement","ModelIR to ExecIR",["lower succeeds","ExecIR validation succeeds"],.planned⟩,
   ⟨"runtime-refinement","C++ interpreter",[],.planned⟩,
   ⟨"adapter-refinement","SystemC bridge",["legal external environment"],.planned⟩,
   ⟨"end-to-end-progress","SystemC",["fair scheduler","peer progress","non-Zeno"],.planned⟩]
structure ProofAudit where
  theoremName : Lean.Name
  axioms : Array Lean.Name
  usesNativeEvaluation : Bool
  deriving Repr
/-- Looks up the actual theorem and traverses its kernel dependencies; strings alone are not certificates. -/
def checkProof (name : Lean.Name) : Lean.CoreM (Except String ProofAudit) := do
  let env ← Lean.getEnv
  let some info := env.find? name | return .error "UnknownTheorem"
  match info with
  | .thmInfo _ =>
    let axioms ← Lean.collectAxioms name
    return .ok ⟨name,axioms,axioms.any (fun n => n.toString.startsWith "Lean.ofReduce" || n.toString == "Lean.trustCompiler")⟩
  | _ => return .error "NotATheorem"
end LeanAT.Proofs

