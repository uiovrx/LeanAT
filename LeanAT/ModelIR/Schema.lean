import LeanAT.Pure
namespace LeanAT.ModelIR
def schemaVersion : Nat := 1
inductive Expr where
  | literal (typeId : TypeId) (value : Value)
  | local (typeId : TypeId) (id : Nat)
  | state (typeId : TypeId) (id : Nat)
  | select (typeId : TypeId) (condition yes no : Expr)
  | binary (typeId : TypeId) (op : Pure.BinaryOp) (left right : Expr)
  | field (typeId : TypeId) (object : Expr) (index : Nat)
  | index (typeId : TypeId) (object index : Expr)
  | makeRecord (typeId : TypeId) (fields : List Expr)
  | makeVariant (typeId : TypeId) (tag : Nat) (fields : List Expr)
  | unary (typeId : TypeId) (mode : Nat) (value : Expr)
  | compare (typeId : TypeId) (mode : Nat) (left right : Expr)
  | convert (typeId : TypeId) (mode : Nat) (value : Expr)
  | variantTag (typeId : TypeId) (value : Expr)
  | variantGet (typeId : TypeId) (value : Expr) (tag field : Nat)
  | makeVec (typeId : TypeId) (fields : List Expr)
  | vecSet (typeId : TypeId) (vec index value : Expr)
  | callPure (typeId : TypeId) (handlerId : Nat) (args : List Expr)
  deriving Repr, BEq

def Expr.typeId : Expr → TypeId
  | .literal t _ | .local t _ | .state t _ | .select t _ _ _ | .binary t _ _ _ | .field t _ _ | .index t _ _ | .makeRecord t _ | .makeVariant t _ _
  | .unary t _ _ | .compare t _ _ _ | .convert t _ _ | .variantTag t _ | .variantGet t _ _ _ | .makeVec t _ | .vecSet t _ _ _ | .callPure t _ _ => t
structure LocalBinderIR where
  id : Nat
  typeId : TypeId
  source : SourceSpan := {}
  deriving Repr, BEq
inductive Stmt where
  | readNow (destination : LocalBinderIR)
  | serviceCall (destination : Option LocalBinderIR) (serviceId : Nat) (args : List Expr)
  | await (wait : Expr) (outcomeBinder : Nat) (outcomeType : TypeId)
  | letVal (id : Nat) (value : Expr)
  | writeState (id : Nat) (value : Expr)
  | check (condition : Expr) (error : String)
  | branch (condition : Expr) (yes no : List Stmt)
  | repeat (bound : Nat) (body : List Stmt)
  | emit (tag : String) (values : List Expr)
  | fail (error : String)
  | transportReturn (value : Expr)
  | ret (values : List Expr)
  | unsupported (feature : String)
  deriving Repr, BEq
structure StateSlot where
  id : Nat
  typeId : TypeId
  initial : Value
  deriving Repr, BEq
inductive ContextKind where
  | transportEntry | timedHandler | process | debugEntry | dmiEntry | pureFunction | protocolGuard
  deriving Repr, BEq
inductive OverflowPolicy where
  | reject | awaitSlot (waiterLimit : Nat) | queue (depth : Nat)
  deriving Repr, BEq
structure ProcessCapacityIR where
  maxInstances : Nat := 1
  frameBytesLimit : Nat
  resultCapacity : Nat
  overflow : OverflowPolicy := .reject
  deriving Repr, BEq
structure Handler where
  id : Nat
  body : List Stmt
  source : SourceSpan := {}
  context : ContextKind := .timedHandler
  trigger : String := "manual"
  endpoint : Option Nat := none
  parameters : List LocalBinderIR := []
  processCapacity : Option ProcessCapacityIR := none
  declaredResultTypes : Option (List TypeId) := none
  deriving Repr, BEq
def Handler.frameBytesLimit (handler : Handler) : Nat := handler.processCapacity.map ProcessCapacityIR.frameBytesLimit |>.getD 0
structure Profile where
  instructionFuel : Nat := 10000
  eventCapacity : Nat := 1024
  maxEventsPerTick : Nat := 1000
  deriving Repr, BEq
structure DefinitionId where
  value : Nat
  deriving Repr, BEq, DecidableEq
structure InstanceId where
  value : Nat
  deriving Repr, BEq, DecidableEq
structure ConnectionId where
  value : Nat
  deriving Repr, BEq, DecidableEq
inductive Direction where
  | input | output
  deriving Repr, BEq
structure ParamDeclIR where
  id : Nat
  typeId : TypeId
  defaultValue : Option Value := none
  source : SourceSpan := {}
  deriving Repr, BEq
structure PortDeclIR where
  id : Nat
  direction : Direction
  typeId : TypeId
  initial : Option Value := none
  source : SourceSpan := {}
  deriving Repr, BEq
inductive EndpointRole where
  | initiator | target
  deriving Repr, BEq
structure EndpointIR where
  id : Nat
  role : EndpointRole
  busWidth : Nat
  maxBindings : Nat
  maxOutstanding : Nat
  maxPayloadBytes : Nat
  maxByteEnableBytes : Nat
  protocolRef : String := "tlm.base.v1"
  source : SourceSpan := {}
  deriving Repr, BEq
structure ProcessIR where
  id : Nat
  params : List LocalBinderIR
  resultType : TypeId
  body : List Stmt
  capacity : ProcessCapacityIR
  instructionFuel : Nat
  ownerPolicy : String
  resultLifetimePolicy : String
  source : SourceSpan := {}
  deriving Repr, BEq
structure ComponentIR where
  id : DefinitionId
  parameters : List ParamDeclIR := []
  states : List StateSlot := []
  endpoints : List EndpointIR := []
  sidebands : List PortDeclIR := []
  handlers : List Handler := []
  processes : List ProcessIR := []
  resetPolicy : String := "cancel-business-drain-wire"
  source : SourceSpan := {}
  deriving Repr, BEq
structure InstanceIR where
  id : InstanceId
  definition : DefinitionId
  resolvedConfig : List (Nat × Value) := []
  source : SourceSpan := {}
  deriving Repr, BEq
structure EndpointRef where
  instanceId : InstanceId
  endpoint : Nat
  bindingIndex : Nat
  deriving Repr, BEq
structure BindingIR where
  id : ConnectionId
  sourceEndpoint : EndpointRef
  sinkEndpoint : EndpointRef
  source : SourceSpan := {}
  deriving Repr, BEq
inductive SidebandPortRef where
  | top (port : Nat) | instance (id : InstanceId) (port : Nat)
  deriving Repr, BEq
structure SidebandBindingIR where
  id : Nat
  sourcePort : SidebandPortRef
  sinkPort : SidebandPortRef
  source : SourceSpan := {}
  deriving Repr, BEq
structure AddressMapIR where
  id : Nat
  decoder : InstanceId
  outputEndpoint : Nat
  bindingIndex : Nat
  sourceStart : Nat
  size : Nat
  targetStart : Nat
  aliasDeclared : Bool := false
  source : SourceSpan := {}
  deriving Repr, BEq
structure SystemIR where
  id : Nat
  instances : List InstanceIR
  bindings : List BindingIR
  topPorts : List PortDeclIR := []
  sidebandBindings : List SidebandBindingIR := []
  addressMaps : List AddressMapIR := []
  runtimeDomain : Nat
  source : SourceSpan := {}
  deriving Repr, BEq
structure ExternalContractIR where
  id : Nat
  cppType : String
  header : String
  library : String
  constructorMapping : List (String × Value)
  requires : List String
  ensures : List String
  assumptions : List String
  evidence : List String
  source : SourceSpan := {}
  deriving Repr, BEq
structure ServiceIR where
  id : Nat
  opcode : String
  inputTypes : List TypeId
  resultTypes : List TypeId
  contextMask : Nat
  effectMask : Nat
  extraFuel : Nat
  providerKey : String
  providerVersion : String
  abiHash : List UInt8
  source : SourceSpan := {}
  deriving Repr, BEq
/-- The intrinsic effect floor is closed; caller-provided masks can only strengthen it. -/
def serviceContract (opcode : String) : Option (Nat × Nat) :=
  if ["getContextField","loadInput"].contains opcode then some (3,0)
  else if opcode == "bufferOutputWrite" then some (3,4)
  else if ["payloadGet","bufferPayloadWrite","extensionGet","bufferExtensionWrite"].contains opcode then some (7,8)
  else if ["newTransaction","stagePhase","ackResponse","cancelLocal","scheduleEvent","cancelEvent"].contains opcode then some (3,8)
  else if opcode == "setTransportReturn" then some (4,8)
  else if ["registerWait","readWaitResult","waitGroupNew","waitArm","waitResultGet","waitGroupRelease"].contains opcode then some (2,16)
  else if opcode == "requestTaskSlot" then some (2,32)
  else if ["spawnProcess","trySpawnProcess","submitTask","cancelTask","scopeNew","scopeTransfer","scopeCancel","scopeClose"].contains opcode then some (3,32)
  else if ["resultGet","resultRelease","taskResultGet","taskResultRelease"].contains opcode then some (3,64)
  else if opcode == "objectCall" then some (3,128)
  else if opcode == "debugTransfer" then some (8,128)
  else if ["requestManaged","beginManagedRead","beginManagedWrite","invalidateManaged","releaseLease"].contains opcode then some (3,256)
  else if opcode == "callExternPure" then some (3,512)
  else if ["grantRawDmi","denyDmi"].contains opcode then some (16,1024)
  else if opcode == "invalidateRawDmi" then some (3,1024)
  else none
structure Project where
  version : Nat := schemaVersion
  types : TypeEnvironment := []
  states : List StateSlot := []
  handlers : List Handler := []
  profile : Profile := {}
  components : List ComponentIR := []
  systems : List SystemIR := []
  topSystemId : Option Nat := none
  externalContracts : List ExternalContractIR := []
  enabledCapabilities : List String := []
  services : List ServiceIR := []
  deriving Repr, BEq
end LeanAT.ModelIR










