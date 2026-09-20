import LeanAT.Compiler.OpcodeCase
import LeanAT.Reference.Runtime
import LeanAT.Reference.Storage

namespace LeanAT.Compiler.OpcodeCases.Core
open LeanAT Reference ExecIR
private def u (n : Nat) : Value := .bits 64 n
private def types : TypeEnvironment := [.bits 64,.bool,.bytes 16,.handle .event,.handle .process,
  .handle .wait,.unit,.variant [[6],[0]],.record [6],.variant [[],[0]],
  .record [0,2,1],.variant [[],[10]],.record [0,9,0,11]]
private def hex (s : String) : List UInt8 := Id.run do
  let chars := s.toList.toArray
  let digit := fun c => if c ≤ '9' then c.toNat-'0'.toNat else c.toNat-'a'.toNat+10
  return (List.range 32).map fun i => UInt8.ofNat (16*digit chars[2*i]!+digit chars[2*i+1]!)
private def sig (op : Op) (key hash : String) (ins outs : List Nat) (mask effect fuel : Nat) : ModelIR.ServiceIR :=
  {id := 0,opcode := opcodeNames[op.tag]!,inputTypes := ins,resultTypes := outs,
   contextMask := mask,effectMask := effect,extraFuel := fuel,providerKey := key,
   providerVersion := "1",abiHash := hex hash}
private def schedule := sig .scheduleEvent "leanat.core.event.schedule" "bffe39a0efb9a6f7c75fbb789e564aac1286c4f9b6c00e6f2971d896c39b8705" [0,2] [3] 3 8 0
private def cancel := sig .cancelEvent "leanat.core.event.cancel" "214aeb1ccb48bbaeabe889924cbb909b6f2049b64b7fd587fff0fb25e8ffc479" [3] [1] 3 8 0
private def output := sig .bufferOutputWrite "leanat.core.output.write" "87d06714cd584408dbba0e953b91928c11867e65713f710a4256be0632bebdbc" [0,1] [] 3 4 1
private def current := sig .getContextField "leanat.core.context.process" "f36277e80b561e7f2fccbe5e1fde0aa9b6599153f09130bd1f1c96f12133a0da" [] [4] 3 0 1
private def timer := sig .registerWait "leanat.core.wait.timer" "783dda9c9ea7bd22ee2b2b075230e110219632fb463886aed1579583d2aea36f" [4,0,0] [5] 2 16 1
private def waitResult := sig .readWaitResult "leanat.core.wait.result" "24ad58653d80da7b299d174f9f6f1c9fbb9f6b14159ca7846741b8214f981184" [7] [8] 2 16 0
private def transport := sig .setTransportReturn "leanat.core.transport.return" "eb4a92463bf8fc5c11181b492b8ad37f91671b8f703bdc5370604ef49ace0ffe" [12] [] 4 8 1
private def base : Context := {domain := 1,instanceId := 1,owner := 7,connection := 1,now := 4,turn := 2}
private def process : HandleIdentity := ⟨.process,1,10,0,1,7⟩
private def event : HandleIdentity := ⟨.event,1,1,0,2,7⟩
private def allocatorRules : List AllocationRule := [
  {kind := .event,store := 1,group := "runtime.event",perSlot := true,capacity := some 1024},
  {kind := .process,store := 10,group := "runtime.process",allowMax := false,capacity := some 64},
  {kind := .wait,store := 11,group := "runtime.process",allowMax := false,capacity := some 128}]
private def single (name : String) (op : Op) (signature : ModelIR.ServiceIR)
    (args : List Value) (ctx : Context := base) (world : State := {})
    (failure : Bool := false) : OpcodeCase := Id.run do
  let processNext := (world.objects.filter (fun item => item.identity.kind == .process || item.identity.kind == .wait)
    |>.map (fun item => item.identity.generation.toNat+1)).foldl max 1
  let counters : List AllocationCounter := [{group := "runtime.process",domain := ctx.domain,nextGeneration := processNext}] ++
    (world.objects.filter (fun item => item.identity.kind == .event) |>.map fun item =>
      {group := "runtime.event",domain := ctx.domain,slot := some item.identity.slot.toNat,nextGeneration := item.identity.generation.toNat+1}) ++
    (if world.objects.any (fun item => item.identity.kind == .event && item.identity.slot == 0) then [] else
      [{group := "runtime.event",domain := ctx.domain,slot := some 0,nextGeneration := 2}])
  let parameters := signature.inputTypes.zipIdx.map fun (typeId,id) => ({id,typeId} : ModelIR.LocalBinderIR)
  let destination := signature.resultTypes.head?.map fun typeId => ({id := parameters.length,typeId} : ModelIR.LocalBinderIR)
  let handler : ModelIR.Handler := {
    id := 0,parameters,
    context := if ctx.kind == 1 then .process else if ctx.kind == 2 then .transportEntry else .timedHandler,
    processCapacity := if ctx.kind == 1 then some {frameBytesLimit := 32768,resultCapacity := 16} else none,
    body := if ctx.kind == 2 then [.transportReturn (.local 12 0)] else
      [.serviceCall destination 0 (parameters.map fun p => .local p.typeId p.id),
       .ret (destination.toList.map fun p => .local p.typeId p.id)],
    declaredResultTypes := some (if ctx.kind == 2 then [12] else signature.resultTypes)}
  return {
    id := name,providerFamily := "core",variant := if failure then "failure" else "success",
    model := {types,handlers := [handler],services := [signature]},input := {
      inputs := args
      world := {world with
        allocationRules := allocatorRules,allocationCounters := counters,
        nextSequence := max 2 world.nextSequence,nextGeneration := max 2 world.nextGeneration}
      context := ctx},
    expectedOpcodeTags := [op.tag],expectedOutcome := if failure then "failure" else "success",
    sourceModule := "LeanAT.Compiler.OpcodeCases.Core",sourceFiles := ["LeanAT/Compiler/OpcodeCases/Core.lean"]}

def cases : Except String (List OpcodeCase) := do
  let processContext := {base with
    kind := 1,processIdentity := some process,
    environment := [("runtime.process.identity",.handle process),("runtime.process.program",u 0)]}
  let processWorld ← Reference.Runtime.seedProcess {} processContext process 0
  let eventContext := {base with environment := [("storage.event.identity",.handle event),
    ("storage.event.record",.record [u 9,u 0,u 3,u 1,u 1,.bytes [4,5]])]}
  let eventWorld : State := {
    nextGeneration := 3,nextSequence := 3,
    objects := [⟨event,"reference.event",u 2,true⟩],
    events := [{
      identity := event,time := 9,turn := 0,stage := 3,instanceId := 1,connection := 1,
      sequence := 2,kind := "storage.event",values := [.bytes [4,5]]}]}
  let mut result := [
    single "core-schedule-future" .scheduleEvent schedule [u 9,.bytes [1,2]],
    single "core-schedule-successor" .scheduleEvent schedule [u 4,.bytes [3]],
    single "core-schedule-past" .scheduleEvent schedule [u 3,.bytes [1]] base {} true,
    single "core-schedule-turn-overflow" .scheduleEvent schedule [u 4,.bytes []] {base with turn := 2^64-1} {} true,
    single "core-cancel-live" .cancelEvent cancel [.handle event] eventContext eventWorld,
    single "core-cancel-owner" .cancelEvent cancel [.handle {event with owner := 8}] eventContext eventWorld true,
    single "core-cancel-stale" .cancelEvent cancel [.handle {event with generation := 3}] eventContext eventWorld true,
    single "core-output-false" .bufferOutputWrite output [u 1,.bool false],
    single "core-output-true" .bufferOutputWrite output [u 1,.bool true] {base with instanceId := 2},
    single "core-output-port-overflow" .bufferOutputWrite output [u (2^32),.bool true] base {} true,
    single "core-current-process" .getContextField current [] processContext processWorld,
    single "core-current-missing" .getContextField current [] base {} true,
    single "core-wait-result-ready" .readWaitResult waitResult [.variant 0 [.unit]] processContext processWorld,
    single "core-wait-result-error" .readWaitResult waitResult [.variant 1 [u 3]] processContext processWorld true,
    single "core-timer-invalid-kind" .registerWait timer [.handle process,u 9,u 1] processContext processWorld true,
    single "core-timer-owner" .registerWait timer [.handle {process with owner := 8},u 3,u 1] processContext processWorld true,
    single "core-timer-orphan" .registerWait timer [.handle process,u 3,u 1] processContext processWorld true]
  let accepted := Value.record [u 0,.variant 0 [],u 0,.variant 0 []]
  result := result ++ [single "core-transport-invalid-sync" .setTransportReturn transport
    [.record [u 3,.variant 0 [],u 0,.variant 0 []]] {base with kind := 2} {} true]
  let returned := single "core-transport-accepted" .setTransportReturn transport [accepted] {base with kind := 2}
  let some handler := returned.model.handlers.head? | throw "CoreHandlerMissing"
  let handler := {handler with body := [.transportReturn (.local 12 0)],declaredResultTypes := some [12]}
  result := result ++ [{returned with model := {returned.model with handlers := [handler]}}]
  for (name,kind,time) in [("core-timer-after",3,2),("core-timer-ready",3,0),("core-timer-until-past",4,1)] do
    let candidate := single name .registerWait timer [.handle process,u kind,u time] processContext processWorld
    let some body := candidate.model.handlers.head? | throw "CoreTimerHandlerMissing"
    let body := {body with
      declaredResultTypes := some [6],
      body := [.serviceCall (some ⟨3,5,{}⟩) 0 [.local 4 0,.local 0 1,.local 0 2],
        .await (.local 5 3) 4 6,.ret [.local 6 4]]}
    result := result ++ [{candidate with expectedOutcome := "suspended",model := {candidate.model with handlers := [body]}}]
  let chained := single "core-schedule-cancel-own" .scheduleEvent schedule [u 9,.bytes [8]]
  let some chainedHandler := chained.model.handlers.head? | throw "CoreChainHandlerMissing"
  let chainedHandler := {chainedHandler with
    declaredResultTypes := some [1],
    body := [.serviceCall (some ⟨2,3,{}⟩) 0 [.local 0 0,.local 2 1],
      .serviceCall (some ⟨3,1,{}⟩) 1 [.local 3 2],.ret [.local 1 3]]}
  result := result ++ [{chained with model := {chained.model with
    handlers := [chainedHandler],
    services := [schedule,{cancel with id := 1}]},expectedOpcodeTags := [Op.scheduleEvent.tag,Op.cancelEvent.tag]}]
  pure result
end LeanAT.Compiler.OpcodeCases.Core






