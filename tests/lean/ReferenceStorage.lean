import LeanAT.Reference.Storage
open LeanAT LeanAT.Reference LeanAT.Reference.Storage LeanAT.ExecIR
set_option maxRecDepth 4096
namespace ReferenceStorageTests

def types : TypeEnvironment := [.unit,.bool,.bits 64,.handle .transaction,.bytes 4,.handle .result,.handle .consumer,.handle .event,.variant [[],[4]]]
def context : Context := {kind := 0,now := 5,turn := 2,domain := 1,instanceId := 3,owner := 9,environment := [("profile.valueNodeBytes",.bits 64 40)]}
def transaction : HandleIdentity := ⟨.transaction,1,40,0,1,9⟩
def hop : HandleIdentity := ⟨.hop,1,41,0,1,9⟩
def initialPayload : PayloadRecord := {transaction,hop,instanceId := 3,localSide := 7,target := true,writable := true,command := 0,address := 4096,streamingWidth := 4,baseline := [1,2,3,4],data := [1,2,3,4],byteEnable := [255,0],extensions := [{name := "trace",maxBytes := 4}]}
def access : PayloadAccess := {hop,localSide := 7,responseWritePermit := true}

def coreSig (op : Op) (inputs outputs : List Nat) : Except String ServiceSignature := do
  let (key,hash) ← coreIdentity op
  pure {id := op.tag,op,inputTypes := inputs,resultTypes := outputs,contextMask := 3,effectMask := if op == .resultGet || op == .resultRelease then 64 else 8,extraFuel := 0,providerKey := key,providerVersion := "1",abiHash := hash}
private def must {α : Type} : Except String α → IO α
  | .ok value => pure value
  | .error error => throw (IO.userError error)
private def check (b : Bool) (message : String) : IO Unit := unless b do throw (IO.userError message)
private def rejected {α : Type} (r : Except String α) : Bool := r.toOption.isNone
-- This is the same all-or-nothing boundary used by a reference segment caller.
def atomic (state : State) (operation : State → Except String (List Value × State)) : State :=
  match operation state with | .ok (_,next) => next | .error _ => state

-- Public deterministic fixtures for code-generated cross-backend provider programs.
def payloadCases : Except String (Context × State × List (ServiceSignature × List Value)) := do
  let (view,state) ← createPayload context {} initialPayload
  let ctx := bindPayload context view access
  let getter ← payloadSignature types 0 "leanat.payload.data.get" 3 4 0
  let writer ← payloadSignature types 1 "leanat.payload.data.write" 3 4 0
  let extGet ← payloadSignature types 2 "leanat.extension.trace.get" 3 8 0
  let extWrite ← payloadSignature types 3 "leanat.extension.trace.write" 3 4 0
  pure (ctx,state,[(getter,[.handle transaction]),(writer,[.handle transaction,.bytes [9,9,9,9]]),(extGet,[.handle transaction]),(extWrite,[.handle transaction,.bytes [8,7]])])

end ReferenceStorageTests
open ReferenceStorageTests

def main : IO Unit := do
  check ((createPayload context {} {initialPayload with maxBytes := 6}).toOption.isSome) "absent extension incorrectly charged key bytes"
  check (rejected (createPayload context {} {initialPayload with streamingWidth := 0})) "zero streaming width accepted"
  check (rejected (createPayload context {} {initialPayload with extensions := [{name := "trace",maxBytes := 4,requestAllowed := false,value := some [1]}]})) "initial extension wrong phase accepted"
  let (ctx,initial,cases) ← must payloadCases
  let [(getter,getArgs),(writer,writeArgs),(extGet,extGetArgs),(extWrite,extWriteArgs)] := cases | throw (IO.userError "payload cases")
  let (read,unchanged) ← must (Storage.invoke types ctx getter getArgs initial)
  check (read == [.bytes [1,2,3,4]] && unchanged == initial) "payload read changed full state"
  let (out,written) ← must (Storage.invoke types ctx writer writeArgs initial)
  let some view := initial.objects.head? | throw (IO.userError "view")
  let expectedValue := {initialPayload with data := [9,2,9,4]}.encode
  let expectedState := {initial with objects := [{view with value := expectedValue}]}
  check (out == [.unit] && written == expectedState) "masked write complete state mismatch"
  let (read,unchanged) ← must (Storage.invoke types ctx getter getArgs written)
  check (read == [.bytes [9,2,9,4]] && unchanged == written) "own-segment read"
  check (atomic initial (fun s => do
    let (_,next) ← Storage.invoke types ctx writer writeArgs s
    Storage.invoke types {ctx with instanceId := 4} getter getArgs next) == initial) "later failure leaked payload write"
  for bad in [{ctx with owner := 8},{ctx with domain := 2},{ctx with instanceId := 4},{ctx with connection := 1},{ctx with kind := 3},bindPayload context view.identity {access with localSide := 8},bindPayload context view.identity {access with phase := 2,forward := true},bindPayload context view.identity {access with validated := false}] do
    check (rejected (Storage.invoke types bad getter getArgs initial)) "payload authority accepted"
    check (atomic initial (Storage.invoke types bad writer writeArgs) == initial) "negative full state changed"
  check (rejected (Storage.invoke types ctx getter [.handle {transaction with generation := 2}] initial)) "wrong transaction accepted"
  check (rejected (Storage.invoke types ctx writer [.handle transaction,.bytes [1]] initial)) "short READ accepted"
  check (rejected (Storage.invoke types ctx writer [.handle transaction,.bytes [1,2,3,4,5]] initial)) "typed bound accepted"
  let (writeView,writeState) ← must (createPayload context {} {initialPayload with command := 1})
  check (rejected (Storage.invoke types (bindPayload context writeView access) writer writeArgs writeState)) "WRITE payload changed"
  check (rejected (Storage.invoke types (bindPayload context view.identity {access with responseWritePermit := false}) writer writeArgs initial)) "response permit ignored"
  let (absent,readState) ← must (Storage.invoke types ctx extGet extGetArgs initial)
  check (absent == [.variant 0 []] && readState == initial) "absent extension"
  let (_,extended) ← must (Storage.invoke types ctx extWrite extWriteArgs initial)
  let expectedExt := {initialPayload with extensions := [{name := "trace",maxBytes := 4,value := some [8,7]}]}
  check (extended == {initial with objects := [{view with value := expectedExt.encode}]}) "extension full state"
  let (present,_) ← must (Storage.invoke types ctx extGet extGetArgs extended)
  check (present == [.variant 1 [.bytes [8,7]]]) "extension own snapshot"
  let widenedTypes := types ++ [.bytes 8,.variant [[],[9]]]
  let widenedGet ← must (payloadSignature widenedTypes 30 "leanat.extension.trace.get" 3 10 0)
  let widenedWrite ← must (payloadSignature widenedTypes 31 "leanat.extension.trace.write" 3 9 0)
  check (rejected (Storage.invoke widenedTypes ctx widenedGet getArgs initial)) "registered extension getter bound mismatch"
  check (rejected (Storage.invoke widenedTypes ctx widenedWrite [.handle transaction,.bytes []] initial)) "registered extension writer bound mismatch"
  let forged := {getter with abiHash := ⟨Array.replicate 32 0⟩}
  for bad in [forged,{getter with providerVersion := "2"},{getter with contextMask := 3},{getter with extraFuel := 0},{getter with effectMask := 0}] do
    check (rejected (Storage.invoke types ctx bad getArgs initial)) "noncanonical payload ABI"
  let unknown ← must (payloadSignature types 4 "leanat.extension.missing.get" 3 8 0)
  check (rejected (Storage.invoke types ctx unknown getArgs initial)) "unregistered extension"
  let constrained := {initial with maxBytes := ownedBytes initial}
  check (atomic constrained (Storage.invoke types ctx extWrite extWriteArgs) == constrained) "extension capacity rollback"
  let status ← must (payloadSignature types 5 "leanat.payload.status.write" 3 2 0)
  check (rejected (Storage.invoke types ctx status [.handle transaction,.bits 64 7] initial)) "status range"
  let (_,statusState) ← must (Storage.invoke types ctx status [.handle transaction,.bits 64 1] initial)
  check (statusState == {initial with objects := [{view with value := {initialPayload with status := 1}.encode}]}) "status write"
  let resultGet ← must (coreSig .resultGet [5,6] [4])
  let resultRelease ← must (coreSig .resultRelease [5,6] [0])
  let (result,consumer,reserved) ← must (reserveResult types context {} transaction 4 64 9)
  check (rejected (Storage.invoke types context resultGet [.handle result,.handle consumer] reserved)) "unpublished result"
  let published ← must (publishResult types context reserved result (.bytes [1,2,3]))
  let (value,same) ← must (Storage.invoke types context resultGet [.handle result,.handle consumer] published)
  check (value == [.bytes [1,2,3]] && same == published) "result get must not consume"
  let metadata ← must (readResult context published result)
  check (metadata.readyTime == 5 && metadata.readyTurn == 2) "original ready key"
  let (second,retained) ← must (retainResult context published result consumer 10)
  let (_,released) ← must (Storage.invoke types context resultRelease [.handle result,.handle consumer] retained)
  let some deadConsumer := released.objects.find? (fun e => e.identity == consumer) | throw (IO.userError "consumer tombstone missing")
  let deadMetadata ← must (ConsumerRecord.decode deadConsumer.value)
  check (!deadConsumer.alive && deadMetadata.result == result && !deadMetadata.dropOnTerminal) "consumer tombstone ownership metadata lost"
  check (rejected (ConsumerRecord.decode (.record [.handle result,.bool true]))) "unsupported deferred-consumer policy accepted"
  check (rejected (Storage.invoke types context resultGet [.handle result,.handle consumer] released)) "released consumer alias alive"
  let (lateValue,_) ← must (Storage.invoke types {context with owner := 10,now := 100} resultGet [.handle result,.handle second] released)
  check (lateValue == [.bytes [1,2,3]]) "independent consumer loses payload"
  check (rejected (Storage.invoke types context resultGet [.handle result,.handle second] released)) "foreign consumer owner"
  let (other,otherConsumer,otherState) ← must (reserveResult types context released transaction 4 64 9)
  check (rejected (Storage.invoke types context resultGet [.handle other,.handle second] otherState)) "cross-result consumer"
  check (other != result && otherConsumer != consumer) "distinct result identities"
  check (rejected (pinResult context {published with maxPins := 0} result consumer)) "zero pin budget"
  let onePin ← must (pinResult context {published with maxPins := 1} result consumer)
  check (rejected (pinResult context onePin result consumer)) "same result pin budget"
  let (pinOther,pinConsumer,pinState) ← must (reserveResult types context onePin transaction 4 64 9)
  let pinPublished ← must (publishResult types context pinState pinOther (.bytes [4]))
  check (rejected (pinResult context pinPublished pinOther pinConsumer)) "global pin budget across results"
  let pinFreed ← must (unpinResult context pinPublished result)
  check ((pinResult context pinFreed pinOther pinConsumer).toOption.isSome) "unpin did not restore global capacity"
  let pinned ← must (pinResult {context with owner := 10} released result second)
  let ownerReleased ← must (releaseProducer context pinned result)
  let noConsumers ← must (releaseResult {context with owner := 10} ownerReleased result second)
  check ((readResult context noConsumers result).toOption.isSome) "pin failed to retain payload"
  let collected ← must (unpinResult context noConsumers result)
  let some deadResult := collected.objects.find? (fun e => e.identity == result) | throw (IO.userError "result tombstone missing")
  let deadResultMetadata ← must (ResultRecord.decode deadResult.value)
  check (!deadResult.alive && !deadResultMetadata.published && deadResultMetadata.value == .unit && deadResultMetadata.source == transaction && deadResultMetadata.producerOwner == 9 && deadResultMetadata.initialConsumerOwner == 9) "result tombstone create metadata lost"
  check (rejected (readResult context collected result)) "last pin failed collection"
  let (reused,fresh,reusedState) ← must (reserveResult types context collected transaction 4 64 9)
  check (reused.generation != result.generation && fresh.generation != consumer.generation && rejected (readResult context reusedState result)) "recycled generation resurrected"
  check (rejected (reserveResult types context {maxObjects := 1} transaction 4 64 9)) "result two-identity capacity"
  let (tinyResult,_,tinyState) ← must (reserveResult types context {} transaction 1 39 9)
  check (rejected (publishResult types context tinyState tinyResult (.bool true))) "native Value40 budget boundary"
  let (exactResult,_,exactState) ← must (reserveResult types context {} transaction 1 40 9)
  check ((publishResult types context exactState exactResult (.bool true)).toOption.isSome) "native Value40 exact budget"
  check (rejected (publishResult types {context with environment := []} exactState exactResult (.bool true))) "missing native value ABI"
  check (rejected (publishResult types context reserved result (.bool true))) "result type mismatch"
  check (rejected (publishResult types context published result (.bytes []))) "duplicate publication"
  let schedule ← must (coreSig .scheduleEvent [2,4] [7])
  let cancel ← must (coreSig .cancelEvent [7] [1])
  let (_,connectedEvents) ← must (Storage.invoke types {context with connection := 23,environment := ("storage.connection",.bits 64 999)::context.environment} schedule [.bits 64 5,.bytes []] {})
  check ((connectedEvents.events.head?).any (fun e => e.connection == 23)) "authoritative nonzero event connection ignored"
  let (scheduled,events) ← must (Storage.invoke types context schedule [.bits 64 5,.bytes [4]] {})
  let [.handle event] := scheduled | throw (IO.userError "scheduled handle")
  let expectedEvent : Event := {identity := event,time := 5,turn := 3,stage := 3,instanceId := 3,connection := 0,sequence := 1,kind := "storage.event",values := [.bytes [4]]}
  check (events.events == [expectedEvent] && events.nextGeneration == 2 && events.nextSequence == 2 && events.observations.isEmpty) "scheduled full event state"
  let (cancelled,cancelState) ← must (Storage.invoke types context cancel [.handle event] events)
  check (cancelled == [.bool true] && cancelState.events == [{expectedEvent with cancelled := true}] && cancelState.objects == events.objects) "cancel complete state"
  let (again,same) ← must (Storage.invoke types context cancel [.handle event] cancelState)
  check (again == [.bool false] && same == cancelState) "repeat cancellation"
  check (rejected (Storage.invoke types context schedule [.bits 64 6,.bytes []] {cancelState with maxEvents := 1})) "cancel freed event capacity before collection"
  let collectedEvents ← must (releaseObject cancelState context event "reference.event")
  let collectedEvents := {collectedEvents with events := []}
  let (_,reusedEvents) ← must (Storage.invoke types context schedule [.bits 64 6,.bytes []] collectedEvents)
  check (rejected (Storage.invoke types context cancel [.handle event] reusedEvents)) "stale event generation"
  for state in [{maxEvents := 0},{nextSequence := 2^64-1},{maxBytes := 1}] do
    check (rejected (Storage.invoke types context schedule [.bits 64 5,.bytes []] state)) "event bound accepted"
    check (atomic state (Storage.invoke types context schedule [.bits 64 5,.bytes []]) == state) "event failure full state rollback"
  check (rejected (Storage.invoke types context schedule [.bits 64 4,.bytes []] {})) "past event"
  check (rejected (Storage.invoke types {context with turn := 2^64-1} schedule [.bits 64 5,.bytes []] {})) "turn overflow"
  check (rejected (Storage.invoke types {context with owner := 8} cancel [.handle event] events)) "foreign event owner"
  let pool ← must (seedResultStore {} context 2 1 1)
  let (_,_,pool) ← must (reserveResult types context pool transaction 4 64 9)
  match (reserveResultAttempt types context transaction 4 64 9).run pool with
  | .ok _ _ => throw (IO.userError "full consumer pool accepted")
  | .error _ failed =>
    check (failed.objects == pool.objects) "failed result reservation changed objects"
    check ((failed.allocationCounters.find? (fun c => c.group == "storage.result")).any (fun c => c.nextGeneration == 3)) "result generation burn lost"
    check ((failed.allocationCounters.find? (fun c => c.group == "storage.consumer")).any (fun c => c.nextGeneration == 2)) "consumer failure burned generation"
  check (rejected (seedResultStore pool context 2 1 1)) "seed allocator rewound imported live records"
  let exhausted : State := {
    maxEvents := 1
    nextGeneration := 2^64
    allocationCounters := [{group := "runtime.event",domain := 1,slot := some 0,nextGeneration := 2^64}]}
  match (Storage.invokeAttempt types context schedule [.bits 64 5,.bytes []]).run exhausted with
  | .ok _ _ => throw (IO.userError "max event generation reused")
  | .error _ failed =>
    check (failed.objects.isEmpty && failed.events.isEmpty && failed.nextSequence == 1) "retirement consumed event sequence"
    check (failed.allocationCounters.any (fun c => c.retired)) "failed scan lost permanent retirement"
  let almost : State := {
    maxEvents := 1
    nextGeneration := 2^64-1
    allocationCounters := [{group := "runtime.event",domain := 1,slot := some 0,nextGeneration := 2^64-1}]}
  let (last,lastState) ← must (Storage.invoke types context schedule [.bits 64 5,.bytes []] almost)
  check (last.any (fun v => match v with | .handle h => h.generation.toNat == 2^64-1 | _ => false)) "last event generation not usable"
  check (lastState.nextSequence == 2) "last event allocation sequence"
  IO.println "ReferenceStorage: eight providers and full-state lifecycle/rollback regressions passed"








