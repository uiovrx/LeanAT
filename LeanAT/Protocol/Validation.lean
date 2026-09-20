import Lean

namespace LeanAT.Protocol

inductive Flow where | forward | backward deriving BEq, Repr, Inhabited
inductive Sync where | accepted | updated | completed deriving BEq, Repr, Inhabited
inductive Kind where | standalone | ignorableBaseExtension | baseBuiltin deriving BEq, Repr, Inhabited
inductive Action where
  | openHop | requestReleased | responseReady | closeHop
  | acquireLane (lane : Nat) | releaseLane (lane : Nat) | traceTag (tag : String)
  deriving BEq, Repr, Inhabited

-- Facts are immutable snapshots; no arbitrary Lean function or device-state escape.
inductive Guard where
  | always | fieldEq (field value : Nat) | and (a b : Guard) | not (a : Guard)
  deriving BEq, Repr, Inhabited

def Guard.depth : Guard → Nat
  | .always | .fieldEq _ _ => 1
  | .and a b => 1 + max a.depth b.depth
  | .not a => 1 + a.depth

def Guard.fieldsValid : Guard → Bool
  | .always => true
  | .fieldEq f _ => f < 8
  | .and a b => a.fieldsValid && b.fieldsValid
  | .not a => a.fieldsValid

def Guard.fieldsBelow (limit : Nat) : Guard → Bool
  | .always => true
  | .fieldEq f _ => f < limit
  | .and a b => a.fieldsBelow limit && b.fieldsBelow limit
  | .not a => a.fieldsBelow limit

def Guard.eval (facts : List Nat) : Guard → Bool
  | .always => true
  | .fieldEq f v => facts[f]? == some v
  | .and a b => a.eval facts && b.eval facts
  | .not a => !a.eval facts

def Guard.available (facts : List Nat) : Guard → Bool
  | .always => true
  | .fieldEq f _ => f < facts.length
  | .and a b => a.available facts && b.available facts
  | .not a => a.available facts

structure Phase where
  name : String
  flow : Flow
  deriving BEq, Repr, Inhabited
structure Rule where
  pre : Nat
  flow : Flow
  phase : Nat
  sync : Sync
  returned : Option Nat := none
  callGuard : Guard := .always
  guard : Guard := .always
  priority : Option Int := none
  entryLanes : List Nat := []
  callActions : List Action := []
  returnActions : List Action := []
  post : Nat
  deriving BEq, Repr, Inhabited
structure Package where
  name : String
  version : String
  kind : Kind := .standalone
  phases : List Phase
  states : List String
  initial : Nat := 0
  terminal : List Nat
  lanes : List Nat
  rules : List Rule
  cancelPolicy : String := "drainOnly"
  ignorePolicy : String := "acceptedNoForward"
  sourceFile : String := "<generated>"
  sourceLine : Nat := 1
  sourceColumn : Nat := 1
  deriving BEq, Repr, Inhabited

def unique [BEq α] : List α → Bool
  | [] => true
  | a :: rest => !rest.contains a && unique rest

def sameCall (a b : Rule) : Bool :=
  a.pre == b.pre && a.flow == b.flow && a.phase == b.phase
def sameOutcome (a b : Rule) : Bool :=
  sameCall a b && a.sync == b.sync && a.returned == b.returned
def disjoint : Guard → Guard → Bool
  | .fieldEq f v, .fieldEq g w => f == g && v != w
  | .not a, b => a == b
  | a, .not b => a == b
  | _, _ => false
def distinctPriority (a b : Option Int) : Bool :=
  match a, b with | some x, some y => x != y | _, _ => false

def actionValid (p : Package) : Action → Bool
  | .acquireLane n | .releaseLane n => n < p.lanes.length
  | _ => true

def validate (p : Package) (allowBuiltin : Bool := false) : Except String Unit := do
  if p.kind == .baseBuiltin && !allowBuiltin then throw "ReservedBuiltin: user protocol cannot define BaseBuiltin"
  if p.name.isEmpty || p.version.isEmpty then throw "InvalidIdentity: protocol name/version required"
  if p.sourceFile.isEmpty || p.sourceLine == 0 || p.sourceColumn == 0 then throw "MissingSource"
  if p.states.isEmpty || p.phases.isEmpty || p.rules.isEmpty then throw "EmptyProtocol"
  if p.states.length > 256 || p.phases.length > 256 || p.rules.length > 4096 || p.lanes.length > 32 then throw "ProtocolTableLimit"
  if !unique p.states || !unique (p.phases.map (·.name)) then throw "DuplicateProtocolKey"
  if p.initial ≥ p.states.length || p.terminal.isEmpty || !unique p.terminal then throw "InvalidInitialOrTerminal"
  if p.terminal.any (· ≥ p.states.length) then throw "UnknownTerminalState"
  if p.lanes.any (· == 0) then throw "LaneCapacity: capacity must be positive"
  if p.cancelPolicy != "drainOnly" then throw "UnsupportedCancelPolicy"
  if p.kind == .ignorableBaseExtension && p.ignorePolicy != "acceptedNoForward" then throw "InvalidIgnorePolicy"
  for r in p.rules do
    if r.pre ≥ p.states.length || r.post ≥ p.states.length then throw "UnknownState"
    if p.terminal.contains r.pre then throw "TerminalOutgoing"
    let some phase := p.phases[r.phase]? | throw "UnknownPhase"
    if phase.flow != r.flow then throw "WrongCallFlow"
    if r.guard.depth > 32 || !r.guard.fieldsValid then throw "IllegalGuardEffectOrDepth"
    if r.callGuard.depth > 32 || !r.callGuard.fieldsBelow 4 then throw "FutureFactInCallGuard"
    match r.sync, r.returned with
    | .updated, some n => if n ≥ p.phases.length then throw "UnknownReturnedPhase"
    | .updated, none => throw "UpdatedRequiresPhase"
    | _, some _ => throw "UnexpectedReturnedPhase"
    | _, none => pure ()
    if !unique r.entryLanes || r.entryLanes.any (· ≥ p.lanes.length) then throw "InvalidEntryLane"
    if !(r.callActions ++ r.returnActions).all (actionValid p) then throw "UnknownLane"
    let lifecycle := (r.callActions ++ r.returnActions).filter (fun a => match a with | .traceTag _ => false | _ => true)
    if !unique lifecycle then throw "DuplicateLifecycleAction"
    if r.callActions.contains .closeHop then throw "FutureReturnActionAtCall"
    if (r.callActions ++ r.returnActions).contains .closeHop && !p.terminal.contains r.post then throw "CloseHopNonterminal"
    if p.terminal.contains r.post && !(r.callActions ++ r.returnActions).contains .closeHop then throw "TerminalWithoutCloseHop"
    if p.kind == .ignorableBaseExtension then
      if r.sync != .accepted || r.pre != r.post || !r.entryLanes.isEmpty ||
          !(r.callActions ++ r.returnActions).all (fun a => match a with | .traceTag _ => true | _ => false) then
        throw "NonConservativeExtension"
  for i in List.range p.rules.length do
    for j in List.range i do
      let a := p.rules[i]!
      let b := p.rules[j]!
      if sameCall a b && (a.entryLanes != b.entryLanes || a.callActions != b.callActions || a.callGuard != b.callGuard) then throw "AmbiguousCallSplit"
      if sameOutcome a b && !disjoint a.guard b.guard && !distinctPriority a.priority b.priority then throw "AmbiguousRule"

structure CheckedProtocol where
  package : Package
  checked : validate package = .ok ()

def check (p : Package) : Except String CheckedProtocol :=
  match h : validate p with
  | .ok () => .ok ⟨p, h⟩
  | .error e => .error e

/-- Select a complete call/return exchange using saved snapshot facts. -/
def selectExchange (p : Package) (pre : Nat) (flow : Flow) (phase : Nat)
    (sync : Sync) (returned : Option Nat) (facts : List Nat) : Except String Rule := do
  validate p (p.kind == .baseBuiltin)
  let rows := p.rules.filter (fun r => r.pre == pre && r.flow == flow && r.phase == phase &&
    r.sync == sync && r.returned == returned && r.callGuard.available facts && r.guard.available facts &&
    r.callGuard.eval facts && r.guard.eval facts)
  let some first := rows.head? | throw "UnmatchedExchange"
  pure (rows.foldl (fun best r => if r.priority.getD 0 < best.priority.getD 0 then r else best) first)

structure GraphEdge where
  pre : Nat
  post : Nat
  flow : Flow
  phase : Nat
  sync : Sync
  returned : Option Nat
  deriving Repr, BEq
def protocolGraph (p : Package) : List GraphEdge :=
  p.rules.map (fun r => ⟨r.pre,r.post,r.flow,r.phase,r.sync,r.returned⟩)

end LeanAT.Protocol

