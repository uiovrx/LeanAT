import LeanAT.Compiler.OpcodeCase
import LeanAT.Reference.Protocol

namespace LeanAT.Compiler.OpcodeCases.Protocol
open LeanAT.Reference LeanAT.ExecIR
private def u (n : Nat) : Value := .bits 64 n
def types : TypeEnvironment := [.unit,.bits 64,.bytes 16,.handle .transaction,.handle .result,.handle .consumer]
private def source : SourceSpan := {file := "LeanAT/Compiler/OpcodeCases/Protocol.lean",line := 1,column := 1}
private def context : Context := {
  domain := 1,instanceId := 1,connection := 1,owner := 9,
  environment := [("profile.valueNodeBytes",u 40),("runtime.maxPayloadBytes",u 4096)]}
private def fromHex (s : String) : ByteArray :=
  let ns := s.toList.map fun c => if c ≤ '9' then c.toNat - '0'.toNat else c.toNat - 'a'.toNat + 10
  ⟨(List.range (ns.length/2)).toArray.map (fun i => UInt8.ofNat (ns[2*i]! * 16 + ns[2*i+1]!))⟩
private def signature (op : Op) : Except String ServiceSignature := do
  let (inputs,outputs,key,hash,effect) ← match op with
    | .newTransaction => pure ([1,1,1,1,1,2],[3],"leanat.protocol",ABI.sha256 "leanat.protocol.v1:newTransaction".toUTF8,8)
    | .stagePhase => pure ([3,1,1,1],[0],"leanat.protocol",ABI.sha256 "leanat.protocol.v1:stagePhase".toUTF8,8)
    | .ackResponse => pure ([3,1,1],[0],"leanat.protocol",ABI.sha256 "leanat.protocol.v1:ackResponse".toUTF8,8)
    | .cancelLocal => pure ([3,1],[0],"leanat.core.transaction.cancel",fromHex "fd95191dfcd83497a02c4edcca1089d64b98ba014a288bf1800bc12df81a6956",8)
    | .resultGet => let (key,hash) ← Storage.coreIdentity op; pure ([4,5],[2],key,hash,64)
    | .resultRelease => let (key,hash) ← Storage.coreIdentity op; pure ([4,5],[0],key,hash,64)
    | _ => throw "ProtocolCaseOpcode"
  pure {
    id := 0,op,inputTypes := inputs,resultTypes := outputs,contextMask := 3,effectMask := effect,
    extraFuel := 0,providerKey := key,providerVersion := "1",abiHash := hash}
private def model (s : ServiceSignature) : ModelIR.Project :=
  let parameters := s.inputTypes.zipIdx |>.map (fun (typeId,id) => ({id,typeId,source} : ModelIR.LocalBinderIR))
  let result := s.resultTypes.head!
  {
    types,services := [{
      id := s.id,opcode := opcodeNames[s.op.tag]!,inputTypes := s.inputTypes,
      resultTypes := s.resultTypes,contextMask := s.contextMask,effectMask := s.effectMask,
      extraFuel := s.extraFuel,providerKey := s.providerKey,providerVersion := s.providerVersion,
      abiHash := s.abiHash.data.toList,source}],handlers := [{
      id := 0,parameters,source,
      context := .timedHandler,declaredResultTypes := some s.resultTypes,
      body := [.serviceCall (some {id := 20,typeId := result,source}) s.id
        (parameters.map (fun p => .local p.typeId p.id)),.ret [.local result 20]]}]}
private def make (op : Op) (variant : String) (fixture : JsonIO.Input) (failure := false) : Except String OpcodeCase := do
  let s ← signature op
  pure {
    id := opcodeNames[op.tag]!,variant,providerFamily := "protocol",model := model s,input := fixture,
    expectedOpcodeTags := [op.tag],expectedOutcome := if failure then "failure" else "success",
    sourceModule := "LeanAT.Compiler.OpcodeCases.Protocol",sourceFiles := [source.file,"LeanAT/Reference/Protocol.lean","LeanAT/Reference/Storage.lean"]}
private def initial : Except String State := do
  Reference.Protocol.seedProtocol context (← Storage.seedResultStore {} context 128 256 128)
private def seedTransaction (phase : Nat) : Except String (HandleIdentity × State) := do
  let s ← signature .newTransaction
  let (txn,world) ← Reference.Protocol.seedInitiator types context s [u 1,u 1,u 1,u 0,u 0,.bytes [4,9]] (← initial)
  let world ← if phase == 0 then pure world else Reference.Protocol.observeInitialExchange context world txn (phase == 4)
  pure (txn,world)
private def seeded (op : Op) (state : Nat) (tail : List Value) : Except String JsonIO.Input := do
  let (txn,world) ← seedTransaction (if state == 1 then 3 else if state == 2 then 4 else 0)
  pure {inputs := .handle txn :: tail,world,context := {context with environment := context.environment ++
    [("operation",u op.tag),("protocolState",u state),("requestBytes",.bytes [4,9]),("transactionIdentity",.handle txn)]}}
private def resultInput (published released : Bool) : Except String JsonIO.Input := do
  let (source,world) ← allocate (← initial) context .process 10 "runtime.process" (.record [u 0])
  let (result,consumer,world) ← Storage.reserveResult types context world source 2 4096 context.owner
  let world ← if published then Storage.publishResult types context world result (.bytes [4,9]) else pure world
  let world ← if released then Storage.releaseResult context world result consumer else pure world
  pure {inputs := [.handle result,.handle consumer],world,context := {context with environment := context.environment ++
    [("resultType",u 2),("resultValue",.bytes [4,9]),("resultPublished",.bool published),("resultReleased",.bool released),
     ("resultIdentity",.handle result),("consumerIdentity",.handle consumer)]}}
private def staleInput (fixture : JsonIO.Input) (index : Nat) : Except String JsonIO.Input := do
  let some (.handle h) := fixture.inputs[index]? | throw "StaleFixtureHandle"
  pure {fixture with inputs := fixture.inputs.zipIdx |>.map (fun (value,n) =>
    if n == index then .handle {h with generation := h.generation+1} else value)}

/-- Source programs plus actual initial provider worlds, never expected backend results. -/
def cases : Except String (List OpcodeCase) := do
  pure [
    ← make .newTransaction "success" {inputs := [u 1,u 1,u 1,u 0,u 0,.bytes [4,9]],context,world := ← initial},
    ← make .newTransaction "zero-generation" {inputs := [u 1,u 1,u 0,u 0,u 0,.bytes [4,9]],context,world := ← initial} true,
    ← make .stagePhase "success" (← seeded .stagePhase 0 [u 1,u 1,u 0]),
    ← make .stagePhase "wrong-connection" (← seeded .stagePhase 0 [u 2,u 1,u 0]) true,
    ← make .stagePhase "stale-generation" (← staleInput (← seeded .stagePhase 0 [u 1,u 1,u 0]) 0) true,
    ← make .ackResponse "success" (← seeded .ackResponse 1 [u 1,u 0]),
    ← make .ackResponse "before-response" (← seeded .ackResponse 0 [u 1,u 0]) true,
    ← make .cancelLocal "success" (← seeded .cancelLocal 0 [u 0]),
    ← make .cancelLocal "response-pending" (← seeded .cancelLocal 1 [u 0]),
    ← make .cancelLocal "invalid-reason" (← seeded .cancelLocal 0 [u 4]) true,
    ← make .resultGet "success" (← resultInput true false),
    ← make .resultGet "not-published" (← resultInput false false) true,
    ← make .resultGet "stale-consumer-generation" (← staleInput (← resultInput true false) 1) true,
    ← make .resultRelease "success" (← resultInput true false),
    ← make .resultRelease "already-released" (← resultInput true true) true]
end LeanAT.Compiler.OpcodeCases.Protocol
