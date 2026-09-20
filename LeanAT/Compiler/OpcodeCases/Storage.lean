import LeanAT.Compiler.OpcodeCase
import LeanAT.Reference.Storage

namespace LeanAT.Compiler.OpcodeCases.Storage
open LeanAT.Reference LeanAT.Reference.Storage

def types : TypeEnvironment := [.unit,.bool,.bits 64,.handle .transaction,.bytes 4,.variant [[],[4]]]
def context : Context := {kind := 0,now := 5,turn := 2,domain := 1,instanceId := 0,owner := 9,environment := [("profile.valueNodeBytes",.bits 64 40)]}
def transaction : HandleIdentity := ⟨.transaction,1,40,0,1,9⟩
def hop : HandleIdentity := ⟨.hop,1,41,0,1,9⟩
def payload : PayloadRecord := {transaction,hop,instanceId := 0,localSide := 7,target := true,writable := true,command := 0,address := 4096,streamingWidth := 4,baseline := [1,2,3,4],data := [1,2,3,4],byteEnable := [255,0],extensions := [{name := "trace",maxBytes := 4}]}
def access : PayloadAccess := {hop,localSide := 7,responseWritePermit := true}
private def source : SourceSpan := {file := "LeanAT/Compiler/OpcodeCases/Storage.lean",line := 1,column := 1}
private def modelService (s : ExecIR.ServiceSignature) : ModelIR.ServiceIR := {
  id := s.id,opcode := ExecIR.opcodeNames[s.op.tag]!,inputTypes := s.inputTypes,resultTypes := s.resultTypes,
  contextMask := s.contextMask,effectMask := s.effectMask,extraFuel := s.extraFuel,providerKey := s.providerKey,
  providerVersion := s.providerVersion,abiHash := s.abiHash.data.toList,source}
private def input (values : List Value) (p : PayloadRecord := payload) (a : PayloadAccess := access) : Except String JsonIO.Input := do
  let (view,world) ← createPayload context {} p
  let ctx := bindPayload context view a
  let environment := [("storage.payload.record",p.encode),("storage.payload.access",a.encode)] ++ ctx.environment.filter (fun pair => pair.1 != "storage.payload.access")
  pure {inputs := values,world,context := {ctx with environment}}
private def buildModel (services : List ExecIR.ServiceSignature) (parameters : List Nat)
    (resultTypes : List Nat) (body : List ModelIR.Stmt) : ModelIR.Project := {
  types, services := services.map modelService,handlers := [{id := 0,source,context := .timedHandler,parameters := parameters.zipIdx |>.map (fun (typeId,id) => {id,typeId,source}),declaredResultTypes := some resultTypes,body}]}
private def makeCase (id variant : String) (model : ModelIR.Project) (fixture : JsonIO.Input)
    (tags : List Nat) (failure : Bool := false) : OpcodeCase := {
  id,variant,providerFamily := "payload",model,input := fixture,expectedOpcodeTags := tags,
  expectedOutcome := if failure then "failure" else "success",
  sourceModule := "LeanAT.Compiler.OpcodeCases.Storage",sourceFiles := [source.file]}

/-- Real ModelIR source programs and initial owned worlds. The generated descriptor,
    VM, and independent interpreter compute outputs; this catalog stores none. -/
def cases : Except String (List OpcodeCase) := do
  let getter ← payloadSignature types 0 "leanat.payload.data.get" 3 4 0
  let writer ← payloadSignature types 1 "leanat.payload.data.write" 3 4 0
  let extGet ← payloadSignature types 2 "leanat.extension.trace.get" 3 5 0
  let extWrite ← payloadSignature types 3 "leanat.extension.trace.write" 3 4 0
  let getModel := buildModel [getter] [3] [4]
    [.serviceCall (some {id := 10,typeId := 4,source}) getter.id [.local 3 0],.ret [.local 4 10]]
  let writeModel := buildModel [writer,getter] [3,4] [4]
    [.serviceCall (some {id := 10,typeId := 0,source}) writer.id [.local 3 0,.local 4 1],
     .serviceCall (some {id := 11,typeId := 4,source}) getter.id [.local 3 0],.ret [.local 4 11]]
  let extGetModel := buildModel [extGet] [3] [5]
    [.serviceCall (some {id := 10,typeId := 5,source}) extGet.id [.local 3 0],.ret [.local 5 10]]
  let extWriteModel := buildModel [extWrite,extGet] [3,4] [5]
    [.serviceCall (some {id := 10,typeId := 0,source}) extWrite.id [.local 3 0,.local 4 1],
     .serviceCall (some {id := 11,typeId := 5,source}) extGet.id [.local 3 0],.ret [.local 5 11]]
  let getInput ← input [.handle transaction]
  let staleInput ← input [.handle {transaction with generation := 2}]
  let writeInput ← input [.handle transaction,.bytes [9,9,9,9]]
  let shortInput ← input [.handle transaction,.bytes [9]]
  let writeCommandInput ← input [.handle transaction,.bytes [9,9,9,9]] {payload with command := 1}
  let noPermitInput ← input [.handle transaction,.bytes [9,9,9,9]] payload {access with responseWritePermit := false}
  let extensionInput ← input [.handle transaction,.bytes [8,7]]
  let extensionNoPermit ← input [.handle transaction,.bytes [8,7]] payload {access with responseWritePermit := false}
  let readOnlyExtension ← input [.handle transaction] {payload with extensions := [{name := "trace",maxBytes := 4,value := some [6,5]}]}
  let rollbackModel := buildModel [writer] [3,4] []
    [.serviceCall (some {id := 10,typeId := 0,source}) writer.id [.local 3 0,.local 4 1],.fail "intentional post-write rollback"]
  pure [
    makeCase "payloadGet" "success" getModel getInput [getter.op.tag],
    makeCase "payloadGet" "wrong-connection" getModel {getInput with context := {getInput.context with connection := 1}} [getter.op.tag] true,
    makeCase "payloadGet" "stale-transaction" getModel staleInput [getter.op.tag] true,
    makeCase "bufferPayloadWrite" "success" writeModel writeInput [writer.op.tag,getter.op.tag],
    makeCase "bufferPayloadWrite" "short-read-response" writeModel shortInput [writer.op.tag] true,
    makeCase "bufferPayloadWrite" "write-request-immutable" writeModel writeCommandInput [writer.op.tag] true,
    makeCase "bufferPayloadWrite" "missing-response-permit" writeModel noPermitInput [writer.op.tag] true,
    makeCase "bufferPayloadWrite" "segment-rollback" rollbackModel writeInput [writer.op.tag] true,
    makeCase "extensionGet" "absent" extGetModel getInput [extGet.op.tag],
    makeCase "extensionGet" "present" extGetModel readOnlyExtension [extGet.op.tag],
    makeCase "extensionGet" "stale-transaction" extGetModel staleInput [extGet.op.tag] true,
    makeCase "bufferExtensionWrite" "success" extWriteModel extensionInput [extWrite.op.tag,extGet.op.tag],
    makeCase "bufferExtensionWrite" "missing-response-permit" extWriteModel extensionNoPermit [extWrite.op.tag] true]
end LeanAT.Compiler.OpcodeCases.Storage





