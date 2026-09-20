import LeanAT.Reference.Managed
open LeanAT LeanAT.Reference
namespace ReferenceManagedTest
private def n := Value.bits 64
private def types : TypeEnvironment := [.bits 64,.handle .lease,.handle .access,.bytes 8,.variant [[0],[1]],.variant [[0],[2]],.unit,.record [0,0,3],.bool,.record [8,0,0,0,0,0,0,0],.variant [[0],[0]]]
private def sig (op : ExecIR.Op) (ins : List Nat) (out : Nat) : ExecIR.ServiceSignature := Id.run do
  let raw := [ExecIR.Op.grantRawDmi,.denyDmi,.invalidateRawDmi].contains op
  let s : ExecIR.ServiceSignature := ⟨op.tag,op,ins,[out],if op == .grantRawDmi || op == .denyDmi then 16 else 3,if raw then 1024 else if op == .resultGet || op == .resultRelease then 64 else 256,0,(if raw then "leanat.raw-dmi." else "leanat.managed.") ++ toString op.tag,"1",ByteArray.empty⟩
  return {s with abiHash := Managed.managedHash types s (if raw then "raw-dmi-v1" else "managed-v1")}
private def c : Context := {domain := 1,owner := 9,environment := [("profile.valueNodeBytes",n 40),("managed.enabled",.bool true),("managed.invalidateOwner",n 9),("raw.enabled",.bool true),("raw.invalidateOwner",n 9)]}
private def expect (b : Bool) (message : String) : Except String Unit := if b then .ok () else .error message
private def getHandle : List Value → Except String HandleIdentity
  | [.variant 1 [.handle h]] => .ok h | _ => .error "admission shape"
private def deliverNext (s : State) : Except String State := do
  let some event := (← Managed.scheduled c s).head? | throw "event missing"
  Managed.deliver types {c with now := event.time,turn := event.turn} s event
private def tests : Except String Unit := do
  let (_,initial) ← Managed.installRegion c {} ⟨1,0,3,1,2,0,7,true,[0,0,0,0]⟩
  let (values,s) ← Managed.invoke types c (sig .requestManaged [0,0,0,0] 4) [n 1,n 0,n 3,n 3] initial
  let lease ← getHandle values
  let noRuntimeEvents := {s with maxEvents := 0}
  let (zeroQueueValues,zeroQueueState) ← Managed.invoke types c (sig .beginManagedRead [1,0,0,0] 5) [.handle lease,n 0,n 1,n 0] noRuntimeEvents
  let _ ← getHandle zeroQueueValues
  expect (zeroQueueState.events.isEmpty && (← Managed.scheduled c zeroQueueState).length == 1) "managed scheduling independent of EventQueue capacity"
  expect (!(zeroQueueState.allocationCounters.any (fun counter => counter.group == "runtime.event"))) "managed work burns no Runtime event generations"
  let rolledWork := rollbackAllocations noRuntimeEvents zeroQueueState
  expect ((← Managed.scheduled c rolledWork).isEmpty) "discard restores private scheduler queue"
  let (retriedWork,retriedState) ← Managed.invoke types c (sig .beginManagedRead [1,0,0,0] 5) [.handle lease,n 0,n 1,n 0] rolledWork
  let retriedHandle ← getHandle retriedWork
  let discardedHandle ← getHandle zeroQueueValues
  expect (retriedHandle.generation == discardedHandle.generation+1 && (← Managed.scheduled c retriedState).head?.any (fun event => event.sequence == 1)) "discard burns access identity but restores private sequence"
  let (values,s) ← Managed.invoke types c (sig .beginManagedWrite [1,0,3,0] 5) [.handle lease,n 1,.bytes [7,8],n 0] s
  let access ← getHandle values
  expect ((Managed.invoke types c (sig .resultGet [2] 7) [.handle access] s).toOption.isNone) "premature result"
  expect ((← Managed.scheduled c s).length == 1) "admission event"
  let s ← deliverNext s
  expect ((← Managed.scheduled c s).length == 1 && (← Managed.scheduled c s).head?.any (fun e => e.time == 2)) "checked service finish"
  let s ← deliverNext s
  let (result,s) ← Managed.invoke types {c with now := 2} (sig .resultGet [2] 7) [.handle access] s
  expect (result == [.record [n 0,n 2,.bytes []]]) "write completion"
  let (read,s) ← Managed.invoke types {c with now := 2} (sig .beginManagedRead [1,0,0,0] 5) [.handle lease,n 1,n 2,n 2] s
  let read ← getHandle read
  let s ← deliverNext s
  let s ← deliverNext s
  let (result,s) ← Managed.invoke types {c with now := 4} (sig .resultGet [2] 7) [.handle read] s
  expect (result == [.record [n 0,n 1,.bytes [7,8]]]) "read observes backing"
  let (_,s) ← Managed.invoke types {c with now := 4} (sig .resultRelease [2] 6) [.handle read] s
  let (_,s) ← Managed.invoke types {c with now := 4} (sig .releaseLease [1] 6) [.handle lease] s
  expect ((Managed.invoke types c (sig .beginManagedRead [1,0,0,0] 5) [.handle lease,n 0,n 1,n 4] s).toOption.isNone) "released generation"
  let previous ← observe initial ⟨"previous-write","","",0,0,[n 77]⟩
  let full := {c with environment := c.environment ++ [("managed.limits",.record [n 0,n 128,n 65536])]}
  let (denial,preserved) ← Managed.invoke types full (sig .requestManaged [0,0,0,0] 4) [n 1,n 0,n 3,n 3] previous
  expect (denial == [.variant 0 [n 2]] && preserved == previous) "capacity denial preserves prior effects"
  let .ok discarded draft := (Managed.requestAttempt c 1 0 3 3).run previous | throw "request attempt"
  let discarded ← getHandle discarded
  let rolledBack := rollbackAllocations previous draft
  expect (rolledBack.objects == previous.objects) "attempt rollback restores semantic objects"
  let (retried,_) ← Managed.request c rolledBack 1 0 3 3
  let retried ← getHandle retried
  expect (retried.slot == discarded.slot && retried.generation == discarded.generation+1) "lease allocator burn survives discard"
  let bounded := {c with environment := c.environment ++ [("managed.limits",.record [n 128,n 128,n 2])]}
  let (leases,leased) ← Managed.request bounded previous 1 0 3 3
  let boundedLease ← getHandle leases
  let (_,pending) ← Managed.invoke types bounded (sig .beginManagedWrite [1,0,3,0] 5) [.handle boundedLease,n 0,.bytes [7,8],n 0] leased
  let (denial,preserved) ← Managed.invoke types bounded (sig .beginManagedWrite [1,0,3,0] 5) [.handle boundedLease,n 2,.bytes [9],n 0] pending
  expect (denial == [.variant 0 [n 2]] && preserved == pending) "pending input aggregate capacity preserves prior access"
  let (bad,same) ← Managed.request c initial 1 3 2 3
  expect (bad == [.variant 0 [n 0]] && same == initial) "range rejection atomic"
  let (_,invalidating) ← Managed.invoke types c (sig .invalidateManaged [0,0,0,0] 6) [n 1,n 0,n 3,n 0] initial
  let invalidated ← deliverNext invalidating
  expect ((← Managed.scheduled c invalidated).isEmpty) "invalidation delivered"
  expect ((Managed.invoke types {c with owner := 10} (sig .invalidateManaged [0,0,0,0] 6) [n 1,n 0,n 3,n 0] initial).toOption.isNone) "invalidation authority"
  let (_,raw) ← Managed.installRawRegion c {} 3 100 103 3 2 4 1 (some [0,0,0,0])
  expect ((Managed.invoke types c (sig .invalidateRawDmi [0,0,0] 6) [n 99,n 100,n 103] raw).toOption.isNone) "raw invalidation requires existing region"
  let mut fullRaw := raw
  for _ in List.range 128 do
    let (value,next) ← Managed.invoke types {c with kind := 4} (sig .grantRawDmi [0,0,0] 9) [n 3,n 100,n 0] fullRaw
    expect (value == [.record [.bool true,n 3,n 100,n 103,n 3,n 2,n 4,n 1]]) "raw bounded grant admitted"
    fullRaw := next
  let (denied,unchanged) ← Managed.invoke types {c with kind := 4} (sig .grantRawDmi [0,0,0] 9) [n 3,n 100,n 0] fullRaw
  expect (denied == [.record [.bool false,n 0,n 0,n 0,n 0,n 0,n 0,n 0]] && unchanged == fullRaw) "raw 129th grant denied without state change"
  let (grant,raw) ← Managed.invoke types {c with kind := 4} (sig .grantRawDmi [0,0,0] 9) [n 3,n 101,n 0] raw
  expect (grant == [.record [.bool true,n 3,n 100,n 103,n 3,n 2,n 4,n 1]]) "raw exact grant"
  let (_,raw) ← Managed.invoke types c (sig .invalidateRawDmi [0,0,0] 6) [n 3,n 101,n 101] raw
  expect (raw.observations.map Observation.kind == ["raw.grant","raw.invalidate"]) "raw callbacks"
  let (deny,_) ← Managed.invoke types {c with kind := 4} (sig .denyDmi [] 9) [] raw
  expect (deny == [.record [.bool false,n 0,n 0,n 0,n 0,n 0,n 0,n 0]]) "raw explicit denial"
  expect ((Managed.invoke types c (sig .grantRawDmi [0,0,0] 9) [n 3,n 100,n 0] raw).toOption.isNone) "raw context"
  let reference := Value.variant 2 [n 3,.variant 0 [],.variant 1 [n 1]]
  let pre := Value.variant 1 [.bool true]
  let ext : ExecIR.ServiceSignature := ⟨99,.callExternPure,[0],[10],3,512,0,"leanat.external.add1","ref|reference|contract",ByteArray.empty⟩
  let semantics := "external-v1:1:1:add1:" ++ ext.providerVersion
  let ext := {ext with abiHash := Managed.managedHash types ext semantics}
  let env := [("external.enabled",Value.bool true),(ext.providerKey,.record [.bytes ext.providerVersion.toUTF8.data.toList,.bytes semantics.toUTF8.data.toList,reference,pre,.bool true,n 6])]
  let (out,unchanged) ← Managed.invoke types {c with environment := env} ext [n 5] initial
  expect (out == [.variant 1 [n 6]] && unchanged == initial) "independent external program"
  expect ((Managed.invoke types {c with environment := env} ext [n 6] initial).toOption.isNone) "native mismatch"
  expect ((Managed.invoke types c {sig .requestManaged [0,0,0,0] 4 with providerVersion := "bad"} [n 1,n 0,n 1,n 1] initial).toOption.isNone) "ABI identity"
def main : IO Unit := match tests with | .ok () => IO.println "ReferenceManaged: passed" | .error e => throw (IO.userError e)
end ReferenceManagedTest

def main := ReferenceManagedTest.main

