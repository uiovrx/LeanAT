import LeanAT.Compiler.Descriptor

namespace LeanAT.Compiler
open ExecIR

structure LoadPolicy where
  maxFileBytes : Nat := 16777216
  maxRows : Nat := 65536
  maxStringBytes : Nat := 65536
  maxDepth : Nat := 64
  maxWork : Nat := 1000000
  expectedProfile : String := "AT-Core-1.1-draft"
  allowedCapabilities : List String := []
  deriving Inhabited
private structure Reader where
  bytes : ByteArray
  offset : Nat := 0
  work : Nat := 0
  policy : LoadPolicy := {}
private abbrev Read := StateT Reader (Except String)
private def take (n : Nat) : Read ByteArray := do
  let r ← get
  if n > r.bytes.size - r.offset then throw "TruncatedInput"
  if r.work+n > r.policy.maxWork then throw "BudgetExceeded"
  set {r with offset := r.offset+n, work := r.work+n}
  pure (r.bytes.extract r.offset (r.offset+n))
private def number (n : Nat) : Read Nat := do
  let b ← take n
  pure ((List.range n).foldl (fun v i => v+b[i]!.toNat*256^i) 0)
private def u32 := number 4
private def string : Read String := do
  let n ← u32
  if n > (← get).policy.maxStringBytes then throw "StringLimit"
  let bytes ← take n
  match String.fromUTF8? bytes with | some s => pure s | none => throw "InvalidUtf8"
private def list (read : Read α) : Read (List α) := do
  let n ← u32
  if n > (← get).policy.maxRows then throw "TableLimit"
  let mut xs := []
  for _ in [:n] do xs := (← read)::xs
  pure xs.reverse
private def register : Read VReg := return ⟨← u32,← u32⟩
private def handleKind (tag : Nat) : Except String HandleKind :=
  match ([.transaction,.hop,.event,.process,.wait,.result,.consumer,.scope,.task,.access,.lease,.resourceTicket,.spawnTicket,.drain,.gateTicket] : List HandleKind)[tag]? with
  | some k => .ok k | none => .error "UnknownHandleKind"
private def readType : Read TypeSchema := do
  let tag ← number 1
  let bound ← number 8
  let fs ← list u32
  let cs ← list (list u32)
  match tag with
  | 0 => pure .unit | 1 => pure .bool | 2 => pure (.bits bound) | 3 => pure (.fin bound)
  | 4 => pure (.record fs) | 5 => pure (.variant cs)
  | 8 => pure (.bytes bound)
  | 9 => return .handle (← handleKind bound)
  | 6 | 7 =>
    let [t] := fs | throw "VectorElementLayout"
    pure (if tag == 6 then .vec t bound else .boundedVec t bound)
  | _ => throw "UnknownType"
private def readValue : Nat → Read Value
  | 0 => throw "DepthLimit"
  | depth+1 => do
    match ← number 1 with
    | 0 => pure .unit
    | 1 =>
      let b ← number 1
      if b > 1 then throw "NonCanonicalBool"
      pure (.bool (b == 1))
    | 2 => return .bits (← u32) (← number 8)
    | 3 => return .record (← list (readValue depth))
    | 4 => return .variant (← number 8) (← list (readValue depth))
    | 5 => return .vec (← list (readValue depth))
    | 6 => return .bytes (← list (return UInt8.ofNat (← number 1)))
    | 7 => do
      let kind ← handleKind (← number 1)
      let domain := UInt32.ofNat (← u32)
      let store := UInt32.ofNat (← u32)
      let slot := UInt32.ofNat (← u32)
      let generation := UInt64.ofNat (← number 8)
      let owner := UInt64.ofNat (← number 8)
      pure (.handle ⟨kind,domain,store,slot,generation,owner⟩)
    | _ => throw "UnknownValue"
private def op (n : Nat) : Except String Op :=
  match allOps[n]? with | some opcode => .ok opcode | none => .error "UnsupportedOpcode"
private def binary (n : Nat) : Except String Pure.BinaryOp :=
  match n with
  | 0 => .ok .addWrap | 1 => .ok .subWrap | 2 => .ok .mulWrap | 3 => .ok .addChecked
  | 4 => .ok .divChecked | 5 => .ok .eq | 6 => .ok .lt | 7 => .ok .and | 8 => .ok .or
  | _ => .error "UnknownBinaryOperator"
private def readInstruction : Read Instruction := do
  let opcode ← op (← number 1)
  let args ← list register
  let present ← number 1
  if present > 1 then throw "NonCanonicalBool"
  let dest ← if present == 1 then (do pure (some (← register))) else pure none
  let value ← readValue (← get).policy.maxDepth
  let immediate ← u32
  let operator ← binary (← number 1)
  let text ← string
  let source ← string
  pure {op := opcode,args,dest,value,immediate,operator,text,source}
private def readEdge : Read Edge := return ⟨← u32,← list register⟩
private def readTerm : Read Terminator := do
  match ← number 1 with
  | 0 => return .jump (← readEdge)
  | 1 => return .branch (← register) (← readEdge) (← readEdge)
  | 2 => return .switch (← register) (← list (return (← number 8,← readEdge))) (← readEdge)
  | 3 => return .ret (← list register)
  | 4 => return .fail (← string)
  | 5 => return .transportReturn (← register)
  | 6 => return .suspend (← register) (← u32) (← list register)
  | _ => throw "UnsupportedTerminator"
private def readBlock : Read Block := return ⟨← u32,← list register,← list readInstruction,← readTerm⟩
private def readProgram (version : Nat) : Read Program := do
  let p : Program := {id := ← u32,inputTypes := ← list u32,resultTypes := ← list u32,blocks := ← list readBlock,entry := ← u32,source := ← string}
  if version == 1 then pure p else do
    let context ← number 1
    let frame ← list (do
      let typeId ← u32
      let offset ← u32
      let alignment ← u32
      let registerId ← if version ≥ 3 then u32 else pure 0
      pure ({typeId,offset,alignment,registerId} : FrameSlot))
    let frameBytes ← u32
    let effectMask ← number 8
    let instructionFuel ← if version ≥ 5 then number 8 else pure 0
    let ownerPolicy ← if version ≥ 5 then string else pure ""
    let resultLifetimePolicy ← if version ≥ 5 then string else pure ""
    pure {p with context,frame,frameBytes,effectMask,instructionFuel,ownerPolicy,resultLifetimePolicy}
private def readService : Read ServiceSignature := do
  return ⟨← u32,← op (← number 1),← list u32,← list u32,← u32,← number 8,← number 8,← string,← string,← take 32⟩

private def optional (read : Read α) : Read (Option α) := do
  match ← number 1 with
  | 0 => pure none | 1 => return some (← read) | _ => throw "NonCanonicalOption"
private def boolean : Read Bool := do
  match ← number 1 with | 0 => pure false | 1 => pure true | _ => throw "NonCanonicalBool"
private def sourceSpan : Read SourceSpan := return ⟨← string,← u32,← u32⟩
private def ownedValue : Read Value := do readValue (← get).policy.maxDepth
private def readParameter : Read ModelIR.ParamDeclIR := return ⟨← u32,← u32,← optional ownedValue,← sourceSpan⟩
private def readState : Read ModelIR.StateSlot := return ⟨← u32,← u32,← ownedValue⟩
private def readPort : Read ModelIR.PortDeclIR := do
  let id ← u32
  let direction ← number 1
  if direction > 1 then throw "InvalidDirection"
  return ⟨id,if direction == 0 then .input else .output,← u32,← optional ownedValue,← sourceSpan⟩
private def readEndpoint : Read ModelIR.EndpointIR := do
  let id ← u32
  let role ← number 1
  if role > 1 then throw "InvalidEndpointRole"
  return ⟨id,if role == 0 then .initiator else .target,← u32,← u32,← u32,← u32,← u32,← string,← sourceSpan⟩
private def readCapacity : Read ModelIR.ProcessCapacityIR := do
  let maxInstances ← u32
  let frameBytesLimit ← u32
  let resultCapacity ← u32
  let mode ← number 1
  let bound ← u32
  let overflow ← match mode with
    | 0 => if bound == 0 then pure .reject else throw "NonCanonicalOverflow"
    | 1 => pure (.awaitSlot bound) | 2 => pure (.queue bound) | _ => throw "InvalidOverflowPolicy"
  pure {maxInstances,frameBytesLimit,resultCapacity,overflow}
private def readHandlerBinding : Read HandlerBinding := return ⟨← u32,← u32,← number 1,← string,← optional u32,← optional readCapacity,← string⟩
private def readComponent : Read ComponentDesc := return ⟨← u32,← list readParameter,← list readState,← list readEndpoint,← list readPort,← string,← string⟩
private def readConfig : Read (Nat × Value) := return (← u32,← ownedValue)
private def readInstanceDesc : Read InstanceDesc := return ⟨← u32,← u32,← u32,← u32,← list readHandlerBinding,← list readConfig,← string⟩
private def readOriginalInstance : Read ModelIR.InstanceIR := return ⟨⟨← u32⟩,⟨← u32⟩,← list readConfig,← sourceSpan⟩
private def readEndpointRef : Read ModelIR.EndpointRef := return ⟨⟨← u32⟩,← u32,← u32⟩
private def readBinding : Read ModelIR.BindingIR := return ⟨⟨← u32⟩,← readEndpointRef,← readEndpointRef,← sourceSpan⟩
private def readPortRef : Read ModelIR.SidebandPortRef := do
  let tag ← number 1
  let id ← u32
  let port ← u32
  match tag with
  | 0 => if id == 0 then pure (.top port) else throw "NonCanonicalTopPort"
  | 1 => pure (.instance ⟨id⟩ port) | _ => throw "InvalidPortRef"
private def readSidebandBinding : Read ModelIR.SidebandBindingIR := return ⟨← u32,← readPortRef,← readPortRef,← sourceSpan⟩
private def readAddressMap : Read ModelIR.AddressMapIR := return ⟨← u32,⟨← u32⟩,← u32,← u32,← number 8,← number 8,← number 8,← boolean,← sourceSpan⟩
private def readSystemMetadata : Read ModelIR.SystemIR := return ⟨← u32,← list readOriginalInstance,← list readBinding,← list readPort,← list readSidebandBinding,← list readAddressMap,← u32,← sourceSpan⟩
private def readExternalContract : Read ModelIR.ExternalContractIR := return ⟨← u32,← string,← string,← string,← list (return (← string,← ownedValue)),← list string,← list string,← list string,← list string,← sourceSpan⟩
private def readMetadata (p : ExecProject) : Read ExecProject := do
  let components ← list readComponent
  let instances ← list readInstanceDesc
  let systemMetadata ← optional readSystemMetadata
  let externalContracts ← list readExternalContract
  let capabilities ← list string
  pure {p with components,instances,systemMetadata,externalContracts,capabilities}

def deserialize (bytes : ByteArray) (policy : LoadPolicy := {}) : Except String ValidatedProject := do
  if bytes.size > policy.maxFileBytes then throw "FileLimit"
  if bytes.size < 104 then throw "TruncatedInput"
  let read : Read ExecProject := do
    if (← take 4) != "LATR".toUTF8 then throw "InvalidMagic"
    let version ← number 2
    if (version < 1 || version > 5) || (← number 2) != 0 then throw "UnsupportedSchema"
    if (← number 8) != bytes.size then throw "FileLengthMismatch"
    if (← u32) != 1 || (← u32) != 1 || (← number 8) != 104 || (← number 8) != bytes.size-104 then throw "SectionDirectory"
    let schema := if version == 5 then "LeanAT.ExecIR.v5" else if version == 4 then "LeanAT.ExecIR.v4" else if version == 3 then "LeanAT.ExecIR.v3" else if version == 2 then "LeanAT.ExecIR.v2" else "LeanAT.ExecIR.v1.core16"
    if (← take 32) != computeArtifactHash schema.toUTF8 then throw "SchemaHashMismatch"
    let expected ← take 32
    let zeros : ByteArray := ⟨Array.replicate 32 0⟩
    if expected != computeArtifactHash (bytes.extract 0 72 ++ zeros ++ bytes.extract 104 bytes.size) then throw "HashMismatch"
    let profile ← string
    let rawTypes ← list readType
    let types := if version ≥ 5 then rawTypes.map (fun t => match t with | .fin 0 => .fin (2^64) | _ => t) else rawTypes
    let stateTypes ← list u32
    let initialState ← list (readValue policy.maxDepth)
    let programs ← list (readProgram version)
    let services ← if version ≥ 2 then list readService else pure []
    let project : ExecProject := {types,stateTypes,initialState,programs,profile,schemaMajor := version,services}
    if version ≥ 4 then readMetadata project else pure project
  let (p,r) ← read.run {bytes,policy}
  if p.profile != policy.expectedProfile then throw "ProfileMismatch"
  if !p.capabilities.all policy.allowedCapabilities.contains then throw "CapabilityDeniedByHostPolicy"
  if r.offset != bytes.size then throw "TrailingBytes"
  let _ ← typeLayouts p.types policy.maxDepth policy.maxWork
  let validated ← validateExec p
  if serialize validated != bytes then throw "NonCanonicalEncoding"
  pure validated

end LeanAT.Compiler
