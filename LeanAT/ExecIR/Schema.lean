import LeanAT.ModelIR.Schema

namespace LeanAT.ExecIR

instance : Repr ByteArray where
  reprPrec bytes _ := repr bytes.data

structure VReg where
  id : Nat
  typeId : Nat
  deriving Repr, BEq, Inhabited

inductive Op where
  | const | move | binary | makeRecord | getField | makeVariant | variantTag | variantGet
  | makeVec | vecGet | vecSet | selectValue | loadState | bufferStateWrite | check | trace
  | unary | compare | convert | callPure | getNow | getContextField | loadInput | bufferOutputWrite
  | payloadGet | bufferPayloadWrite | extensionGet | bufferExtensionWrite | objectCall | newTransaction
  | stagePhase | ackResponse | cancelLocal | resultGet | resultRelease | scheduleEvent | cancelEvent
  | spawnProcess | trySpawnProcess | registerWait | readWaitResult | setTransportReturn | debugTransfer
  | grantRawDmi | denyDmi | invalidateRawDmi | waitGroupNew | waitArm | waitResultGet | waitGroupRelease
  | scopeNew | scopeTransfer | scopeCancel | scopeClose | submitTask | cancelTask | taskResultGet
  | taskResultRelease | requestTaskSlot | requestManaged | beginManagedRead | beginManagedWrite
  | invalidateManaged | releaseLease | callExternPure
  deriving Repr, BEq, Inhabited

/-- The executable subset is explicit: other specification opcodes cannot be encoded as aliases. -/
def opcodeNames : List String :=
  ["const", "move", "binary", "makeRecord", "getField", "makeVariant", "variantTag",
   "variantGet", "makeVec", "vecGet", "vecSet", "selectValue", "loadState",
   "bufferStateWrite", "check", "trace", "unary", "compare", "convert", "callPure", "getNow",
   "getContextField", "loadInput", "bufferOutputWrite", "payloadGet", "bufferPayloadWrite", "extensionGet",
   "bufferExtensionWrite", "objectCall", "newTransaction", "stagePhase", "ackResponse", "cancelLocal",
   "resultGet", "resultRelease", "scheduleEvent", "cancelEvent", "spawnProcess", "trySpawnProcess",
   "registerWait", "readWaitResult", "setTransportReturn", "debugTransfer", "grantRawDmi", "denyDmi",
   "invalidateRawDmi", "waitGroupNew", "waitArm", "waitResultGet", "waitGroupRelease", "scopeNew",
   "scopeTransfer", "scopeCancel", "scopeClose", "submitTask", "cancelTask", "taskResultGet",
   "taskResultRelease", "requestTaskSlot", "requestManaged", "beginManagedRead", "beginManagedWrite",
   "invalidateManaged", "releaseLease", "callExternPure"]

def allOps : List Op := [.const,.move,.binary,.makeRecord,.getField,.makeVariant,.variantTag,.variantGet,
  .makeVec,.vecGet,.vecSet,.selectValue,.loadState,.bufferStateWrite,.check,.trace,.unary,.compare,.convert,
  .callPure,.getNow,.getContextField,.loadInput,.bufferOutputWrite,.payloadGet,.bufferPayloadWrite,
  .extensionGet,.bufferExtensionWrite,.objectCall,.newTransaction,.stagePhase,.ackResponse,.cancelLocal,
  .resultGet,.resultRelease,.scheduleEvent,.cancelEvent,.spawnProcess,.trySpawnProcess,.registerWait,
  .readWaitResult,.setTransportReturn,.debugTransfer,.grantRawDmi,.denyDmi,.invalidateRawDmi,.waitGroupNew,
  .waitArm,.waitResultGet,.waitGroupRelease,.scopeNew,.scopeTransfer,.scopeCancel,.scopeClose,.submitTask,
  .cancelTask,.taskResultGet,.taskResultRelease,.requestTaskSlot,.requestManaged,.beginManagedRead,
  .beginManagedWrite,.invalidateManaged,.releaseLease,.callExternPure]

def Op.tag (op : Op) := (allOps.idxOf? op).getD 255

structure FrameSlot where
  typeId : Nat
  offset : Nat
  alignment : Nat
  registerId : Nat := 0
  deriving Repr, Inhabited

structure ServiceSignature where
  id : Nat
  op : Op
  inputTypes : List Nat
  resultTypes : List Nat
  contextMask : Nat
  effectMask : Nat
  extraFuel : Nat
  providerKey : String
  providerVersion : String
  abiHash : ByteArray
  deriving Repr

structure Instruction where
  op : Op
  args : List VReg := []
  dest : Option VReg := none
  value : Value := .unit
  immediate : Nat := 0
  operator : Pure.BinaryOp := .addWrap
  text : String := ""
  source : String := ""
  deriving Repr

structure Edge where
  target : Nat
  args : List VReg := []
  deriving Repr, Inhabited

inductive Terminator where
  | jump (edge : Edge)
  | branch (condition : VReg) (yes no : Edge)
  | switch (value : VReg) (cases : List (Nat × Edge)) (default : Edge)
  | ret (values : List VReg)
  | fail (error : String)
  | transportReturn (result : VReg)
  | suspend (wait : VReg) (resumeBlock : Nat) (live : List VReg)
  deriving Repr, Inhabited

structure Block where
  id : Nat
  parameters : List VReg := []
  instructions : List Instruction := []
  terminator : Terminator
  deriving Repr, Inhabited

structure Program where
  id : Nat
  inputTypes : List Nat := []
  resultTypes : List Nat := []
  blocks : List Block
  entry : Nat := 0
  source : String := ""
  context : Nat := 0
  frame : List FrameSlot := []
  frameBytes : Nat := 0
  effectMask : Nat := 0
  instructionFuel : Nat := 0
  ownerPolicy : String := ""
  resultLifetimePolicy : String := ""
  deriving Repr, Inhabited

structure HandlerBinding where
  localId : Nat
  programId : Nat
  context : Nat
  trigger : String
  endpoint : Option Nat
  processCapacity : Option ModelIR.ProcessCapacityIR
  source : String
  deriving Repr

structure ComponentDesc where
  id : Nat
  parameters : List ModelIR.ParamDeclIR
  states : List ModelIR.StateSlot
  endpoints : List ModelIR.EndpointIR
  sidebands : List ModelIR.PortDeclIR
  resetPolicy : String
  source : String
  deriving Repr

structure InstanceDesc where
  id : Nat
  definition : Nat
  stateBase : Nat
  stateCount : Nat
  handlers : List HandlerBinding
  resolvedConfig : List (Nat × Value)
  source : String
  deriving Repr

structure ExecProject where
  types : TypeEnvironment
  stateTypes : List Nat
  initialState : List Value
  programs : List Program
  profile : String := "AT-Core-1.1-draft"
  schemaMajor : Nat := 1
  services : List ServiceSignature := []
  components : List ComponentDesc := []
  instances : List InstanceDesc := []
  systemMetadata : Option ModelIR.SystemIR := none
  externalContracts : List ModelIR.ExternalContractIR := []
  capabilities : List String := []
  deriving Repr

end LeanAT.ExecIR
