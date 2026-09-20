import LeanAT.Compiler.OpcodeCase
import LeanAT.Reference.Runtime

namespace LeanAT.Compiler.OpcodeCases.Sideband
open LeanAT Reference ExecIR

private def u (n : Nat) : Value := .bits 64 n
private def types : TypeEnvironment := [.bits 64,.bool,.bits 8,.bits 1]

private def service (resultType : Nat) : Except String ModelIR.ServiceIR := do
  let key := "leanat.core.input.read"
  let hash := "e084d863eaf370dd7b8004942c1720fac8f02977fb9efa8d01ba85183012d30f"
  let bytes := hash.toList.toArray
  let digit := fun c => if c ≤ '9' then c.toNat-'0'.toNat else c.toNat-'a'.toNat+10
  let digest := (List.range 32).map (fun i => UInt8.ofNat (16*digit bytes[2*i]!+digit bytes[2*i+1]!))
  pure {
    id := 0,opcode := "loadInput",inputTypes := [0],resultTypes := [resultType],
    contextMask := 3,effectMask := 0,extraFuel := 0,providerKey := key,
    providerVersion := "1",abiHash := digest}

private def initial (typeId : Nat) : Value :=
  if typeId == 1 then .bool false else .bits (if typeId == 2 then 8 else if typeId == 3 then 1 else 64) 0

/-- The metadata and samples are actual host configuration. Both instances have
    independent scalar inputs; no row is a returned-value oracle. -/
private def environment (changed missing : Bool := false) : List (String × Value) :=
  [("runtime.owners",.vec [.record [u 0,u 7],.record [u 1,u 9]]),
   ("runtime.inputPorts",.vec (([0,1] : List Nat).flatMap fun instanceId =>
      [0,1,2,3].map fun port => .record [u instanceId,u port,u (if port == 0 then 1 else if port == 1 then 0 else port)])),
   ("runtime.inputs",.vec ((if missing then [] else [.record [u 0,u 0,.bool changed]]) ++
      [.record [u 0,u 1,u (if changed then 2^64-1 else 17)],
       .record [u 0,u 2,.bits 8 255],.record [u 0,u 3,.bits 1 1],
       .record [u 1,u 0,.bool true],.record [u 1,u 1,u 99],
       .record [u 1,u 2,.bits 8 128],.record [u 1,u 3,.bits 1 0]]))]

private def make (variant : String) (resultType port : Nat)
    (context : Context) (failure : Bool := false) (badABI : Bool := false) : Except String OpcodeCase := do
  let signature ← service resultType
  let signature := if badABI then {signature with abiHash := List.replicate 32 0} else signature
  let model : ModelIR.Project := {
    types,states := [⟨0,resultType,initial resultType⟩,⟨1,0,u 0⟩],services := [signature],
    handlers := [{id := 0,declaredResultTypes := some [resultType],body :=
      (if context.kind == 2 then [] else [.writeState 1 (.literal 0 (u 73))]) ++ [
      .serviceCall (some ⟨0,resultType,{}⟩) 0 [.literal 0 (u port)],
      .writeState 0 (.local resultType 0),.ret [.local resultType 0]]}]}
  pure {
    id := "loadInput",variant,providerFamily := "sideband",model,
    input := {committed := [initial resultType,u 0],world := {},context,fuel := 100},
    expectedOpcodeTags := [Op.loadInput.tag],expectedOutcome := if badABI then "binding-failure" else if context.kind == 2 then "preflight-failure" else if failure then "failure" else "success",
    sourceModule := "LeanAT.Compiler.OpcodeCases.Sideband",
    sourceFiles := ["LeanAT/Compiler/OpcodeCases/Sideband.lean"]}

/-- Cases contain a real source-level service call over an independent input
    snapshot. ABI/context preflight failures receive no opcode execution credit. -/
def cases : Except String (List OpcodeCase) := do
  let base : Context := {
    domain := 1,instanceId := 0,owner := 7,now := 4,turn := 2,
    environment := environment}
  let changed := {base with environment := environment true}
  pure [
    ← make "bool-false" 1 0 base,
    ← make "bool-changed-true" 1 0 changed,
    ← make "bits64-initial" 0 1 base,
    ← make "bits64-changed-max" 0 1 changed,
    ← make "bits8-max" 2 2 base,
    ← make "bits1-one" 3 3 base,
    ← make "second-instance-bool" 1 0 {base with instanceId := 1,owner := 9},
    ← make "second-instance-bits" 0 1 {base with instanceId := 1,owner := 9},
    ← make "wrong-owner" 1 0 {base with owner := 9} true,
    ← make "unknown-instance" 1 0 {base with instanceId := 2} true,
    ← make "missing-port" 1 10 base true,
    ← make "port-overflow" 1 (2^32) base true,
    ← make "wrong-result-type" 0 0 base true,
    ← make "not-sampled" 1 0 {base with environment := environment false true} true,
    ← make "wrong-context" 1 0 {base with kind := 2} true,
    ← make "wrong-abi" 1 0 base true true]

end LeanAT.Compiler.OpcodeCases.Sideband
