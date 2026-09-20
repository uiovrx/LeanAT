import LeanAT.Reference.Source
import LeanAT.Reference.Exec
import LeanAT.Reference.Runner

open LeanAT LeanAT.Reference

private def ensure (condition : Bool) (message : String) : IO Unit :=
  unless condition do throw (IO.userError message)
private def u (n : Nat) : Value := .bits 64 n
private def lit (n : Nat) : ModelIR.Expr := .literal 2 (u n)
private def types : TypeEnvironment := [.unit,.bool,.bits 64,.handle .wait]
private def model (body : List ModelIR.Stmt) : ModelIR.Project :=
  {types,states := [⟨0,2,u 0⟩],handlers := [{id := 0,body}]}
private def sourceMachine : Source.Machine :=
  {values := [(0,u 0)],world := {},context := {},remainingFuel := 20,program := 0}
private def reg (id typeId : Nat) : ExecIR.VReg := ⟨id,typeId⟩

private def sourceFailures : IO Unit := do
  let body := [ModelIR.Stmt.letVal 0 (.binary 2 .divChecked (lit 7) (lit 0))]
  match Source.run (model body) body {sourceMachine with remainingFuel := 10} with
  | .ok _ _ => throw (IO.userError "Source division unexpectedly succeeded")
  | .error _ state =>
    ensure (state.remainingFuel == 6) "Source nested failure lost spent fuel"
    ensure (state.trace.any (fun t => t.location == "handler/0/body/0/expr/0/expr/1" && t.operation == "literal")) "Source nested expression path"
    ensure (state.trace.any (fun t => t.location == "handler/0/body/0" && t.operation == "error:statement" && t.fuelBefore == 10 && t.fuelAfter == 6)) "Source outer failure trace"
  let caller := [ModelIR.Stmt.ret [.callPure 2 1 []]]
  let project := {model caller with
    handlers := [
      {id := 0,body := caller},
      {id := 1,context := .pureFunction,body := [.fail "callee"],declaredResultTypes := some [2]}]}
  match Source.run project caller {sourceMachine with locals := [(9,u 42)]} with
  | .ok _ _ => throw (IO.userError "Source pure failure unexpectedly succeeded")
  | .error _ state =>
    ensure (state.program == 0 && state.locals.lookup 9 == some (u 42)) "Source pure failure did not restore caller"
    ensure (state.trace.any (fun t => t.program == 1 && t.operation == "error:fail")) "Source callee error trace missing"
    ensure (state.trace.any (fun t => t.program == 0 && t.location == "handler/0/body/0/expr/0" && t.operation == "error:callPure")) "Source parent call error trace missing"

private def sourceLoop : IO Unit := do
  let body := [ModelIR.Stmt.repeat 2 [.emit "tick" [lit 42]],.ret []]
  match Source.run (model body) body sourceMachine with
  | .error error _ => throw (IO.userError error)
  | .ok _ state =>
    ensure (state.remainingFuel == 12) "Source loop fuel"
    ensure ((state.trace.filter (fun t => t.operation == "loop")).length == 2) "Source loop fuel trace missing"
    ensure (state.world.observations.length == 2 && state.world.observations.all (fun o => o.source == "handler/0/body/0/body/0")) "Source loop canonical paths"

private def execTraceAndCaps : IO Unit := do
  let program : ExecIR.Program := {
    id := 0
    blocks := [{
      id := 0
      parameters := []
      instructions := [
        {op := .const,dest := some (reg 0 2),value := u 42,source := "handler/0/body/0/expr/0"},
        {op := .trace,args := [reg 0 2],text := "tick",source := "handler/0/body/0"}]
      terminator := .ret []}]}
  let project : ExecIR.ExecProject := {types,stateTypes := [],initialState := [],programs := [program]}
  let machine : Exec.Machine := {txn := {state := [],world := some {}},remainingFuel := 10}
  match Exec.run project {now := 7,referenceContext := some {now := 7,turn := 3}} program [] machine with
  | .error error _ => throw (IO.userError error)
  | .ok _ state =>
    let observations := state.txn.world.map (·.observations) |>.getD []
    ensure (observations == [⟨"trace","tick","handler/0/body/0",7,3,[u 42]⟩]) "Exec trace was not committed to reference world"
  let capped := {program with instructionFuel := 2}
  match Exec.run {project with programs := [capped]} {referenceContext := some {}} capped [] machine with
  | .ok _ _ => throw (IO.userError "Per-program fuel cap ignored")
  | .error error state =>
    ensure (error == "FuelExhausted" && state.remainingFuel == 8) "Program cap corrupted shared remaining fuel"
    ensure (state.trace.any (fun t => t.operation == "error:terminator" && t.fuelAfter == 8)) "Cap exhaustion trace missing"
  let caller : ExecIR.Program := {
    id := 0
    inputTypes := [2]
    blocks := [{
      id := 0
      parameters := [reg 0 2]
      instructions := [
        {op := .callPure,immediate := 1,args := [reg 0 2],dest := some (reg 1 2),source := "handler/0/body/0/expr/0"}]
      terminator := .ret [reg 1 2]}]}
  let callee : ExecIR.Program := {id := 1,context := 5,inputTypes := [2],blocks := [{id := 0,parameters := [reg 0 2],instructions := [],terminator := .fail "callee"}]}
  match Exec.run {project with programs := [caller,callee]} {} caller [u 0] machine with
  | .ok _ _ => throw (IO.userError "Exec pure failure unexpectedly succeeded")
  | .error _ state =>
    ensure (state.remainingFuel == 8) "Exec pure failure lost shared fuel"
    ensure (state.trace.any (fun t => t.program == 0 && t.operation == "error:callPure" && t.args == [u 0])) "Exec parent call error arguments missing"

private def readyWorld : Except String (Context × HandleIdentity × HandleIdentity × State) := do
  let process : HandleIdentity := ⟨.process,1,10,0,1,7⟩
  let context : Context := {kind := 1,domain := 1,owner := 7,processIdentity := some process}
  let world ← Runtime.seedProcess {} context process 0
  let (wait,world) ← Runtime.newWait world context process 4 0 none (some (0,0)) (some .unit)
  pure (context,process,wait,world)

private def sourceResume : IO Unit := do
  let (context,process,wait,world) ← match readyWorld with | .ok v => pure v | .error e => throw (IO.userError e)
  let loop := [ModelIR.Stmt.branch (.binary 1 .lt (.state 2 0) (lit 1)) [.await (.local 3 0) 1 0] [],
    .writeState 0 (.binary 2 .addWrap (.state 2 0) (lit 1))]
  let body := [ModelIR.Stmt.repeat 2 loop,.ret [.state 2 0]]
  let project := {model body with
    handlers := [{
      id := 0
      body := body
      context := .process
      parameters := [{id := 0,typeId := 3}]
      declaredResultTypes := some [2]
      processCapacity := some {
        maxInstances := 1
        frameBytesLimit := 1024
        resultCapacity := 1}}]}
  let machine := {sourceMachine with context,world,locals := [(0,.handle wait),(9,u 999)],localTypes := [(0,3),(9,2)],frameBytesLimit := some 29,remainingFuel := 100}
  let saved ← match Source.run project body machine with | .ok _ state => pure state | .error e _ => throw (IO.userError e)
  ensure (saved.exit == "suspended" && saved.returned.isNone && saved.values.lookup 0 == some (u 0)) "Source await completed implicitly"
  ensure (saved.locals.lookup 9 == none) "Source kept dead frame local"
  match Source.run project body {machine with frameBytesLimit := some 28} with
  | .error "FrameCapacityExceeded" _ => pure () | _ => throw (IO.userError "Source frame capacity ignored")
  ensure (saved.pendingWork.any (fun item => match item with | .loop "handler/0/body/0" 1 _ => true | _ => false)) "Source lost pending loop/path"
  match Source.resume project (.bool true) saved with
  | .error "WaitOutcomeType" _ => pure () | _ => throw (IO.userError "Source wrong outcome type accepted")
  let resumeContext := {context with turn := 1}
  let some event := saved.world.events.find? (fun event => event.kind == "runtime.resume")
    | throw (IO.userError "Missing queued source resume")
  let delivered ← match Runtime.deliver resumeContext saved.world event with
    | .ok v => pure v | .error e => throw (IO.userError e)
  let (outcome,world) ← match Runtime.claimWait delivered resumeContext process wait with
    | .ok v => pure v | .error e => throw (IO.userError e)
  match Source.resume project outcome {saved with world,context := resumeContext,remainingFuel := 100} with
  | .error e _ => throw (IO.userError e)
  | .ok _ state =>
    ensure (state.returned == some [u 2] && state.values.lookup 0 == some (u 2)) "Source continuation lost locals/loop work"
    ensure (state.trace.any (fun t => t.location == "handler/0/body/0/body/1" && t.operation == "writeState")) "Source resume changed original paths"

private def execResume : IO Unit := do
  let (context,process,wait,world) ← match readyWorld with | .ok v => pure v | .error e => throw (IO.userError e)
  let program : ExecIR.Program := {
    id := 0
    context := 1
    inputTypes := [3,2]
    blocks := [
      {id := 0,parameters := [reg 0 3,reg 1 2],instructions := [],terminator := .suspend (reg 0 3) 1 [reg 1 2]},
      {id := 1,parameters := [reg 5 0,reg 6 2],instructions := [],terminator := .ret [reg 6 2]}]}
  let project : ExecIR.ExecProject := {types,stateTypes := [],initialState := [],programs := [program]}
  let execution : ExecIR.ExecutionContext := {kind := 1,referenceContext := some context,processIdentity := some process}
  let machine : Exec.Machine := {txn := {state := [],world := some world},remainingFuel := 100}
  let saved ← match Exec.run project execution program [.handle wait,u 42] machine with
    | .ok _ state => pure state | .error e _ => throw (IO.userError e)
  ensure (saved.exit == "suspended" && saved.resumeBlock == some 1 && saved.live == [u 42]) "Exec did not save actual resume edge/live values"
  match Exec.resume project execution program (.bool true) saved with
  | .error "ContinuationType" _ => pure () | _ => throw (IO.userError "Exec wrong outcome type accepted")
  let resumeContext := {context with turn := 1}
  let waiting := saved.txn.world.getD {}
  let some event := waiting.events.find? (fun event => event.kind == "runtime.resume")
    | throw (IO.userError "Missing queued Exec resume")
  let delivered ← match Runtime.deliver resumeContext waiting event with
    | .ok v => pure v | .error e => throw (IO.userError e)
  let (outcome,world) ← match Runtime.claimWait delivered resumeContext process wait with
    | .ok v => pure v | .error e => throw (IO.userError e)
  let resumed := {saved with txn := {saved.txn with world := some world},remainingFuel := 100}
  match Exec.resume project {execution with referenceContext := some resumeContext} program outcome resumed with
  | .error e _ => throw (IO.userError e)
  | .ok values state => ensure (values == [u 42] && state.wait.isNone) "Exec resumed entry block instead of saved edge"

/-- The cap counts runtime instructions; the host budget counts AST work.
    These examples deliberately leave host fuel high and check exact boundaries. -/
private def runtimePolicyUnits : IO Unit := do
  let base := {sourceMachine with
    remainingFuel := 100
    locals := [(0,u 42),(1,.bool true)]
    localTypes := [(0,2),(1,1)]}
  let check := fun (name : String) (body : List ModelIR.Stmt) (cost : Nat) => do
    let project := model body
    match Source.run project body {base with fuelCaps := [cost]} with
    | .error e _ => throw (IO.userError (name ++ ": " ++ e))
    | .ok _ state =>
      ensure (state.runtimeSpent == cost && state.fuelCaps == [0]) (name ++ ": runtime units")
      ensure (state.remainingFuel > 0) (name ++ ": host budget unexpectedly exhausted")
    match Source.run project body {base with fuelCaps := [cost-1]} with
    | .error "FuelExhausted" _ => pure ()
    | _ => throw (IO.userError (name ++ ": cap boundary ignored"))
  check "local return" [.ret [.local 2 0]] 1
  check "literal return" [.ret [lit 42]] 2
  check "local copy" [.letVal 2 (.local 2 0),.ret [.local 2 2]] 2
  check "branch returning arm" [.branch (.local 1 1) [.ret [.local 2 0]] [.ret [lit 7]]] 2
  check "branch join" [.branch (.local 1 1) [] [],.ret [.local 2 0]] 3
  check "unrolled loop" [.repeat 2 [.letVal 2 (.local 2 0)],.ret [.local 2 0]] 3
  check "eager leaf select" [.ret [.select 2 (.local 1 1) (lit 42) (lit 7)]] 4
  check "lazy select" [.ret [.select 2 (.local 1 1) (.binary 2 .addWrap (lit 40) (lit 2)) (lit 7)]] 6
  check "implicit return" [] 1
  let outer := [ModelIR.Stmt.ret [.callPure 2 1 [.local 2 0]]]
  let middle := [ModelIR.Stmt.ret [.callPure 2 2 [.local 2 0]]]
  let project := {model outer with
    handlers := [
      {id := 0,body := outer},
      {id := 1,context := .pureFunction,parameters := [{id := 0,typeId := 2}],body := middle,declaredResultTypes := some [2]},
      {id := 2,context := .pureFunction,parameters := [{id := 0,typeId := 2}],body := [.ret [.local 2 0]],declaredResultTypes := some [2]}]}
  match Source.run project outer {base with fuelCaps := [5]} with
  | .error e _ => throw (IO.userError ("nested call: " ++ e))
  | .ok _ state => ensure (state.runtimeSpent == 5 && state.returned == some [u 42]) "Nested call runtime boundary"
  match Source.run project outer {base with fuelCaps := [4]} with
  | .error "FuelExhausted" state => ensure (state.program == 0 && state.runtimeSpent == 4) "Nested call cap unwind"
  | _ => throw (IO.userError "Nested call cap ignored")
  let process : ModelIR.ProcessIR := {
    id := 0
    params := [{id := 0,typeId := 2}]
    resultType := 2
    body := [.ret [.local 2 0]]
    capacity := {maxInstances := 1,frameBytesLimit := 1024,resultCapacity := 1}
    instructionFuel := 1
    ownerPolicy := "caller"
    resultLifetimePolicy := "until-release"}
  let processProject : ModelIR.Project := {
    types := types
    states := []
    handlers := []
    components := [{id := ⟨0⟩,processes := [process]}]
    systems := [{id := 0,instances := [{id := ⟨0⟩,definition := ⟨0⟩}],bindings := [],runtimeDomain := 1}]
    topSystemId := some 0}
  let validated ← match ModelIR.validateSchema processProject with
    | .ok p => pure p
    | .error e => throw (IO.userError ("Process policy schema: " ++ e))
  let processIdentity : HandleIdentity := ⟨.process,1,10,0,1,7⟩
  let processContext : Context := {kind := 1,domain := 1,owner := 7,instanceId := 0,processIdentity := some processIdentity}
  let result := runModelSegment validated 0 [u 42] [] {} processContext 100
  ensure (result.ok && result.returned == [u 42] && result.runtimeFuelRemaining == some 0 && result.remainingFuel == 98) ("Real ProcessIR policy1: " ++ result.error)
  -- A minimal CFG provides an independent runtime boundary for the reported case.
  let program : ExecIR.Program := {
    id := 0
    inputTypes := [2]
    instructionFuel := 1
    blocks := [{id := 0,parameters := [reg 0 2],instructions := [],terminator := .ret [reg 0 2]}]}
  let execProject : ExecIR.ExecProject := {types,stateTypes := [],initialState := [],programs := [program]}
  match Exec.run execProject {} program [u 42] {txn := {state := []},remainingFuel := 100} with
  | .ok [value] state => ensure (value == u 42 && state.remainingFuel == 99) "Exec local return policy unit"
  | _ => throw (IO.userError "Exec local return policy failed")

def main : IO Unit := do
  runtimePolicyUnits
  sourceFailures
  sourceLoop
  execTraceAndCaps
  sourceResume
  execResume
  IO.println "Independent reference control flow, traces, fuel, and typed resume checks passed"
