import LeanAT.ModelIR.Layout
import LeanAT.Proofs
namespace LeanAT.ModelIR.Tests
private def isError {α ε : Type} : Except ε α → Bool
  | .error _ => true | _ => false
private def isOk {α ε : Type} : Except ε α → Bool
  | .ok _ => true | _ => false
#guard isError (ATRepr.decode (α := UInt8) (.bits 8 256))
#guard isError (ATRepr.decode (α := UInt8) (.bits 7 127))
#guard isOk (ATRepr.decode (α := UInt64) (ATRepr.encode (18446744073709551615 : UInt64)))
#guard isOk (ATRepr.decode (α := BoundedBytes 0) (.vec []))
#guard isError (ATRepr.decode (α := BoundedBytes 1) (.vec [.bits 8 1,.bits 8 2]))
#guard isError (boundBytes 0 [1])
#guard isOk (boundBytes 1 [255])
instance : FinBound 0 := ⟨by decide⟩
instance : FinBound 3 := ⟨by decide⟩
#guard isError (ATRepr.decode (α := Fin 0) (.bits 64 0))
#guard isError (ATRepr.decode (α := Fin 3) (.bits 64 3))
#guard isOk (ATRepr.decode (α := Fin 3) (.bits 64 2))
#guard isError (deriveLayout 0 [.record [0]])
#guard isError (deriveLayout 0 [.record [1],.record [0]])
#guard isError (deriveLayout 0 [.record [8]])
#guard isOk (deriveLayout 0 [.vec 1 0,.bits 8])
#guard isError (deriveLayout 0 [.vec 1 100,.bits 64] {maxBytes := 100})
#guard isError (checkedAddTime ⟨18446744073709551615⟩ ⟨1⟩)
#guard match Pure.evalBinary .addWrap (.bits 8 255) (.bits 8 1) with | .ok v => v == .bits 8 0 | _ => false
#guard isError (Pure.evalBinary .addChecked (.bits 8 255) (.bits 8 1))
#guard isError (Pure.evalBinary .divChecked (.bits 8 1) (.bits 8 0))
private def pureBranch := Pure.Program.select (.literal (.bool false)) (.field (.literal (.record [])) 99) (.literal (.bits 8 7))
#guard match Pure.eval pureBranch .unit with | .ok v => v == .bits 8 7 | _ => false
#guard match Pure.evalBounded pureBranch .unit 0 with | .limitReached => true | _ => false
private def p : Project := {
  types := [.bool,.bits 8]
  states := [⟨0,1,.bits 8 0⟩]
  handlers := [⟨0,[.writeState 0 (.literal 1 (.bits 8 9)),.emit "commit" [.state 1 0]],{},.timedHandler,"manual",none,[],none,none⟩,
    ⟨1,[.writeState 0 (.literal 1 (.bits 8 99)),.check (.literal 0 (.bool false)) "PrepareFailed"],{},.timedHandler,"manual",none,[],none,none⟩]
}
#guard isOk (validateSchema p)
#guard isError (validateSchema {p with handlers := [⟨0,[.ret [.local 1 99]],{},.timedHandler,"manual",none,[],none,none⟩]})
#guard isError (validateSchema {p with types := [.bool,.bits 65]})
#guard match validateSchema p with
  | .error _ => false
  | .ok checked =>
    let r := runBounded checked [⟨100,0,0⟩] 1 none
    r.reason == .quiescent && r.state.values == [(0,.bits 8 9)] && r.state.trace == [⟨"commit",[.bits 8 9]⟩]
#guard match validateSchema p with
  | .error _ => false
  | .ok checked =>
    let r := runBounded checked [⟨100,0,1⟩] 1 none
    r.reason == .failed "PrepareFailed" && r.state.values == [(0,.bits 8 0)] && r.state.trace.isEmpty
#guard match validateSchema p with
  | .error _ => false
  | .ok checked =>
    let r := runBounded checked [⟨100,0,0⟩] 1 (some 99)
    r.reason == .horizonReached && r.remaining.length == 1 && r.state.trace.isEmpty
#guard match validateSchema p with
  | .error _ => false
  | .ok checked => (runBounded checked [⟨100,0,0⟩] 0 none).reason == .runBudgetReached
#guard match validateSchema p with
  | .error _ => false
  | .ok checked => (runBounded checked [⟨100,1,0⟩,⟨99,2,0⟩] 2 none).reason == .failed "EnvironmentViolation: event order"
#print axioms LeanAT.Proofs.segment_failure_atomic
#print axioms LeanAT.Proofs.transportSafety
#print axioms LeanAT.Pure.bounded_agrees
#guard isError (validateSchema {p with handlers := [{id := 0, body := [.ret [.binary 0 .addWrap (.literal 0 (.bool true)) (.literal 0 (.bool false))]]}]})
#guard isError (validateSchema {p with handlers := [{id := 0, context := .transportEntry, body := [.ret [.state 1 0]]}]})
#guard isError (validateSchema {p with enabledCapabilities := ["unknown"]})
#guard isError (validateSchema {p with components := [{id := ⟨0⟩, endpoints := [{id := 0, role := .target, busWidth := 0,maxBindings := 1,maxOutstanding := 1,maxPayloadBytes := 8,maxByteEnableBytes := 8}]}]})
#guard isError (validateSchema {p with systems := [{id := 0, instances := [{id := ⟨0⟩, definition := ⟨99⟩}],bindings := [],runtimeDomain := 0}]})
#guard isOk (validateSchema {types := [.fin 0,.variant []]})
#guard match validateSchema {p with profile := {p.profile with maxEventsPerTick := 1}} with
  | .error _ => false
  | .ok checked =>
    let first := runBounded checked [⟨100,0,0⟩,⟨100,1,0⟩] 1 none
    let second := runFrom checked none 1 first.state first.remaining
    first.reason == .runBudgetReached && second.reason == .failed "ZenoDetected" && second.state.eventsAtTick == 1
#guard isError (execStmts p 4 {} [.repeat 1000000000 [.repeat 1000000000 [.emit "never" []]]])
#guard match evalExpr {} 10 (.select 1 (.literal 0 (.bool false)) (.field 1 (.literal 2 (.record [])) 99) (.literal 1 (.bits 8 7))) with
  | .ok v => v == .bits 8 7 | _ => false
private def staleIdentity : HandleIdentity := ⟨.transaction,1,2,3,4,5⟩
#guard isOk (ATRepr.decode (α := HandleValue .transaction) (.handle staleIdentity))
#guard isError (ATRepr.decode (α := HandleValue .task) (.handle staleIdentity))
example (checked : SchemaCheckedProject) : checkSchema checked.project = .ok () := checked.certificate
private def diamondTypes : TypeEnvironment := [.unit] ++ (List.range 63).map (fun i => .record [i,i])
#guard isOk (deriveLayout 63 diamondTypes)
example : checkTypes [.unit,.bool,.bits 64,.handle .wait] = .ok () := by rfl
example : checkTypes [.record [1],.record [0]] = .error "RecursiveValueLayout" := by rfl
private def variantExample : FiniteValue [.bool,.bits 8] (.variant [[0],[1]]) :=
  ⟨.variant 1 [.bits 8 255],by decide⟩
#guard match ATRepr.decode (ATRepr.encode variantExample) with
  | .ok (v : FiniteValue [.bool,.bits 8] (.variant [[0],[1]])) => v.value == .variant 1 [.bits 8 255]
  | _ => false
#guard match ATRepr.decode (α := FiniteValue [.bool,.bits 8] (.variant [[0],[1]])) (.variant 2 []) with
  | .error .invalidTag => true | _ => false
private def metadataBase : Project := {
  types := [.bool]
  components := [{id := ⟨0⟩,parameters := [{id := 0,typeId := 0}],sidebands := [{id := 0,direction := .input,typeId := 0}]}]
  systems := [{id := 0,instances := [{id := ⟨0⟩,definition := ⟨0⟩,resolvedConfig := [(0,.bool true)]}],bindings := [],topPorts := [{id := 0,direction := .input,typeId := 0}],sidebandBindings := [{id := 0,sourcePort := .top 0,sinkPort := .instance ⟨0⟩ 0}],runtimeDomain := 0}]
  topSystemId := some 0
}
#guard isOk (validateSchema metadataBase)
#guard isError (validateSchema {metadataBase with systems := metadataBase.systems.map (fun s => {s with sidebandBindings := []})})
#guard isError (validateSchema {metadataBase with systems := metadataBase.systems.map (fun s => {s with instances := s.instances.map (fun i => {i with resolvedConfig := []})})})
#guard isError (validateSchema {metadataBase with systems := metadataBase.systems.map (fun s => {s with instances := s.instances.map (fun i => {i with resolvedConfig := [(0,.unit)]})})})
#guard isError (validateSchema {metadataBase with systems := metadataBase.systems.map (fun s => {s with sidebandBindings := s.sidebandBindings ++ [{id := 1,sourcePort := .top 0,sinkPort := .instance ⟨0⟩ 0}]})})
end LeanAT.ModelIR.Tests





