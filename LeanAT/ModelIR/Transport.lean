import LeanAT.ModelIR.Semantics
namespace LeanAT.ModelIR.Transport
inductive Flow where
  | forward | backward
  deriving Repr, BEq
inductive Phase where
  | beginReq | endReq | beginResp | endResp
  deriving Repr, BEq
inductive Sync where
  | accepted | updated | completed
  deriving Repr, BEq
structure Payload where
  command : UInt8
  address : UInt64
  data : List UInt8
  length : UInt32
  streamingWidth : UInt32
  byteEnable : List UInt8
  status : UInt8
  dmiHint : Bool := false
  extensions : List (String × Value) := []
  deriving Repr, BEq
structure WireCall where
  id : Nat
  connection : Nat
  transaction : Nat
  flow : Flow
  phase : Phase
  callTime : Nat
  incomingDelay : Nat
  payload : Payload
  deriving Repr, BEq
structure WireReturn where
  sync : Sync
  phase : Option Phase := none
  outgoingDelay : Nat := 0
  response : Option Payload := none
  deriving Repr, BEq
inductive HopState where
  | request | requestReleased | response | terminal
  deriving Repr, BEq
structure Hop where
  connection : Nat
  transaction : Nat
  state : HopState
  response : Option Payload := none
  request : Option Payload := none
  releasedRecorded : Bool := false
  responseRecorded : Bool := false
  terminalRecorded : Bool := false
  deriving Repr, BEq
inductive TimedKind where
  | input (phase : Phase) | requestReleased | responseReady | terminal
  deriving Repr, BEq
structure TimedEvent where
  time : Nat
  sequence : Nat
  connection : Nat
  transaction : Nat
  kind : TimedKind
  snapshot : Payload
  turn : Nat := 0
  deriving Repr, BEq
inductive TraceRecord where
  | call (call : WireCall) | reply (id : Nat) (reply : WireReturn) | timed (event : TimedEvent)
  deriving Repr, BEq
structure Config where
  maxOutstanding : Nat
  maxPayloadBytes : Nat
  maxByteEnableBytes : Nat
  eventCapacity : Nat
  maxEventsPerTick : Nat
  deriving Repr
structure SendIntent where
  id : Nat
  connection : Nat
  transaction : Nat
  flow : Flow
  phase : Phase
  notBefore : Nat
  incomingDelay : Nat
  payload : Payload
  deriving Repr, BEq
structure State where
  hops : List Hop := []
  intents : List SendIntent := []
  pending : List WireCall := []
  events : List TimedEvent := []
  trace : List TraceRecord := []
  nextSequence : Nat := 0
  reservedEvents : Nat := 0
  lastCallTime : Nat := 0
  lastEventTime : Option Nat := none
  lastEventTurn : Nat := 0
  eventsAtTick : Nat := 0
  deriving Repr, BEq
private def checkedTime (a b : Nat) : Except String Nat :=
  if a+b < 2^64 then .ok (a+b) else .error "TimeOverflow"
private def checkedReadyTime (s : State) (a b : Nat) : Except String Nat := do
  let time ← checkedTime a b
  if s.lastEventTime.any (fun previous => time < previous) then throw "TimeRegression"
  if s.lastEventTime == some time && s.lastEventTurn+1 ≥ 2^64 then throw "TurnExhausted"
  pure time
private def putHop (s : State) (hop : Hop) : State :=
  {s with hops := hop :: s.hops.filter (fun h => !(h.connection == hop.connection && h.transaction == hop.transaction))}
private def findHop (s : State) (c : WireCall) : Except String Hop :=
  match s.hops.find? (fun h => h.connection == c.connection && h.transaction == c.transaction) with
  | some h => .ok h | none => .error "UnknownHop"
private def laneFree (s : State) (connection transaction : Nat) (lane : HopState) : Bool :=
  !(s.hops.any (fun h => h.connection == connection && h.transaction != transaction && h.state == lane))
/-- This reference owns one instance. Input deliveries use the connection key;
    protocol milestones use Internal stage and the absent-connection key zero. -/
def TimedEvent.stage (e : TimedEvent) : Nat := match e.kind with | .input _ => 2 | _ => 3
def TimedEvent.keyConnection (e : TimedEvent) : Nat := match e.kind with | .input _ => e.connection | _ => 0
private def eventBefore (a b : TimedEvent) : Bool :=
  a.time < b.time || (a.time == b.time &&
    (a.turn < b.turn || (a.turn == b.turn &&
      (a.stage < b.stage || (a.stage == b.stage &&
        (a.keyConnection < b.keyConnection || (a.keyConnection == b.keyConnection && a.sequence < b.sequence)))))))
private def insertEvent (e : TimedEvent) : List TimedEvent → List TimedEvent
  | [] => [e]
  | head::tail => if eventBefore e head then e::head::tail else head::insertEvent e tail
private def enqueue (s : State) (c : WireCall) (time : Nat) (kind : TimedKind) (snapshot : Payload) : State :=
  let turn := if s.lastEventTime == some time then s.lastEventTurn+1 else 0
  let e := TimedEvent.mk time s.nextSequence c.connection c.transaction kind snapshot turn
  {s with events := insertEvent e s.events, nextSequence := s.nextSequence+1}
private def enqueueMilestone (s : State) (c : WireCall) (time : Nat) (kind : TimedKind) (snapshot : Payload) : State :=
  match findHop s c with
  | .error _ => s
  | .ok hop =>
    let recorded := match kind with
      | .requestReleased => hop.releasedRecorded
      | .responseReady => hop.responseRecorded
      | .terminal => hop.terminalRecorded
      | .input _ => false
    if recorded then s else
      let hop := match kind with
        | .requestReleased => {hop with releasedRecorded := true}
        | .responseReady => {hop with responseRecorded := true}
        | .terminal => {hop with terminalRecorded := true}
        | .input _ => hop
      enqueue (putHop s hop) c time kind snapshot
private def validPayload (config : Config) (payload : Payload) : Bool :=
  payload.data.length ≤ config.maxPayloadBytes && payload.byteEnable.length ≤ config.maxByteEnableBytes &&
  payload.length.toNat == payload.data.length && payload.streamingWidth.toNat > 0 &&
  payload.streamingWidth.toNat ≤ payload.length.toNat

private def validResponse (config : Config) (hop : Hop) (response : Payload) : Bool :=
  validPayload config response && hop.request.any (fun request =>
    request.command != 0 || response.status != 1 || response.data.length == request.length.toNat)
/-- Call-time wire transition and owned input snapshot; no future business is run here. -/
def startCall (config : Config) (s : State) (c : WireCall) : Except String State := do
  if config.maxOutstanding == 0 || config.eventCapacity == 0 || config.maxEventsPerTick == 0 then throw "InvalidRuntimeBudget"
  if c.callTime < s.lastCallTime then throw "EnvironmentViolation: call order"
  if c.id ≥ 2^64 || c.connection ≥ 2^32 || c.transaction ≥ 2^64 then throw "IdentityOutOfRange"
  if s.nextSequence + 4 ≥ 2^64 then throw "SequenceExhausted"
  if s.trace.any (fun r => match r with | .call old => old.id == c.id | _ => false) then throw "DuplicateCallId"
  if s.pending.any (fun old => old.connection == c.connection) then throw "NestedTransportCall"
  if s.events.length + s.reservedEvents + 4 > config.eventCapacity then throw "EventCapacity"
  if (c.phase == .beginReq || c.phase == .beginResp) && !validPayload config c.payload then throw "InvalidPayloadSnapshot"
  if (c.phase == .beginReq || c.phase == .endResp) != (c.flow == .forward) then throw "IllegalCallFlow"
  let time ← checkedReadyTime s c.callTime c.incomingDelay
  let next ← match c.phase with
  | .beginReq => do
    if s.hops.any (fun h => h.connection == c.connection && h.transaction == c.transaction) then throw "DuplicateTransactionIdentity"
    if !laneFree s c.connection c.transaction .request then throw "RequestLaneBusy"
    if (s.hops.filter (fun h => h.connection == c.connection && h.state != .terminal)).length ≥ config.maxOutstanding then throw "AdmissionCapacity"
    pure (putHop s {connection := c.connection, transaction := c.transaction, state := .request, request := some c.payload})
  | .endReq => do
    let h ← findHop s c
    if h.state != .request then throw "IllegalEndReq"
    pure (putHop s {h with state := .requestReleased})
  | .beginResp => do
    let h ← findHop s c
    if h.state != .request && h.state != .requestReleased then throw "IllegalBeginResp"
    if !validResponse config h c.payload then throw "InvalidResponseSnapshot"
    if !laneFree s c.connection c.transaction .response then throw "ResponseLaneBusy"
    pure (putHop s {h with state := .response, response := some c.payload})
  | .endResp => do
    let h ← findHop s c
    if h.state != .response then throw "IllegalEndResp"
    pure (putHop s {h with state := .terminal})
  let next := enqueueMilestone next c time (.input c.phase) c.payload
  pure {next with pending := next.pending ++ [c], reservedEvents := next.reservedEvents+3, trace := next.trace ++ [.call c], lastCallTime := c.callTime}

/-- A return applies once to its call ticket. ACCEPTED ignores all rewritten return parameters. -/
def recordReturn (config : Config) (s : State) (id : Nat) (reply : WireReturn) : Except String State := do
  let some c := s.pending.find? (fun c => c.id == id) | throw "UnknownOrConsumedCallTicket"
  let hop ← findHop s c
  let mut next := {s with pending := s.pending.filter (fun c => c.id != id), reservedEvents := s.reservedEvents-3, trace := s.trace ++ [.reply id reply]}
  if reply.sync == .accepted then
    let time ← checkedReadyTime s c.callTime c.incomingDelay
    match c.phase with
    | .beginReq => pure ()
    | .endReq => next := enqueueMilestone next c time .requestReleased c.payload
    | .beginResp =>
      next := enqueueMilestone next c time .requestReleased c.payload
      next := enqueueMilestone next c time .responseReady c.payload
    | .endResp => next := enqueueMilestone next c time .terminal (hop.response.getD c.payload)
    return next
  if c.phase == .endReq then throw "IllegalEndReqReturn"
  if c.phase == .endResp && reply.sync == .updated then throw "IllegalEndRespReturn"
  if reply.outgoingDelay < c.incomingDelay then throw "ReturnDelayBeforeInput"
  let time ← checkedReadyTime s c.callTime reply.outgoingDelay
  if reply.sync == .updated then
    match c.phase, reply.phase with
    | .beginReq, some .endReq =>
      next := putHop next {hop with state := .requestReleased}
      next := enqueueMilestone next c time .requestReleased c.payload
    | .beginReq, some .beginResp =>
      let some snapshot := reply.response | throw "MissingResponseSnapshot"
      if !validResponse config hop snapshot then throw "InvalidResponseSnapshot"
      if !laneFree next c.connection c.transaction .response then throw "ResponseLaneBusy"
      next := putHop next {hop with state := .response, response := some snapshot}
      next := enqueueMilestone next c time .requestReleased snapshot
      next := enqueueMilestone next c time .responseReady snapshot
    | .beginResp, some .endResp =>
      let inputTime ← checkedReadyTime s c.callTime c.incomingDelay
      next := putHop next {hop with state := .terminal}
      next := enqueueMilestone next c inputTime .requestReleased c.payload
      next := enqueueMilestone next c inputTime .responseReady c.payload
      next := enqueueMilestone next c time .terminal c.payload
    | _, _ => throw "IllegalUpdatedPhase"
  else
    let snapshot ← match c.phase with
      | .beginReq => match reply.response with | some snapshot => pure snapshot | none => throw "MissingResponseSnapshot"
      | .beginResp => pure c.payload
      | .endResp => pure (hop.response.getD c.payload)
      | .endReq => throw "IllegalEndReqReturn"
    if (c.phase == .beginReq || c.phase == .beginResp) && !validResponse config hop snapshot then throw "InvalidResponseSnapshot"
    next := putHop next {hop with state := .terminal, response := some snapshot}
    let observationTime ← if c.phase == .beginResp then checkedReadyTime s c.callTime c.incomingDelay else pure time
    if c.phase == .beginReq || c.phase == .beginResp then next := enqueueMilestone next c observationTime .requestReleased snapshot
    if c.phase != .endResp then next := enqueueMilestone next c observationTime .responseReady snapshot
    next := enqueueMilestone next c time .terminal snapshot
  if next.events.length + next.reservedEvents > config.eventCapacity then throw "InternalReservationMismatch"
  pure next

def queueIntent (config : Config) (s : State) (intent : SendIntent) : Except String State := do
  if s.intents.length ≥ config.maxOutstanding then throw "IntentCapacity"
  if s.intents.any (fun i => i.id == intent.id) then throw "DuplicateIntentId"
  pure {s with intents := s.intents ++ [intent]}
def startOutbound (config : Config) (s : State) (call : WireCall) : Except String State := do
  let some intent := s.intents.head? | throw "EnvironmentViolation: unexpected outbound call"
  if intent.id != call.id || intent.connection != call.connection || intent.transaction != call.transaction ||
      intent.flow != call.flow || intent.phase != call.phase || intent.incomingDelay != call.incomingDelay ||
      intent.payload != call.payload || call.callTime < intent.notBefore then
    throw "EnvironmentViolation: outbound intent mismatch"
  startCall config {s with intents := s.intents.drop 1} call
inductive TranscriptRecord where
  | call (call : WireCall) | outbound (call : WireCall) | reply (callId : Nat) (reply : WireReturn)
  deriving Repr, BEq
structure RunResult where
  state : State
  remaining : List TranscriptRecord
  reason : ModelIR.StopReason
  deriving Repr, BEq
private def finished (s : State) : ModelIR.StopReason :=
  if s.pending.isEmpty && s.intents.isEmpty && s.hops.all (fun h => h.state == .terminal) then .quiescent else .waitingForEnvironment
private def consumeEvent (config : Config) (s : State) (event : TimedEvent) (rest : List TimedEvent) : Except String State := do
  let count := if s.lastEventTime == some event.time then s.eventsAtTick else 0
  if count ≥ config.maxEventsPerTick then throw "ZenoDetected"
  pure {s with events := rest, trace := s.trace ++ [.timed event], lastEventTime := some event.time, lastEventTurn := event.turn, eventsAtTick := count+1}
/-- Host prefix budget is per call, return, or timed observation. Continuations retain reservations and counters. -/
private def runLoop (config : Config) (horizon : Option Nat) : Nat → State → List TranscriptRecord → RunResult
  | fuel, s, records =>
    if records.isEmpty && s.events.isEmpty then ⟨s,[],finished s⟩ else
    match fuel with
    | 0 => ⟨s,records,.runBudgetReached⟩
    | fuel+1 =>
      match records with
      | .reply id reply :: rest =>
        match recordReturn config s id reply with
        | .error error => ⟨{s with trace := s.trace ++ [.reply id reply]},records,.failed error⟩
        | .ok next => runLoop config horizon fuel next rest
      | _ =>
        let callTime := match records with | .call c :: _ | .outbound c :: _ => some c.callTime | _ => none
        let processEvent := s.events.head?.any (fun e => callTime.all (fun t => e.time < t))
        if processEvent then
          match s.events with
          | [] => ⟨s,records,.failed "InternalEventSelection"⟩
          | e::es =>
            if horizon.any (fun h => e.time > h) then ⟨s,records,.horizonReached⟩ else
            match consumeEvent config s e es with
            | .error error => ⟨s,records,.failed error⟩
            | .ok next => runLoop config horizon fuel next records
        else
          match records with
          | .outbound c :: rest =>
            if horizon.any (fun h => c.callTime > h) then ⟨s,records,.horizonReached⟩ else
            match startOutbound config s c with
            | .error error => ⟨{s with trace := s.trace ++ [.call c]},records,.failed error⟩
            | .ok next => runLoop config horizon fuel next rest
          | .call c :: rest =>
            if horizon.any (fun h => c.callTime > h) then ⟨s,records,.horizonReached⟩ else
            match startCall config s c with
            | .error error => ⟨{s with trace := s.trace ++ [.call c]},records,.failed error⟩
            | .ok next => runLoop config horizon fuel next rest
          | _ => ⟨s,records,.failed "InvalidTranscriptRecord"⟩
/-- An explicitly zero budget stops immediately; a positive run may naturally finish on its last step. -/
def runFrom (config : Config) (horizon : Option Nat) (fuel : Nat) (state : State) (records : List TranscriptRecord) : RunResult :=
  if fuel == 0 then ⟨state,records,.runBudgetReached⟩ else runLoop config horizon fuel state records
end LeanAT.ModelIR.Transport








