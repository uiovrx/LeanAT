import LeanAT.ModelIR.Validation
import LeanAT.Pure.Numeric
import LeanAT.Reference.Source
namespace LeanAT.ModelIR
structure TraceEntry where
  tag : String
  values : List Value
  deriving Repr, BEq
structure RuntimeState where
  values : List (Nat × Value) := []
  locals : List (Nat × Value) := []
  trace : List TraceEntry := []
  returned : Option (List Value) := none
  lastEventTime : Option Nat := none
  eventsAtTick : Nat := 0
  deriving Repr, BEq
private def getValue (xs : List (Nat × Value)) (id : Nat) : Except String Value :=
  match xs.find? (fun x => x.1 == id) with | some x => .ok x.2 | none => .error "UnknownValue"
private inductive PureWork where
  | statement (stmt : Stmt)
  | loop (remaining : Nat) (body : List Stmt)
mutual
private def evalExprIn (p : Project) (s : RuntimeState) : Nat → Expr → Except String Value
  | 0, _ => .error "FuelExhausted"
  | fuel+1, e => do
    let ev := evalExprIn p s fuel
    match e with
    | .literal _ v => pure v
    | .local _ id => getValue s.locals id
    | .state _ id => getValue s.values id
    | .select _ c y n =>
      match ← ev c with
      | .bool true => ev y
      | .bool false => ev n
      | _ => throw "ExpectedBool"
    | .binary _ op a b =>
      match Pure.evalBinary op (← ev a) (← ev b) with
      | .ok v => pure v
      | .error err => throw (reprStr err)
    | .field _ obj idx =>
      let .record vs ← ev obj | throw "ExpectedRecord"
      match vs[idx]? with | some v => pure v | none => throw "IndexOutOfRange"
    | .index _ obj idx =>
      let .vec vs ← ev obj | throw "ExpectedVector"
      let .bits _ i ← ev idx | throw "ExpectedIndex"
      match vs[i]? with | some v => pure v | none => throw "IndexOutOfRange"
    | .makeRecord _ fields => return .record (← fields.mapM ev)
    | .makeVariant _ tag fields => return .variant tag (← fields.mapM ev)
    | .unary _ mode value => Numeric.evalUnary mode (← ev value)
    | .compare _ mode a b => Numeric.evalCompare mode (← ev a) (← ev b)
    | .convert typeId mode value =>
      let some (.bits width) := p.types[typeId]? | throw "ConvertType"
      Numeric.evalConvert mode width (← ev value)
    | .variantTag _ value =>
      let .variant tag _ ← ev value | throw "ExpectedVariant"
      pure (.bits 64 tag)
    | .variantGet _ value tag index =>
      let .variant actual fields ← ev value | throw "ExpectedVariant"
      if actual != tag then throw "VariantTagMismatch"
      match fields[index]? with | some value => pure value | none => throw "IndexOutOfRange"
    | .makeVec _ values => pure (.vec (← values.mapM ev))
    | .vecSet _ vec index value =>
      let .vec values ← ev vec | throw "ExpectedVector"
      let .bits _ index ← ev index | throw "ExpectedIndex"
      let value ← ev value
      if index ≥ values.length then throw "IndexOutOfRange"
      pure (.vec (values.set index value))
    | .callPure _ id args =>
      let some callee := p.handlers.find? (fun h => h.id == id) | throw "UnknownPureFunction"
      if callee.context != .pureFunction then throw "ImpureCall"
      let values ← args.mapM ev
      if values.length != callee.parameters.length then throw "PureInputArity"
      let locals := (callee.parameters.zip values).map (fun (binder,value) => (binder.id,value))
      let result ← evalPureWork p {locals} fuel (callee.body.map PureWork.statement)
      let [value] := result | throw "PureReturnArity"
      pure value
private def evalPureWork (p : Project) (s : RuntimeState) : Nat → List PureWork → Except String (List Value)
  | 0, _ => throw "FuelExhausted"
  | _+1, [] => pure []
  | fuel+1, item::rest => do
    let ev := evalExprIn p s fuel
    match item with
    | .loop 0 _ => evalPureWork p s fuel rest
    | .loop (n+1) body => evalPureWork p s fuel (body.map PureWork.statement ++ [.loop n body] ++ rest)
    | .statement stmt => match stmt with
      | .letVal id expr =>
        let value ← ev expr
        evalPureWork p {s with locals := (id,value)::s.locals.filter (fun pair => pair.1 != id)} fuel rest
      | .check condition error =>
        let .bool condition ← ev condition | throw "ExpectedBool"
        if !condition then throw error
        evalPureWork p s fuel rest
      | .branch condition yes no =>
        let .bool condition ← ev condition | throw "ExpectedBool"
        evalPureWork p s fuel ((if condition then yes else no).map PureWork.statement ++ rest)
      | .repeat n body => evalPureWork p s fuel (.loop n body::rest)
      | .ret values => values.mapM ev
      | .fail error => throw error
      | _ => throw "ImpureFunctionBody"
end

def evalExpr (s : RuntimeState) (fuel : Nat) (expr : Expr) (project : Project := {}) : Except String Value :=
  evalExprIn project s fuel expr
private def putValue (xs : List (Nat × Value)) (id : Nat) (v : Value) :=
  (id,v)::xs.filter (fun x => x.1 != id)
/-- Fuel is shared across nested blocks and loops. Partial writes never escape an error. -/
private inductive WorkItem where
  | statement (stmt : Stmt)
  | loop (remaining : Nat) (body : List Stmt)
private def execWork (p : Project) : Nat → RuntimeState → List WorkItem → Except String (RuntimeState × Nat)
  | fuel, s, [] => .ok (s,fuel)
  | 0, _, _::_ => .error "FuelExhausted"
  | fuel+1, s, work::rest => do
    if s.returned.isSome then return (s,fuel+1)
    match work with
    | .loop 0 _ => execWork p fuel s rest
    | .loop (n+1) body => execWork p fuel s (body.map WorkItem.statement ++ [.loop n body] ++ rest)
    | .statement stmt =>
      let ev := fun expr => evalExpr s p.profile.instructionFuel expr p
      match stmt with
      | .branch c yes no =>
        let .bool b ← ev c | throw "ExpectedBool"
        execWork p fuel s ((if b then yes else no).map WorkItem.statement ++ rest)
      | .repeat n body => execWork p fuel s (.loop n body :: rest)
      | _ =>
        let next ← match stmt with
        | .readNow binder => pure {s with locals := putValue s.locals binder.id (.bits 64 (s.lastEventTime.getD 0))}
        | .serviceCall _ _ _ => throw "UnsupportedRuntimeServiceReference"
        | .await _ _ _ => throw "UnsupportedProcessSuspensionReference"
        | .letVal id e => do pure {s with locals := putValue s.locals id (← ev e)}
        | .writeState id e => do pure {s with values := putValue s.values id (← ev e)}
        | .check c err => do
          let .bool b ← ev c | throw "ExpectedBool"
          if b then pure s else throw err
        | .emit tag es => do pure {s with trace := s.trace ++ [⟨tag, ← es.mapM ev⟩]}
        | .fail err => throw err
        | .transportReturn e => do pure {s with returned := some [← ev e]}
        | .ret es => do pure {s with returned := some (← es.mapM ev)}
        | .unsupported feature => throw ("UnsupportedFeature: " ++ feature)
        | _ => throw "InternalControlFlow"
        if next.returned.isSome then pure (next,fuel) else execWork p fuel next rest

def execStmts (p : Project) (fuel : Nat) (s : RuntimeState) (stmts : List Stmt) : Except String (RuntimeState × Nat) :=
  execWork p fuel s (stmts.map WorkItem.statement)
/-- Full service-capable bounded AST execution, with actual per-node fuel and failure traces. -/
def runReferenceSegment (p : Project) (body : List Stmt) (machine : LeanAT.Reference.Source.Machine) :=
  LeanAT.Reference.Source.run p body machine

def runSegment (p : Project) (s : RuntimeState) (body : List Stmt) : Except String RuntimeState :=
  (execStmts p p.profile.instructionFuel {s with locals := [], returned := none} body).map Prod.fst
structure Event where
  time : Nat
  ordinal : Nat
  handler : Nat
  deriving Repr, BEq
inductive StopReason where
  | quiescent | waitingForEnvironment | runBudgetReached | horizonReached | failed (error : String)
  deriving Repr, BEq
structure RunResult where
  state : RuntimeState
  remaining : List Event
  reason : StopReason
  deriving Repr, BEq

def stepEvent (p : ValidatedProject) (s : RuntimeState) (e : Event) : Except String RuntimeState := do
  let some h := p.project.handlers.find? (fun h => h.id == e.handler) | throw "UnknownHandler"
  if h.context != .timedHandler then throw "HandlerNotTimed"
  if e.time ≥ 2^64 then throw "TimeOutOfRange"
  if s.lastEventTime.any (fun time => e.time < time) then throw "EnvironmentViolation: event time reversal"
  let count := if s.lastEventTime == some e.time then s.eventsAtTick else 0
  if count ≥ p.project.profile.maxEventsPerTick then throw "ZenoDetected"
  let next ← runSegment p.project {s with lastEventTime := some e.time} h.body
  pure {next with lastEventTime := some e.time, eventsAtTick := count+1}
inductive Step (p : ValidatedProject) : RuntimeState → Event → RuntimeState → Prop where
  | event {s e next} : stepEvent p s e = .ok next → Step p s e next

def runFrom (p : ValidatedProject) (horizon : Option Nat) : Nat → RuntimeState → List Event → RunResult
  | _, s, [] => ⟨s,[],.quiescent⟩
  | 0, s, es => ⟨s,es,.runBudgetReached⟩
  | fuel+1, s, e::es =>
    if horizon.any (fun h => e.time > h) then ⟨s,e::es,.horizonReached⟩ else
    match stepEvent p s e with
    | .error err => ⟨s,e::es,.failed err⟩
    | .ok next => runFrom p horizon fuel next es

def runBounded (p : ValidatedProject) (events : List Event) (fuel : Nat) (horizon : Option Nat) : RunResult :=
  let initial : RuntimeState := { values := p.project.states.map (fun s => (s.id,s.initial)) }
  if !p.project.components.isEmpty || !p.project.systems.isEmpty || !p.project.externalContracts.isEmpty || !p.project.enabledCapabilities.isEmpty then
    ⟨initial,events,.failed "UnsupportedStructuredProjectExecution"⟩
  else if events.length > p.project.profile.eventCapacity then ⟨initial,events,.failed "EventCapacity"⟩
  else if !(events.zip (events.drop 1)).all (fun (a,b) => a.time < b.time || (a.time == b.time && a.ordinal < b.ordinal)) then
    ⟨initial,events,.failed "EnvironmentViolation: event order"⟩
  else runFrom p horizon fuel initial events
end LeanAT.ModelIR







