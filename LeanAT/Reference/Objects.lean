import LeanAT.Reference.ABI
import LeanAT.Reference.Storage
import LeanAT.ModelIR.Object

namespace LeanAT.Reference.Objects
open ExecIR
abbrev R := Except String
/-- Imported standard objects already have identity generations; seed their highwater explicitly. -/
def stateWithObjects (objects : List ObjectEntry) : State :=
  {objects, nextGeneration := (objects.map (fun entry => entry.identity.generation.toNat+1)).foldl max 1}
private def u (n : Nat) : Value := .bits 64 n
private def nat : Value → R Nat
  | .bits 64 n => if n < 2^64 then .ok n else .error "ObjectUInt64"
  | _ => .error "ObjectUInt64"
private def bytes : Value → R (List UInt8)
  | .bytes b => .ok b | _ => .error "ObjectBytes"
private def fields : Value → R (List Value)
  | .record r | .vec r => .ok r | _ => .error "ObjectRecord"
private def handle : Value → R HandleIdentity
  | .handle h => .ok h | _ => .error "ObjectHandle"
private def get (xs : List α) (i : Nat) : R α := match xs[i]? with | some x => .ok x | none => .error "ObjectArity"
private def num (xs : List Value) (i : Nat) : R Nat := get xs i >>= nat
private def octets (xs : List Value) (i : Nat) : R (List UInt8) := get xs i >>= bytes
private def ident (xs : List Value) (i : Nat) : R HandleIdentity := get xs i >>= handle
private def row (xs : List Value) (i : Nat) : R (List Value) := get xs i >>= fields
private def env (c : Context) (key : String) : Option Value := (c.environment.find? (fun p => p.1 == key)).map Prod.snd
private def nodeBytes (c : Context) : Nat :=
  match env c "objects.valueNodeBytes" with | some (.bits 64 n) => n | _ => 0
private def rawOutputBytes (c : Context) (mid : Nat) (out : List Value) : Nat :=
  let n := nodeBytes c
  if mid == 1 || mid == 6 then
    match out with | [.record fs] => (fs.map (nativeValueBytes n)).sum | _ => 2^64
  else if mid == 15 then
    match out with
    | [.variant 0 []] => n
    | [.variant 1 [.record [element,consumer,owner,id]]] =>
      let unoption := fun v => match v with | .variant 1 [x] => x | _ => Value.unit
      let owner := match unoption owner with | .variant _ [h] => h | _ => Value.unit
      n+nativeValueBytes n (unoption element)+nativeValueBytes n (unoption consumer)+nativeValueBytes n owner+nativeValueBytes n id
    | _ => 2^64
  else (out.map (nativeValueBytes n)).sum
private def option (v : Option Value) : Value := match v with | none => .variant 0 [] | some x => .variant 1 [x]

def encodeServiceType (types : TypeEnvironment) : Nat → Nat → R ByteArray
  | 0, _ => .error "ObjectTypeDepth"
  | fuel+1, id => do
    let some t := types[id]? | throw "ObjectUnknownType"
    let (tag,bound,fs,cs) := match t with
      | .unit => (0,0,[],[]) | .bool => (1,0,[],[]) | .bits w => (2,w,[],[])
      | .fin n => (3,n,[],[]) | .record fs => (4,0,fs,[]) | .variant cs => (5,0,[],cs)
      | .vec t n => (6,n,[t],[]) | .boundedVec t n => (7,n,[t],[])
      | .bytes n => (8,n,[],[]) | .handle k => (9,ABI.handleKindTag k,[],[])
    if bound ≥ 2^64 then throw "ObjectTypeBound"
    let mut out := ABI.le tag 8 ++ ABI.le bound 8 ++ ABI.le fs.length 8
    for f in fs do out := out ++ (← encodeServiceType types fuel f)
    out := out ++ ABI.le cs.length 8
    for c in cs do
      out := out ++ ABI.le c.length 8
      for f in c do out := out ++ (← encodeServiceType types fuel f)
    pure out

private def methods (kind : Nat) : List Nat := match kind with
  | 0 => [1,2,3,4,5,6] | 1 => [1,5,6,7,8] | 2 => [9,10,11,12,13]
  | 3 => [14,15,16,19,20,21] | 4 => [11,13,17,18] | _ => []
private def effects (kind method : Nat) : Nat :=
  if [2,7,13,16].contains method then 129
  else if [3,4,5,8,12].contains method || (method == 11 && kind == 2) then 130
  else if kind == 4 && [11,17].contains method then 139 else 131
private def parseKey (key : String) : R (Nat × Nat) := do
  let ["leanat","stdlib","object",obj,"method",method] := key.splitOn "." | throw "ObjectProviderKey"
  let some objId := obj.toNat? | throw "ObjectProviderKey"
  let some mid := method.toNat? | throw "ObjectProviderKey"
  if obj != toString objId || method != toString mid || objId ≥ 2^32 then throw "ObjectProviderKey"
  pure (objId,mid)

def supports (signature : ServiceSignature) : Bool :=
  (signature.op == .objectCall || signature.op == .debugTransfer) && signature.providerKey.startsWith "leanat.stdlib.object."

def makeSignature (types : TypeEnvironment) (kind objectId method : Nat)
    (inputs outputs : List Nat) (maxBytes maxEntries : Nat) (id : Nat := 1) : R ServiceSignature := do
  let contexts := if method == 6 then 8 else 3
  let effect := effects kind method
  let fuel := maxBytes+maxEntries+1
  let mut canonical := ABI.cat ([kind,method,contexts,effect,fuel].map (fun n => ABI.le n 8))
  for t in inputs do canonical := canonical ++ (← encodeServiceType types 64 t)
  canonical := canonical.push 255
  for t in outputs do canonical := canonical ++ (← encodeServiceType types 64 t)
  pure ⟨id,if method == 6 then .debugTransfer else .objectCall,inputs,outputs,contexts,effect,fuel,
    s!"leanat.stdlib.object.{objectId}.method.{method}","2",ABI.sha256 canonical⟩

/-- Structural shape checks precede the ABI digest and all provider execution. -/
private def shape (ts : TypeEnvironment) (ids : List Nat) (patterns : List TypeSchema) : Bool :=
  ids.length == patterns.length && (ids.zip patterns).all (fun (id,p) => (ts[id]?).any (· == p))
private def isBytes (ts : TypeEnvironment) (id : Nat) : Bool := match ts[id]? with | some (.bytes n) => n > 0 | _ => false
private def outputRecord (ts : TypeEnvironment) (ids : List Nat) : Option (List Nat) := do
  let [id] := ids | none
  let some (.record fs) := ts[id]? | none
  pure fs
private def grantShape (ts : TypeEnvironment) (id : Nat) : Bool :=
  match ts[id]? with
  | some (.record fs) => shape ts fs [.bits 64,.bits 64,.bits 64,.handle .resourceTicket,.bits 64]
  | _ => false
private def pipelineShape (ts : TypeEnvironment) (id : Nat) : Bool :=
  match ts[id]? with
  | some (.record [g,o,e,v]) => grantShape ts g && shape ts [o,e,v] [.handle .transaction,.handle .event,.bits 64]
  | _ => false
private def optionType (ts : TypeEnvironment) (id : Nat) : Option Nat :=
  match ts[id]? with | some (.variant [[],[t]]) => some t | _ => none
private def queuePopShape (ts : TypeEnvironment) (id : Nat) : Bool := Id.run do
  let some recordId := optionType ts id | return false
  let some (.record [element,consumer,owner,sequence]) := ts[recordId]? | return false
  let some elementId := optionType ts element | return false
  let some consumerId := optionType ts consumer | return false
  let some ownerId := optionType ts owner | return false
  let some (.record consumerFields) := ts[consumerId]? | return false
  let some (.variant [[scope],[drain]]) := ts[ownerId]? | return false
  return (ts[elementId]?).isSome && shape ts consumerFields [.handle .result,.handle .consumer] &&
    shape ts [scope,drain,sequence] [.handle .scope,.handle .drain,.bits 64]
private def signatureShape (ts : TypeEnvironment) (kind mid : Nat) (s : ServiceSignature) : Bool := Id.run do
  let ins := s.inputTypes; let outs := s.resultTypes
  let scalar := fun (ps : List TypeSchema) => shape ts outs ps
  match mid with
  | 1 =>
    let [a,b,c,d,e] := ins | return false
    let some [status,data] := outputRecord ts outs | return false
    return shape ts [a,b,d,status] [.bits 64,.bits 64,.bits 64,.bits 64] && isBytes ts c && ts[c]? == ts[e]? && ts[c]? == ts[data]?
  | 2 => return shape ts ins [.bits 64,.bits 64] && outs.length == 1 && outs.all (isBytes ts)
  | 3 =>
    let [a,b,c] := ins | return false
    return shape ts [a] [.bits 64] && isBytes ts b && ts[b]? == ts[c]? && outs.isEmpty
  | 4 | 5 => return ins.isEmpty && outs.isEmpty
  | 6 =>
    let [a,b,c] := ins | return false
    let some [status,data] := outputRecord ts outs | return false
    return shape ts [a,b,status] [.bits 64,.bits 64,.bits 64] && isBytes ts c && ts[c]? == ts[data]?
  | 7 => return shape ts ins [.bits 64] && outs.length == 1 && outs.all (isBytes ts)
  | 8 =>
    let [a,b] := ins | return false
    return shape ts [a] [.bits 64] && isBytes ts b && outs.isEmpty
  | 9 | 10 => return shape ts ins (if mid == 9 then [.handle .transaction,.bits 64,.bits 64] else [.handle .transaction,.bits 64]) && outs.length == 1 && outs.all (grantShape ts)
  | 11 => return (if kind == 4 then ins.length == 1 && ins.all (pipelineShape ts) else shape ts ins [.handle .resourceTicket]) && scalar [.bits 64]
  | 12 => return shape ts ins [.handle .resourceTicket,.bits 64] && scalar [.bool]
  | 13 =>
    let [id] := outs | return false
    return ins.isEmpty && match ts[id]? with | some (.boundedVec t n) => n > 0 && grantShape ts t | _ => false
  | 14 => return ins.length == 1 && scalar [.bool]
  | 15 =>
    let [id] := outs | return false
    return ins.isEmpty && queuePopShape ts id
  | 16 => return ins.isEmpty && scalar [.bits 64]
  | 17 => return shape ts ins [.handle .transaction,.bits 64] && outs.length == 1 && outs.all (pipelineShape ts)
  | 18 =>
    let [p,e] := ins | return false
    return pipelineShape ts p && shape ts [e] [.handle .event] && scalar [.bool]
  | 19 => return shape ts ins [.handle .result,.handle .consumer,.bits 64] && scalar [.bool]
  | 20 | 21 =>
    let [id] := outs | return false
    return shape ts ins (if mid == 20 then [.handle .scope] else [.handle .scope,.handle .drain,.bits 64]) &&
      match ts[id]? with | some (.boundedVec t n) => n > 0 && shape ts [t] [.bits 64] | _ => false
  | _ => return false
private def resolve (ts : TypeEnvironment) (s : ServiceSignature) : R (Nat × Nat × Nat) := do
  if !supports s || s.providerVersion != "2" || s.extraFuel == 0 || s.extraFuel ≥ 2^64 then throw "ObjectServiceContract"
  let (obj,mid) ← parseKey s.providerKey
  for kind in List.range 5 do
    if (methods kind).contains mid && signatureShape ts kind mid s then
      let expected ← makeSignature ts kind obj mid s.inputTypes s.resultTypes (s.extraFuel-1) 0 s.id
      if expected.op == s.op && expected.contextMask == s.contextMask && expected.effectMask == s.effectMask && expected.abiHash == s.abiHash then
        return (kind,obj,mid)
  throw "ObjectServiceABI"
def validate (ts : TypeEnvironment) (s : ServiceSignature) : R Unit := do let _ ← resolve ts s; pure ()

private def tag (kind : Nat) : String := "reference.object." ++ (["memory","register","resource","queue","pipeline"][kind]?).getD "invalid"
def memoryEntry (c : Context) (objectId : Nat) (initial : List UInt8) : ObjectEntry :=
  ⟨⟨.resourceTicket,UInt32.ofNat c.domain,300,UInt32.ofNat objectId,1,0⟩,tag 0,
    .record [u c.instanceId,.bytes initial,.bytes initial,u 0,.bool false],true⟩
private def lookup (state : State) (c : Context) (kind obj : Nat) : R ObjectEntry := do
  let some entry := state.objects.find? (fun e => e.alive && e.tag == tag kind && e.identity.slot.toNat == obj && e.identity.domain.toNat == c.domain) | throw "ObjectNotFound"
  let fs ← fields entry.value
  if (← num fs 0) != c.instanceId then throw "ObjectInstance"
  pure entry
private def install (state : State) (entry : ObjectEntry) (value : Value) : R State := do
  let next := {state with objects := state.objects.map (fun e => if e.identity == entry.identity then {e with value} else e)}
  checkCapacity next
  pure next
private def configuration (c : Context) (key : String) : R Nat := do
  let some value := env c key | throw ("MissingObjectConfiguration:" ++ key)
  nat value
private def hostContract (ts : TypeEnvironment) (c : Context) (s : ServiceSignature)
    (kind mid : Nat) (entry : ObjectEntry) : R Unit := do
  let maxBytes ← configuration c "objects.maxBytes"
  let maxEntries ← configuration c "objects.maxEntries"
  if maxBytes == 0 || maxEntries == 0 || maxBytes+maxEntries+1 != s.extraFuel then throw "ObjectHostFuelContract"
  if [1,2,3,6,7,8].contains mid then
    let byteId ← match mid with
      | 1 | 6 => get s.inputTypes 2
      | 2 | 7 => get s.resultTypes 0
      | _ => get s.inputTypes 1
    if ts[byteId]? != some (.bytes maxBytes) then throw "ObjectHostByteContract"
  if mid == 13 || mid == 20 || mid == 21 then
    let id ← get s.resultTypes 0
    let some (.boundedVec _ capacity) := ts[id]? | throw "ObjectHostVectorContract"
    let expected ← if mid == 13 then fields entry.value >>= fun f => num f 5 else pure maxEntries
    if capacity != expected then throw "ObjectHostVectorContract"
  if kind == 3 && (mid == 14 || mid == 15) then
    let element ← configuration c "objects.elementType"
    let actual ← if mid == 14 then get s.inputTypes 0 else do
      let id ← get s.resultTypes 0
      let some recordId := optionType ts id | throw "ObjectHostQueueContract"
      let some (.record (optionalElement::_)) := ts[recordId]? | throw "ObjectHostQueueContract"
      let some id := optionType ts optionalElement | throw "ObjectHostQueueContract"
      pure id
    if (← encodeServiceType ts 64 element) != (← encodeServiceType ts 64 actual) then throw "ObjectHostQueueContract"
private def command (n : Nat) : ModelIR.Object.Command := match n with | 0 => .read | 1 => .write | 2 => .ignore | _ => .unknown
private def status : ModelIR.Object.ResponseStatus → Nat
  | .ok => 1 | .commandError => 4 | .burstError => 5 | .byteEnableError => 6 | .addressError => 3 | .budgetExceeded => 2
private def memory (mid : Nat) (args : List Value) (entry : ObjectEntry) (state : State) : R (List Value × State) := do
  let fs ← fields entry.value
  let data ← octets fs 1
  let initial ← octets fs 2
  let _ ← num fs 3
  let .bool alreadyDirty ← get fs 4 | throw "MemoryDirtyCodec"
  if fs.length != 5 || data.isEmpty || data.length != initial.length then throw "MemoryState"
  let mut next := data
  let mut dirty := alreadyDirty
  let mut output := []
  match mid with
  | 1 =>
    let request : ModelIR.Object.MemoryRequest := ⟨command (← num args 0),← num args 1,← octets args 2,← num args 3,← octets args 4⟩
    let result := ModelIR.Object.memoryTransfer data request 65536
    if result.status == .budgetExceeded then throw "ObjectArgumentBudget"
    if (← num args 0) == 1 && result.status == .ok then dirty := true
    next := result.memory
    output := [.record [u (status result.status),.bytes result.data]]
  | 2 =>
    let address ← num args 0; let count ← num args 1
    if count != 0 && (address ≥ data.length || count > data.length-address) then throw "MemoryRange"
    output := [.bytes (data.drop address |>.take count)]
  | 3 =>
    let address ← num args 0; let input ← octets args 1; let mask ← octets args 2
    if !input.isEmpty then
      if address ≥ data.length || input.length > data.length-address || !(mask.all (fun b => b == 0 || b == 255)) then throw "MemoryRangeOrMask"
      for i in List.range input.length do
        if mask.isEmpty || mask[i % mask.length]?.getD 0 == 255 then next := next.set (address+i) (input[i]?.getD 0)
      dirty := true
  | 4 => next := List.replicate data.length 0; dirty := true
  | 5 => next := initial; dirty := true
  | 6 =>
    let request : ModelIR.Object.MemoryRequest := ⟨command (← num args 0),← num args 1,← octets args 2,1,[]⟩
    let result := ModelIR.Object.memoryDebug data request 65536
    next := result.memory; output := [.record [u result.count,.bytes result.data]]
    if (← num args 0) == 1 && (← num args 1) < data.length then dirty := true
  | _ => throw "MemoryMethod"
  pure (output,← install state entry (.record (fs.set 1 (.bytes next) |>.set 4 (.bool dirty))))

/-- EventTxn increments each dirty backing once at commit, even if its bytes did not change. -/
def committed (state : State) : R State := do
  let objects ← state.objects.mapM fun entry => do
    if !entry.alive || entry.tag != tag 0 then return entry
    let fs ← fields entry.value
    let version ← num fs 3
    let .bool dirty ← get fs 4 | throw "MemoryDirtyCodec"
    let current ← octets fs 1
    let initial ← octets fs 2
    if fs.length != 5 || current.isEmpty || current.length != initial.length then throw "MemoryState"
    if !dirty then return entry
    if version == 2^64-1 then throw "MemoryVersionOverflow"
    pure {entry with value := .record (fs.set 3 (u (version+1)) |>.set 4 (.bool false))}
  let next := {state with objects}
  checkCapacity next
  pure next

private def testBit (bs : List UInt8) (i : Nat) : Bool := (bs[i/8]?.getD 0).toNat / 2^(i%8) % 2 == 1
private def setBit (bs : List UInt8) (i : Nat) (v : Bool) : List UInt8 :=
  let old := (bs[i/8]?.getD 0).toNat
  let bit := 2^(i%8)
  bs.set (i/8) (UInt8.ofNat (old - (old/bit%2)*bit + if v then bit else 0))
/-- Register descriptor: offset,widthBits,allowedAccessBytes,alignment(0=access width),bigEndian,fields.
    Field descriptor: id,lsb,width,access (RO/RW/WO/W1C/W1S/RC = 0..5). Backing is packed physical bytes. -/
private def register (mid : Nat) (args : List Value) (entry : ObjectEntry) (state : State) : R (List Value × State) := do
  let fs ← fields entry.value
  let data ← octets fs 1; let initial ← octets fs 2; let specs ← row fs 3
  let mut descriptors : List (Nat × Nat × Nat × List Nat × Nat × Bool × List (Nat × Nat × Nat × Nat)) := []
  let mut base := 0
  let mut ids : List Nat := []
  for value in specs do
    let r ← fields value
    let offset ← num r 0; let width ← num r 1; let sizes ← row r 2 >>= List.mapM nat
    let align ← num r 3
    let .bool big ← get r 4 | throw "RegisterEndian"
    if width == 0 || width%8 != 0 || width > 4096 || sizes.isEmpty || sizes.any (fun n => n == 0 || n > width/8) || offset+width/8 > 2^64 then throw "RegisterSpec"
    if descriptors.any (fun (o,w,_,_,_,_,_) => offset < o+w/8 && o < offset+width/8) then throw "RegisterOverlap"
    let mut defs := []
    for field in (← row r 5) do
      let f ← fields field
      let id ← num f 0; let lsb ← num f 1; let bits ← num f 2; let access ← num f 3
      if id ≥ 2^32 || ids.contains id || bits == 0 || lsb+bits > width || access > 5 || defs.any (fun (_,l,w,_) => lsb < l+w && l < lsb+bits) then throw "RegisterField"
      ids := ids ++ [id]; defs := defs ++ [(id,lsb,bits,access)]
    descriptors := descriptors ++ [(offset,width,base,sizes,align,big,defs)]
    base := base+width/8
  if data.length != base || initial.length != base then throw "RegisterBacking"
  let mut next := data
  let mut output : List Value := []
  if mid == 5 then next := initial
  else if mid == 7 || mid == 8 then
    let id ← num args 0
    if id ≥ 2^32 then throw "RegisterFieldId"
    let mut found := false
    for (_,width,start,_,_,big,defs) in descriptors do
      for (field,lsb,bits,_) in defs do
        if id == field then
          found := true
          let physical := fun k => start*8 + (if big then width/8-1-k/8 else k/8)*8 + k%8
          if mid == 7 then
            let mut result := List.replicate ((bits+7)/8) (0 : UInt8)
            for k in List.range bits do result := setBit result k (testBit data (physical (lsb+k)))
            output := [.bytes result]
          else
            let input ← octets args 1
            if input.length != (bits+7)/8 || (bits%8 != 0 && (input.getLast?.getD 0).toNat ≥ 2^(bits%8)) then throw "RegisterFieldWidth"
            for k in List.range bits do next := setBit next (physical (lsb+k)) (testBit input k)
    if !found then throw "RegisterFieldId"
  else
    let cmd ← num args 0; let address ← num args 1; let input ← octets args 2
    let debug := mid == 6
    if !debug && cmd == 2 then return ([.record [u 1,.bytes input]],state)
    if cmd > 1 then return ([.record [u (if debug then 0 else 4),.bytes input]],state)
    let streaming ← if debug then pure input.length else num args 3
    let mask ← if debug then pure [] else octets args 4
    let unchanged := fun n => ([.record [u n,.bytes input]],state)
    if !debug then
      if input.isEmpty || streaming < input.length then return unchanged 5
      if !(mask.all (fun b => b == 0 || b == 255)) then return unchanged 6
      let some (offset,width,_,sizes,align,_,_) := descriptors.find? (fun (o,w,_,_,_,_,_) => o ≤ address && address < o+w/8) | return unchanged 3
      if input.length > width/8-(address-offset) then return unchanged 3
      if !sizes.contains input.length then return unchanged 5
      if address % (if align == 0 then input.length else align) != 0 then return unchanged 3
    let mut result := input
    let mut count := 0
    let mut stopped := false
    for j in List.range (min 65536 input.length) do
      if !stopped then
        let loc := descriptors.find? (fun (o,w,_,_,_,_,_) => o ≤ address+j && address+j < o+w/8 && address+j < 2^64)
        if let some (offset,width,start,_,_,big,defs) := loc then
          count := count+1
          if debug || mask.isEmpty || mask[j%mask.length]?.getD 0 == 255 then
            if cmd == 0 then result := result.set j 0
            let physical := start+(address+j-offset)
            for k in List.range 8 do
              let logical := (if big then width/8-1-(address+j-offset) else address+j-offset)*8+k
              if let some (_,_,_,access) := defs.find? (fun (_,lsb,bits,_) => lsb ≤ logical && logical < lsb+bits) then
                let old := testBit next (physical*8+k)
                let incoming := testBit input (j*8+k)
                if cmd == 0 then
                  if debug || access != 2 then result := setBit result (j*8+k) old
                  if !debug && access == 5 then next := setBit next (physical*8+k) false
                else if debug || access == 1 || access == 2 then next := setBit next (physical*8+k) incoming
                else if access == 3 then next := setBit next (physical*8+k) (old && !incoming)
                else if access == 4 then next := setBit next (physical*8+k) (old || incoming)
        else stopped := true
    output := [.record [u (if debug then count else 1),.bytes result]]
  pure (output,← install state entry (.record (fs.set 1 (.bytes next))))

/-- Resource value carries spec and mutable state: instance,kind,latency,II,capacity,recordLimit,
    ticketStore,nextIssue,lastStart,lastArrival,channels,records. Records retain retired generations. -/
def resourceEntry (c : Context) (objectId latency ii capacity recordLimit : Nat) (pipeline : Bool := false) : ObjectEntry :=
  let kind := if pipeline then 4 else 2
  ⟨⟨.resourceTicket,UInt32.ofNat c.domain,UInt32.ofNat (300+kind),UInt32.ofNat objectId,1,0⟩,tag kind,
    .record [u c.instanceId,u (if pipeline then 2 else if capacity == 1 then 0 else 1),u latency,u ii,u capacity,u recordLimit,u (310+objectId),u 0,u 0,u 0,.vec (List.replicate capacity (u 0)),.vec []],true⟩
private def grant (r : List Value) : R Value := do pure (.record [← get r 2,← get r 3,← get r 4,← get r 1,← get r 5])
private def baseline (c : Context) (entry : ObjectEntry) : R (List Value) := do
  let some (.vec entries) := env c "objects.committed" | throw "ObjectCommittedBaselineRequired"
  let some (.record [_,value]) := entries.find? (fun v => match v with | .record [.handle h,_] => h == entry.identity | _ => false) | throw "ObjectCommittedBaselineMissing"
  fields value
private def resource (mid : Nat) (args : List Value) (c : Context) (entry : ObjectEntry) (state : State) : R (List Value × State) := do
  let mut fs ← fields entry.value
  let kind ← num fs 1; let latency ← num fs 2; let ii ← num fs 3; let capacity ← num fs 4
  let limit ← num fs 5; let store ← num fs 6
  let mut channels ← row fs 10 >>= List.mapM nat
  let mut records ← row fs 11 >>= List.mapM fields
  if kind > 2 || capacity == 0 || limit == 0 || store ≥ 2^32 || channels.length != capacity || records.length > limit || (kind == 0 && capacity != 1) then throw "ResourceState"
  let mut output := []
  if mid == 9 || mid == 10 then
    let owner ← ident args 0; let earliest ← num args 1
    let duration ← if mid == 9 then num args 2 else pure latency
    if owner.kind != .transaction || owner.domain.toNat != c.domain || owner.owner.toNat != c.owner || owner.generation == 0 then throw "ResourceOwner"
    if earliest < c.now || c.now < (← num fs 9) then throw "ResourceArrival"
    let slot := ((records.zipIdx).find? (fun (r,_) => match r with | .bool false::.handle h::_ => h.generation.toNat < 2^64-1 | _ => false)).map (·.2) |>.getD records.length
    if slot ≥ limit then throw "ResourceCapacity"
    let channel := ((channels.zipIdx).foldl (fun best x => if x.1 < best.1 then x else best) ((channels.head?.getD 0),0)).2
    let issue ← num fs 7
    let start := max earliest (max (← num fs 8) (max (channels[channel]?.getD 0) (if kind == 2 then issue else 0)))
    let finish := start+duration; let nextIssue := start+(if kind == 2 then ii else 0)
    if finish ≥ 2^64 || nextIssue ≥ 2^64 then throw "ResourceTimeOverflow"
    let generation ← if slot < records.length then do pure ((← ident (← get records slot) 1).generation.toNat+1) else pure 1
    let ticket : HandleIdentity := ⟨.resourceTicket,UInt32.ofNat c.domain,UInt32.ofNat store,UInt32.ofNat slot,UInt64.ofNat generation,UInt64.ofNat c.owner⟩
    let r := [.bool true,.handle ticket,u start,u finish,u (start-earliest),u channel,.bool false]
    records := if slot == records.length then records ++ [r] else records.set slot r
    channels := channels.set channel finish
    fs := fs.set 7 (u nextIssue) |>.set 8 (u start) |>.set 9 (u c.now)
    output := [← grant r]
  else if mid == 13 then
    let mut grants := []
    for r in records do if (← get r 0) == .bool true then grants := grants ++ [← grant r]
    output := [.vec grants]
  else
    let ticket ← ident args 0
    if ticket.kind != .resourceTicket || ticket.store.toNat != store || ticket.domain.toNat != c.domain || ticket.owner.toNat != c.owner then throw "ResourceTicketOwner"
    let slot := ticket.slot.toNat
    let mut r ← get records slot
    if (← ident r 1) != ticket then throw "ResourceStaleTicket"
    let active := (← get r 0) == .bool true
    let cancelled := (← get r 6) == .bool true
    if mid == 12 then
      if (← num args 1) != c.now then throw "ResourceCompletionTime"
      if !active then output := [.bool false]
      else
        if c.now < (← num r 3) then throw "ResourceNotReady"
        r := r.set 0 (.bool false); output := [.bool (!cancelled)]
    else if mid == 11 then
      if !active then output := [u 4]
      else if cancelled then output := [u 3]
      else
        r := r.set 6 (.bool true)
        let old ← baseline c entry
        let oldRecords ← row old 11
        let committed := match oldRecords[slot]? with
          | some (.record (.bool true::.handle h::_)) => h == ticket
          | _ => false
        if committed then output := [u (if c.now ≥ (← num r 2) then 2 else 1)]
        else
          r := r.set 0 (.bool false)
          channels ← row old 10 >>= List.mapM nat
          fs := fs.set 7 (← get old 7) |>.set 8 (← get old 8)
          records := records.set slot r
          for live in records do
            if (← get live 0) == .bool true then
              let ch ← num live 5; let finish ← num live 3; let start ← num live 2
              channels := channels.set ch (max (channels[ch]?.getD 0) finish)
              fs := fs.set 8 (u (max (← num fs 8) start)) |>.set 7 (u (max (← num fs 7) (start+if kind == 2 then ii else 0)))
          output := [u 0]
    else throw "ResourceMethod"
    records := records.set slot r
  fs := fs.set 10 (.vec (channels.map u)) |>.set 11 (.vec (records.map Value.record))
  pure (output,← install state entry (.record fs))

/-- Queue entry rows: id,element option,owner option,published,consumer-pair option. -/
def queueEntry (c : Context) (objectId capacity maxValueBytes scanBudget consumerOwner : Nat) : ObjectEntry :=
  ⟨⟨.resourceTicket,UInt32.ofNat c.domain,303,UInt32.ofNat objectId,1,0⟩,tag 3,
    .record [u c.instanceId,u capacity,u maxValueBytes,u scanBudget,u consumerOwner,u 1,.vec []],true⟩
private def liveOwner (c : Context) (s : State) (h : HandleIdentity) (kind : HandleKind) : R Unit := do
  if h.kind != kind || h.domain.toNat != c.domain || h.generation == 0 || !s.objects.any (fun e => e.alive && e.identity == h) then throw "QueueOwner"
section
local instance : MonadLift (Except String) Attempt where
  monadLift := fromExcept

private def queueAttempt (mid : Nat) (args : List Value) (c : Context) (entry : ObjectEntry) : Attempt (List Value) := do
  let mut fs ← fields entry.value
  let cap ← num fs 1; let maxBytes ← num fs 2; let scan ← num fs 3; let owner ← num fs 4
  let mut nextId ← num fs 5
  let mut rows ← fromExcept (row fs 6 >>= List.mapM fields)
  if cap == 0 || maxBytes == 0 || scan == 0 || rows.length > cap then throw "QueueState"
  let mut state ← getThe State
  let mut output := []
  if mid == 14 || mid == 19 then
    if mid == 19 then
      let policy ← num args 2; let consumer ← ident args 1
      if policy != 1 && policy != 2 then throw "QueueOwnershipPolicy"
      if consumer.owner.toNat != c.owner then throw "QueueConsumerOwner"
    if mid == 14 && nativeValueBytes (nodeBytes c) (← get args 0) > maxBytes then throw "QueueElementBytes"
    if rows.length == cap then return [.bool false]
    if nextId ≥ 2^64-1 then throw "QueueSequence"
    let mut element := option (some (← get args 0))
    let mut consumer := option none
    if mid == 19 then
      let result ← ident args 0; let old ← ident args 1; let policy ← num args 2
      if policy != 1 && policy != 2 then throw "QueueOwnershipPolicy"
      let fresh ← if policy == 1 then Storage.transferResultAttempt c result old owner else Storage.retainResultAttempt c result old owner
      state ← getThe State
      consumer := option (some (.record [.handle result,.handle fresh])); element := option none
    rows := rows ++ [[u nextId,element,option none,.bool false,consumer]]
    nextId := nextId+1; output := [.bool true]
  else if mid == 15 then
    if let r::tail := rows then
      let mut consumer ← get r 4
      if let .variant 1 [.record [.handle result,.handle old]] := consumer then
        let fresh ← Storage.transferResultAttempt {c with owner := old.owner.toNat} result old c.owner
        state ← getThe State
        consumer := option (some (.record [.handle result,.handle fresh]))
      let rawOwner ← get r 2
      let ownerValue := match rawOwner with
        | .variant 1 [.handle h] => option (some (.variant (if h.kind == .scope then 0 else 1) [.handle h]))
        | _ => option none
      output := [option (some (.record [← get r 1,consumer,ownerValue,← get r 0]))]
      rows := tail
    else output := [option none]
  else if mid == 16 then output := [u rows.length]
  else if mid == 20 || mid == 21 then
    let scope ← ident args 0
    liveOwner c state scope .scope
    let drain ← if mid == 21 then ident args 1 else pure scope
    if mid == 21 then liveOwner c state drain .drain
    if rows.length > scan then throw "QueueScanBudget"
    let limit ← if mid == 21 then num args 2 else pure cap
    if mid == 21 && (rows.filter (fun r => r[2]? == some (option (some (.handle scope))) && r[3]? == some (.bool true))).length > limit then throw "QueueDrainCapacity"
    let mut report := []; let mut keep := []
    for r in rows do
      let matching := (← get r 2) == option (some (.handle scope)) && (← get r 3) == .bool (mid == 21)
      if matching then
        if report.length == limit then throw "QueueDrainCapacity"
        report := report ++ [← get r 0]
        let mut updated := r
        if let .variant 1 [.record [.handle result,.handle consumer]] := (← get r 4) then
          let ownerContext := {c with owner := consumer.owner.toNat}
          if mid == 20 then
            Storage.releaseResultAttempt ownerContext result consumer
            state ← getThe State
          else
            let fresh ← Storage.transferResultAttempt ownerContext result consumer drain.owner.toNat
            state ← getThe State
            updated := updated.set 4 (option (some (.record [.handle result,.handle fresh])))
        if mid == 21 then keep := keep ++ [updated.set 2 (option (some (.handle drain)))]
      else keep := keep ++ [r]
    rows := keep; output := [.vec report]
  else throw "QueueMethod"
  fs := fs.set 5 (u nextId) |>.set 6 (.vec (rows.map Value.record))
  updateChecked (fun current => install current entry (.record fs))
  pure output

private def epoch (c : Context) : R Nat := do
  let some v := env c "objects.epoch" | throw "ObjectEpochRequired"
  nat v
private def pipelineAttempt (mid : Nat) (args : List Value) (c : Context) (entry : ObjectEntry) : Attempt (List Value) := do
  if mid == 13 then return ← modifyChecked (fun state => resource 13 args c entry state)
  if mid == 17 then
    let grants ← modifyChecked (fun state => resource 10 args c entry state)
    let g ← get grants 0
    let row ← fields g
    let finish ← num row 1; let ticket ← ident row 3
    let event ← enqueueAttempt c finish 3 c.connection "object.pipeline.ready" [.handle ticket,u (← epoch c)]
    pure [.record [g,← get args 0,.handle event,u (← epoch c)]]
  else
    let p ← row args 0
    let g ← row p 0; let ticket ← ident g 3; let ready ← ident p 2
    let owner ← ident p 1; let generation ← num p 3
    if mid == 11 then
      let out ← modifyChecked (fun state => resource 11 [.handle ticket] c entry state)
      if out == [u 0] then cancelEventAttempt c ready
      pure out
    else if mid == 18 then
      let received ← ident args 1
      let some (.handle active) := env c "objects.activeEvent" | throw "PipelineActiveEventRequired"
      let state ← getThe State
      let some event := state.events.find? (fun e => e.identity == received) | throw "PipelineEventMissing"
      if active != received || received != ready || event.time != c.now || event.turn != c.turn || event.values != [.handle ticket,u generation] || owner.owner != ticket.owner then throw "PipelineEventMismatch"
      let result ← modifyChecked (fun state => resource 12 [.handle ticket,u c.now] c entry state)
      let currentEpoch ← epoch c
      pure [.bool (result == [.bool true] && generation == currentEpoch)]
    else throw "PipelineMethod"

/-- Service attempt records successful native allocations before any later failure. -/
def invokeAttempt (types : TypeEnvironment) (context : Context) (signature : ServiceSignature)
    (arguments : List Value) : Attempt (List Value) := do
  let state ← getThe State
  let (kind,obj,mid) ← resolve types signature
  checkContext context
  checkCapacity state
  if nodeBytes context == 0 || nodeBytes context > 4096 then throw "ObjectValueABIRequired"
  if signature.contextMask / 2^context.kind % 2 != 1 then throw "ObjectContext"
  if arguments.length != signature.inputTypes.length || !(arguments.zip signature.inputTypes).all (fun (v,id) => (types[id]?).any (fun t => conforms types 65 t v)) then throw "ObjectArgumentType"
  if (arguments.map (nativeValueBytes (nodeBytes context))).sum > 65536 then throw "ObjectArgumentBudget"
  let entry ← lookup state context kind obj
  hostContract types context signature kind mid entry
  let output ← match kind with
    | 0 => modifyChecked (fun state => memory mid arguments entry state)
    | 1 => modifyChecked (fun state => register mid arguments entry state)
    | 2 => modifyChecked (fun state => resource mid arguments context entry state)
    | 3 => queueAttempt mid arguments context entry
    | 4 => pipelineAttempt mid arguments context entry
    | _ => throw "ObjectKind"
  if output.length != signature.resultTypes.length || !(output.zip signature.resultTypes).all (fun (v,id) => (types[id]?).any (fun t => conforms types 65 t v)) then throw "ObjectResultType"
  if rawOutputBytes context mid output > 65536 then throw "ObjectResultBudget"
  checkCapacity (← getThe State)
  pure output

/-- Compatibility interface for pure single-call checks; runners use invokeAttempt. -/
def invoke (types : TypeEnvironment) (context : Context) (signature : ServiceSignature)
    (arguments : List Value) (state : State) : R (List Value × State) :=
  attemptExcept ((invokeAttempt types context signature arguments).run state)

/-- Segment combinator used in lifecycle fixtures: failures return the original state. -/
def atomic (types : TypeEnvironment) (context : Context) (calls : List (ServiceSignature × List Value))
    (state : State) : Except String (List (List Value)) × State :=
  match (calls.mapM (fun (sig,args) => invokeAttempt types context sig args)).run state with
  | .error e draft => (.error e,rollbackAllocations state draft)
  | .ok out next => match committed next with
    | .ok done => (.ok out,done)
    | .error error => (.error error,rollbackAllocations state next)
end
end LeanAT.Reference.Objects








