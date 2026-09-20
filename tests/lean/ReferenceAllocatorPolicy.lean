import LeanAT.Reference.Json

open LeanAT LeanAT.Reference LeanAT.Reference.JsonIO

private def check (ok : Bool) (message : String) : IO Unit :=
  unless ok do throw (IO.userError message)

private def accept (state : State) (message : String) : IO Unit := do
  check ((validateState state).toOption.isSome) message
  check ((stateOfJson (stateJson state)).toOption == some state) (message ++ " JSON")

private def reject (state : State) (message : String) : IO Unit := do
  check ((validateState state).toOption.isNone) message
  check ((stateOfJson (stateJson state)).toOption.isNone) (message ++ " JSON")

private def entry (kind : HandleKind) (store slot generation : Nat) (alive : Bool := true) : ObjectEntry :=
  ⟨⟨kind,1,UInt32.ofNat store,UInt32.ofNat slot,UInt64.ofNat generation,7⟩,"fixture",.unit,alive⟩

def main : IO Unit := do
  accept {} "empty journal"
  let event : AllocationRule := ⟨.event,1,"runtime.event",true,true,true,some 2⟩
  for changed in [
      {event with group := "forged"},
      {event with perSlot := false},
      {event with persistent := false},
      {event with allowMax := false}] do
    reject {
      allocationRules := [changed] } "protected rule override"
  let aliasedEvent : AllocationRule := ⟨.lease,999,"runtime.event",true,true,true,some 2⟩
  reject {
    allocationRules := [aliasedEvent] } "protected group alias"
  reject {
    allocationRules := [⟨.task,131,"structured.task",false,true,true,none⟩] }
    "structured exhaustion policy override"
  reject {
    allocationRules := [⟨.wait,11,"other.process",false,true,false,none⟩] }
    "process and wait share canonical counter"
  reject {
    allocationCounters := [{
      group := "orphan"
      domain := 1 }] } "unknown counter"
  reject {
    allocationCounters := [{
      group := "runtime.event"
      domain := 1 }] } "implicit per-slot policy"
  reject {
    allocationCounters := [{
      group := "runtime.process"
      domain := 1
      persistent := false }] }
    "implicit persistence policy"
  reject {
    allocationCounters := [{
      group := "runtime.process"
      domain := 1
      nextGeneration := 2^64 }] }
    "process cannot issue MAX"
  accept {
    allocationCounters := [{
      group := "runtime.process"
      domain := 1
      nextGeneration := 2^64-1 }] }
    "process exhausted next value"
  reject {
    allocationCounters := [{
      group := "managed.lease"
      domain := 1
      slot := some 0
      nextGeneration := 2^64
      retired := true }] } "managed exhaustion is not event retirement"
  accept {
    allocationCounters := [{
      group := "managed.lease"
      domain := 1
      slot := some 0
      nextGeneration := 2^64 }] } "managed exhausted counter"
  accept {
    allocationCounters := [{
      group := "runtime.event"
      domain := 1
      slot := some 0
      nextGeneration := 2^64
      retired := true }] } "retired event without retained object"
  reject {
    generationLimit := 20 } "custom generation limit rejected"
  reject {
    allocationRules := [event]
    allocationCounters := [{
      group := "runtime.event"
      domain := 1
      slot := some 2 }] } "counter outside explicit capacity"
  reject {
    maxEvents := 1
    allocationCounters := [{
      group := "runtime.event"
      domain := 1
      slot := some 1 }] } "counter outside implicit capacity"
  reject {
    objects := [entry .result 20 128 1]
    nextGeneration := 2 } "object outside default pool"
  reject {
    allocationRules := [event]
    objects := [entry .event 1 2 1 false]
    nextGeneration := 2 }
    "tombstone outside configured pool"
  accept {
    allocationRules := [event]
    objects := [entry .event 1 1 8 false]
    nextGeneration := 9 }
    "sparse imported slot"
  reject {
    objects := [entry .result 20 0 9]
    nextGeneration := 10
    allocationCounters := [{
      group := "storage.result"
      domain := 1
      nextGeneration := 9 }] }
    "counter cannot rewind imported identity"
  reject {
    objects := [entry .process 10 0 (2^64-1)]
    nextGeneration := 2^64 }
    "process MAX identity rejected"
  accept {
    objects := [entry .result 20 0 (2^64-1)]
    nextGeneration := 2^64
    allocationCounters := [{
      group := "storage.result"
      domain := 1
      nextGeneration := 2^64 }] }
    "result MAX identity allowed"
  let context : Context := {
    domain := 1
    owner := 7 }
  let transient : AllocationRule := ⟨.hop,77,"fixture.transient",false,false,true,some 8⟩
  let before : State := {
    allocationRules := [transient] }
  let .ok (_,draft) := allocate before context .hop 77 "transient" .unit
    | throw (IO.userError "transient setup")
  check (rollbackAllocations before draft == before) "nonpersistent rollback restores high-water and journal"
  let .ok (_,persistentDraft) := allocate {} context .hop 88 "persistent" .unit
    | throw (IO.userError "persistent setup")
  let rolled := rollbackAllocations {} persistentDraft
  check (rollbackAllocations persistentDraft persistentDraft == persistentDraft)
    "no-op with implicit object policy does not materialize a rule"
  accept rolled "failed first default allocation retains interpretable policy"
  check (rolled.objects.isEmpty && rolled.nextGeneration == 2 && rolled.allocationCounters.length == 1)
    "persistent burn survives without object"
  let .ok (next,_) := allocate rolled context .hop 88 "persistent" .unit
    | throw (IO.userError "persistent retry")
  check (next.generation == 2) "retry cannot reuse provisional generation"
  let unused : AllocationRule := ⟨.hop,89,"fixture.unused",false,true,true,none⟩
  check (rollbackAllocations {} {
    allocationRules := [unused] } == {}) "unused staged policy rolls back"
  check (rollbackAllocations before before == before) "no-op does not burn"
  let seeded : State := {
    allocationCounters := [{
      group := "runtime.process"
      domain := 1
      nextGeneration := 2^64-1 }] }
  check (rollbackAllocations seeded seeded == seeded) "no-op preserves independently seeded global diagnostic"
  IO.println "ReferenceAllocatorPolicy: canonical policies, bounded imports, exhaustion and rollback PASS"
