import LeanAT.Compiler.OpcodeCase
import LeanAT.Reference.Objects
import LeanAT.Reference.Structured
import LeanAT.Reference.Protocol

namespace LeanAT.Compiler.OpcodeCases.Objects
open LeanAT.Reference LeanAT.ExecIR
private def u (n : Nat) : Value := .bits 64 n
-- Mirrors only source ABI types, never results supplied by either execution backend.
def types : TypeEnvironment := [.bits 64,.bytes 16,.bool,.handle .transaction,.handle .resourceTicket,
  .record [0,0,0,4,0],.record [0,1],.handle .event,.record [5,3,7,0],.boundedVec 5 4,
  .handle .result,.handle .consumer,.record [10,11],.variant [[],[12]],.handle .scope,.handle .drain,
  .variant [[14],[15]],.variant [[],[16]],.variant [[],[0]],.record [18,13,17,0],.variant [[],[19]],.boundedVec 0 4]
private def context : Context := {
  domain := 1,instanceId := 2,owner := 7,connection := 9,
  environment := [("profile.valueNodeBytes",u 40),("objects.valueNodeBytes",u 40),("objects.epoch",u 0)]}
private def transaction : HandleIdentity := ⟨.transaction,1,50,0,1,7⟩
private def service (s : ServiceSignature) : ModelIR.ServiceIR :=
  {id := s.id,opcode := opcodeNames[s.op.tag]!,inputTypes := s.inputTypes,resultTypes := s.resultTypes,
   contextMask := s.contextMask,effectMask := s.effectMask,extraFuel := s.extraFuel,
   providerKey := s.providerKey,providerVersion := s.providerVersion,abiHash := s.abiHash.data.toList}
private def signature (kind obj mid : Nat) (inputs outputs : List Nat) (id : Nat := 1) :=
  Reference.Objects.makeSignature types kind obj mid inputs outputs 16 4 id
private def sourceModule := "LeanAT.Compiler.OpcodeCases.Objects"
private def moduleFile := "LeanAT/Compiler/OpcodeCases/Objects.lean"
private def fixtureContext (kind objectId : Nat) (entry : ObjectEntry) (debug : Bool) : Context :=
  {context with
    kind := if debug then 3 else 0
    environment := context.environment ++ [("objects.kind",u kind),("objects.id",u objectId),
      ("objects.record",entry.value),("objects.maxBytes",u 16),("objects.maxEntries",u 4),
      ("objects.elementType",u 0)]}
private def single (name : String) (kind mid : Nat) (entry : ObjectEntry)
    (inputs outputs : List Nat) (arguments : List Value) (negative : Bool := false) : Except String OpcodeCase := do
  let sig ← signature kind entry.identity.slot.toNat mid inputs outputs
  let parameters := inputs.zipIdx |>.map (fun (typeId,id) => ({id,typeId} : ModelIR.LocalBinderIR))
  let destination := (outputs.head?).map (fun typeId => ({id := inputs.length,typeId} : ModelIR.LocalBinderIR))
  let call := ModelIR.Stmt.serviceCall destination sig.id (parameters.map (fun p => .local p.typeId p.id))
  let returned := destination.toList.map (fun p => ModelIR.Expr.local p.typeId p.id)
  let handler : ModelIR.Handler := {
    id := 0,parameters,context := if mid == 6 then .debugEntry else .timedHandler,
    body := [call,.ret returned],declaredResultTypes := some outputs,source := {file := moduleFile}}
  pure {
    id := name,variant := if negative then "failure" else "success",providerFamily := "objects",
    model := {types,handlers := [handler],services := [service sig]},
    input := {inputs := arguments,world := Reference.Objects.stateWithObjects [entry],context := fixtureContext kind entry.identity.slot.toNat entry (mid == 6)},
    expectedOpcodeTags := [sig.op.tag],expectedOutcome := if negative then "failure" else "success",
    sourceModule,sourceFiles := [moduleFile,"LeanAT/Reference/Objects.lean"]}
private def chain (name : String) (kind : Nat) (entry : ObjectEntry) (signatures : List ServiceSignature)
    (body : List ModelIR.Stmt) (negative : Bool := false) : OpcodeCase :=
  {id := name,variant := if negative then "failure" else "success",providerFamily := "objects",
   model := {types,services := signatures.map service,handlers := [{id := 0,body,source := {file := moduleFile}}]},
   input := {world := Reference.Objects.stateWithObjects [entry],context := fixtureContext kind entry.identity.slot.toNat entry false},
   expectedOpcodeTags := signatures.map (fun s => s.op.tag) |>.eraseDups,
   expectedOutcome := if negative then "failure" else "success",sourceModule,sourceFiles := [moduleFile,"LeanAT/Reference/Objects.lean"]}
private def lit (t : Nat) (v : Value) : ModelIR.Expr := .literal t v

def cases : Except String (List OpcodeCase) := do
  let mem := Reference.Objects.memoryEntry context 1 [1,2,3,4]
  let mut result := []
  result := result ++ [← single "object-memory-transfer" 0 1 mem [0,0,1,0,1] [6] [u 1,u 0,.bytes [5,6,7],u 2,.bytes [255,0]]]
  result := result ++ [← single "object-memory-read" 0 2 mem [0,0] [1] [u 0,u 4]]
  result := result ++ [← single "object-memory-read-empty" 0 2 mem [0,0] [1] [u (2^64-1),u 0]]
  result := result ++ [← single "object-memory-write" 0 3 mem [0,1,1] [] [u 1,.bytes [8,9],.bytes [255,0]]]
  result := result ++ [← single "object-memory-clear" 0 4 mem [] [] []]
  result := result ++ [← single "object-memory-reset" 0 5 mem [] [] []]
  let resetMemory ← signature 0 1 5 [] []
  result := result ++ [chain "object-memory-reset-twice" 0 mem [resetMemory]
    [.serviceCall none 1 [],.serviceCall none 1 [],.ret []]]
  result := result ++ [← single "object-memory-debug-prefix" 0 6 mem [0,0,1] [6] [u 1,u 3,.bytes [8,9]]]
  result := result ++ [← single "object-memory-range-failure" 0 3 mem [0,1,1] [] [u 4,.bytes [8],.bytes []] true]
  result := result ++ [← single "object-memory-mask-failure" 0 3 mem [0,1,1] [] [u 0,.bytes [8],.bytes [1]] true]
  let write ← signature 0 1 3 [0,1,1] []
  result := result ++ [chain "object-memory-later-failure" 0 mem [write]
    [.serviceCall none 1 [lit 0 (u 0),lit 1 (.bytes [9]),lit 1 (.bytes [])],.fail "ObjectRollback"] true]
  let reg : ObjectEntry := {mem with
    identity := {mem.identity with store := 301,slot := 2}
    tag := "reference.object.register"
    value := .record [u 2,.bytes [255],.bytes [255],.vec [.record [u 0,u 8,.vec [u 1],u 0,.bool false,
      .vec [.record [u 1,u 0,u 4,u 5],.record [u 2,u 4,u 4,u 3]]]]]}
  result := result ++ [← single "object-register-rc" 1 1 reg [0,0,1,0,1] [6] [u 0,u 0,.bytes [0],u 1,.bytes []]]
  result := result ++ [← single "object-register-w1c" 1 1 reg [0,0,1,0,1] [6] [u 1,u 0,.bytes [48],u 1,.bytes []]]
  result := result ++ [← single "object-register-field" 1 7 reg [0] [1] [u 1]]
  result := result ++ [← single "object-register-update" 1 8 reg [0,1] [] [u 1,.bytes [3]]]
  result := result ++ [← single "object-register-field-width" 1 8 reg [0,1] [] [u 1,.bytes [16]] true]
  result := result ++ [← single "object-register-debug" 1 6 reg [0,0,1] [6] [u 1,u 0,.bytes [9,9]]]
  let r := Reference.Objects.resourceEntry context 3 5 0 1 4
  result := result ++ [← single "object-resource-reserve" 2 9 r [3,0,0] [5] [.handle transaction,u 2,u 5]]
  result := result ++ [← single "object-resource-default" 2 10 r [3,0] [5] [.handle transaction,u 2]]
  result := result ++ [← single "object-resource-owner-failure" 2 10 r [3,0] [5] [.handle {transaction with owner := 8},u 0] true]
  let reserve ← signature 2 3 9 [3,0,0] [5]
  let cancel ← signature 2 3 11 [4] [0] 2
  let complete ← signature 2 3 12 [4,0] [2] 2
  let inspect ← signature 2 3 13 [] [9] 2
  let reserveStmt := ModelIR.Stmt.serviceCall (some ⟨0,5,{}⟩) 1 [lit 3 (.handle transaction),lit 0 (u 0),lit 0 (u 0)]
  let ticket := ModelIR.Expr.field 4 (.local 5 0) 3
  result := result ++ [chain "object-resource-cancel-unpublished" 2 r [reserve,cancel]
    [reserveStmt,.serviceCall (some ⟨1,0,{}⟩) 2 [ticket],.ret [.local 0 1]]]
  result := result ++ [chain "object-resource-complete-zero" 2 r [reserve,complete]
    [reserveStmt,.serviceCall (some ⟨1,2,{}⟩) 2 [ticket,lit 0 (u 0)],.ret [.local 2 1]]]
  result := result ++ [chain "object-resource-inspect" 2 r [reserve,inspect]
    [reserveStmt,.serviceCall (some ⟨1,9,{}⟩) 2 [],.ret [.local 9 1]]]
  let q := Reference.Objects.queueEntry context 4 1 128 4 99
  result := result ++ [← single "object-queue-push" 3 14 q [0] [2] [u 42]]
  result := result ++ [← single "object-queue-empty" 3 15 q [] [20] []]
  result := result ++ [← single "object-queue-size" 3 16 q [] [0] []]
  let push ← signature 3 4 14 [0] [2]
  let pop ← signature 3 4 15 [] [20] 2
  result := result ++ [chain "object-queue-fifo" 3 q [push,pop]
    [.serviceCall (some ⟨0,2,{}⟩) 1 [lit 0 (u 42)],.serviceCall (some ⟨1,20,{}⟩) 2 [],.ret [.local 20 1]]]
  let (resultId,consumerId,ownedWorld) ← Storage.reserveResult types context (Reference.Objects.stateWithObjects [q]) transaction 0 128 context.owner
  for (name,policy) in [("object-queue-move-owned",1),("object-queue-retain-owned",2)] do
    let fixture ← single name 3 19 q [10,11,0] [2] [.handle resultId,.handle consumerId,u policy]
    let seed := .record [.handle transaction,u 0,u 128,.handle resultId,.handle consumerId]
    let ownedContext := {fixture.input.context with environment := fixture.input.context.environment ++ [("objects.resultSeed",seed)]}
    let ownedInput := {fixture.input with world := ownedWorld,context := ownedContext}
    result := result ++ [{fixture with input := ownedInput}]
  let (scope,scopeWorld) ← Structured.seedRoot context {}
  let .record qfields := q.value | throw "QueueSourceRecord"
  let qOwned := {q with value := .record (qfields.set 5 (u 2) |>.set 6 (.vec [.record [u 1,.variant 1 [u 42],.variant 1 [.handle scope],.bool false,.variant 0 []]]))}
  let removal ← single "object-queue-remove-scope" 3 20 qOwned [14] [21] [.handle scope]
  let removalContext := {removal.input.context with environment := removal.input.context.environment ++ [("objects.scopeSeed",.handle scope)]}
  let removalWorld := {scopeWorld with objects := qOwned::scopeWorld.objects}
  result := result ++ [{removal with input := {removal.input with context := removalContext,world := removalWorld}}]
  let newTransaction : ServiceSignature := {
    id := 50
    op := .newTransaction
    inputTypes := [0,0,0,0,0,1]
    resultTypes := [3]
    contextMask := 3
    effectMask := 8
    extraFuel := 0
    providerKey := "leanat.protocol"
    providerVersion := "1"
    abiHash := ABI.sha256 "leanat.protocol.v1:newTransaction".toUTF8 }
  let protocolWorld ← Reference.Protocol.seedProtocol context (← Reference.Storage.seedResultStore scopeWorld context 128 256 256)
  let (drainTxn,protocolWorld) ← Reference.Protocol.seedInitiator types context newTransaction
    [u context.connection,u 1,u 1,u 0,u 0,.bytes [4,9]] protocolWorld
  let protocolWorld ← Reference.Protocol.observeInitialExchange context protocolWorld drainTxn false
  let (drain,protocolWorld) ← Reference.Protocol.seedOwnedDrain context protocolWorld drainTxn 1
  let qPublished := {q with value := .record (qfields.set 5 (u 2) |>.set 6
    (.vec [.record [u 1,.variant 1 [u 42],.variant 1 [.handle scope],.bool true,.variant 0 []]]))}
  let transfer ← single "object-queue-transfer-drain" 3 21 qPublished [14,15,0] [21]
    [.handle scope,.handle drain,u 1]
  let transferContext := {transfer.input.context with
    environment := transfer.input.context.environment ++ [
      ("objects.scopeSeed",.handle scope),
      ("objects.drainSeed",.record [u context.connection,u 1,.bytes [4,9],.handle drainTxn,.handle drain])] }
  result := result ++ [{transfer with input := {transfer.input with
    context := transferContext
    world := {protocolWorld with objects := qPublished::protocolWorld.objects} }}]
  let p := Reference.Objects.resourceEntry context 5 0 0 1 4 true
  result := result ++ [← single "object-pipeline-zero-event" 4 17 p [3,0] [8] [.handle transaction,u 0]]
  let submit ← signature 4 5 17 [3,0] [8]
  let pcancel ← signature 4 5 11 [8] [0] 2
  result := result ++ [chain "object-pipeline-cancel-unpublished" 4 p [submit,pcancel]
    [.serviceCall (some ⟨0,8,{}⟩) 1 [lit 3 (.handle transaction),lit 0 (u 0)],
      .serviceCall (some ⟨1,0,{}⟩) 2 [.local 8 0],.ret [.local 0 1]]]
  let ready ← signature 4 5 18 [8,7] [2] 2
  result := result ++ [chain "object-pipeline-ready-before-dispatch" 4 p [submit,ready]
    [.serviceCall (some ⟨0,8,{}⟩) 1 [lit 3 (.handle transaction),lit 0 (u 0)],
      .serviceCall (some ⟨1,2,{}⟩) 2 [.local 8 0,.field 7 (.local 8 0) 2],.ret [.local 2 1]] true]
  -- Setup is independently executed by the native fixture through submit/pop_batch.
  -- Only initial configuration and logical capabilities cross to that fixture.
  let (seededValues,seededWorld) ← Reference.Objects.invoke types (fixtureContext 4 5 p false)
    submit [.handle transaction,u 0] (Reference.Objects.stateWithObjects [p])
  let [pending] := seededValues | throw "PipelineSourceSeed"
  let .record [_,_,.handle readyEvent,_] := pending | throw "PipelineSourceTicket"
  for (name,received,negative) in [("object-pipeline-ready",readyEvent,false),
      ("object-pipeline-stale-ready",{readyEvent with generation := readyEvent.generation+1},true)] do
    let fixture ← single name 4 18 p [8,7] [2] [pending,.handle received] negative
    let readyContext := {fixture.input.context with
      turn := 1
      environment := fixture.input.context.environment ++ [("objects.activeEvent",.handle readyEvent),
        ("objects.pipelineSeed",.record [.handle transaction,u 0,u 0,u 0,pending])]}
    result := result ++ [{fixture with input := {fixture.input with world := seededWorld,context := readyContext}}]
  pure result
end LeanAT.Compiler.OpcodeCases.Objects


