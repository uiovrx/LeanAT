import LeanAT.Types
namespace LeanAT.ModelIR.Object
inductive Command where
  | read | write | ignore | unknown
  deriving Repr, BEq
inductive ResponseStatus where
  | ok | commandError | burstError | byteEnableError | addressError | budgetExceeded
  deriving Repr, BEq
structure MemoryRequest where
  command : Command
  address : Nat
  data : List UInt8
  streamingWidth : Nat
  byteEnable : List UInt8 := []
  deriving Repr, BEq
structure MemoryResult where
  memory : List UInt8
  data : List UInt8
  status : ResponseStatus
  deriving Repr, BEq
private def enabled (mask : List UInt8) (index : Nat) : Bool :=
  mask.isEmpty || mask[index % mask.length]?.getD 0 == 255
/-- Complete preflight includes disabled generated addresses; failures preserve memory and input data. -/
def memoryTransfer (memory : List UInt8) (request : MemoryRequest) (workBudget : Nat) : MemoryResult := Id.run do
  let unchanged := fun status => MemoryResult.mk memory request.data status
  if request.command == .ignore then return unchanged .ok
  if request.command == .unknown then return unchanged .commandError
  if request.data.isEmpty || request.streamingWidth == 0 then return unchanged .burstError
  if request.data.length > workBudget then return unchanged .budgetExceeded
  if !(request.byteEnable.all (fun b => b == 0 || b == 255)) then return unchanged .byteEnableError
  if !(List.range request.data.length).all (fun i =>
    request.address + i % request.streamingWidth < memory.length && request.address + i % request.streamingWidth < 2^64) then
    return unchanged .addressError
  let mut nextMemory := memory
  let mut data := request.data
  for i in List.range request.data.length do
    if enabled request.byteEnable i then
      let address := request.address + i % request.streamingWidth
      if request.command == .read then data := data.set i (memory[address]?.getD 0)
      else nextMemory := nextMemory.set address (request.data[i]?.getD 0)
  return ⟨nextMemory,data,.ok⟩

structure DebugResult where
  memory : List UInt8
  data : List UInt8
  count : Nat
  deriving Repr, BEq
def memoryDebug (memory : List UInt8) (request : MemoryRequest) (maxDebugBytes : Nat) : DebugResult := Id.run do
  if request.command == .ignore || request.command == .unknown || request.address ≥ memory.length then
    return ⟨memory,request.data,0⟩
  let count := min request.data.length (min maxDebugBytes (memory.length-request.address))
  let mut nextMemory := memory
  let mut data := request.data
  for i in List.range count do
    if request.command == .read then data := data.set i (memory[request.address+i]?.getD 0)
    else nextMemory := nextMemory.set (request.address+i) (request.data[i]?.getD 0)
  return ⟨nextMemory,data,count⟩

structure QueueState where
  capacity : Nat
  typeId : TypeId
  values : List Value := []
  deriving Repr, BEq
/-- Value-only FIFO reference. Copying an identity never creates an owning consumer. -/
def queuePush (types : TypeEnvironment) (s : QueueState) (v : Value) : Except String (QueueState × Bool) := do
  if s.capacity == 0 || s.values.length > s.capacity then throw "InvalidQueueState"
  let some schema := types[s.typeId]? | throw "UnknownQueueElementType"
  if !conforms types (types.length+1) schema v then throw "QueueTypeMismatch"
  if s.values.length == s.capacity then pure (s,false) else pure ({s with values := s.values ++ [v]},true)
def queuePop (s : QueueState) : QueueState × Option Value :=
  match s.values with
  | [] => (s,none)
  | v::rest => ({s with values := rest},some v)

structure ResourceSpec where
  capacity : Nat
  reservationCapacity : Nat
  initiationInterval : Nat
  pipeline : Bool
  deriving Repr, BEq
structure Grant where
  ticket : Nat
  owner : Nat
  channel : Nat
  start : Nat
  finish : Nat
  wait : Nat
  deriving Repr, BEq
structure ResourceState where
  channelAvailable : List Nat
  lastArrival : Nat := 0
  lastGrantedStart : Nat := 0
  nextIssue : Nat := 0
  nextTicket : Nat := 0
  reservations : List Grant := []
  deriving Repr, BEq
/-- Online FCFS respects already granted future starts; channel ordinal resolves ties. -/
def resourceReserve (spec : ResourceSpec) (s : ResourceState) (owner arrival earliest duration : Nat) : Except String (ResourceState × Grant) := do
  if spec.capacity == 0 || s.channelAvailable.length != spec.capacity || spec.reservationCapacity == 0 then throw "InvalidCapacity"
  if arrival < s.lastArrival || earliest < arrival then throw "IllegalArrivalOrder"
  if s.reservations.length ≥ spec.reservationCapacity then throw "ReservationFull"
  if s.nextTicket ≥ 2^64 then throw "TicketExhausted"
  let available := s.channelAvailable.foldl min (s.channelAvailable.head?.getD 0)
  let some channel := s.channelAvailable.idxOf? available | throw "InvalidChannelState"
  let start := max earliest (max s.lastGrantedStart (max available (if spec.pipeline then s.nextIssue else 0)))
  let finish := start + duration
  let nextIssue := if spec.pipeline then start + spec.initiationInterval else s.nextIssue
  if start ≥ 2^64 || finish ≥ 2^64 || nextIssue ≥ 2^64 then throw "TimeOverflow"
  let grant := Grant.mk s.nextTicket owner channel start finish (start-earliest)
  pure ({s with channelAvailable := s.channelAvailable.set channel finish, lastArrival := arrival, lastGrantedStart := start, nextIssue := nextIssue, nextTicket := s.nextTicket+1, reservations := s.reservations ++ [grant]},grant)
end LeanAT.ModelIR.Object

