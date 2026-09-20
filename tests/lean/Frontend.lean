import LeanAT.Frontend.BlockSyntax
import LeanAT.Protocol.Builtins
set_option maxRecDepth 8192
set_option maxHeartbeats 2000000
open LeanAT LeanAT.Frontend LeanAT.Protocol

def emptyComponent : Component := {program := {}, contexts := []}
example : checkComponent emptyComponent = .ok () := by rfl
example : checkBody .timed 20 [.unsupported "awaitSlot"] = .error "UnsupportedSyntax: awaitSlot" := by rfl
example : checkBody .transport 20 [.writeState 0 (.literal 0 .unit)] = .error "IllegalEffect: synchronous mutable state write" := by rfl
example : checkBody .timed 20 [.ret [], .emit "late" []] = .error "StatementAfterReturn" := by rfl
example : checkBody .timed 20 [.branch (.literal 0 (.bool true)) [.ret []] []] = .ok false := by rfl
example : checkBody .debug 20 [.emit "irq" []] = .error "IllegalEffect: synchronous event emission" := by rfl
example : checkBody .dmi 20 [.ret [.state 0 0]] = .error "IllegalEffect: synchronous return reads business state" := by rfl
example : checkBody .debug 20 [.ret [.state 0 0]] = .error "IllegalEffect: synchronous return reads business state" := by rfl
example : checkSystem {instances := [⟨"a","D",emptyComponent⟩,⟨"a","D",emptyComponent⟩]} = .error "DuplicateInstance" := by rfl
example : validate ProbeV1 = .ok () := by rfl
example : validate TraceMarkerV1 = .ok () := by rfl
example : validate BaseBuiltin true = .ok () := by rfl
example : validate BaseBuiltin = .error "ReservedBuiltin: user protocol cannot define BaseBuiltin" := by rfl
example : validate {ProbeV1 with lanes := [0,1]} = .error "LaneCapacity: capacity must be positive" := by rfl
example : validate {ProbeV1 with rules := ProbeV1.rules ++ [ProbeV1.rules[0]!]} = .error "AmbiguousRule" := by rfl
example : validate {ProbeV1 with cancelPolicy := "silentTerminal"} = .error "UnsupportedCancelPolicy" := by rfl
example : validate {ProbeV1 with rules := [{pre:=0,flow:=.forward,phase:=0,sync:=.updated,post:=1}]} = .error "UpdatedRequiresPhase" := by rfl
example : validate {ProbeV1 with rules := [{pre:=0,flow:=.backward,phase:=0,sync:=.accepted,post:=1}]} = .error "WrongCallFlow" := by rfl
example : validate {ProbeV1 with rules := [{pre:=4,flow:=.forward,phase:=0,sync:=.accepted,post:=4}]} = .error "TerminalOutgoing" := by rfl
example : validate {ProbeV1 with rules := [{pre:=0,flow:=.forward,phase:=0,sync:=.accepted,guard:=.fieldEq 8 0,post:=1}]} = .error "IllegalGuardEffectOrDepth" := by rfl
example : (selectExchange ProbeV1 0 .forward 0 .completed none []).toOption = none := by rfl
example : (selectExchange ProbeV1 0 .forward 0 .updated (some 1) []).map (·.post) = .ok 2 := by rfl
example : (protocolGraph ProbeV1).length = 8 := by rfl
example : validate {ProbeV1 with rules := [{pre:=0,flow:=.forward,phase:=0,sync:=.accepted,callGuard:=.fieldEq 4 0,post:=1}]} = .error "FutureFactInCallGuard" := by rfl
def prioritized : Package := {ProbeV1 with rules := [
  {pre:=0,flow:=.forward,phase:=0,sync:=.accepted,priority:=some 9,post:=1},
  {pre:=0,flow:=.forward,phase:=0,sync:=.accepted,priority:=some (-1),post:=2}]}
example : (selectExchange prioritized 0 .forward 0 .accepted none []).map (·.post) = .ok 2 := by rfl
example : (selectExchange {prioritized with rules := prioritized.rules.reverse} 0 .forward 0 .accepted none []).map (·.post) = .ok 2 := by rfl

def initiatorPort : Port := {name := "out", role := .initiator, width := 64, maxOutstanding := 1, maxPayloadBytes := 16, maxByteEnableBytes := 16}
def targetPort : Port := {initiatorPort with name := "in", role := .target}
def connected : System := {instances := [⟨"a","A",{emptyComponent with ports := [initiatorPort]}⟩,⟨"b","B",{emptyComponent with ports := [targetPort]}⟩], bindings := [⟨⟨0,0⟩,⟨1,0⟩⟩]}
example : checkSystem connected = .ok () := by rfl
example : checkSystem {connected with bindings := []} = .error "UnboundPort" := by rfl
example : checkSystem {connected with bindings := [⟨⟨1,0⟩,⟨0,0⟩⟩]} = .error "BindingDirection" := by rfl
example : checkSystem {connected with maps := [⟨0,16,0,0⟩,⟨8,16,0,0⟩]} = .error "OverlappingAddressMap" := by rfl
example : checkSystem {connected with maps := [⟨2^64-1,2,0,0⟩]} = .error "AddressMapOverflow" := by rfl

at_component CheckedExample where
  requires (2 < 4) := by decide
  state value : UInt16 := 2
  process worker : Unit maxInstances 1 frameBytes 128 results 1 do
    set value := 7
    return
example : CheckedExample.model.states.length = 1 := by rfl
example : CheckedExample.model.handlers.length = 1 := by rfl

at_component Source := {emptyComponent with ports := [initiatorPort]}
at_component Sink := {emptyComponent with ports := [targetPort]}
at_system NativeTopology where
  instance first := Source
  instance second := Sink
  bind first.out => second.in
#guard NativeTopology.model.components.length == 2
example : NativeTopology.model.systems.length = 1 := by rfl
#guard (ModelIR.validateSchema NativeTopology.model).isOk

def sharedDefinition : System := {instances := [⟨"one","Shared",CheckedExample.checked.val⟩,⟨"two","Shared",CheckedExample.checked.val⟩]}
example : checkSystem sharedDefinition = .ok () := by rfl
#guard (exportSystem sharedDefinition).components.length == 1
#guard ((exportSystem sharedDefinition).systems.head?.map (fun s => s.instances.length)) == some 2







