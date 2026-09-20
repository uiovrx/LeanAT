import LeanAT.Frontend.Commands
import LeanAT.Frontend.Builtins
import LeanAT.Frontend.Source
import LeanAT.Frontend.PureCalls
open Lean Elab Command
namespace LeanAT.Frontend

declare_syntax_cat atStmt
syntax "set " ident " := " term : atStmt
syntax "set " "output " ident " := " term : atStmt
syntax "let " ident " : " ident " := " term : atStmt
syntax "await " "until " term : atStmt
syntax "await " "after " term : atStmt
syntax "let " ident " ← " "registerWait " "until " term : atStmt
syntax "let " ident " ← " "await " ident : atStmt
syntax "return" : atStmt
syntax "fail " str : atStmt
syntax "emit " str : atStmt
syntax "await " ident : atStmt
syntax "awaitSlot " ident : atStmt
syntax "returnTransport " ident : atStmt
declare_syntax_cat atMember
syntax "state " ident " : " ident " := " num : atMember
syntax "requires " term " := " term : atMember
syntax "input " ident " : " ident : atMember
syntax "output " ident " : " ident " := " term : atMember
syntax "on " ident ident " do" ppLine (colGt atStmt)* : atMember
syntax "on " ident ident ident ident " do" ppLine (colGt atStmt)* : atMember
syntax "process " ident " do" ppLine (colGt atStmt)* : atMember
syntax "process " ident " : " ident " maxInstances " num " frameBytes " num " results " num " do" ppLine (colGt atStmt)* : atMember
syntax "target " ident " : " "TlmBase " num " capacity " num " payload " num " mask " num : atMember
syntax "initiator " ident " : " "TlmBase " num " capacity " num " payload " num " mask " num : atMember
syntax (name := atComponentBlock) "at_component " ident " where" ppLine (colGt atMember)* : command
declare_syntax_cat atSystemMember
syntax "instance " ident " := " ident : atSystemMember
syntax "bind " ident " => " ident : atSystemMember
syntax "input " ident " : " ident " := " term : atSystemMember
syntax "output " ident " : " ident : atSystemMember
syntax "connect " ident " => " ident : atSystemMember
syntax (name := atSystemBlock) "at_system " ident " where" ppLine (colGt atSystemMember)* : command

private def typeInfo (stx : TSyntax `ident) : CommandElabM (Nat × Nat) :=
  match stx.getId with
  | `UInt8 => pure (2,8)
  | `UInt16 => pure (3,16)
  | `UInt32 => pure (4,32)
  | `UInt64 => pure (5,64)
  | _ => throwErrorAt stx "UnsupportedRuntimeType: block state supports UInt8/16/32/64"

private def signalType (stx : TSyntax `ident) : CommandElabM (Nat × Nat) :=
  if stx.getId == `Bool then pure (1,0) else typeInfo stx

private def signalValue (typ : TSyntax `ident) (value : TSyntax `term) : CommandElabM (TSyntax `term) := do
  let (_,width) ← signalType typ
  if width == 0 then
    match value with
    | `(true) => `(LeanAT.Value.bool true)
    | `(false) => `(LeanAT.Value.bool false)
    | _ => throwErrorAt value "InvalidBoolInitializer"
  else
    match value with
    | `($n:num) => `(LeanAT.Value.bits $(quote width) $n)
    | _ => throwErrorAt value "InvalidBitsInitializer"

private partial def valueExpr (states : Array (Name × Nat × Nat)) (locals : Array (Name × Nat)) (pureFunctions : Array (Name × Nat)) (expected : Nat) (value : TSyntax `term) : CommandElabM (TSyntax `term) := do
  let infer := fun (value : TSyntax `term) => match value with
    | `($n:ident) => ((locals.find? (fun x => x.1 == n.getId)).map (·.2)).orElse
        (fun _ => (states.find? (fun x => x.1 == n.getId)).map (·.2.1))
    | `(true) | `(false) => some 1
    | _ => none
  match value with
  | `(($e:term)) => valueExpr states locals pureFunctions expected e
  | `(!$e:term) =>
    let a ← valueExpr states locals pureFunctions 1 e
    if expected != 1 then throwErrorAt value "ExpectedBool"
    `(LeanAT.ModelIR.Expr.unary 1 0 $a)
  | `(~~~$e:term) | `(-$e:term) =>
    let mode := match value with | `(~~~$_:term) => 1 | _ => 2
    let a ← valueExpr states locals pureFunctions expected e
    `(LeanAT.ModelIR.Expr.unary $(quote expected) $(quote mode) $a)
  | `($a:term + $b:term) | `($a:term - $b:term) | `($a:term * $b:term) =>
    let op ← match value with
      | `($_:term + $_:term) => `(LeanAT.Pure.BinaryOp.addWrap)
      | `($_:term - $_:term) => `(LeanAT.Pure.BinaryOp.subWrap)
      | _ => `(LeanAT.Pure.BinaryOp.mulWrap)
    let left ← valueExpr states locals pureFunctions expected a
    let right ← valueExpr states locals pureFunctions expected b
    `(LeanAT.ModelIR.Expr.binary $(quote expected) $op $left $right)
  | `($a:term == $b:term) | `($a:term != $b:term) | `($a:term < $b:term)
  | `($a:term ≤ $b:term) | `($a:term > $b:term) | `($a:term ≥ $b:term) =>
    if expected != 1 then throwErrorAt value "ExpectedBool"
    let mode := match value with
      | `($_:term == $_:term) => 0 | `($_:term != $_:term) => 1
      | `($_:term < $_:term) => 2 | `($_:term ≤ $_:term) => 3
      | `($_:term > $_:term) => 4 | _ => 5
    let inputType := ((infer a).orElse (fun _ => infer b)).getD 5
    let left ← valueExpr states locals pureFunctions inputType a
    let right ← valueExpr states locals pureFunctions inputType b
    `(LeanAT.ModelIR.Expr.compare 1 $(quote mode) $left $right)
  | `(if $c:term then $y:term else $n:term) =>
    let condition ← valueExpr states locals pureFunctions 1 c
    let yes ← valueExpr states locals pureFunctions expected y
    let no ← valueExpr states locals pureFunctions expected n
    `(LeanAT.ModelIR.Expr.select $(quote expected) $condition $yes $no)
  | `($fn:ident $arg:term) =>
    let conversion := match fn.getId with
      | `UInt8.toUInt64 => some (2,5,0) | `UInt16.toUInt64 => some (3,5,0)
      | `UInt32.toUInt64 => some (4,5,0) | `UInt64.toUInt8 => some (5,2,1)
      | `UInt64.toUInt16 => some (5,3,1) | `UInt64.toUInt32 => some (5,4,1)
      | _ => none
    if let some (_,handlerId) := pureFunctions.find? (fun item => item.1 == fn.getId) then
      if expected != 1 then throwErrorAt value "PureResultTypeMismatch"
      let argument ← valueExpr states locals pureFunctions 1 arg
      return ← `(LeanAT.ModelIR.Expr.callPure 1 $(quote handlerId) [$argument])
    let some (inputType,outputType,mode) := conversion | throwErrorAt fn "UnsupportedPureFunction"
    if expected != outputType then throwErrorAt value "ConversionTypeMismatch"
    let argument ← valueExpr states locals pureFunctions inputType arg
    `(LeanAT.ModelIR.Expr.convert $(quote outputType) $(quote mode) $argument)
  | `(true) =>
    if expected != 1 then throwErrorAt value "ExpectedBool"
    `(LeanAT.ModelIR.Expr.literal 1 (.bool true))
  | `(false) =>
    if expected != 1 then throwErrorAt value "ExpectedBool"
    `(LeanAT.ModelIR.Expr.literal 1 (.bool false))
  | `($n:num) =>
    let width := if expected == 2 then 8 else if expected == 3 then 16 else if expected == 4 then 32 else 64
    `(LeanAT.ModelIR.Expr.literal $(quote expected) (.bits $(quote width) $n))
  | `($n:ident) =>
    if let some idx := locals.findIdx? (fun x => x.1 == n.getId) then
      if locals[idx]!.2 != expected then throwErrorAt n "LocalTypeMismatch"
      `(LeanAT.ModelIR.Expr.local $(quote expected) $(quote idx))
    else if let some idx := states.findIdx? (fun x => x.1 == n.getId) then
      if states[idx]!.2.1 != expected then throwErrorAt n "StateTypeMismatch"
      `(LeanAT.ModelIR.Expr.state $(quote expected) $(quote idx))
    else throwErrorAt n "UnknownValue"
  | _ => throwErrorAt value "UnsupportedExpression: expected bounded literal, local, or state"

private def originTerm (path : String) (stx : Syntax) (generated : Bool := false) (ancestry : List String := []) : CommandElabM (TSyntax `term) := do
  let some start := stx.getPos? | throwErrorAt stx "MissingNativeSourceStart"
  let some finish := stx.getTailPos? | throwErrorAt stx "MissingNativeSourceEnd"
  let file ← getFileName
  let kind ← if generated then `(LeanAT.SourceMapping.OriginKind.generated) else `(LeanAT.SourceMapping.OriginKind.native)
  `(({nodePath := $(quote path), documentPath := $(quote file), startByte := $(quote start.byteIdx), endByte := $(quote finish.byteIdx), kind := $kind, ancestry := $(quote ancestry)} : LeanAT.SourceMapping.NodeOrigin))

private partial def expressionOrigins (path : String) (value : TSyntax `term) : CommandElabM (Array (TSyntax `term)) := do
  if let `(($e:term)) := value then return ← expressionOrigins path e
  let children : Array (TSyntax `term) := match value with
    | `(!$e:term) | `(~~~$e:term) | `(-$e:term) | `($_:ident $e:term) => #[e]
    | `($a:term + $b:term) | `($a:term - $b:term) | `($a:term * $b:term)
    | `($a:term == $b:term) | `($a:term != $b:term) | `($a:term < $b:term)
    | `($a:term ≤ $b:term) | `($a:term > $b:term) | `($a:term ≥ $b:term) => #[a,b]
    | `(if $c:term then $y:term else $n:term) => #[c,y,n]
    | _ => #[]
  let mut origins := #[← originTerm path value]
  for (child,idx) in children.toList.zipIdx do
    origins := origins ++ (← expressionOrigins s!"{path}/expr/{idx}" child)
  return origins

private def bundleTerm (origins : Array (TSyntax `term)) (extraOrigins : Array (TSyntax `term) := #[]) : CommandElabM (TSyntax `term) := do
  let file ← getFileName
  let map ← getFileMap
  `(({documents := [{path := $(quote file), content := $(quote map.source)}], origins := [$origins,*] ++ List.flatten [$extraOrigins,*]} : LeanAT.SourceMapping.Bundle))

private def bodyTerm (states : Array (Name × Nat × Nat)) (body : Array (TSyntax `atStmt)) (isProcess : Bool := false) (isTransport : Bool := false) (signals : Array (Name × Nat × Bool) := #[]) (pureFunctions : Array (Name × Nat) := #[]) : CommandElabM (TSyntax `term × Bool × Array (TSyntax `term)) := do
  let mut out : Array (TSyntax `term) := #[]
  let mut origins : Array (TSyntax `term) := #[]
  let mut locals : Array (Name × Nat) := #[]
  let mut usesServices := false
  let mut consumedWaits : List Nat := []
  let mut pendingWait : Option Nat := none
  for stmt in body do
    let first := out.size
    match stmt with
    | `(atStmt| set output $name:ident := $value:term) =>
      if isTransport then throwErrorAt name "OutputWriteForbiddenInContext"
      let some idx := signals.findIdx? (fun x => x.1 == name.getId) | throwErrorAt name "UnknownOutput"
      let (_,tid,isOutput) := signals[idx]!
      if !isOutput then throwErrorAt name "CannotWriteInput"
      let e ← valueExpr states locals pureFunctions tid value
      out := out.push (← `(LeanAT.ModelIR.Stmt.serviceCall none $(quote (100+tid)) [.literal 5 (.bits 64 $(quote idx)), $e]))
    | `(atStmt| set $name:ident := $value:term) =>
      let some idx := states.findIdx? (fun s => s.1 == name.getId) | throwErrorAt name "UnknownState"
      let (_, typ, _) := states[idx]!
      let e ← valueExpr states locals pureFunctions typ value
      out := out.push (← `(LeanAT.ModelIR.Stmt.writeState $(quote idx) $e))
    | `(atStmt| let $name:ident : $typ:ident := now) =>
      if locals.any (·.1 == name.getId) then throwErrorAt name "DuplicateLocal"
      if typ.getId != `UInt64 then throwErrorAt typ "NowRequiresTickRepresentation"
      out := out.push (← `(LeanAT.ModelIR.Stmt.readNow {id := $(quote locals.size), typeId := 5}))
      locals := locals.push (name.getId,5)
    | `(atStmt| let $name:ident : $typ:ident := $value:term) =>
      if locals.any (·.1 == name.getId) then throwErrorAt name "DuplicateLocal"
      let (tid,_) ← signalType typ
      let e ← valueExpr states locals pureFunctions tid value
      out := out.push (← `(LeanAT.ModelIR.Stmt.letVal $(quote locals.size) $e))
      locals := locals.push (name.getId,tid)
    | `(atStmt| await until $time:term) | `(atStmt| await after $time:term) =>
      if !isProcess then throwErrorAt stmt "AwaitForbiddenInContext"
      if pendingWait.isSome then throwErrorAt stmt "ProcessAlreadyHasRegisteredWait"
      let isAfter := match stmt with | `(atStmt| await after $_:term) => true | _ => false
      let tick ← valueExpr states locals pureFunctions 5 time
      let contextId := locals.size
      out := out.push (← `(LeanAT.ModelIR.Stmt.serviceCall (some {id := $(quote contextId), typeId := 6}) 0 []))
      locals := locals.push (Name.mkSimple s!"_context{contextId}",6)
      let waitId := locals.size
      out := out.push (← `(LeanAT.ModelIR.Stmt.serviceCall (some {id := $(quote waitId), typeId := 7}) 1 [.local 6 $(quote contextId), .literal 5 (.bits 64 $(quote (if isAfter then 3 else 4))), $tick]))
      locals := locals.push (Name.mkSimple s!"_wait{waitId}",7)
      out := out.push (← `(LeanAT.ModelIR.Stmt.await (.local 7 $(quote waitId)) $(quote locals.size) 0))
      locals := locals.push (Name.mkSimple s!"_outcome{locals.size}",0)
      consumedWaits := waitId :: consumedWaits
      usesServices := true
    | `(atStmt| let $name:ident ← registerWait until $time:term) =>
      if !isProcess then throwErrorAt stmt "AwaitForbiddenInContext"
      if pendingWait.isSome then throwErrorAt stmt "ProcessAlreadyHasRegisteredWait"
      if locals.any (·.1 == name.getId) then throwErrorAt name "DuplicateLocal"
      let tick ← valueExpr states locals pureFunctions 5 time
      let contextId := locals.size
      out := out.push (← `(LeanAT.ModelIR.Stmt.serviceCall (some {id := $(quote contextId), typeId := 6}) 0 []))
      locals := locals.push (Name.mkSimple s!"_context{contextId}",6)
      out := out.push (← `(LeanAT.ModelIR.Stmt.serviceCall (some {id := $(quote locals.size), typeId := 7}) 1 [.local 6 $(quote contextId), .literal 5 (.bits 64 4), $tick]))
      pendingWait := some locals.size
      locals := locals.push (name.getId,7)
      usesServices := true
    | `(atStmt| let $name:ident ← await $wait:ident) =>
      if !isProcess then throwErrorAt stmt "AwaitForbiddenInContext"
      if locals.any (·.1 == name.getId) then throwErrorAt name "DuplicateLocal"
      let some id := locals.findIdx? (fun x => x.1 == wait.getId) | throwErrorAt wait "UnknownWait"
      if locals[id]!.2 != 7 then throwErrorAt wait "ExpectedWaitHandle"
      if consumedWaits.contains id then throwErrorAt wait "WaitAlreadyConsumed"
      out := out.push (← `(LeanAT.ModelIR.Stmt.await (.local 7 $(quote id)) $(quote locals.size) 0))
      locals := locals.push (name.getId,0)
      consumedWaits := id :: consumedWaits
      pendingWait := none
    | `(atStmt| return) =>
      if pendingWait.isSome then throwErrorAt stmt "UnconsumedRegisteredWait"
      out := out.push (← `(LeanAT.ModelIR.Stmt.ret []))
    | `(atStmt| fail $message:str) => out := out.push (← `(LeanAT.ModelIR.Stmt.fail $message))
    | `(atStmt| emit $tag:str) => out := out.push (← `(LeanAT.ModelIR.Stmt.emit $tag []))
    | `(atStmt| await $kind:ident) => throwErrorAt kind "UnsupportedSyntax: await requires process frame/scheduler schema"
    | `(atStmt| awaitSlot $pool:ident) =>
      if !isProcess then throwErrorAt pool "AwaitSlotForbiddenInContext"
      throwErrorAt pool "UnsupportedSyntax: task-slot provider must be registered through typed service declaration"
    | `(atStmt| returnTransport $kind:ident) =>
      if !isTransport then throwErrorAt kind "TransportReturnForbiddenInContext"
      if kind.getId != `accepted then throwErrorAt kind "UnpreparedTransportResponse: Updated/Completed require a checked call-time response provider"
      out := out.push (← `(LeanAT.ModelIR.Stmt.transportReturn (.literal 12 LeanAT.Frontend.acceptedTransport)))
    | _ => throwUnsupportedSyntax
    let root := s!"body/{first}"
    let anchor := if out.size > first + 1 then s!"body/{out.size-1}" else root
    for idx in [first:out.size] do
      let generated := idx + 1 != out.size
      origins := origins.push (← originTerm s!"body/{idx}" stmt generated (if generated then [anchor] else []))
    match stmt with
    | `(atStmt| set output $_:ident := $value:term) =>
      origins := origins.push (← originTerm (root ++ "/expr/0") stmt true [anchor])
      origins := origins ++ (← expressionOrigins (root ++ "/expr/1") value)
    | `(atStmt| let $_:ident : $_:ident := now) => pure ()
    | `(atStmt| set $_:ident := $value:term) | `(atStmt| let $_:ident : $_:ident := $value:term) =>
      origins := origins ++ (← expressionOrigins (root ++ "/expr/0") value)
    | `(atStmt| await until $time:term) | `(atStmt| await after $time:term) =>
      for arg in [0:2] do
        origins := origins.push (← originTerm s!"body/{first+1}/expr/{arg}" stmt true [anchor])
      origins := origins ++ (← expressionOrigins s!"body/{first+1}/expr/2" time)
      origins := origins.push (← originTerm s!"body/{first+2}/expr/0" stmt true [anchor])
    | `(atStmt| let $_:ident ← registerWait until $time:term) =>
      for arg in [0:2] do
        origins := origins.push (← originTerm s!"body/{first+1}/expr/{arg}" stmt true [anchor])
      origins := origins ++ (← expressionOrigins s!"body/{first+1}/expr/2" time)
    | `(atStmt| let $_:ident ← await $wait:ident) =>
      origins := origins.push (← originTerm (root ++ "/expr/0") wait)
    | `(atStmt| returnTransport $kind:ident) =>
      origins := origins.push (← originTerm (root ++ "/expr/0") kind true [anchor])
    | _ => pure ()
  pure (← `([$out,*]), usesServices, origins)

private partial def pureReferences (stx : Syntax) : Array (TSyntax `ident) := Id.run do
  let mut refs := #[]
  if let `($fn:ident $_:term) := (⟨stx⟩ : TSyntax `term) then
    if !([`UInt8.toUInt64,`UInt16.toUInt64,`UInt32.toUInt64,`UInt64.toUInt8,`UInt64.toUInt16,`UInt64.toUInt32].contains fn.getId) then
      refs := refs.push fn
  for child in stx.getArgs do
    refs := refs ++ pureReferences child
  return refs

@[command_elab atComponentBlock] def elabComponentBlock : CommandElab := fun stx => do
  match stx with
  | `(at_component $name:ident where $members:atMember*) =>
    let mut names : Array (Name × Nat × Nat) := #[]
    let mut endpointNames : Array (Name × Bool) := #[]
    let mut signalNames : Array (Name × Nat × Bool) := #[]
    let mut states : Array (TSyntax `term) := #[]
    let mut ports : Array (TSyntax `term) := #[]
    let mut sidebands : Array (TSyntax `term) := #[]
    let mut handlers : Array (TSyntax `term) := #[]
    let mut contexts : Array (TSyntax `term) := #[]
    let mut handlerNames : Array (TSyntax `term) := #[]
    let mut requirements : Array (TSyntax `term) := #[]
    let handlerCount := (members.toList.map (·.raw)).countP fun member => match member with
      | `(atMember| on $_:ident $_:ident do $_:atStmt*)
      | `(atMember| on $_:ident $_:ident $_:ident $_:ident do $_:atStmt*)
      | `(atMember| process $_:ident : $_:ident maxInstances $_:num frameBytes $_:num results $_:num do $_:atStmt*) => true
      | _ => false
    let mut pureFunctions : Array (Name × Nat) := #[]
    let mut pureHandlers : Array (TSyntax `term) := #[]
    let mut generatedOrigins : Array (TSyntax `term) := #[]
    let mut callerId := 0
    for member in members do
      let body := match member with
        | `(atMember| on $_:ident $_:ident do $body:atStmt*)
        | `(atMember| on $_:ident $_:ident $_:ident $_:ident do $body:atStmt*)
        | `(atMember| process $_:ident : $_:ident maxInstances $_:num frameBytes $_:num results $_:num do $body:atStmt*) => some body
        | _ => none
      if let some body := body then
        for statement in body do
          for reference in pureReferences statement do
            if !pureFunctions.any (fun item => item.1 == reference.getId) then
              let id := handlerCount + pureHandlers.size
              let handler ← reifiedHandlerTerm reference id
              pureFunctions := pureFunctions.push (reference.getId,id)
              pureHandlers := pureHandlers.push handler
              let path := s!"handler/{id}"
              let origin ← originTerm path reference true [s!"handler/{callerId}"]
              generatedOrigins := generatedOrigins.push (← `((LeanAT.SourceMapping.handlerNodes $(quote path) $handler).map (fun node => ({$origin with nodePath := node.path} : LeanAT.SourceMapping.NodeOrigin))))
        callerId := callerId + 1
    let mut nodeOrigins : Array (TSyntax `term) := #[]
    let mut needsServices := false
    let mut needsTransport := false
    for member in members do
      match member with
      | `(atMember| state $n:ident : $typ:ident := $value:num) =>
        if names.any (fun s => s.1 == n.getId) then throwErrorAt n "DuplicateState"
        let (tid,width) ← typeInfo typ
        states := states.push (← `(LeanAT.ModelIR.StateSlot.mk $(quote names.size) $(quote tid) (.bits $(quote width) $value)))
        names := names.push (n.getId, tid, width)
      | `(atMember| target $n:ident : TlmBase $_:num capacity $_:num payload $_:num mask $_:num) =>
        endpointNames := endpointNames.push (n.getId, true)
      | `(atMember| initiator $n:ident : TlmBase $_:num capacity $_:num payload $_:num mask $_:num) =>
        endpointNames := endpointNames.push (n.getId, false)
      | `(atMember| input $n:ident : $typ:ident) =>
        let (tid,_) ← signalType typ
        signalNames := signalNames.push (n.getId,tid,false)
      | `(atMember| output $n:ident : $typ:ident := $_:term) =>
        let (tid,_) ← signalType typ
        signalNames := signalNames.push (n.getId,tid,true)
      | _ => pure ()
    for member in members do
      match member with
      | `(atMember| state $_:ident : $_:ident := $_:num) => pure ()
      | `(atMember| requires $prop:term := $evidence:term) =>
        requirements := requirements.push (← `((let _ : $prop := $evidence; $(quote (prop.raw.reprint.getD "requires")))))
      | `(atMember| input $n:ident : $typ:ident) =>
        let (tid,_) ← signalType typ
        sidebands := sidebands.push (← `(({name := $(quote n.getId.toString), direction := .input, typeId := $(quote tid)} : LeanAT.Frontend.Sideband)))
      | `(atMember| output $n:ident : $typ:ident := $initial:term) =>
        let (tid,_) ← signalType typ
        let value ← signalValue typ initial
        sidebands := sidebands.push (← `(({name := $(quote n.getId.toString), direction := .output, typeId := $(quote tid), initial := some $value} : LeanAT.Frontend.Sideband)))
      | `(atMember| target $n:ident : TlmBase $width:num capacity $cap:num payload $pb:num mask $mb:num) =>
        ports := ports.push (← `(({name := $(quote n.getId.toString), role := .target, width := $width, maxOutstanding := $cap, maxPayloadBytes := $pb, maxByteEnableBytes := $mb} : LeanAT.Frontend.Port)))
      | `(atMember| initiator $n:ident : TlmBase $width:num capacity $cap:num payload $pb:num mask $mb:num) =>
        ports := ports.push (← `(({name := $(quote n.getId.toString), role := .initiator, width := $width, maxOutstanding := $cap, maxPayloadBytes := $pb, maxByteEnableBytes := $mb} : LeanAT.Frontend.Port)))
      | `(atMember| on $event:ident $flow:ident $phase:ident $_:ident do $body:atStmt*) =>
        if event.getId.getString! != "transport" then throwErrorAt event "UnsupportedSynchronousHandler"
        let some endpointId := endpointNames.findIdx? (fun e => e.1 == event.getId.getPrefix) | throwErrorAt event "UnknownHandlerEndpoint"
        let forward := flow.getId == `fw
        if !forward && flow.getId != `bw then throwErrorAt flow "UnknownCallFlow"
        let forwardPhase := phase.getId == `beginReq || phase.getId == `endResp
        if !forwardPhase && phase.getId != `endReq && phase.getId != `beginResp then throwErrorAt phase "UnknownCallPhase"
        if forward != forwardPhase || endpointNames[endpointId]!.2 != forward then throwErrorAt phase "HandlerCallDirection"
        let (term,_,origins) ← bodyTerm names body false true signalNames pureFunctions
        let nodePrefix := s!"handler/{handlers.size}"
        nodeOrigins := nodeOrigins.push (← originTerm nodePrefix member)
        for origin in origins do
          nodeOrigins := nodeOrigins.push (← `(({ $origin with nodePath := $(quote (nodePrefix ++ "/")) ++ ($origin).nodePath, ancestry := ($origin).ancestry.map ($(quote (nodePrefix ++ "/")) ++ ·) } : LeanAT.SourceMapping.NodeOrigin)))
        needsTransport := true
        let trigger := s!"{event.getId}.{flow.getId}.{phase.getId}"
        handlers := handlers.push (← `(({id := $(quote handlers.size), body := $term, context := .transportEntry, endpoint := some $(quote endpointId), trigger := $(quote trigger), declaredResultTypes := some [12]} : LeanAT.ModelIR.Handler)))
        contexts := contexts.push (← `(LeanAT.Frontend.Context.transport))
        handlerNames := handlerNames.push (← `($(quote trigger)))
      | `(atMember| on $event:ident $_:ident do $body:atStmt*) =>
        let (term,needed,origins) ← bodyTerm names body false false signalNames pureFunctions
        let nodePrefix := s!"handler/{handlers.size}"
        nodeOrigins := nodeOrigins.push (← originTerm nodePrefix member)
        for origin in origins do
          nodeOrigins := nodeOrigins.push (← `(({ $origin with nodePath := $(quote (nodePrefix ++ "/")) ++ ($origin).nodePath, ancestry := ($origin).ancestry.map ($(quote (nodePrefix ++ "/")) ++ ·) } : LeanAT.SourceMapping.NodeOrigin)))
        needsServices := needsServices || needed
        if event.getId.getPrefix == `internal then
          handlers := handlers.push (← `(({id := $(quote handlers.size), body := $term, context := .timedHandler, trigger := $(quote event.getId.toString)} : LeanAT.ModelIR.Handler)))
        else
          if event.getId.getString! != "beginReq" then throwErrorAt event "UnsupportedHandler: expected endpoint.beginReq or internal.tag"
          let some endpointId := endpointNames.findIdx? (fun e => e.1 == event.getId.getPrefix) | throwErrorAt event "UnknownHandlerEndpoint"
          if !endpointNames[endpointId]!.2 then throwErrorAt event "HandlerDirection: beginReq requires a target endpoint"
          handlers := handlers.push (← `(({id := $(quote handlers.size), body := $term, context := .timedHandler, trigger := $(quote event.getId.toString), endpoint := some $(quote endpointId)} : LeanAT.ModelIR.Handler)))
        contexts := contexts.push (← `(LeanAT.Frontend.Context.timed))
        handlerNames := handlerNames.push (← `($(quote event.getId.toString)))
      | `(atMember| process $procName:ident do $body:atStmt*) =>
        let _ ← bodyTerm names body
        throwErrorAt procName "MissingProcessCapacity: declare result type, maxInstances, frameBytes, and results"
      | `(atMember| process $procName:ident : $result:ident maxInstances $instanceCount:num frameBytes $frame:num results $rc:num do $body:atStmt*) =>
        if result.getId != `Unit then throwErrorAt result "UnsupportedProcessResult: use Unit until native returned-value grammar is declared"
        let (term,needed,origins) ← bodyTerm names body true false signalNames pureFunctions
        let nodePrefix := s!"handler/{handlers.size}"
        nodeOrigins := nodeOrigins.push (← originTerm nodePrefix member)
        for origin in origins do
          nodeOrigins := nodeOrigins.push (← `(({ $origin with nodePath := $(quote (nodePrefix ++ "/")) ++ ($origin).nodePath, ancestry := ($origin).ancestry.map ($(quote (nodePrefix ++ "/")) ++ ·) } : LeanAT.SourceMapping.NodeOrigin)))
        needsServices := needsServices || needed
        handlers := handlers.push (← `(({id := $(quote handlers.size), body := $term, context := .process, trigger := $(quote procName.getId.toString), processCapacity := some {«maxInstances» := $instanceCount, frameBytesLimit := $frame, resultCapacity := $rc}, declaredResultTypes := some []} : LeanAT.ModelIR.Handler)))
        contexts := contexts.push (← `(LeanAT.Frontend.Context.process))
        handlerNames := handlerNames.push (← `($(quote procName.getId.toString)))
      | _ => throwUnsupportedSyntax
    for (handler,idx) in pureHandlers.toList.zipIdx do
      handlers := handlers.push handler
      contexts := contexts.push (← `(LeanAT.Frontend.Context.pureFunction))
      handlerNames := handlerNames.push (← `($(quote s!"pure:{pureFunctions[idx]!.1}")))
    let types ← if needsTransport then `(LeanAT.Frontend.transportTypes) else if needsServices then `(LeanAT.Frontend.processTypes) else `([.unit,.bool,.bits 8,.bits 16,.bits 32,.bits 64])
    let services ← if needsServices && needsTransport then `(LeanAT.Frontend.timerServices ++ [LeanAT.Frontend.transportService]) else if needsTransport then `([LeanAT.Frontend.transportService]) else if needsServices then `(LeanAT.Frontend.timerServices) else `([])
    let outputIds := (signalNames.toList.filter (·.2.2)).map (·.2.1) |>.eraseDups
    let services ← `($services ++ ($(quote outputIds)).map LeanAT.Frontend.outputService)
    elabCommand (← `(at_component $name := { program := {types := $types, services := $services, states := [$states,*], handlers := [$handlers,*]}, contexts := [$contexts,*], ports := [$ports,*], sidebands := [$sidebands,*], requirements := [$requirements,*], handlerNames := [$handlerNames,*]}))
    let bundle ← bundleTerm nodeOrigins generatedOrigins
    let relative := mkIdent (name.getId ++ `componentSourceBundle)
    let sourceBundle := mkIdent (name.getId ++ `sourceBundle)
    let checked := mkIdent (name.getId ++ `checked)
    elabCommand (← `(def $relative : LeanAT.SourceMapping.Bundle := ($bundle).bindHandlers ($checked).val.program.handlers))
    elabCommand (← `(def $sourceBundle : LeanAT.SourceMapping.Bundle :=
      if ($checked).val.program.handlers.any (fun h => h.endpoint.isSome) || !($checked).val.sidebands.isEmpty then
        ($relative).rebase "component/0/" else $relative))
  | _ => throwUnsupportedSyntax

@[command_elab atSystemBlock] def elabSystemBlock : CommandElab := fun stx => do
  match stx with
  | `(at_system $name:ident where $members:atSystemMember*) =>
    let mut names : Array Name := #[]
    let mut defs : Array (TSyntax `ident) := #[]
    let mut instances : Array (TSyntax `term) := #[]
    let mut bindings : Array (TSyntax `term) := #[]
    let mut signals : Array (TSyntax `term) := #[]
    let mut topPorts : Array (TSyntax `term) := #[]
    let mut topNames : Array Name := #[]
    for member in members do
      match member with
      | `(atSystemMember| instance $n:ident := $d:ident) =>
        if names.contains n.getId then throwErrorAt n "DuplicateInstance"
        names := names.push n.getId
        let checked := mkIdent (d.getId ++ `checked)
        defs := defs.push checked
        instances := instances.push (← `(LeanAT.Frontend.Instance.mk $(quote n.getId.toString) $(quote d.getId.toString) ($checked).val))
      | `(atSystemMember| input $n:ident : $typ:ident := $initial:term) =>
        let (tid,_) ← signalType typ
        let value ← signalValue typ initial
        topNames := topNames.push n.getId
        topPorts := topPorts.push (← `(({name := $(quote n.getId.toString), direction := .input, typeId := $(quote tid), initial := some $value} : LeanAT.Frontend.Sideband)))
      | `(atSystemMember| output $n:ident : $typ:ident) =>
        let (tid,_) ← signalType typ
        topNames := topNames.push n.getId
        topPorts := topPorts.push (← `(({name := $(quote n.getId.toString), direction := .output, typeId := $(quote tid)} : LeanAT.Frontend.Sideband)))
      | _ => pure ()
    let signalRef := fun (e : TSyntax `ident) => do
      if e.getId.getPrefix == `top then
        let some idx := topNames.findIdx? (·.toString == e.getId.getString!) | throwErrorAt e "UnknownTopPort"
        `(LeanAT.ModelIR.SidebandPortRef.top $(quote idx))
      else
        let some idx := names.findIdx? (· == e.getId.getPrefix) | throwErrorAt e "UnknownSidebandInstance"
        let d := defs[idx]!
        let port := e.getId.getString!
        `(LeanAT.ModelIR.SidebandPortRef.instance ⟨$(quote idx)⟩ ((($d).val.sidebands.findIdx? (fun p => p.name == $(quote port))).getD ($d).val.sidebands.length))
    for member in members do
      match member with
      | `(atSystemMember| connect $a:ident => $b:ident) =>
        let source ← signalRef a
        let sink ← signalRef b
        signals := signals.push (← `(Prod.mk $source $sink))
      | `(atSystemMember| bind $a:ident => $b:ident) =>
        let some ai := names.findIdx? (· == a.getId.getPrefix) | throwErrorAt a "UnknownInstance"
        let some bi := names.findIdx? (· == b.getId.getPrefix) | throwErrorAt b "UnknownInstance"
        let ad := defs[ai]!
        let bd := defs[bi]!
        let ap := a.getId.getString!
        let bp := b.getId.getString!
        bindings := bindings.push (← `(LeanAT.Frontend.Binding.mk
          ⟨$(quote ai), (($ad).val.ports.findIdx? (fun p => p.name == $(quote ap))).getD ($ad).val.ports.length⟩
          ⟨$(quote bi), (($bd).val.ports.findIdx? (fun p => p.name == $(quote bp))).getD ($bd).val.ports.length⟩))
      | _ => pure ()
    elabCommand (← `(at_system $name := {instances := [$instances,*], bindings := [$bindings,*], topPorts := [$topPorts,*], sidebandBindings := [$signals,*], types := [.unit,.bool,.bits 8,.bits 16,.bits 32,.bits 64]}))
    let definitions := (defs.toList.map (fun d => d.getId.getPrefix)).mergeSort (fun a b => a.toString ≤ b.toString) |>.eraseDups
    let mut bundles : Array (TSyntax `term) := #[]
    for (definition,idx) in definitions.zipIdx do
      let resolvedChecked ← resolveGlobalConstNoOverload (mkIdent (definition ++ `checked))
      let bundleName := resolvedChecked.getPrefix ++ `componentSourceBundle
      if (← getEnv).contains bundleName then
        let bundle := mkIdent bundleName
        bundles := bundles.push (← `(($bundle).rebase $(quote s!"component/{idx}/")))
    let sourceBundle := mkIdent (name.getId ++ `sourceBundle)
    elabCommand (← `(def $sourceBundle : LeanAT.SourceMapping.Bundle := LeanAT.SourceMapping.Bundle.merge [$bundles,*]))
  | _ => throwUnsupportedSyntax
end LeanAT.Frontend










