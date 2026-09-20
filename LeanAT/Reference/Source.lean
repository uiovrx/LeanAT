import LeanAT.ModelIR.Validation
import LeanAT.ModelIR.Layout
import LeanAT.Pure.Numeric
import LeanAT.Reference.Dispatch
namespace LeanAT.Reference.Source
inductive Work where
  | stmt (path : String) (stmt : ModelIR.Stmt)
  | loop (path : String) (remaining : Nat) (body : List ModelIR.Stmt)
  | join (path : String)
  deriving Repr, BEq

structure Machine where
  values : List (Nat × Value)
  locals : List (Nat × Value) := []
  localTypes : List (Nat × Nat) := []
  frameBytesLimit : Option Nat := none
  world : State
  context : Context
  remainingFuel : Nat
  fuelCaps : List Nat := []
  runtimeSpent : Nat := 0
  preparedTransportLocal : Option Nat := none
  sourceRoot : Option String := none
  sourcePrograms : List (Nat × Nat) := []
  program : Nat
  trace : List ExecutionTrace := []
  returned : Option (List Value) := none
  exit : String := "returned"
  wait : Option Value := none
  outcomeType : Option Nat := none
  outcomeBinder : Option Nat := none
  continuation : List ModelIR.Stmt := []
  pendingWork : List Work := []
  deriving Repr
abbrev Eval := EStateM String Machine
private def spend : Eval Nat := do
  let state ← get
  if state.remainingFuel == 0 then throw "FuelExhausted"
  set {state with remainingFuel := state.remainingFuel-1}
  pure state.remainingFuel
/-- Runtime instruction units are separate from AST host microsteps. A local
    lookup and an unrolled loop cost no runtime instruction; concrete operations
    and traversed control-flow edges do. Charges occur after their operands. -/
private def spendRuntime (amount : Nat := 1) : Eval Unit := do
  let state ← get
  if state.fuelCaps.any (· < amount) then throw "FuelExhausted"
  set {state with fuelCaps := state.fuelCaps.map (·-amount),runtimeSpent := state.runtimeSpent+amount,preparedTransportLocal := if amount == 0 then state.preparedTransportLocal else none}
private def observeNode (path operation : String) (before : Nat) (args results : List Value) : Eval Unit := do
  modify fun state => {state with trace := state.trace ++ [⟨"ModelIR",state.program,path,operation,args,results,before,state.remainingFuel⟩]}
private def checkedValue (path operation : String) (before : Nat) (args : List Value) (result : Except String Value) : Eval Value :=
  match result with
  | .ok value => pure value
  | .error error => do observeNode path ("error:" ++ operation) before args []; throw error
private def bind (id : Nat) (value : Value) (typeId : Nat) : Eval Unit :=
  modify fun state => {state with locals := (id,value)::state.locals.filter (fun pair => pair.1 != id),localTypes := (id,typeId)::state.localTypes.filter (fun pair => pair.1 != id)}
private def liftResult (value : Except String α) : Eval α := match value with | .ok value => pure value | .error error => throw error
private def work (path : String) (body : List ModelIR.Stmt) : List Work := body.zipIdx |>.map (fun (stmt,index) => .stmt (path ++ "/" ++ toString index) stmt)
private def unionIds (left right : List Nat) : List Nat := (left ++ right).eraseDups
private partial def expressionLocals : ModelIR.Expr → List Nat
  | .local _ id => [id]
  | .literal .. | .state .. => []
  | .select _ c y n | .vecSet _ c y n => unionIds (expressionLocals c) (unionIds (expressionLocals y) (expressionLocals n))
  | .binary _ _ a b | .compare _ _ a b | .index _ a b => unionIds (expressionLocals a) (expressionLocals b)
  | .field _ a _ | .unary _ _ a | .convert _ _ a | .variantTag _ a | .variantGet _ a _ _ => expressionLocals a
  | .makeRecord _ xs | .makeVariant _ _ xs | .makeVec _ xs | .callPure _ _ xs => (xs.flatMap expressionLocals).eraseDups
mutual
private def neededStatements : Nat → List ModelIR.Stmt → List Nat → Except String (List Nat)
  | 0, _, _ => throw "LivenessBudget"
  | _+1, [], needed => pure needed
  | fuel+1, stmt::rest, needed => do
    neededStatement fuel stmt (← neededStatements fuel rest needed)
private def neededStatement : Nat → ModelIR.Stmt → List Nat → Except String (List Nat)
  | 0, _, _ => throw "LivenessBudget"
  | fuel+1, stmt, needed =>
    match stmt with
    | .readNow binder => pure (needed.erase binder.id)
    | .letVal id e => pure (unionIds (expressionLocals e) (needed.erase id))
    | .serviceCall destination _ args => pure (unionIds (args.flatMap expressionLocals) (match destination with | none => needed | some binder => needed.erase binder.id))
    | .await e binder _ => pure (unionIds (expressionLocals e) (needed.erase binder))
    | .writeState _ e | .check e _ => pure (unionIds (expressionLocals e) needed)
    | .branch c yes no => do
      pure (unionIds (expressionLocals c) (unionIds (← neededStatements fuel yes needed) (← neededStatements fuel no needed)))
    | .repeat count body => neededLoop fuel count body needed
    | .emit _ args => pure (unionIds (args.flatMap expressionLocals) needed)
    | .ret args => pure ((args.flatMap expressionLocals).eraseDups)
    | .transportReturn e => pure (expressionLocals e)
    | .fail _ | .unsupported _ => pure []
private def neededLoop : Nat → Nat → List ModelIR.Stmt → List Nat → Except String (List Nat)
  | _, 0, _, needed => pure needed
  | 0, _, _, _ => throw "LivenessBudget"
  | fuel+1, count+1, body, needed => do
    let next ← neededStatements fuel body needed
    if next == needed then pure next else neededLoop fuel count body next
end
private def neededWork (pending : List Work) : Except String (List Nat) :=
  pending.reverse.foldlM (fun needed item => match item with
    | .stmt _ stmt => neededStatement 8192 stmt needed
    | .loop _ count body => neededLoop 8192 count body needed
    | .join _ => pure needed) []
private def retainedFrame (project : ModelIR.Project) (pending : List Work) (outcomeBinder : Nat) (state : Machine) : Except String (List (Nat × Value) × List (Nat × Nat)) := do
  let needed := (← neededWork pending).erase outcomeBinder
  let locals := state.locals.filter (fun entry => needed.contains entry.1)
  let types := state.localTypes.filter (fun entry => needed.contains entry.1)
  if needed.any (fun id => !(locals.lookup id).isSome || !(types.lookup id).isSome) then throw "MissingFrameLocalType"
  let limit := state.frameBytesLimit.getD state.world.maxBytes
  let mut bytes := 0
  for (_,typeId) in types do
    let layout ← (deriveLayout typeId project.types {maxBytes := limit}).mapError (fun _ => "FrameCapacityExceeded")
    bytes := ((bytes+layout.alignment-1)/layout.alignment)*layout.alignment + layout.size
  if bytes > limit then throw "FrameCapacityExceeded"
  pure (locals,types)
private def getLocal (items : List (Nat × Value)) (id : Nat) : Eval Value :=
  match items.lookup id with | some value => pure value | none => throw "UnknownValue"
private def expressionName : ModelIR.Expr → String
  | .literal _ _ => "literal" | .local _ _ => "local" | .state _ _ => "state" | .select _ _ _ _ => "select"
  | .binary _ _ _ _ => "binary" | .field _ _ _ => "field" | .index _ _ _ => "index" | .makeRecord _ _ => "makeRecord"
  | .makeVariant _ _ _ => "makeVariant" | .unary _ _ _ => "unary" | .compare _ _ _ _ => "compare" | .convert _ _ _ => "convert"
  | .variantTag _ _ => "variantTag" | .variantGet _ _ _ _ => "variantGet" | .makeVec _ _ => "makeVec" | .vecSet _ _ _ _ => "vecSet" | .callPure _ _ _ => "callPure"
private def observeFailure (path operation : String) (before : Nat) : Eval Unit := do
  let state ← get
  if !(state.trace.getLast?).any (fun entry => entry.location == path && entry.operation.startsWith "error:") then
    observeNode path ("error:" ++ operation) before [] []

mutual
private partial def expression (project : ModelIR.Project) (path : String) (expr : ModelIR.Expr) : Eval Value := do
  let before := (← get).remainingFuel
  try expressionCore project path expr
  catch error =>
    observeFailure path (expressionName expr) before
    throw error
private partial def expressionCore (project : ModelIR.Project) (path : String) (expr : ModelIR.Expr) : Eval Value := do
  let before ← spend
  observeNode path ("enter:" ++ expressionName expr) before [] []
  let state ← get
  let ev := fun index value => expression project (path ++ "/expr/" ++ toString index) value
  let (op,args,value) ← match expr with
    | .literal _ value => do spendRuntime; pure ("literal",[],value)
    | .local _ id => do pure ("local",[],← getLocal state.locals id)
    | .state _ id => do spendRuntime; pure ("state",[],← getLocal state.values id)
    | .select _ condition yes no => do
      let c ← ev 0 condition
      let .bool selected := c | throw "ExpectedBool"
      let totalLeaf := fun e => match e with | ModelIR.Expr.literal .. | .local .. => true | _ => false
      if totalLeaf yes && totalLeaf no then
        let y ← ev 1 yes
        let n ← ev 2 no
        spendRuntime
        pure ("select",[c,y,n],if selected then y else n)
      else
        spendRuntime
        let value ← ev (if selected then 1 else 2) (if selected then yes else no)
        spendRuntime
        pure ("select",[c],value)
    | .binary _ op left right => do
      let a ← ev 0 left; let b ← ev 1 right
      spendRuntime
      let value ← checkedValue path "binary" before [a,b] ((Pure.evalBinary op a b).mapError reprStr)
      pure ("binary",[a,b],value)
    | .unary _ mode input => do
      let a ← ev 0 input
      spendRuntime
      pure ("unary",[a],← checkedValue path "unary" before [a] (Numeric.evalUnary mode a))
    | .compare _ mode left right => do
      let a ← ev 0 left; let b ← ev 1 right
      spendRuntime
      pure ("compare",[a,b],← checkedValue path "compare" before [a,b] (Numeric.evalCompare mode a b))
    | .convert typeId mode input => do
      let a ← ev 0 input
      spendRuntime
      let some (.bits width) := project.types[typeId]? | throw "ConvertType"
      pure ("convert",[a],← checkedValue path "convert" before [a] (Numeric.evalConvert mode width a))
    | .field _ input index => do
      let a ← ev 0 input
      spendRuntime
      let .record values := a | throw "ExpectedRecord"
      let some value := values[index]? | throw "IndexOutOfRange"
      pure ("field",[a],value)
    | .index _ input index => do
      let a ← ev 0 input; let b ← ev 1 index
      spendRuntime
      let .vec values := a | throw "ExpectedVector"
      let .bits _ index := b | throw "ExpectedIndex"
      let some value := values[index]? | throw "IndexOutOfRange"
      pure ("index",[a,b],value)
    | .makeRecord _ fields => do
      let values ← (fields.zipIdx).mapM (fun (value,index) => ev index value)
      spendRuntime
      pure ("makeRecord",values,.record values)
    | .makeVariant _ tag fields => do
      let values ← (fields.zipIdx).mapM (fun (value,index) => ev index value)
      spendRuntime
      pure ("makeVariant",values,.variant tag values)
    | .variantTag _ input => do
      let a ← ev 0 input
      spendRuntime
      let .variant tag _ := a | throw "ExpectedVariant"
      pure ("variantTag",[a],.bits 64 tag)
    | .variantGet _ input tag index => do
      let a ← ev 0 input
      spendRuntime
      let .variant actual fields := a | throw "ExpectedVariant"
      if actual != tag then throw "VariantTagMismatch"
      let some value := fields[index]? | throw "IndexOutOfRange"
      pure ("variantGet",[a],value)
    | .makeVec _ fields => do
      let values ← (fields.zipIdx).mapM (fun (value,index) => ev index value)
      spendRuntime
      pure ("makeVec",values,.vec values)
    | .vecSet _ input index replacement => do
      let a ← ev 0 input; let b ← ev 1 index; let c ← ev 2 replacement
      spendRuntime
      let .vec values := a | throw "ExpectedVector"
      let .bits _ index := b | throw "ExpectedIndex"
      if index ≥ values.length then throw "IndexOutOfRange"
      pure ("vecSet",[a,b,c],.vec (values.set index c))
    | .callPure _ id arguments => do
      let values ← (arguments.zipIdx).mapM (fun (value,index) => ev index value)
      spendRuntime
      let some callee := project.handlers.find? (fun handler => handler.id == id) | throw "UnknownPureFunction"
      if callee.context != .pureFunction || values.length != callee.parameters.length then throw "PureCallSignature"
      let saved ← get
      let root := saved.sourceRoot.getD ("handler/" ++ toString saved.program)
      let owner := if root.startsWith "component/" then String.intercalate "/" ((root.splitOn "/").take 2) ++ "/" else ""
      let calleeRoot := owner ++ "handler/" ++ toString id
      modify fun state => {state with locals := (callee.parameters.zip values).map (fun (binder,value) => (binder.id,value)),localTypes := callee.parameters.map (fun binder => (binder.id,binder.typeId)),returned := none,program := (saved.sourcePrograms.lookup id).getD id,sourceRoot := some calleeRoot,context := {state.context with kind := 5}}
      let value ← try
        statements project (work (calleeRoot ++ "/body") callee.body)
        if (← get).returned.isNone && (← get).wait.isNone then
          spendRuntime
          modify fun state => {state with returned := some []}
        let completed ← get
        let some [value] := completed.returned | throw "PureResultArity"
        pure value
      catch error =>
        modify fun state => {state with locals := saved.locals,localTypes := saved.localTypes,returned := saved.returned,program := saved.program,sourceRoot := saved.sourceRoot,context := saved.context}
        throw error
      modify fun state => {state with locals := saved.locals,localTypes := saved.localTypes,returned := saved.returned,program := saved.program,sourceRoot := saved.sourceRoot,context := saved.context}
      pure ("callPure",values,value)
  observeNode path op before args [value]
  pure value
private partial def statements (project : ModelIR.Project) : List Work → Eval Unit
  | [] => pure ()
  | item::rest => do
    let state ← get
    if state.returned.isSome || state.wait.isSome then return
    let path := match item with | .stmt path _ | .loop path _ _ | .join path => path
    let operation := match item with | .stmt _ _ => "statement" | .loop _ _ _ => "loop" | .join _ => "join"
    let before := state.remainingFuel
    let next ← try statementStep project item rest
      catch error =>
        observeFailure path operation before
        throw error
    statements project next
private partial def statementStep (project : ModelIR.Project) (item : Work) (rest : List Work) : Eval (List Work) := do
  let state ← get
  match item with
  | .join path => do
    spendRuntime
    observeNode path "join" state.remainingFuel [] []
    pure rest
  | .loop _ 0 _ => pure rest
  | .loop path (count+1) body => do
    let before ← spend
    observeNode path "loop" before [] []
    pure (work (path ++ "/body") body ++ [.loop path count body] ++ rest)
  | .stmt path stmt => do
    let before ← spend
    observeNode path "enter:statement" before [] []
    let ev := fun index expr => expression project (path ++ "/expr/" ++ toString index) expr
    match stmt with
    | .branch condition yes no => do
      let value ← ev 0 condition
      let .bool selected := value | throw "ExpectedBool"
      spendRuntime
      observeNode path "branch" before [value] []
      pure (work (path ++ if selected then "/yes" else "/no") (if selected then yes else no) ++ [.join path] ++ rest)
    | .repeat count body => do
      observeNode path "repeat" before [] []
      pure (.loop path count body::rest)
    | _ => do
      let (op,args,results) ← match stmt with
        | .readNow binder => do
          spendRuntime
          if state.context.kind > 2 then throw "ReadNowContext"
          let value := Value.bits 64 state.context.now
          bind binder.id value binder.typeId
          pure ("readNow",[],[value])
        | .letVal id expr => do
          let value ← ev 0 expr
          if let .local .. := expr then spendRuntime
          bind id value expr.typeId
          pure ("letVal",[value],[value])
        | .writeState id expr => do
          if state.context.kind > 1 then throw "StateWriteContext"
          let value ← ev 0 expr
          spendRuntime
          modify fun state => {state with values := (id,value)::state.values.filter (fun pair => pair.1 != id)}
          pure ("writeState",[value],[])
        | .check condition error => do
          let value ← ev 0 condition
          spendRuntime
          let .bool condition := value | throw "ExpectedBool"
          if !condition then
            if state.context.kind == 5 then spendRuntime
            observeNode path "error:check" before [value] []
            throw error
          pure ("check",[value],[])
        | .emit tag expressions => do
          let values ← (expressions.zipIdx).mapM (fun (value,index) => ev index value)
          spendRuntime
          let state ← get
          let world ← liftResult (observe state.world ⟨"trace",tag,path,state.context.now,state.context.turn,values⟩)
          modify fun state => {state with world}
          pure ("emit",values,[])
        | .serviceCall destination id expressions => do
          let values ← (expressions.zipIdx).mapM (fun (value,index) => ev index value)
          let some signature := project.services.find? (fun service => service.id == id) | throw "UnknownService"
          let some operation := ExecIR.allOps.find? (fun op => ExecIR.opcodeNames[op.tag]? == some signature.opcode) | throw "UnknownServiceOpcode"
          let signature : ExecIR.ServiceSignature := ⟨signature.id,operation,signature.inputTypes,signature.resultTypes,signature.contextMask,signature.effectMask,signature.extraFuel,signature.providerKey,signature.providerVersion,⟨signature.abiHash.toArray⟩⟩
          let state ← get
          spendRuntime (1+signature.extraFuel)
          observeNode path ("enter:" ++ signature.providerKey) before values []
          let results ← match Dispatch.attempt project.types state.context signature values state.world with
            | .ok results world => do modify (fun state => {state with world}); pure results
            | .error error world => do
              modify fun state => {state with world}
              observeNode path ("error:" ++ signature.providerKey) before values []
              throw error
          match destination,results with
          | some binder,[value] => bind binder.id value binder.typeId
          | none,[] => pure ()
          | _,_ => throw "ServiceResultArity"
          if signature.op == .setTransportReturn then
            if let [.local _ id] := expressions then
              modify fun state => {state with preparedTransportLocal := some id}
          pure ("serviceCall:" ++ signature.providerKey,values,results)
        | .await wait binder typeId => do
          let value ← ev 0 wait
          spendRuntime
          let .handle handle := value | throw "WaitType"
          let state ← get
          let some process := state.context.processIdentity | throw "MissingProcessContext"
          let (liveLocals,liveTypes) ← liftResult (retainedFrame project rest binder state)
          let world ← match (Runtime.suspendProcessAttempt state.context process handle).run state.world with
            | .ok _ world => pure world
            | .error error world => do
              modify fun state => {state with world}
              throw error
          let continuation := rest.map (fun item => match item with | .stmt _ stmt => stmt | .loop _ count body => .repeat count body | .join _ => .repeat 0 [])
          modify fun state => {state with world,locals := liveLocals,localTypes := liveTypes,wait := some value,outcomeBinder := some binder,outcomeType := some typeId,continuation,pendingWork := rest,exit := "suspended"}
          pure ("await",[value],[])
        | .transportReturn expr => do
          let value ← ev 0 expr
          let state ← get
          if state.context.kind != 2 then throw "TransportContext"
          let alreadyPrepared := match expr with
            | .local _ id => state.preparedTransportLocal == some id
            | _ => false
          if !alreadyPrepared then
            let some sourceSignature := project.services.find? (fun service => service.opcode == "setTransportReturn" && service.inputTypes == [expr.typeId]) | throw "MissingTransportPrepareService"
            let signature : ExecIR.ServiceSignature := ⟨sourceSignature.id,.setTransportReturn,sourceSignature.inputTypes,sourceSignature.resultTypes,sourceSignature.contextMask,sourceSignature.effectMask,sourceSignature.extraFuel,sourceSignature.providerKey,sourceSignature.providerVersion,⟨sourceSignature.abiHash.toArray⟩⟩
            spendRuntime (1+signature.extraFuel)
            match Dispatch.attempt project.types state.context signature [value] state.world with
            | .ok _ world => modify fun state => {state with world}
            | .error error world => do
              modify fun state => {state with world}
              throw error
          spendRuntime
          modify fun state => {state with returned := some [value],exit := "transport"}
          pure ("transportReturn",[value],[value])
        | .ret expressions => do
          let values ← (expressions.zipIdx).mapM (fun (value,index) => ev index value)
          spendRuntime
          let state ← get
          if state.context.kind == 1 then
            let some process := state.context.processIdentity | throw "MissingProcessContext"
            let data ← liftResult (Runtime.getProcess state.world state.context process)
            if data.wait.isSome then throw "RegisteredWaitWithoutSuspension"
            let world ← liftResult (Runtime.completeProcess state.world state.context process values)
            modify fun state => {state with world}
          modify fun state => {state with returned := some values}
          pure ("ret",values,values)
        | .fail error => do spendRuntime; observeNode path "error:fail" before [] []; throw error
        | .unsupported feature => throw ("UnsupportedFeature:" ++ feature)
        | _ => throw "InternalControlFlow"
      observeNode path op before args results
      pure rest
end

def run (project : ModelIR.Project) (body : List ModelIR.Stmt) (machine : Machine) : EStateM.Result String Machine Unit :=
  (show Eval Unit from do
    if (← get).wait.isSome then throw "UseSourceResume"
    statements project (work (machine.sourceRoot.getD ("handler/" ++ toString machine.program) ++ "/body") body)
    if (← get).returned.isNone && (← get).wait.isNone then spendRuntime).run machine
/-- Low-level typed continuation entry. Runner must first authenticate and claim
    the exact queued wait, then supply its updated world/context and segment fuel. -/
def resume (project : ModelIR.Project) (outcome : Value) (machine : Machine) : EStateM.Result String Machine Unit :=
  (show Eval Unit from do
    let state ← get
    if state.exit != "suspended" || state.wait.isNone then throw "MissingSourceSuspension"
    let some typeId := state.outcomeType | throw "MissingWaitOutcomeType"
    let some binder := state.outcomeBinder | throw "MissingWaitOutcomeBinder"
    if !(project.types[typeId]?).any (fun schema => conforms project.types (project.types.length+1) schema outcome) then
      throw "WaitOutcomeType"
    let pending := state.pendingWork
    modify fun state => {state with wait := none,outcomeType := none,outcomeBinder := none,pendingWork := [],continuation := [],returned := none,exit := "returned"}
    bind binder outcome typeId
    statements project pending
    if (← get).returned.isNone && (← get).wait.isNone then spendRuntime).run machine
end LeanAT.Reference.Source



