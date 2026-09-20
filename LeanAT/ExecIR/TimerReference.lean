import LeanAT.ExecIR.Reference

namespace LeanAT.ExecIR.TimerReference

inductive Status where
  | waiting (segment : SegmentResult)
  | done (values : List Value)
  deriving Repr
structure Frame where
  programId : Nat
  context : ExecutionContext
  state : List Value
  reference : ReferenceState
  status : Status
  turn : Nat := 0
  segmentFuel : Nat := 10000
  outputs : List ReferenceOutput := []
  outputCapacity : Nat := 1024
  deriving Repr
inductive StopReason where
  | quiescent | horizonReached | runBudgetReached | failed (error : String)
  deriving Repr, BEq
structure RunResult where
  frame : Frame
  reason : StopReason
  deriving Repr

private def finish (p : ValidatedProject) (frame : Frame) (segment : SegmentResult) : Except String Frame := do
  let status ← match segment.exit with
    | .returned => do
      if segment.txn.reference.registered.isSome then throw "RegisteredWaitWithoutSuspension"
      pure (.done segment.returned)
    | .transport _ => throw "TimerReferenceTransportExit"
    | .suspended wait _ _ outcomeType => do
      let .handle wait := wait | throw "InvalidWaitValue"
      let some record := segment.txn.reference.registered | throw "UnknownTimerRegistration"
      if record.identity != wait then throw "StaleSuspensionToken"
      if p.project.types[outcomeType]? != some .unit then throw "TimerOutcomeLayout"
      pure (.waiting segment)
  if frame.outputs.length+segment.txn.outputs.length > frame.outputCapacity then throw "OutputCapacity"
  pure {frame with state := commit segment.txn,reference := segment.txn.reference,status,outputs := frame.outputs ++ segment.txn.outputs}

def start (p : ValidatedProject) (programId : Nat) (process : HandleIdentity) (args : List Value)
    (fuel : Nat := 10000) (time : Nat := 0) (instanceId : Nat := 0) (outputCapacity : Nat := 1024) (initialTurn : Nat := 0) : Except String Frame := do
  if initialTurn ≥ 2^64 then throw "TurnOverflow"
  if process.kind != .process || process.generation == 0 then throw "InvalidProcessIdentity"
  let context : ExecutionContext := {kind := 1,now := time,domain := process.domain.toNat,instanceId,processIdentity := some process,owner := some process.owner.toNat,schedulerFrontier := some (time,initialTurn)}
  let frame : Frame := {programId,context,state := p.project.initialState,reference := {},status := .done [],segmentFuel := fuel,outputCapacity,turn := initialTurn}
  let segment ← evalExecSegment p programId args {state := frame.state,outputCapacity} fuel context
  finish p frame segment

/-- Claims the current wait generation. A previous await's notification cannot resume a later await. -/
def resume (p : ValidatedProject) (frame : Frame) (delivered : HandleIdentity) (maxTurns : Nat := 1000) : Except String Frame := do
  let .waiting prepared := frame.status | throw "ProcessNotSuspended"
  let some record := frame.reference.registered | throw "UnknownTimerRegistration"
  if record.identity != delivered then throw "StaleSuspensionToken"
  let turn := if record.deadline == frame.context.now then frame.turn+1 else 0
  if turn ≥ maxTurns then throw "ZenoDetected"
  let context := {frame.context with now := record.deadline,schedulerFrontier := some (record.deadline,turn)}
  let transaction : EventTxn := {state := frame.state,reference := {frame.reference with registered := none},outputCapacity := frame.outputCapacity-frame.outputs.length}
  let segment ← evalPreparedResume p frame.programId prepared .unit transaction frame.segmentFuel context
  finish p {frame with context,turn} segment

def runFrom (p : ValidatedProject) (horizon : Option Nat) (maxTurns : Nat := 1000) : Nat → Frame → RunResult
  | fuel, frame => match frame.status with
    | .done _ => ⟨frame,.quiescent⟩
    | .waiting _ =>
      match frame.reference.registered with
      | none => ⟨frame,.failed "UnknownTimerRegistration"⟩
      | some record =>
        if horizon.any (fun limit => record.deadline > limit) then ⟨frame,.horizonReached⟩ else
        match fuel with
        | 0 => ⟨frame,.runBudgetReached⟩
        | fuel+1 => match resume p frame record.identity maxTurns with
          | .error error => ⟨frame,.failed error⟩
          | .ok next => runFrom p horizon maxTurns fuel next

end LeanAT.ExecIR.TimerReference


