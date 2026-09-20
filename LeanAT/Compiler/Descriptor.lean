import LeanAT.Compiler.Lowering

namespace LeanAT.Compiler
open ExecIR

private def le (n width : Nat) : ByteArray := ⟨(List.range width).toArray.map (fun i => UInt8.ofNat (n / 256^i % 256))⟩
private def cat (xs : List ByteArray) : ByteArray := xs.foldl (· ++ ·) ByteArray.empty
private def vector (f : α → ByteArray) (xs : List α) := le xs.length 4 ++ cat (xs.map f)
private def str (s : String) := le s.toUTF8.size 4 ++ s.toUTF8
private def word (n : Nat) := le n 4
private def reg (r : VReg) := word r.id ++ word r.typeId

private def roundConstants : Array UInt32 := #[
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]
private def rotr (x : UInt32) (n : UInt32) := (x >>> n) ||| (x <<< (32-n))
private def be (n width : Nat) : ByteArray := ⟨(List.range width).toArray.map (fun i => UInt8.ofNat (n / 256^(width-1-i) % 256))⟩

/-- SHA-256 over owned bytes; the artifact hash has no recursive self-reference. -/
def computeArtifactHash (bytes : ByteArray) : ByteArray := Id.run do
  let zeroCount := (64 - ((bytes.size + 9) % 64)) % 64
  let padded := bytes ++ le 128 1 ++ le 0 zeroCount ++ be (bytes.size*8) 8
  let mut hash : Array UInt32 := #[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]
  for block in [:padded.size / 64] do
    let mut w : Array UInt32 := #[]
    for i in [:16] do
      let offset := block*64+i*4
      let x := (List.range 4).foldl (fun n k => n*256+(padded[offset+k]!).toNat) 0
      w := w.push (UInt32.ofNat x)
    for i in [16:64] do
      let x := w[i-15]!
      let y := w[i-2]!
      let s0 := rotr x 7 ^^^ rotr x 18 ^^^ (x >>> 3)
      let s1 := rotr y 17 ^^^ rotr y 19 ^^^ (y >>> 10)
      w := w.push (w[i-16]! + s0 + w[i-7]! + s1)
    let mut a := hash[0]!
    let mut b := hash[1]!
    let mut c := hash[2]!
    let mut d := hash[3]!
    let mut e := hash[4]!
    let mut f := hash[5]!
    let mut g := hash[6]!
    let mut h := hash[7]!
    for i in [:64] do
      let s1 := rotr e 6 ^^^ rotr e 11 ^^^ rotr e 25
      let choice := (e &&& f) ^^^ (~~~e &&& g)
      let t1 := h + s1 + choice + roundConstants[i]! + w[i]!
      let s0 := rotr a 2 ^^^ rotr a 13 ^^^ rotr a 22
      let maj := (a &&& b) ^^^ (a &&& c) ^^^ (b &&& c)
      h := g; g := f; f := e; e := d+t1
      d := c; c := b; b := a; a := t1+s0+maj
    let working := #[a,b,c,d,e,f,g,h]
    hash := hash.mapIdx (fun i x => x+working[i]!)
  return cat (hash.toList.map (fun n => be n.toNat 4))

def hex (bytes : ByteArray) : String :=
  String.ofList (bytes.data.toList.flatMap (fun b =>
    let alphabet := "0123456789abcdef".toList
    [alphabet[b.toNat / 16]!,alphabet[b.toNat % 16]!]))

def handleKindTag : HandleKind → Nat
  | .transaction => 0 | .hop => 1 | .event => 2 | .process => 3 | .wait => 4 | .result => 5
  | .consumer => 6 | .scope => 7 | .task => 8 | .access => 9 | .lease => 10
  | .resourceTicket => 11 | .spawnTicket => 12 | .drain => 13 | .gateTicket => 14
private def encodeType (t : TypeSchema) : ByteArray :=
  let (tag,bound,fields,cases) := match t with
    | .unit => (0,0,[],[]) | .bool => (1,0,[],[]) | .bits w => (2,w,[],[])
    | .fin n => (3,if n == 2^64 then 0 else n,[],[]) | .record fs => (4,0,fs,[]) | .variant cs => (5,0,[],cs)
    | .vec t n => (6,n,[t],[]) | .boundedVec t n => (7,n,[t],[])
    | .bytes n => (8,n,[],[]) | .handle k => (9,handleKindTag k,[],[])
  le tag 1 ++ le bound 8 ++ vector word fields ++ vector (vector word) cases
private partial def encodeValue : Value → ByteArray
  | .unit => le 0 1
  | .bool b => le 1 1 ++ le (if b then 1 else 0) 1
  | .bits w n => le 2 1 ++ word w ++ le n 8
  | .record vs => le 3 1 ++ vector encodeValue vs
  | .variant t vs => le 4 1 ++ le t 8 ++ vector encodeValue vs
  | .vec vs => le 5 1 ++ vector encodeValue vs
  | .bytes bs => le 6 1 ++ vector (fun b => le b.toNat 1) bs
  | .handle h => cat [le 7 1,le (handleKindTag h.kind) 1,word h.domain.toNat,word h.store.toNat,word h.slot.toNat,le h.generation.toNat 8,le h.owner.toNat 8]
private def binaryTag : Pure.BinaryOp → Nat
  | .addWrap => 0 | .subWrap => 1 | .mulWrap => 2 | .addChecked => 3 | .divChecked => 4
  | .eq => 5 | .lt => 6 | .and => 7 | .or => 8
private def encodeInstruction (i : Instruction) := cat [le i.op.tag 1,vector reg i.args,
  (match i.dest with | none => le 0 1 | some r => le 1 1 ++ reg r), encodeValue i.value,
  word i.immediate,le (binaryTag i.operator) 1,str i.text,str i.source]
private def edge (e : Edge) := word e.target ++ vector reg e.args
private def term : Terminator → ByteArray
  | .jump e => le 0 1 ++ edge e
  | .branch r y n => cat [le 1 1,reg r,edge y,edge n]
  | .switch r cs d => cat [le 2 1,reg r,vector (fun (n,e) => le n 8 ++ edge e) cs,edge d]
  | .ret rs => le 3 1 ++ vector reg rs
  | .fail error => le 4 1 ++ str error
  | .transportReturn r => le 5 1 ++ reg r
  | .suspend wait resume live => cat [le 6 1,reg wait,word resume,vector reg live]
private def block (b : Block) := cat [word b.id,vector reg b.parameters,vector encodeInstruction b.instructions,term b.terminator]
private def program (version : Nat) (p : Program) :=
  cat [word p.id,vector word p.inputTypes,vector word p.resultTypes,vector block p.blocks,word p.entry,str p.source] ++
  (if version ≥ 2 then cat [le p.context 1,vector (fun s => cat [word s.typeId,word s.offset,word s.alignment] ++ (if version ≥ 3 then word s.registerId else ByteArray.empty)) p.frame,word p.frameBytes,le p.effectMask 8] else ByteArray.empty) ++
  (if version ≥ 5 then cat [le p.instructionFuel 8,str p.ownerPolicy,str p.resultLifetimePolicy] else ByteArray.empty)
private def service (s : ServiceSignature) := cat [word s.id,le s.op.tag 1,vector word s.inputTypes,vector word s.resultTypes,word s.contextMask,le s.effectMask 8,le s.extraFuel 8,str s.providerKey,str s.providerVersion,s.abiHash]

private def option (f : α → ByteArray) : Option α → ByteArray
  | none => le 0 1 | some value => le 1 1 ++ f value
private def span (s : SourceSpan) := cat [str s.file,word s.line,word s.column]
private def parameter (p : ModelIR.ParamDeclIR) := cat [word p.id,word p.typeId,option encodeValue p.defaultValue,span p.source]
private def state (s : ModelIR.StateSlot) := cat [word s.id,word s.typeId,encodeValue s.initial]
private def port (p : ModelIR.PortDeclIR) := cat [word p.id,le (if p.direction == .input then 0 else 1) 1,word p.typeId,option encodeValue p.initial,span p.source]
private def endpoint (e : ModelIR.EndpointIR) := cat [word e.id,le (if e.role == .initiator then 0 else 1) 1,word e.busWidth,word e.maxBindings,word e.maxOutstanding,word e.maxPayloadBytes,word e.maxByteEnableBytes,str e.protocolRef,span e.source]
private def capacity (c : ModelIR.ProcessCapacityIR) :=
  let (mode,bound) := match c.overflow with | .reject => (0,0) | .awaitSlot n => (1,n) | .queue n => (2,n)
  cat [word c.maxInstances,word c.frameBytesLimit,word c.resultCapacity,le mode 1,word bound]
private def handlerBinding (h : HandlerBinding) := cat [word h.localId,word h.programId,le h.context 1,str h.trigger,option word h.endpoint,option capacity h.processCapacity,str h.source]
private def component (c : ComponentDesc) := cat [word c.id,vector parameter c.parameters,vector state c.states,vector endpoint c.endpoints,vector port c.sidebands,str c.resetPolicy,str c.source]
private def config (c : Nat × Value) := word c.1 ++ encodeValue c.2
private def instanceDesc (i : InstanceDesc) := cat [word i.id,word i.definition,word i.stateBase,word i.stateCount,vector handlerBinding i.handlers,vector config i.resolvedConfig,str i.source]
private def originalInstance (i : ModelIR.InstanceIR) := cat [word i.id.value,word i.definition.value,vector config i.resolvedConfig,span i.source]
private def endpointRef (r : ModelIR.EndpointRef) := cat [word r.instanceId.value,word r.endpoint,word r.bindingIndex]
private def binding (b : ModelIR.BindingIR) := cat [word b.id.value,endpointRef b.sourceEndpoint,endpointRef b.sinkEndpoint,span b.source]
private def portRef : ModelIR.SidebandPortRef → ByteArray
  | .top port => cat [le 0 1,word 0,word port]
  | .instance id port => cat [le 1 1,word id.value,word port]
private def sidebandBinding (b : ModelIR.SidebandBindingIR) := cat [word b.id,portRef b.sourcePort,portRef b.sinkPort,span b.source]
private def addressMap (m : ModelIR.AddressMapIR) := cat [word m.id,word m.decoder.value,word m.outputEndpoint,word m.bindingIndex,le m.sourceStart 8,le m.size 8,le m.targetStart 8,le (if m.aliasDeclared then 1 else 0) 1,span m.source]
private def systemMetadata (s : ModelIR.SystemIR) := cat [word s.id,vector originalInstance s.instances,vector binding s.bindings,vector port s.topPorts,vector sidebandBinding s.sidebandBindings,vector addressMap s.addressMaps,word s.runtimeDomain,span s.source]
private def externalContract (e : ModelIR.ExternalContractIR) := cat [word e.id,str e.cppType,str e.header,str e.library,vector (fun (name,value) => str name ++ encodeValue value) e.constructorMapping,vector str e.requires,vector str e.ensures,vector str e.assumptions,vector str e.evidence,span e.source]
private def metadata (p : ExecProject) := cat [vector component p.components,vector instanceDesc p.instances,option systemMetadata p.systemMetadata,vector externalContract p.externalContracts,vector str p.capabilities]

def serialize (p : ValidatedProject) : ByteArray := Id.run do
  let p := p.project
  let body := cat [str p.profile,vector encodeType p.types,vector word p.stateTypes,vector encodeValue p.initialState,vector (program p.schemaMajor) p.programs] ++
    (if p.schemaMajor ≥ 2 then vector service p.services else ByteArray.empty) ++ (if p.schemaMajor ≥ 4 then metadata p else ByteArray.empty)
  let schema := if p.schemaMajor == 5 then "LeanAT.ExecIR.v5" else if p.schemaMajor == 4 then "LeanAT.ExecIR.v4" else if p.schemaMajor == 3 then "LeanAT.ExecIR.v3" else if p.schemaMajor == 2 then "LeanAT.ExecIR.v2" else "LeanAT.ExecIR.v1.core16"
  let header := cat ["LATR".toUTF8,le p.schemaMajor 2,le 0 2,le (104+body.size) 8,word 1,word 1,le 104 8,le body.size 8,
    computeArtifactHash schema.toUTF8,le 0 32]
  let bytes := header ++ body
  let integrity := computeArtifactHash bytes
  return bytes.extract 0 72 ++ integrity ++ bytes.extract 104 bytes.size

end LeanAT.Compiler
