import LeanAT.Reference.Storage
import LeanAT.Reference.ABI
import LeanAT.Pure

namespace LeanAT.Reference.Managed
open ExecIR
private def n := Value.bits 64
private def nat : Value → Except String Nat
  | .bits 64 x => if x < 2^64 then .ok x else .error "ManagedNumberRange"
  | _ => .error "ManagedNumberType"
private def handle : Value → Except String HandleIdentity
  | .handle h => .ok h | _ => .error "ManagedHandleType"
private def environment (c : Context) (key : String) : Except String Value :=
  match c.environment.find? (fun e => e.1 == key) with
  | some e => .ok e.2 | none => .error ("ManagedEnvironment:" ++ key)
private def enabled (c : Context) : Except String Unit := do
  if (← environment c "managed.enabled") != .bool true then throw "ManagedDisabled"

structure Region where
  id : Nat
  start : Nat
  permission : Nat
  version : Nat
  latency : Nat
  freeAt : Nat
  resultType : Nat
  cancelInFlight : Bool
  backing : List UInt8
  deriving Repr, BEq

def Region.encode (r : Region) : Value := .record [n r.id,n r.start,n r.permission,n r.version,n r.latency,n r.freeAt,n r.resultType,.bool r.cancelInFlight,.bytes r.backing]
def Region.decode : Value → Except String Region
  | .record [a,b,c,d,e,f,g,.bool cancel,.bytes bytes] => do
    pure ⟨← nat a,← nat b,← nat c,← nat d,← nat e,← nat f,← nat g,cancel,bytes⟩
  | _ => .error "ManagedRegionLayout"
/-- Host installs an owning logical mapping. No native pointer is represented. -/
def installRegion (c : Context) (s : State) (r : Region) : Except String (HandleIdentity × State) := do
  if r.backing.isEmpty || r.start+r.backing.length > 2^64 || r.permission == 0 || r.permission > 3 || r.latency ≥ 2^64 then throw "ManagedRegionRange"
  if s.objects.any (fun o => o.alive && o.tag == "managed.region" && (Region.decode o.value).toOption.any (fun x => x.id == r.id)) then throw "ManagedRegionDuplicate"
  allocate s c .lease 400 "managed.region" r.encode
private def region (c : Context) (s : State) (id : Nat) : Except String (HandleIdentity × Region) := do
  let some entry := s.objects.find? (fun o => o.alive && o.tag == "managed.region" && (Region.decode o.value).toOption.any (fun r => r.id == id)) | throw "ManagedRegionMissing"
  let entry ← getObject s c entry.identity "managed.region"
  pure (entry.identity,← Region.decode entry.value)
private def permission (available want : Nat) := want > 0 && want ≤ 3 && Nat.land available want == want
private def inRange (start count base size : Nat) := count > 0 && start ≥ base && start+count ≤ base+size && start+count ≤ 2^64
structure Lease where
  region : Nat
  start : Nat
  count : Nat
  permission : Nat
  version : Nat
  valid : Bool
  deriving Repr, BEq
private def Lease.encode (l : Lease) : Value := .record [n l.region,n l.start,n l.count,n l.permission,n l.version,.bool l.valid]
private def Lease.decode : Value → Except String Lease
  | .record [a,b,c,d,e,.bool valid] => do pure ⟨← nat a,← nat b,← nat c,← nat d,← nat e,valid⟩
  | _ => .error "ManagedLeaseLayout"
private def lease (c : Context) (s : State) (h : HandleIdentity) : Except String Lease := do
  if h.kind != .lease then throw "ManagedLeaseKind"
  Lease.decode (← getObject s c h "managed.lease").value
private def ok (v : Value) : List Value := [.variant 1 [v]]
private def denied (code : Nat) (s : State) : List Value × State := ([.variant 0 [n code]],s)

private def limits (c : Context) : Except String (Nat × Nat × Nat) := do
  match c.environment.lookup "managed.limits" with
  | none => pure (128,128,65536)
  | some (.record [a,b,d]) => pure (← nat a,← nat b,← nat d)
  | _ => throw "ManagedLimitsLayout"
private def liveCount (s : State) (tag : String) := (s.objects.filter (fun o => o.alive && o.tag == tag)).length
private def admission (state : State) (operation : Except String (List Value × State)) : Except String (List Value × State) :=
  match operation with
  | .error "ReferenceCapacity" | .error "ManagedCapacity" | .error "ResultPublicationCapacity" => .ok (denied 2 state)
  | .error "ReferenceIdentityExhausted" | .error "ReferenceSlotExhausted" => .ok (denied 1 state)
  | other => other

private def schedulerTag := "managed.scheduler"
private def scheduleEntry (s : State) := s.objects.find? (fun o => o.alive && o.tag == schedulerTag)
private def scheduleData (c : Context) (s : State) : Except String (Nat × List Value) := do
  match scheduleEntry s with
  | none => pure (0,[])
  | some entry =>
    let entry ← getObject s c entry.identity schedulerTag
    let .record [sequence,.record events] := entry.value | throw "ManagedSchedulerLayout"
    pure (← nat sequence,events)
private def scheduleEvent (c : Context) (v : Value) : Except String Event := do
  let .record [time,sequence,.bytes kind,.record values] := v | throw "ManagedEventLayout"
  let time ← nat time; let sequence ← nat sequence
  let some kind := String.fromUTF8? ⟨kind.toArray⟩ | throw "ManagedEventKind"
  let identity : HandleIdentity := ⟨.event,UInt32.ofNat c.domain,405,0,UInt64.ofNat sequence,UInt64.ofNat c.owner⟩
  pure ⟨identity,time,sequence,0,c.instanceId,0,sequence,kind,values,"",false⟩
/-- Private manager work is independent of Runtime EventQueue capacity and generations. -/
def scheduled (c : Context) (s : State) : Except String (List Event) := do
  let (_,values) ← scheduleData c s
  let events ← values.mapM (scheduleEvent c)
  pure (events.mergeSort (fun a b => a.time < b.time || (a.time == b.time && a.sequence ≤ b.sequence)))
private def putSchedule (c : Context) (sequence : Nat) (events : List Value) : Attempt Unit := do
  let value := Value.record [n sequence,.record events]
  match scheduleEntry (← get) with
  | some entry => updateChecked (fun state => putObject state c entry.identity schedulerTag value)
  | none =>
    updateChecked (fun state => configureAllocator state {kind := .lease,store := 405,group := "logical.managed.scheduler",persistent := false})
    let _ ← allocateAttempt c .lease 405 schedulerTag value
    pure ()
private def enqueueManaged (c : Context) (time : Nat) (kind : String) (values : List Value) (reuse : Option Nat := none) : Attempt Unit := do
  let (sequence,events) ← fromExcept (scheduleData c (← get))
  if time < c.now || time ≥ 2^64 then throw "ManagedScheduleTime"
  if reuse.isNone && sequence ≥ 2^64-1 then throw "ManagedSequenceOverflow"
  let next := reuse.getD (sequence+1)
  putSchedule c (if reuse.isSome then sequence else next) (events ++ [.record [n time,n next,.bytes kind.toUTF8.data.toList,.record values]])

def requestAttempt (c : Context) (id start stop perm : Nat) : Attempt (List Value) := do
  fromExcept (enabled c)
  let s ← get
  let (_,r) ← fromExcept (region c s id)
  if stop < start || !inRange start (stop-start+1) r.start r.backing.length || !permission r.permission perm then return (denied 0 s).1
  let (leaseLimit,_,_) ← fromExcept (limits c)
  if liveCount s "managed.lease" ≥ leaseLimit then throw "ManagedCapacity"
  updateChecked (fun state => configureAllocator state {kind := .lease,store := 401,group := "managed.lease",perSlot := true,capacity := some leaseLimit})
  let h ← allocateAttempt c .lease 401 "managed.lease" (Lease.encode ⟨id,start,stop-start+1,perm,r.version,true⟩)
  pure (ok (.handle h))

def request (c : Context) (s : State) (id start stop perm : Nat) : Except String (List Value × State) :=
  attemptExcept ((requestAttempt c id start stop perm).run s)

structure Access where
  lease : HandleIdentity
  region : Nat
  address : Nat
  count : Nat
  input : List UInt8
  write : Bool
  result : HandleIdentity
  consumer : HandleIdentity
  admitted : Bool
  terminal : Bool
  arrival : Nat := 0
  sequence : Nat := 0
  finish : Nat := 0
  deriving Repr, BEq
private def Access.encode (a : Access) : Value := .record [.handle a.lease,n a.region,n a.address,n a.count,.bytes a.input,.bool a.write,.handle a.result,.handle a.consumer,.bool a.admitted,.bool a.terminal,n a.arrival,n a.sequence,n a.finish]
private def Access.decode : Value → Except String Access
  | .record [l,r,a,k,.bytes bytes,.bool w,result,consumer,.bool admitted,.bool terminal,arrival,sequence,finish] => do
    pure ⟨← handle l,← nat r,← nat a,← nat k,bytes,w,← handle result,← handle consumer,admitted,terminal,← nat arrival,← nat sequence,← nat finish⟩
  | _ => .error "ManagedAccessLayout"
private def access (c : Context) (s : State) (h : HandleIdentity) : Except String Access := do
  if h.kind != .access then throw "ManagedAccessKind"
  Access.decode (← getObject s c h "managed.access").value

def beginAccessAttempt (types : TypeEnvironment) (c : Context) (h : HandleIdentity) (address count arrival : Nat) (input : List UInt8) (write : Bool) : Attempt (List Value) := do
  fromExcept (enabled c)
  let s ← get
  let l ← fromExcept (lease c s h)
  let (_,r) ← fromExcept (region c s l.region)
  if !l.valid || l.version != r.version then throw "ManagedInvalidLease"
  if arrival < c.now then return (denied 10 s).1
  if arrival ≥ 2^64 || !inRange address count l.start l.count || !permission l.permission (if write then 2 else 1) || (write && input.length != count) || (!write && !input.isEmpty) then return (denied 0 s).1
  let (_,accessLimit,byteLimit) ← fromExcept (limits c)
  let mut pendingInput := 0
  for entry in s.objects do
    if entry.alive && entry.tag == "managed.access" then
      let prior ← fromExcept (Access.decode entry.value)
      if !prior.terminal then pendingInput := pendingInput + prior.input.length
  if liveCount s "managed.access" ≥ accessLimit || count > byteLimit || pendingInput + input.length > byteLimit then throw "ManagedCapacity"
  updateChecked (fun state => configureAllocator state {kind := .access,store := 402,group := "managed.access",perSlot := true,capacity := some accessLimit})
  let (sequence,_) ← fromExcept (scheduleData c (← get))
  if sequence ≥ 2^64-1 then throw "ManagedSequenceOverflow"
  let a ← fromExcept (previewAllocation (← get) c .access 402)
  let (result,consumer) ← Storage.reserveResultAttempt types c a r.resultType (count+256) c.owner
  let _ ← commitAllocationAttempt c a "managed.access" (Access.encode ⟨h,l.region,address,count,input,write,result,consumer,false,false,arrival,sequence+1,0⟩)
  enqueueManaged c arrival "managed.admit" [.handle a]
  pure (ok (.handle a))

def beginAccess (types : TypeEnvironment) (c : Context) (s : State) (h : HandleIdentity) (address count arrival : Nat) (input : List UInt8) (write : Bool) : Except String (List Value × State) :=
  attemptExcept ((beginAccessAttempt types c h address count arrival input write).run s)

private def finish (types : TypeEnvironment) (c : Context) (s : State) (h : HandleIdentity) (a : Access) (failure : Nat) : Except String State := do
  let (rh,r) ← region c s a.region
  let bytes := if a.write || failure != 0 then [] else r.backing.drop (a.address-r.start) |>.take a.count
  let value := Value.record [n failure,n (if failure != 0 then 0 else if a.write then 2 else 1),.bytes bytes]
  let next ← Storage.publishResult types c s a.result value
  let next ← Storage.releaseProducer c next a.result
  let backing := if a.write && failure == 0 then r.backing.take (a.address-r.start) ++ a.input ++ r.backing.drop (a.address-r.start+a.count) else r.backing
  let next ← putObject next c rh "managed.region" ({r with backing}.encode)
  putObject next c h "managed.access" ({a with terminal := true}.encode)
/-- Deliver only an actual queued key. Admission pins logical backing until completion. -/
def deliver (types : TypeEnvironment) (c : Context) (s : State) (event : Event) : Except String State := do
  if !((← scheduled c s).any (· == event)) || event.time != c.now || event.turn != c.turn then throw "ManagedEventKey"
  let (sequence,events) ← scheduleData c s
  let (_,next) ← attemptExcept ((putSchedule c sequence (events.filter (fun v => (scheduleEvent c v).toOption.all (fun e => e.identity != event.identity)))).run s)
  match event.kind,event.values with
  | "managed.admit",[.handle h] =>
    let a ← access c next h
    if a.terminal then return next
    let l ← match lease c next a.lease with
      | .ok l => pure l
      | .error _ => pure (Lease.mk a.region 0 0 0 0 false)
    let (rh,r) ← region c next a.region
    if !l.valid || l.version != r.version then return ← finish types c next h a 1
    let time := max c.now r.freeAt + r.latency
    if time ≥ 2^64 then return ← finish types c next h a 4
    let next ← putObject next c rh "managed.region" ({r with freeAt := time}.encode)
    let next ← putObject next c h "managed.access" ({a with admitted := true,finish := time}.encode)
    let (_,next) ← attemptExcept ((enqueueManaged c time "managed.finish" [.handle h] (some event.sequence)).run next)
    pure next
  | "managed.finish",[.handle h] =>
    let a ← access c next h
    if a.terminal then return next
    if !a.admitted then throw "ManagedNotAdmitted"
    finish types c next h a 0
  | "managed.invalidate",[id,start,stop] =>
    let id ← nat id; let start ← nat start; let stop ← nat stop
    let (_,r) ← region c next id
    let mut next := next
    for item in next.objects do
      if item.alive && item.tag == "managed.lease" then
        let l ← Lease.decode item.value
        if l.region == id && l.start ≤ stop && start < l.start+l.count then
          next ← putObject next c item.identity "managed.lease" ({l with valid := false}.encode)
    for item in next.objects do
      if item.alive && item.tag == "managed.access" then
        let a ← Access.decode item.value
        if a.region == id && !a.terminal && a.address ≤ stop && start < a.address+a.count && (!a.admitted || r.cancelInFlight) then
          next ← finish types c next item.identity a 1
    pure next
  | _,_ => throw "ManagedEventLayout"

def invalidateAttempt (c : Context) (id start stop time : Nat) : Attempt Unit := do
  fromExcept (enabled c)
  if (← fromExcept (environment c "managed.invalidateOwner")) != n c.owner then throw "ManagedInvalidationAuthority"
  let (_,r) ← fromExcept (region c (← get) id)
  if stop < start || !inRange start (stop-start+1) r.start r.backing.length then throw "ManagedInvalidateRange"
  let events ← fromExcept (scheduled c (← get))
  let capacity ← match c.environment.lookup "managed.scheduledChanges" with
    | none => pure 128 | some value => fromExcept (nat value)
  if (events.filter (fun event => event.kind == "managed.invalidate")).length ≥ capacity then throw "ManagedCapacity"
  enqueueManaged c time "managed.invalidate" [n id,n start,n stop]

def invalidate (c : Context) (s : State) (id start stop time : Nat) : Except String State := do
  let (_,next) ← attemptExcept ((invalidateAttempt c id start stop time).run s)
  pure next

/-- Reifiable external expression: input, literal, binary, select, field. -/
def decodeProgram : Nat → Value → Except String Pure.Program
  | 0,_ => .error "ExternalReferenceDepth"
  | _+1,.variant 0 [] => .ok .input
  | _+1,.variant 1 [v] => .ok (.literal v)
  | fuel+1,.variant 2 [tag,a,b] => do
    let tag ← nat tag
    let some op := [Pure.BinaryOp.addWrap,.subWrap,.mulWrap,.addChecked,.divChecked,.eq,.lt,.and,.or][tag]? | throw "ExternalReferenceOperator"
    pure (.binary op (← decodeProgram fuel a) (← decodeProgram fuel b))
  | fuel+1,.variant 3 [c,y,n] => do pure (.select (← decodeProgram fuel c) (← decodeProgram fuel y) (← decodeProgram fuel n))
  | fuel+1,.variant 4 [v,i] => do pure (.field (← decodeProgram fuel v) (← nat i))
  | _,_ => .error "ExternalReferenceLayout"

def external (c : Context) (signature : ServiceSignature) (input : Value) (s : State) : Except String (List Value × State) := do
  if (← environment c "external.enabled") != .bool true then throw "ExternalDisabled"
  let .record [.bytes identity,.bytes _semantics,program,precondition,.bool native,nativeOutput] ← environment c signature.providerKey | throw "ExternalBindingLayout"
  if identity != signature.providerVersion.toUTF8.data.toList then throw "ExternalReferenceIdentity"
  let program ← decodeProgram 64 program
  let precondition ← decodeProgram 64 precondition
  if Pure.cost program + Pure.cost precondition > 100000 then throw "ExternalReferenceFuel"
  let .ok (.bool true) := Pure.eval precondition input | throw "ExternalPrecondition"
  let .ok output := Pure.eval program input | throw "ExternalReferenceFailure"
  if native && nativeOutput != output then throw "ExternalNativeMismatch"
  pure (ok output,s)
end LeanAT.Reference.Managed


namespace LeanAT.Reference.Managed
private def typeEncoding (t : TypeSchema) : ByteArray :=
  let (tag,bound,fields,cases) := match t with
    | .unit => (0,0,[],[]) | .bool => (1,0,[],[]) | .bits w => (2,w,[],[])
    | .fin n => (3,n,[],[]) | .record fs => (4,0,fs,[]) | .variant cs => (5,0,[],cs)
    | .vec t n => (6,n,[t],[]) | .boundedVec t n => (7,n,[t],[])
    | .bytes n => (8,n,[],[]) | .handle k => (9,ABI.handleKindTag k,[],[])
  let list (xs : List Nat) := ABI.le xs.length 8 ++ ABI.cat (xs.map (ABI.le · 8))
  ABI.le tag 8 ++ ABI.le bound 8 ++ list fields ++ ABI.le cases.length 8 ++ ABI.cat (cases.map list)
/-- Exact optional_services.cpp managed-v1 encoding, including all type-table rows. -/
def managedHash (types : TypeEnvironment) (s : ExecIR.ServiceSignature) (semantics : String := "managed-v1") : ByteArray :=
  ABI.sha256 (ABI.le semantics.toUTF8.size 8 ++ semantics.toUTF8 ++ ABI.le s.op.tag 8 ++ ABI.le types.length 8 ++ ABI.cat (types.map typeEncoding) ++ ABI.cat ((s.inputTypes ++ s.resultTypes).map (ABI.le · 8)))
private def typeAt (types : TypeEnvironment) (id : Nat) (t : TypeSchema) := types[id]? == some t
private def exceptHandle (types : TypeEnvironment) (id : Nat) (kind : HandleKind) : Bool :=
  match types[id]? with
  | some (.variant [[error],[payload]]) => typeAt types error (.bits 64) && typeAt types payload (.handle kind)
  | _ => false
private def bytesType (types : TypeEnvironment) (id : Nat) : Bool :=
  match types[id]? with | some (.bytes cap) => cap ≤ 65536 | _ => false

/-- Host-owned stable backing and synchronous invalidation are explicit raw mapping assumptions. -/
def installRawRegion (c : Context) (s : State) (id start stop permission readLatency writeLatency generation : Nat) (backing : Option (List UInt8) := none) : Except String (HandleIdentity × State) := do
  if start > stop || stop ≥ 2^64 || permission != 3 || readLatency ≥ 2^64 || writeLatency ≥ 2^64 || generation == 0 || generation ≥ 2^64 then throw "RawRegionRange"
  if backing.any (fun bytes => bytes.length != stop-start+1) then throw "RawBackingExtent"
  let s ← configureAllocator s {kind := .lease,store := 403,group := "logical.raw.region",persistent := false}
  let s ← configureAllocator s {kind := .lease,store := 404,group := "logical.raw.grant",persistent := false}
  allocate s c .lease 403 "raw.region" (.record [n id,n start,n stop,n permission,n readLatency,n writeLatency,n generation,backing.map Value.bytes |>.getD .unit])
private def rawDenied : Value := .record [.bool false,n 0,n 0,n 0,n 0,n 0,n 0,n 0]
private def rawInvokeAttempt (c : Context) (op : ExecIR.Op) (args : List Value) : Attempt (List Value) := do
  if (← fromExcept (environment c "raw.enabled")) != .bool true then throw "RawDisabled"
  let s ← get
  match op,args with
  | .denyDmi,[] => pure [rawDenied]
  | .grantRawDmi,[id,address,command] =>
    let id ← fromExcept (nat id); let address ← fromExcept (nat address); let command ← fromExcept (nat command)
    let some r := s.objects.find? (fun o => o.alive && o.tag == "raw.region" && (match o.value with | .record (a::_) => a == n id | _ => false)) | throw "RawRegionMissing"
    let r ← fromExcept (getObject s c r.identity "raw.region")
    let .record [_,start,stop,perm,read,write,generation,_backing] := r.value | throw "RawRegionLayout"
    if address < (← fromExcept (nat start)) || address > (← fromExcept (nat stop)) || command > 1 then return [rawDenied]
    let capacity ← match c.environment.lookup "raw.capacity" with
      | none => pure 128 | some value => fromExcept (nat value)
    let grants := s.objects.filter (fun o => o.alive && o.tag == "raw.grant" &&
      (match o.value with | .record (_::region::_) => region == n id | _ => false))
    if grants.length ≥ capacity then return [rawDenied]
    let value := Value.record [.bool true,n id,start,stop,perm,read,write,generation]
    let _ ← allocateAttempt c .lease 404 "raw.grant" value
    updateChecked (fun next => observe next ⟨"raw.grant","grantRawDmi","",c.now,c.turn,[value]⟩)
    pure [value]
  | .invalidateRawDmi,[id,start,stop] =>
    let id ← fromExcept (nat id); let start ← fromExcept (nat start); let stop ← fromExcept (nat stop)
    let some region := s.objects.find? (fun o => o.alive && o.tag == "raw.region" &&
      (match o.value with | .record (a::_) => a == n id | _ => false)) | throw "RawRegionMissing"
    let _ ← fromExcept (getObject s c region.identity "raw.region")
    if start > stop then throw "RawInvalidationRange"
    if (← fromExcept (environment c "raw.invalidateOwner")) != n c.owner then throw "RawInvalidationAuthority"
    for o in s.objects do
      if o.alive && o.tag == "raw.grant" then
        let .record [.bool true,r,a,b,_,_,_,_] := o.value | throw "RawGrantLayout"
        if (← fromExcept (nat r)) == id && (← fromExcept (nat a)) ≤ stop && start ≤ (← fromExcept (nat b)) then
          updateChecked (fun next => releaseObject next c o.identity "raw.grant")
          updateChecked (fun next => observe next ⟨"raw.invalidate","invalidateRawDmi","",c.now,c.turn,[n id,a,b]⟩)
    pure [.unit]
  | _,_ => throw "RawInvocation"
private def rawShape (types : TypeEnvironment) (id : Nat) := match types[id]? with
  | some (.record [b,a,c,d,e,f,g,h]) => typeAt types b .bool && [a,c,d,e,f,g,h].all (fun i => typeAt types i (.bits 64))
  | _ => false

def supports (s : ExecIR.ServiceSignature) : Bool :=
  [.requestManaged,.beginManagedRead,.beginManagedWrite,.invalidateManaged,.releaseLease,.callExternPure,.grantRawDmi,.denyDmi,.invalidateRawDmi].contains s.op ||
  ([ExecIR.Op.resultGet,.resultRelease].contains s.op && s.providerKey.startsWith "leanat.managed.")

def validate (types : TypeEnvironment) (s : ExecIR.ServiceSignature) : Except String Unit := do
  if !supports s then throw "ManagedOpcode"
  let [out] := s.resultTypes | throw "ManagedResultArity"
  let shape := match s.op,s.inputTypes with
    | .requestManaged,[a,b,c,d] => [a,b,c,d].all (fun i => typeAt types i (.bits 64)) && exceptHandle types out .lease
    | .beginManagedRead,[a,b,c,d] => typeAt types a (.handle .lease) && [b,c,d].all (fun i => typeAt types i (.bits 64)) && exceptHandle types out .access
    | .beginManagedWrite,[a,b,c,d] => typeAt types a (.handle .lease) && [b,d].all (fun i => typeAt types i (.bits 64)) && bytesType types c && exceptHandle types out .access
    | .invalidateManaged,[a,b,c,d] => [a,b,c,d].all (fun i => typeAt types i (.bits 64)) && typeAt types out .unit
    | .releaseLease,[a] => typeAt types a (.handle .lease) && typeAt types out .unit
    | .resultRelease,[a] => typeAt types a (.handle .access) && typeAt types out .unit
    | .resultGet,[a] => typeAt types a (.handle .access) && (match types[out]? with | some (.record [x,y,z]) => typeAt types x (.bits 64) && typeAt types y (.bits 64) && bytesType types z | _ => false)
    | .grantRawDmi,[a,b,c] => [a,b,c].all (fun i => typeAt types i (.bits 64)) && rawShape types out
    | .denyDmi,[] => rawShape types out
    | .invalidateRawDmi,[a,b,c] => [a,b,c].all (fun i => typeAt types i (.bits 64)) && typeAt types out .unit
    | .callExternPure,[input] => (match types[out]? with | some (.variant [[e],[payload]]) => typeAt types e (.bits 64) && [input,payload].all (fun i => typeAt types i (.bits 64) || typeAt types i .bool || bytesType types i) | _ => false)
    | _,_ => false
  if !shape then throw "ManagedSignatureShape"
  let raw := [ExecIR.Op.grantRawDmi,.denyDmi,.invalidateRawDmi].contains s.op
  let contextMask := if s.op == .grantRawDmi || s.op == .denyDmi then 16 else 3
  if s.contextMask != contextMask || s.extraFuel != 0 then throw "ManagedSignatureContext"
  if raw then
    if s.effectMask != 1024 || s.providerKey != "leanat.raw-dmi." ++ toString s.op.tag || s.providerVersion != "1" || s.abiHash != managedHash types s "raw-dmi-v1" then throw "RawSignatureIdentity"
    return
  if s.op == .callExternPure then
    if s.effectMask != 512 || !s.providerKey.startsWith "leanat.external." || s.providerVersion.isEmpty || s.abiHash.size != 32 then throw "ExternalSignature"
  else
    let effect := if s.op == .resultGet || s.op == .resultRelease then 64 else 256
    if s.effectMask != effect || s.providerKey != "leanat.managed." ++ toString s.op.tag || s.providerVersion != "1" || s.abiHash != managedHash types s then throw "ManagedSignatureIdentity"

def validateEnvironment (types : TypeEnvironment) (c : Context) (s : ExecIR.ServiceSignature) : Except String Unit := do
  validate types s
  if s.op == .callExternPure then
    if (← environment c "external.enabled") != .bool true then throw "ExternalDisabled"
    let .record [.bytes identity,.bytes semantics,program,precondition,.bool _,_] ← environment c s.providerKey | throw "ExternalBindingLayout"
    if identity != s.providerVersion.toUTF8.data.toList then throw "ExternalReferenceIdentity"
    let some semantics := String.fromUTF8? ⟨semantics.toArray⟩ | throw "ExternalBindingUTF8"
    if !semantics.startsWith "external-v1:" || !semantics.endsWith s.providerVersion || s.abiHash != managedHash types s semantics then throw "ExternalBindingABI"
    let _ ← decodeProgram 64 program
    let _ ← decodeProgram 64 precondition
  else if [ExecIR.Op.grantRawDmi,.denyDmi,.invalidateRawDmi].contains s.op then
    if (← environment c "raw.enabled") != .bool true then throw "RawDisabled"
    if let some value := c.environment.lookup "raw.capacity" then
      if (← nat value) == 0 then throw "RawRegionCapacity"
  else
    enabled c

private def admissionAttempt (operation : Attempt (List Value)) : Attempt (List Value) := fun before =>
  match operation.run before with
  | .ok values after => .ok values after
  | .error error draft =>
    let restored := rollbackAllocations before draft
    match error with
    | "ReferenceCapacity" | "ManagedCapacity" | "ResultPublicationCapacity" => .ok (denied 2 restored).1 restored
    | "ReferenceIdentityExhausted" | "ReferenceSlotExhausted" | "ManagedSequenceOverflow" => .ok (denied 1 restored).1 restored
    | _ => .error error draft

def invokeAttempt (types : TypeEnvironment) (c : Context) (s : ExecIR.ServiceSignature) (args : List Value) : Attempt (List Value) := do
  fromExcept (validateEnvironment types c s)
  fromExcept (checkContext c)
  if Nat.land s.contextMask (2^c.kind) == 0 then throw "ManagedContext"
  if args.length != s.inputTypes.length || !(s.inputTypes.zip args).all (fun (t,v) => (types[t]?).any (fun schema => conforms types 64 schema v)) then throw "ManagedArguments"
  let result ← match s.op,args with
    | .requestManaged,[a,b,d,e] => admissionAttempt (requestAttempt c (← fromExcept (nat a)) (← fromExcept (nat b)) (← fromExcept (nat d)) (← fromExcept (nat e)))
    | .beginManagedRead,[h,a,k,t] => admissionAttempt (beginAccessAttempt types c (← fromExcept (handle h)) (← fromExcept (nat a)) (← fromExcept (nat k)) (← fromExcept (nat t)) [] false)
    | .beginManagedWrite,[h,a,.bytes bytes,t] => admissionAttempt (beginAccessAttempt types c (← fromExcept (handle h)) (← fromExcept (nat a)) bytes.length (← fromExcept (nat t)) bytes true)
    | .invalidateManaged,[a,b,d,e] => do
      let a ← fromExcept (nat a); let b ← fromExcept (nat b); let d ← fromExcept (nat d); let e ← fromExcept (nat e)
      invalidateAttempt c a b d e
      pure [.unit]
    | .releaseLease,[h] => do
      let h ← fromExcept (handle h)
      let _ ← fromExcept (lease c (← get) h)
      updateChecked (fun state => releaseObject state c h "managed.lease")
      pure [.unit]
    | .resultGet,[h] => do
      let a ← fromExcept (access c (← get) (← fromExcept (handle h)))
      pure [← fromExcept (Storage.getResult c (← get) a.result a.consumer)]
    | .resultRelease,[h] => do
      let h ← fromExcept (handle h)
      let a ← fromExcept (access c (← get) h)
      Storage.releaseResultAttempt c a.result a.consumer
      updateChecked (fun state => releaseObject state c h "managed.access")
      pure [.unit]
    | .callExternPure,[input] => modifyChecked (external c s input)
    | .grantRawDmi,_ | .denyDmi,_ | .invalidateRawDmi,_ => rawInvokeAttempt c s.op args
    | _,_ => throw "ManagedInvocation"
  if !(s.resultTypes.zip result).all (fun (t,v) => (types[t]?).any (fun schema => conforms types 64 schema v)) then throw "ManagedResultLayout"
  pure result

def invoke (types : TypeEnvironment) (c : Context) (s : ExecIR.ServiceSignature) (args : List Value) (state : State) : Except String (List Value × State) :=
  attemptExcept ((invokeAttempt types c s args).run state)

end LeanAT.Reference.Managed


