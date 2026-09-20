import LeanAT.Compiler.OpcodeCase
import LeanAT.Reference.Managed
namespace LeanAT.Compiler.OpcodeCases.Optional
open Reference
private def n := Value.bits 64
private def types : TypeEnvironment := [.bits 64,.handle .lease,.handle .access,.bytes 8,.variant [[0],[1]],.variant [[0],[2]],.unit,.record [0,0,3],.bool,.record [8,0,0,0,0,0,0,0],.variant [[0],[0]]]
private def context : Context := {domain := 1,owner := 9,environment := [("profile.valueNodeBytes",n 40),("managed.enabled",.bool true),("managed.invalidateOwner",n 9),("raw.enabled",.bool true),("raw.invalidateOwner",n 9)]}
private def region : Managed.Region := ⟨1,0,3,1,2,0,7,true,[0,0,0,0]⟩
private def signature (op : ExecIR.Op) (inputs : List Nat) (output : Nat) : ExecIR.ServiceSignature := Id.run do
  let raw := [ExecIR.Op.grantRawDmi,.denyDmi,.invalidateRawDmi].contains op
  let signature : ExecIR.ServiceSignature := ⟨op.tag,op,inputs,[output],if op == .grantRawDmi || op == .denyDmi then 16 else 3,if raw then 1024 else 256,0,(if raw then "leanat.raw-dmi." else "leanat.managed.") ++ toString op.tag,"1",ByteArray.empty⟩
  return {signature with abiHash := Managed.managedHash types signature (if raw then "raw-dmi-v1" else "managed-v1")}
private def service (s : ExecIR.ServiceSignature) : ModelIR.ServiceIR := ⟨s.id,ExecIR.opcodeNames[s.op.tag]!,s.inputTypes,s.resultTypes,s.contextMask,s.effectMask,s.extraFuel,s.providerKey,s.providerVersion,s.abiHash.data.toList,{}⟩
private def make (id : String) (sig : ExecIR.ServiceSignature) (inputs : List Value) (world : State) (ctx : Context) (variant : String := "success") (expected : String := "success") : OpcodeCase := Id.run do
  let output := sig.resultTypes.head!
  let args := sig.inputTypes.zipIdx |>.map (fun (t,i) => ModelIR.Expr.local t i)
  let parameters := sig.inputTypes.zipIdx |>.map (fun (t,i) => ModelIR.LocalBinderIR.mk i t {})
  let result : ModelIR.LocalBinderIR := ⟨inputs.length,output,{}⟩
  let ending := if variant == "rollback" then ModelIR.Stmt.fail "optional rollback after actual service" else .ret [.local output inputs.length]
  let handler : ModelIR.Handler := {id := 0,parameters,context := if ctx.kind == 4 then .dmiEntry else .timedHandler,declaredResultTypes := some [output],body := (if ctx.kind == 4 then [] else [.writeState 0 (.literal 0 (n 7))]) ++ [.serviceCall (some result) sig.id args,ending],source := {file := "LeanAT/Compiler/OpcodeCases/Optional.lean",line := 22,column := 1}}
  return {id := "optional." ++ id,variant,providerFamily := "optional",model := {types,states := [⟨0,0,n 0⟩],handlers := [handler],services := [service sig]},input := {inputs,committed := [n 0],world,context := ctx,fuel := 1000},expectedOpcodeTags := [sig.op.tag],expectedOutcome := if variant == "rollback" then "failure" else expected,profile := if sig.op.tag ≥ 46 then "AT-Ext-1.1-draft" else "AT-Core-1.1-draft",sourceModule := "LeanAT.Compiler.OpcodeCases.Optional",sourceFiles := ["LeanAT/Compiler/OpcodeCases/Optional.lean"]}
private def admitted : List Value → Except String HandleIdentity
  | [.variant 1 [.handle h]] => .ok h | _ => .error "OptionalSeedLease"
private def managedCases : Except String (List OpcodeCase) := do
  let ctx := {context with environment := context.environment ++ [("managed.region",region.encode)]}
  let (_,empty) ← Managed.installRegion ctx {} region
  -- Match the concrete Runtime ResultStore profile even for handlers that do not
  -- allocate a result, so unused pool limits are part of the authored input.
  let empty ← Storage.seedResultStore empty ctx 128 256 128
  let (leaseValues,seeded) ← Managed.request ctx empty 1 0 3 3
  let lease ← admitted leaseValues
  let seededCtx := {ctx with environment := ctx.environment ++ [("managed.seedLease",.handle lease)]}
  let specs := [("request",signature .requestManaged [0,0,0,0] 4,[n 1,n 0,n 3,n 3],empty),
    ("read",signature .beginManagedRead [1,0,0,0] 5,[.handle lease,n 0,n 2,n 0],seeded),
    ("write",signature .beginManagedWrite [1,0,3,0] 5,[.handle lease,n 1,.bytes [7,8],n 0],seeded),
    ("invalidate",signature .invalidateManaged [0,0,0,0] 6,[n 1,n 0,n 3,n 0],empty),
    ("releaseLease",signature .releaseLease [1] 6,[.handle lease],seeded)]
  let cases := specs.flatMap (fun (id,sig,args,world) => let actualCtx := if id == "read" || id == "write" || id == "releaseLease" then seededCtx else ctx; [make id sig args world actualCtx,make id sig args world actualCtx "rollback"])
  let full := {ctx with environment := ctx.environment ++ [("managed.limits",.record [n 0,n 128,n 65536])]}
  pure (cases ++ [make "request" (signature .requestManaged [0,0,0,0] 4) [n 1,n 0,n 3,n 3] empty full "capacity-denial",
    make "request" (signature .requestManaged [0,0,0,0] 4) [n 1,n 3,n 2,n 3] empty ctx "range-denial",
    make "read" (signature .beginManagedRead [1,0,0,0] 5) [.handle lease,n 3,n 2,n 0] seeded seededCtx "range-denial"] ++
    [("stale-generation",{lease with generation := lease.generation-1}),
     ("future-generation",{lease with generation := lease.generation+1}),
     ("wrong-owner",{lease with owner := 10}),
     ("wrong-domain",{lease with domain := 2})].map (fun (variant,handle) =>
       make "read" (signature .beginManagedRead [1,0,0,0] 5) [.handle handle,n 0,n 2,n 0] seeded seededCtx variant "failure"))
private def rawCases : Except String (List OpcodeCase) := do
  let ctx := {context with environment := context.environment ++ [("raw.region",.record [n 3,n 100,n 103,n 3,n 2,n 4,n 1,.bytes [0,0,0,0]])]}
  let (_,world) ← Managed.installRawRegion ctx {} 3 100 103 3 2 4 1 (some [0,0,0,0])
  let grant := signature .grantRawDmi [0,0,0] 9
  let deny := signature .denyDmi [] 9
  let invalidate := signature .invalidateRawDmi [0,0,0] 6
  let (_,seeded) ← Managed.invoke types {ctx with kind := 4} grant [n 3,n 100,n 0] world
  let seededCtx := {ctx with environment := ctx.environment ++ [("raw.seedGrant",.bool true)]}
  pure [make "rawGrant" grant [n 3,n 101,n 0] world {ctx with kind := 4},make "rawGrant" grant [n 3,n 101,n 0] world {ctx with kind := 4} "rollback",make "rawGrant" grant [n 3,n 104,n 0] world {ctx with kind := 4} "range-denial",
    make "rawDeny" deny [] world {ctx with kind := 4},make "rawDeny" deny [] world {ctx with kind := 4} "rollback",make "rawInvalidate" invalidate [n 3,n 100,n 103] seeded seededCtx,make "rawInvalidate" invalidate [n 3,n 100,n 103] seeded seededCtx "rollback",make "rawInvalidate" invalidate [n 3,n 103,n 100] seeded seededCtx "invalid-range" "failure",
    make "rawInvalidate" invalidate [n 99,n 100,n 103] world ctx "missing-region" "failure",
    make "rawGrant" grant [n 3,n 100,n 0] seeded {seededCtx with kind := 4,environment := seededCtx.environment ++ [("raw.capacity",n 1)]} "capacity-denial"]
private def externalCases : Except String (List OpcodeCase) := do
  let sig : ExecIR.ServiceSignature := ⟨64,.callExternPure,[0],[10],3,512,0,"leanat.external.add1","reference-add1|reference|checked-add1",ByteArray.empty⟩
  let semantics := "external-v1:1:1:add1:" ++ sig.providerVersion
  let sig := {sig with abiHash := Managed.managedHash types sig semantics}
  let bytes (s : String) := Value.bytes s.toUTF8.data.toList
  let program := Value.variant 2 [n 3,.variant 0 [],.variant 1 [n 1]]
  let precondition := Value.variant 2 [n 6,.variant 0 [],.variant 1 [n (2^64-1)]]
  let ctx := {context with environment := context.environment ++ [("external.enabled",.bool true),("external.descriptor",.record [n 1,bytes "add1",bytes "reference-add1",bytes "add1",bytes "native-add1",bytes "checked-add1",.bool false]),(sig.providerKey,.record [bytes sig.providerVersion,bytes semantics,program,precondition,.bool false,.unit])]}
  let nativeSig := {sig with providerVersion := "reference-add1|native-add1|checked-add1"}
  let nativeSemantics := "external-v1:1:1:add1:" ++ nativeSig.providerVersion
  let nativeSig := {nativeSig with abiHash := Managed.managedHash types nativeSig nativeSemantics}
  let nativeCtx := {context with environment := context.environment ++ [("external.enabled",.bool true),("external.descriptor",.record [n 1,bytes "add1",bytes "reference-add1",bytes "add1",bytes "native-add1",bytes "checked-add1",.bool true]),(nativeSig.providerKey,.record [bytes nativeSig.providerVersion,bytes nativeSemantics,program,precondition,.bool true,n 6])]}
  pure [make "external" sig [n 5] {} ctx,make "external" sig [n 5] {} ctx "rollback",make "external" sig [n (2^64-1)] {} ctx "precondition-failure" "failure",make "external" nativeSig [n 5] {} nativeCtx "native",make "externalNative" nativeSig [n 5] {} nativeCtx "rollback"]

def cases : Except String (List OpcodeCase) := do pure ((← managedCases) ++ (← rawCases) ++ (← externalCases))
end LeanAT.Compiler.OpcodeCases.Optional


