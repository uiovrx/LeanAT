import Lean
import LeanAT.ModelIR.Transport

namespace LeanAT.ModelIR.TransportCLI
open Lean
abbrev TConfig := Transport.Config
abbrev TState := Transport.State
abbrev TRecord := Transport.TranscriptRecord

structure Envelope where
  ordinal : Nat
  domain : Nat
  instanceId : Nat
  localSide : String
  connection : Nat
  callId : Nat
  raw : Json

structure Input where
  config : TConfig
  fuel : Nat
  horizon : Option Nat
  records : List TRecord
  envelopes : Array Envelope

structure Evaluation where
  result : Transport.RunResult
  eventSources : List (Nat × Nat)

private def natJson (n : Nat) : Json := toJson (toString n)
private def readNat (json : Json) : Except String Nat :=
  match json with
  | .str value => match value.toNat? with | some n => .ok n | none => .error "ExpectedUnsignedDecimal"
  | _ => json.getNat?
private def field (json : Json) (key : String) : Except String Json :=
  (json.getObjVal? key).mapError (fun _ => "MissingField: " ++ key)
private def natField (json : Json) (key : String) : Except String Nat := do readNat (← field json key)
private def stringField (json : Json) (key : String) : Except String String := do (← field json key).getStr?
private def bounded (n maxValue : Nat) (label : String) : Except String Nat :=
  if n < maxValue then .ok n else .error (label ++ "OutOfRange")
private def readByte (json : Json) : Except String UInt8 := do
  let n ← bounded (← readNat json) 256 "Byte"
  pure (UInt8.ofNat n)
private def readBytes (json : Json) : Except String (List UInt8) := do
  let bytes ← json.getArr?
  if bytes.size > 1048576 then throw "ByteArrayLimit"
  bytes.toList.mapM readByte
private def readPhase (json : Json) : Except String Transport.Phase := do
  match ← json.getStr? with
  | "beginReq" => pure .beginReq
  | "endReq" => pure .endReq
  | "beginResp" => pure .beginResp
  | "endResp" => pure .endResp
  | _ => throw "UnsupportedBasePhase"
private def readOptionalPhase (json : Json) : Except String (Option Transport.Phase) :=
  match json with | .null => .ok none | _ => (readPhase json).map some
private def readFlow (json : Json) : Except String Transport.Flow := do
  match ← json.getStr? with
  | "forward" => pure .forward | "backward" => pure .backward
  | _ => throw "UnsupportedFlow"
private def readPayload (json : Json) : Except String Transport.Payload := do
  let command ← readByte (← field json "command")
  if command.toNat > 2 then throw "UnsupportedPayloadCommand"
  let address ← bounded (← natField json "address") (2^64) "Address"
  let data ← readBytes (← field json "data")
  let length ← bounded (← natField json "length") (2^32) "Length"
  let streaming ← bounded (← natField json "streamingWidth") (2^32) "StreamingWidth"
  let byteEnable ← readBytes (← field json "byteEnable")
  let status ← readByte (← field json "status")
  let dmiHint ← (← field json "dmiHint").getBool?
  let extensions ← (← field json "extensions").getObj?
  if !extensions.toList.isEmpty then throw "UnsupportedRegisteredExtensions: reference has no declared extension codec table"
  pure ⟨command,UInt64.ofNat address,data,UInt32.ofNat length,UInt32.ofNat streaming,byteEnable,status,dmiHint,[]⟩
private def readReply (json : Json) : Except String Transport.WireReturn := do
  let sync ← match ← stringField json "sync" with
    | "accepted" => pure Transport.Sync.accepted
    | "updated" => pure Transport.Sync.updated
    | "completed" => pure Transport.Sync.completed
    | _ => throw "UnsupportedSync"
  let outgoingDelay ← natField json "outgoingDelay"
  -- Unused returned fields remain in the raw envelope, but do not enter semantic decoding.
  let phaseJson ← field json "phase"
  let phase ← if sync == .updated then readOptionalPhase phaseJson else pure none
  let responseJson ← field json "response"
  let response ← if sync == .accepted then pure none else match responseJson with
    | .null => pure none
    | _ => (readPayload responseJson).map some
  pure ⟨sync,phase,outgoingDelay,response⟩
private def readEnvelope (json : Json) : Except String Envelope := do
  let ordinal ← bounded (← natField json "ordinal") (2^64) "Ordinal"
  let domain ← bounded (← natField json "domain") (2^32) "Domain"
  let instanceId ← bounded (← natField json "instance") (2^32) "Instance"
  let connection ← bounded (← natField json "connection") (2^32) "Connection"
  let callId ← bounded (← natField json "id") (2^64) "CallId"
  let localSide ← stringField json "localSide"
  if localSide != "initiator" && localSide != "target" then throw "UnsupportedLocalSide"
  pure ⟨ordinal,domain,instanceId,localSide,connection,callId,json⟩

def parseInput (json : Json) : Except String Input := do
  if (← stringField json "schema") != "leanat.model-transport-input.v1" then throw "InputSchemaMismatch"
  let configJson ← field json "config"
  let config : TConfig := ⟨← natField configJson "maxOutstanding",← natField configJson "maxPayloadBytes",
    ← natField configJson "maxByteEnableBytes",← natField configJson "eventCapacity",← natField configJson "maxEventsPerTick"⟩
  if config.maxOutstanding == 0 || config.maxOutstanding > 65536 || config.maxPayloadBytes > 1048576 || config.maxByteEnableBytes > 1048576 || config.eventCapacity == 0 || config.eventCapacity > 65536 || config.maxEventsPerTick == 0 || config.maxEventsPerTick > 100000 then throw "UnsupportedOracleBudget"
  let fuel ← natField json "fuel"
  if fuel > 100000 then throw "OracleFuelLimit"
  let horizonJson ← field json "horizon"
  let horizon ← match horizonJson with | .null => pure none | _ => do pure (some (← bounded (← readNat horizonJson) (2^64) "Horizon"))
  let rows ← (← field json "records").getArr?
  if rows.size > 8192 then throw "TranscriptRowLimit"
  let mut records := []
  let mut envelopes : Array Envelope := #[]
  let mut calls : List Envelope := []
  let mut ledgerViews : List Envelope := []
  let mut lastOrdinal : Option Nat := none
  let mut domain : Option Nat := none
  let mut instanceId : Option Nat := none
  for row in rows do
    let envelope ← readEnvelope row
    if lastOrdinal.any (fun previous => envelope.ordinal ≤ previous) then throw "TranscriptOrdinalOrder"
    if domain.any (fun previous => previous != envelope.domain) then throw "UnsupportedMultipleDomains"
    if instanceId.any (fun previous => previous != envelope.instanceId) then throw "UnsupportedMultipleInstances"
    instanceId := some envelope.instanceId
    domain := some envelope.domain
    lastOrdinal := some envelope.ordinal
    if let some previous := ledgerViews.find? (fun view => view.connection == envelope.connection) then
      if previous.instanceId != envelope.instanceId || previous.localSide != envelope.localSide then throw "UnsupportedMirroredLocalLedgers"
    else ledgerViews := envelope::ledgerViews
    let record ← match ← stringField row "kind" with
      | "call" => do
        let transaction ← bounded (← natField row "transaction") (2^64) "Transaction"
        let callTime ← bounded (← natField row "callTime") (2^64) "CallTime"
        let incomingDelay ← bounded (← natField row "incomingDelay") (2^64) "IncomingDelay"
        let call : Transport.WireCall := ⟨envelope.callId,envelope.connection,transaction,← readFlow (← field row "flow"),← readPhase (← field row "phase"),callTime,incomingDelay,← readPayload (← field row "payload")⟩
        calls := envelope::calls
        pure (Transport.TranscriptRecord.call call)
      | "return" => do
        if let some call := calls.find? (fun call => call.callId == envelope.callId) then
          if call.connection != envelope.connection || call.instanceId != envelope.instanceId || call.localSide != envelope.localSide then throw "ReturnEnvelopeMismatch"
        pure (Transport.TranscriptRecord.reply envelope.callId (← readReply row))
      | _ => throw "UnsupportedTranscriptRecord"
    records := record::records
    envelopes := envelopes.push envelope
  pure ⟨config,fuel,horizon,records.reverse,envelopes⟩

private def evaluateFrom (config : TConfig) (horizon : Option Nat) : Nat → TState → List TRecord → Nat → List (Nat × Nat) → Evaluation
  | 0, state, records, _, sources => ⟨Transport.runFrom config horizon 0 state records,sources⟩
  | fuel+1, state, records, cursor, sources =>
    let result := Transport.runFrom config horizon 1 state records
    let consumed := records.length-result.remaining.length
    let added := result.state.nextSequence-state.nextSequence
    let sources := sources ++ (List.range added).map (fun offset => (state.nextSequence+offset,cursor))
    if result.reason == .runBudgetReached then evaluateFrom config horizon fuel result.state result.remaining (cursor+consumed) sources
    else ⟨result,sources⟩
def evaluate (input : Input) : Evaluation := evaluateFrom input.config input.horizon input.fuel {} input.records 0 []

private def phaseJson : Transport.Phase → Json
  | .beginReq => toJson "beginReq" | .endReq => toJson "endReq"
  | .beginResp => toJson "beginResp" | .endResp => toJson "endResp"
private def flowJson : Transport.Flow → Json
  | .forward => toJson "forward" | .backward => toJson "backward"
private def syncJson : Transport.Sync → Json
  | .accepted => toJson "accepted" | .updated => toJson "updated" | .completed => toJson "completed"
private def payloadJson (payload : Transport.Payload) : Json := Json.mkObj [
  ("command",toJson payload.command.toNat),("address",natJson payload.address.toNat),
  ("data",toJson (payload.data.map UInt8.toNat)),("length",natJson payload.length.toNat),
  ("streamingWidth",natJson payload.streamingWidth.toNat),("byteEnable",toJson (payload.byteEnable.map UInt8.toNat)),
  ("status",toJson payload.status.toNat),("dmiHint",toJson payload.dmiHint),("extensions",Json.mkObj [])]
private def commonFields (envelope : Envelope) : List (String × Json) := [
  ("ordinal",natJson envelope.ordinal),("domain",natJson envelope.domain),
  ("instance",natJson envelope.instanceId),("localSide",toJson envelope.localSide),
  ("connection",natJson envelope.connection),("id",natJson envelope.callId)]
private def callJson (call : Transport.WireCall) (envelope : Envelope) : Json := Json.mkObj (commonFields envelope ++ [
  ("kind",toJson "call"),("transaction",natJson call.transaction),("flow",flowJson call.flow),
  ("phase",phaseJson call.phase),("callTime",natJson call.callTime),("incomingDelay",natJson call.incomingDelay),
  ("payload",payloadJson call.payload),("raw",envelope.raw)])
private def replyJson (id : Nat) (reply : Transport.WireReturn) (envelope : Envelope) : Json := Json.mkObj (commonFields envelope ++ [
  ("kind",toJson "return"),("callId",natJson id),("sync",syncJson reply.sync),
  ("phase",(envelope.raw.getObjVal? "phase").toOption.getD Json.null),
  ("outgoingDelay",natJson reply.outgoingDelay),
  ("response",(envelope.raw.getObjVal? "response").toOption.getD Json.null),
  ("raw",envelope.raw)])
private def timedJson (event : Transport.TimedEvent) (envelope : Envelope) : Json :=
  let (kind,phase) := match event.kind with
    | .input phase => ("input",phaseJson phase)
    | .requestReleased => ("requestReleased",Json.null)
    | .responseReady => ("responseReady",Json.null)
    | .terminal => ("terminal",Json.null)
  Json.mkObj [("kind",toJson "timed"),("event",toJson kind),("phase",phase),
    ("time",natJson event.time),("sequence",natJson event.sequence),
    ("turn",natJson event.turn),("stage",natJson event.stage),("keyConnection",natJson event.keyConnection),
    ("connection",natJson event.connection),("transaction",natJson event.transaction),
    ("domain",natJson envelope.domain),("instance",natJson envelope.instanceId),("localSide",toJson envelope.localSide),
    ("causedByCallId",natJson envelope.callId),("causedByOrdinal",natJson envelope.ordinal),
    ("payload",payloadJson event.snapshot)]
private def sourceEnvelope (input : Input) (evaluation : Evaluation) (sequence : Nat) : Except String Envelope := do
  let some pair := evaluation.eventSources.find? (fun pair => pair.1 == sequence) | throw "MissingEventProvenance"
  let some envelope := input.envelopes[pair.2]? | throw "InvalidEventProvenance"
  pure envelope
private def stopJson : ModelIR.StopReason → Json
  | .quiescent => Json.mkObj [("kind",toJson "Quiescent")]
  | .waitingForEnvironment => Json.mkObj [("kind",toJson "WaitingForEnvironment")]
  | .runBudgetReached => Json.mkObj [("kind",toJson "RunBudgetReached")]
  | .horizonReached => Json.mkObj [("kind",toJson "HorizonReached")]
  | .failed error => Json.mkObj [("kind",toJson "Failed"),("error",toJson error)]

def resultJson (input : Input) (evaluation : Evaluation) : Except String Json := do
  let mut trace : Array Json := #[]
  let mut cursor := 0
  for record in evaluation.result.state.trace do
    match record with
    | .call call =>
      let some envelope := input.envelopes[cursor]? | throw "MissingCallProvenance"
      trace := trace.push (callJson call envelope)
      cursor := cursor+1
    | .reply id reply =>
      let some envelope := input.envelopes[cursor]? | throw "MissingReturnProvenance"
      trace := trace.push (replyJson id reply envelope)
      cursor := cursor+1
    | .timed event => trace := trace.push (timedJson event (← sourceEnvelope input evaluation event.sequence))
  let events ← evaluation.result.state.events.mapM (fun event => do pure (timedJson event (← sourceEnvelope input evaluation event.sequence)))
  let hops := evaluation.result.state.hops.map (fun hop => Json.mkObj [
    ("connection",natJson hop.connection),("transaction",natJson hop.transaction),
    ("state",toJson (match hop.state with | .request => "request" | .requestReleased => "requestReleased" | .response => "response" | .terminal => "terminal")),
    ("response",hop.response.map payloadJson |>.getD Json.null),
    ("requestReleasedRecorded",toJson hop.releasedRecorded),("responseReadyRecorded",toJson hop.responseRecorded),("terminalRecorded",toJson hop.terminalRecorded)])
  let consumed := input.records.length-evaluation.result.remaining.length
  pure (Json.mkObj [("schema",toJson "leanat.model-transport-output.v1"),
    ("engine",toJson "LeanAT.ModelIR.Transport"),("leanVersion",toJson Lean.versionString),
    ("coverage",toJson "single-domain single-instance single-local-ledger base protocol; socket calls/returns and timed owned snapshots"),
    ("stopReason",stopJson evaluation.result.reason),("trace",toJson trace),
    ("remainingEvents",toJson events),("remainingRecords",toJson ((input.envelopes.toList.drop consumed).map Envelope.raw)),
    ("hops",toJson hops),("reservedEvents",natJson evaluation.result.state.reservedEvents),
    ("pendingCalls",toJson (evaluation.result.state.pending.map (fun call => natJson call.id))),
    ("eventsAtTick",natJson evaluation.result.state.eventsAtTick),
    ("lastEventTime",evaluation.result.state.lastEventTime.map natJson |>.getD Json.null)])

def runJson (json : Json) : Except String Json := do
  let input ← parseInput json
  resultJson input (evaluate input)

def runCLI (args : List String) : IO UInt32 := do
  let stdout ← IO.getStdout
  let stderr ← IO.getStderr
  let [inputPath,outputPath] := args | do
    stderr.putStrLn "usage: lean --run LeanAT/ModelIR/TransportMain.lean INPUT.json OUTPUT.json"
    return 2
  try
    let bytes ← IO.FS.withFile inputPath .read (fun handle => handle.read (8*1024*1024+1))
    if bytes.size > 8*1024*1024 then throw (IO.userError "OracleInputByteLimit")
    let some contents := String.fromUTF8? bytes | throw (IO.userError "InvalidUTF8")
    let .ok json := Json.parse contents | throw (IO.userError "InvalidJSON")
    match runJson json with
    | .error error =>
      let output := Json.mkObj [("schema",toJson "leanat.model-transport-error.v1"),("error",toJson error)]
      IO.FS.writeFile outputPath (output.compress ++ "\n")
      stderr.putStrLn error
      return 2
    | .ok output =>
      let rendered := output.compress ++ "\n"
      if rendered.utf8ByteSize > 64*1024*1024 then throw (IO.userError "OracleOutputByteLimit")
      IO.FS.writeFile outputPath rendered
      stdout.putStrLn "Lean transport oracle wrote actual reference trace."
      return 0
  catch error =>
    stderr.putStrLn error.toString
    return 2
end LeanAT.ModelIR.TransportCLI


