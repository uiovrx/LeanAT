import LeanAT.ExecIR.Reference
namespace LeanAT.Reference.Exec
structure Machine where
  txn : ExecIR.EventTxn
  remainingFuel : Nat
  fuelCaps : List Nat := []
  trace : List ExecutionTrace := []
  exit : String := "returned"
  wait : Option Value := none
  resumeBlock : Option Nat := none
  resumeProgram : Option Nat := none
  liveTypeIds : List Nat := []
  outcomeType : Option Nat := none
  live : List Value := []
  deriving Repr
abbrev Eval := EStateM String Machine
private def liftResult (value : Except String α) : Eval α := match value with | .ok value => pure value | .error error => throw error
private def takeFuel (amount : Nat) : Eval Nat := do
  let state ← get
  if state.remainingFuel < amount || state.fuelCaps.any (· < amount) then throw "FuelExhausted"
  set {state with remainingFuel := state.remainingFuel-amount,fuelCaps := state.fuelCaps.map (·-amount)}
  pure state.remainingFuel
private def regValue (regs : List (Nat × Value)) (reg : ExecIR.VReg) : Eval Value :=
  match regs.lookup reg.id with | some value => pure value | none => throw "UndefinedRegister"
private def bindReg (regs : List (Nat × Value)) (reg : ExecIR.VReg) (value : Value) := (reg.id,value)::regs.filter (fun pair => pair.1 != reg.id)
private def traceNode (program : Nat) (location operation : String) (args values : List Value) (before : Nat) : Eval Unit :=
  modify fun state => {state with trace := state.trace ++ [⟨"ExecIR",program,location,operation,args,values,before,state.remainingFuel⟩]}
private inductive Next where
  | done (values : List Value)
  | step (block offset : Nat) (regs : List (Nat × Value))

private def operandTypes (p : ExecIR.ExecProject) (parameters : List ExecIR.VReg) (values : List Value) : Eval Unit := do
  if parameters.length != values.length then throw "ContinuationArity"
  for (parameter,value) in parameters.zip values do
    if !(p.types[parameter.typeId]?).any (fun schema => conforms p.types (p.types.length+1) schema value) then
      throw "ContinuationType"

mutual
private partial def block (p : ExecIR.ExecProject) (context : ExecIR.ExecutionContext) (program : ExecIR.Program)
    (id offset : Nat) (regs : List (Nat × Value)) : Eval (List Value) := do
  let location := s!"program/{program.id}/block/{id}/{offset}"
  let instruction := (program.blocks.find? (fun b => b.id == id)).bind (fun b => b.instructions[offset]?)
  let source := instruction.map (fun i => if i.source.isEmpty then location else i.source) |>.getD location
  let operation := instruction.map (fun i => ExecIR.opcodeNames[i.op.tag]!) |>.getD "terminator"
  let before := (← get).remainingFuel
  let next ← try step p context program id offset regs
    catch error =>
      let state ← get
      if !(state.trace.getLast?).any (fun entry => entry.program == program.id && entry.location == source && entry.operation.startsWith "error:") then
        let args := instruction.toList.flatMap (fun i => if i.op == .callPure then i.args.filterMap (fun reg => regs.lookup reg.id) else [])
        traceNode program.id source ("error:" ++ operation) args [] before
      throw error
  match next with
  | .done values => pure values
  | .step id offset regs => block p context program id offset regs
private partial def step (p : ExecIR.ExecProject) (context : ExecIR.ExecutionContext) (program : ExecIR.Program)
    (id offset : Nat) (regs : List (Nat × Value)) : Eval Next := do
  let some current := program.blocks.find? (fun b => b.id == id) | throw "MissingBlock"
  let location := "program/" ++ toString program.id ++ "/block/" ++ toString id ++ "/" ++ toString offset
  if let some instruction := current.instructions[offset]? then
    let extra := if instruction.op.tag ≥ 21 then ((p.services.find? (fun s => s.id == instruction.immediate)).map ExecIR.ServiceSignature.extraFuel).getD 0 else 0
    let before ← takeFuel (1+extra)
    let args ← instruction.args.mapM (regValue regs)
    let values ← if instruction.op == .callPure then do
      let some callee := p.programs.find? (fun program => program.id == instruction.immediate) | throw "MissingPureProgram"
      let some entry := callee.blocks.find? (fun block => block.id == callee.entry) | throw "MissingPureEntry"
      if args.length != entry.parameters.length then throw "PureInputArity"
      operandTypes p entry.parameters args
      traceNode program.id (if instruction.source.isEmpty then location else instruction.source) "enter:callPure" args [] before
      let capped := callee.instructionFuel > 0
      if capped then modify fun state => {state with fuelCaps := callee.instructionFuel::state.fuelCaps}
      let calleeContext := {context with kind := callee.context,referenceContext := context.referenceContext.map (fun ctx => {ctx with kind := callee.context})}
      let values ← try block p calleeContext callee callee.entry 0 ((entry.parameters.zip args).map (fun (reg,value) => (reg.id,value)))
        catch error =>
          if capped then modify fun state => {state with fuelCaps := state.fuelCaps.drop 1}
          throw error
      if capped then modify fun state => {state with fuelCaps := state.fuelCaps.drop 1}
      pure values
    else do
      let state ← get
      traceNode program.id location ("enter:" ++ ExecIR.opcodeNames[instruction.op.tag]!) args [] before
      let value ← match ExecIR.evalReferenceInstructionAttempt p context instruction args state.txn with
        | .ok value txn => do modify (fun state => {state with txn}); pure value
        | .error error txn => do
          modify fun state => {state with txn}
          traceNode program.id (if instruction.source.isEmpty then location else instruction.source) ("error:" ++ ExecIR.opcodeNames[instruction.op.tag]!) args [] before
          throw error
      pure value.toList
    let nextRegs ← match instruction.dest,values with
      | none,[] => pure regs
      | some reg,[value] => do
        if !(p.types[reg.typeId]?).any (fun type => conforms p.types (p.types.length+1) type value) then throw "InstructionResultType"
        pure (bindReg regs reg value)
      | _,_ => throw "InstructionResultArity"
    traceNode program.id (if instruction.source.isEmpty then location else instruction.source) (ExecIR.opcodeNames[instruction.op.tag]!) args values before
    pure (.step id (offset+1) nextRegs)
  else
    let before ← takeFuel 1
    let edge ← match current.terminator with
      | .ret values => do
        let values ← values.mapM (regValue regs)
        let state ← get
        if let some world := state.txn.world then
          if let some ctx := context.referenceContext then
            if program.context == 1 then
              let some process := ctx.processIdentity | throw "MissingProcessContext"
              let data ← liftResult (Runtime.getProcess world ctx process)
              if data.wait.isSome then throw "RegisteredWaitWithoutSuspension"
              let world ← liftResult (Runtime.completeProcess world ctx process values)
              modify fun state => {state with txn := {state.txn with world := some world}}
        traceNode program.id location "return" values values before
        return .done values
      | .transportReturn reg => do
        let value ← regValue regs reg
        let state ← get
        if state.txn.preparedTransport != some value then throw "UnpreparedTransportReturn"
        modify fun state => {state with exit := "transport"}
        traceNode program.id location "transportReturn" [value] [value] before
        return .done [value]
      | .suspend wait resume live => do
        let value ← regValue regs wait
        let .handle handle := value | throw "WaitType"
        let state ← get
        let some world := state.txn.world | throw "MissingReferenceWorld"
        let some ctx := context.referenceContext | throw "MissingReferenceContext"
        let some process := ctx.processIdentity | throw "MissingProcessContext"
        let some target := program.blocks.find? (fun b => b.id == resume) | throw "MissingResumeBlock"
        let some outcome := target.parameters.head? | throw "MissingWaitOutcome"
        let live ← live.mapM (regValue regs)
        operandTypes p target.parameters.tail live
        let world ← match (Runtime.suspendProcessAttempt ctx process handle).run world with
          | .ok _ world => pure world
          | .error error world => do
            modify fun state => {state with txn := {state.txn with world := some world}}
            throw error
        modify fun state => {state with txn := {state.txn with world := some world},exit := "suspended",wait := some value,resumeBlock := some resume,resumeProgram := some program.id,outcomeType := some outcome.typeId,live,liveTypeIds := target.parameters.tail.map (·.typeId)}
        traceNode program.id location "suspend" (value::live) [] before
        return .done []
      | .fail error => do traceNode program.id location "fail" [] [] before; throw error
      | .jump edge => do traceNode program.id location "jump" (← edge.args.mapM (regValue regs)) [] before; pure edge
      | .branch condition yes no => do
        let value ← regValue regs condition
        let .bool selected := value | throw "ExpectedBool"
        traceNode program.id location "branch" [value] [] before
        pure (if selected then yes else no)
      | .switch reg cases fallback => do
        let value ← regValue regs reg
        let .bits _ tag := value | throw "ExpectedTag"
        traceNode program.id location "switch" [value] [] before
        pure ((cases.lookup tag).getD fallback)
    let some target := program.blocks.find? (fun b => b.id == edge.target) | throw "MissingTarget"
    let values ← edge.args.mapM (regValue regs)
    if values.length != target.parameters.length then throw "EdgeArity"
    operandTypes p target.parameters values
    let regs := (target.parameters.zip values).foldl (fun regs (reg,value) => bindReg regs reg value) regs
    pure (.step edge.target 0 regs)
end

def run (project : ExecIR.ExecProject) (context : ExecIR.ExecutionContext) (program : ExecIR.Program) (inputs : List Value) (machine : Machine) : EStateM.Result String Machine (List Value) :=
  (show Eval (List Value) from do
    if (← get).wait.isSome then throw "UseExecResume"
    let some entry := program.blocks.find? (fun b => b.id == program.entry) | throw "MissingEntry"
    if inputs.length != entry.parameters.length then throw "InputArity"
    operandTypes project entry.parameters inputs
    if program.instructionFuel > 0 then modify fun state => {state with fuelCaps := [program.instructionFuel]}
    block project context program program.entry 0 ((entry.parameters.zip inputs).map (fun (reg,value) => (reg.id,value)))).run machine
/-- Resume only the saved CFG edge. Queue identity and wait claiming are Runner
    responsibilities; this entry independently checks the owned frame types. -/
def resume (project : ExecIR.ExecProject) (context : ExecIR.ExecutionContext) (program : ExecIR.Program)
    (outcome : Value) (machine : Machine) : EStateM.Result String Machine (List Value) :=
  (show Eval (List Value) from do
    let state ← get
    if state.exit != "suspended" || state.wait.isNone || state.resumeProgram != some program.id then
      throw "MissingExecSuspension"
    let some resumeId := state.resumeBlock | throw "MissingResumeBlock"
    let some target := program.blocks.find? (fun b => b.id == resumeId) | throw "MissingResumeBlock"
    let some parameter := target.parameters.head? | throw "MissingWaitOutcome"
    if state.outcomeType != some parameter.typeId || state.liveTypeIds != target.parameters.tail.map (·.typeId) then
      throw "ContinuationSchemaMismatch"
    let values := outcome::state.live
    operandTypes project target.parameters values
    let regs := (target.parameters.zip values).map (fun (reg,value) => (reg.id,value))
    modify fun state => {state with wait := none,resumeBlock := none,resumeProgram := none,outcomeType := none,live := [],liveTypeIds := [],exit := "returned",fuelCaps := if program.instructionFuel > 0 then [program.instructionFuel] else []}
    block project context program resumeId 0 regs).run machine
end LeanAT.Reference.Exec


