import LeanAT.Proofs
open Lean Elab Command
namespace LeanAT.Proofs
elab "#leanat_audit " theoremId:ident : command => do
  let name ← resolveGlobalConstNoOverload theoremId
  let result ← liftCoreM (checkProof name)
  let .ok audit := result | throwError "Proof audit failed: {repr result}"
  if audit.axioms.any (fun n => n == ``sorryAx) then throwError "Proof audit rejected sorryAx dependency"
  let some info := (← getEnv).find? name | throwError "Audited theorem disappeared"
  let report := Json.mkObj [
    ("schema",toJson "leanat.proof-audit.v1"),
    ("theorem",toJson name.toString),
    ("theoremType",toJson (reprStr info.type)),
    ("axioms",toJson (audit.axioms.map Name.toString)),
    ("usesNativeEvaluation",toJson audit.usesNativeEvaluation),
    ("leanVersion",toJson Lean.versionString)]
  logInfo m!"LEANAT_PROOF_AUDIT_JSON:{report.compress}"
end LeanAT.Proofs

