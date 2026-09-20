import LeanAT.Compiler.OpcodeCase
import LeanAT.Reference.Structured

namespace LeanAT.Compiler.OpcodeCases.Structured
open LeanAT.Reference LeanAT.ExecIR
private def types : TypeEnvironment := [.unit,.bits 64,.handle .scope,.handle .process,.handle .task,.handle .wait,.record [],.bytes 256,.record [1,7],.variant [[4],[1]],.handle .spawnTicket,.variant [[10],[1]],.record [1,8]]
private def u := Value.bits 64
private def pool : Reference.Structured.PoolConfig := {frames := 2,tasks := 8,results := 8,queue := 2,waiters := 4,overflow := 2,resultType := 8}
structure Seed where
  world : State
  context : Context
  root : HandleIdentity
  process : HandleIdentity
  setup : List Value := []
private def seed (config : Reference.Structured.PoolConfig := pool) : Except String Seed := do
  let context : Context := {kind := 1,domain := 1,instanceId := 1,owner := 1,environment := [("profile.valueNodeBytes",u 40),("profile.segmentBytes",u (128*1024*1024)),("structured.pool",Reference.Structured.encodePool config),("structured.program",u 1)]}
  let (root,s) ← Reference.Structured.seedRoot context {maxPins := 128,maxEvents := 256,maxBytes := 128*1024*1024}
  let s ← Reference.Structured.initializeState context s
  let s ← Storage.seedResultStore s context 128 256 128
  let s ← configureAllocator s {kind := .process,store := 10,group := "runtime.process",allowMax := false,capacity := some 64}
  let s ← configureAllocator s {kind := .wait,store := 11,group := "runtime.process",allowMax := false,capacity := some 128}
  let (process,s) ← Runtime.createProcess s context 0
  pure ⟨s,{context with processIdentity := some process},root,process,[]⟩
private def returnedHandle : List Value → Option HandleIdentity
  | [.handle h] | [.variant 0 [.handle h]] => some h
  | _ => none
private def step (s : Seed) (op : Op) (ins : List Nat) (out : Nat) (args : List Value) : Except String (Option HandleIdentity × Seed) := do
  let signature := Reference.Structured.signature types 1 op ins [out]
  let (values,world) ← attemptExcept ((Reference.Structured.invokeAttempt types s.context signature args).run s.world)
  let alias := returnedHandle values
  pure (alias,{s with world,setup := s.setup ++ [.record [u op.tag,alias.map Value.handle |>.getD .unit,.record args]]})
private def created (result : Option HandleIdentity × Seed) : Except String (HandleIdentity × Seed) :=
  match result with | (some h,s) => pure (h,s) | _ => throw "StructuredSeedExpectedHandle"
private def spawn (s : Seed) (scope : HandleIdentity) : Except String (HandleIdentity × Seed) :=
  step s .spawnProcess [2,6] 4 [.handle scope,.record []] >>= created
private def childScope (s : Seed) (parent : HandleIdentity) : Except String (HandleIdentity × Seed) :=
  step s .scopeNew [2] 2 [.handle parent] >>= created
private def completed (s : Seed) (task : HandleIdentity) : Except String Seed := do
  let outcome := Value.record [u 0,.bytes [42]]
  let world ← Reference.Structured.completeTask types s.context s.world task outcome
  pure {s with world,setup := s.setup ++ [.record [u 1000,.unit,.record [.handle task,outcome]]]}
private def suspended (s : Seed) (wait : HandleIdentity) : Except String Seed := do
  let world ← Runtime.suspendProcess s.world s.context s.process wait
  pure {s with world,setup := s.setup ++ [.record [u 1002,.unit,.record [.handle wait]]]}
private def resolved (s : Seed) : Except String Seed := do
  let world ← Reference.Structured.closeBatch types s.context s.world
  pure {s with world,setup := s.setup ++ [.record [u 1001,.unit,.record []]]}
private def separateCaller (s : Seed) : Except String Seed := do
  let (caller,world) ← Runtime.createProcess s.world s.context 0
  pure {s with world,context := {s.context with processIdentity := some caller},setup := s.setup ++ [.record [u 1003,.handle caller,.record []]]}
private def waitSeed (complete : Bool) : Except String (HandleIdentity × HandleIdentity × Seed) := do
  let s ← seed
  let (task,s) ← spawn s s.root
  let (wait,s) ← step s .waitGroupNew [3,2,1,1,1,1,1] 5 [.handle s.process,.handle s.root,u 0,u 1,u 0,u 12,u 4096] >>= created
  if complete then
    let (_,s) ← step s .waitArm [5,1,1,4,2] 0 [.handle wait,u 0,u 0,.handle task,.handle s.root]
    let s ← suspended s wait
    let s ← completed s task
    pure (wait,task,← separateCaller (← resolved s))
  else pure (wait,task,← separateCaller (← suspended s wait))
private def model (op : Op) (ins : List Nat) (out : Nat) : ModelIR.Project := Id.run do
  let sig := Reference.Structured.signature types 1 op ins [out]
  let service : ModelIR.ServiceIR := {id := sig.id,opcode := (opcodeNames[op.tag]?).getD "invalid",inputTypes := sig.inputTypes,resultTypes := sig.resultTypes,contextMask := sig.contextMask,effectMask := sig.effectMask,extraFuel := sig.extraFuel,providerKey := sig.providerKey,providerVersion := sig.providerVersion,abiHash := sig.abiHash.data.toList}
  let parameters := ins.zipIdx |>.map (fun (t,i) => ({id := i,typeId := t} : ModelIR.LocalBinderIR))
  let call := ModelIR.Stmt.serviceCall (some {id := 100,typeId := out}) 1 (ins.zipIdx |>.map (fun (t,i) => .local t i))
  let tail := if op == .waitGroupNew then [ModelIR.Stmt.await (.local out 100) 101 12,.ret []] else [.ret [.local out 100]]
  let caller : ModelIR.Handler :=
    {id := 0
     context := .process
     processCapacity := some {frameBytesLimit := 4096,resultCapacity := 16}
     parameters
     body := call::tail
     declaredResultTypes := some (if op == .waitGroupNew then [] else [out])}
  let child : ModelIR.ProcessIR :=
    {id := 1
     params := []
     resultType := 7
     body := [.ret [.literal 7 (.bytes [42])]]
     capacity := {frameBytesLimit := 4096,resultCapacity := 16}
     instructionFuel := 100
     ownerPolicy := "caller"
     resultLifetimePolicy := "until-release"}
  return {types
          services := [service]
          components := [{id := ⟨1⟩,handlers := [caller],processes := [child]}]
          systems := [{id := 0,runtimeDomain := 1,instances := [{id := ⟨1⟩,definition := ⟨1⟩}],bindings := []}]
          topSystemId := some 0}
private def build (op : Op) (ins : List Nat) (out : Nat) (args : List Value) (s : Seed) (negative : Bool) : OpcodeCase :=
  let name := (opcodeNames[op.tag]?).getD "unknown"
  let actualArgs := if negative then match args with | .handle h::rest => .handle {h with generation := h.generation+1000000}::rest | _ => args else args
  let context := {s.context with environment := s.context.environment ++ [("structured.root",.handle s.root),("structured.process",.handle s.process),("structured.setup",.record s.setup)]}
  {id := "structured." ++ name,variant := if negative then "stale-owner" else "success",providerFamily := "structured",model := model op ins out,
   input := {inputs := actualArgs,world := s.world,context},expectedOpcodeTags := [op.tag],
   expectedOutcome := if negative then "failure" else if op == .waitGroupNew then "suspended" else "success",
   profile := "AT-Ext-1.1-draft",sourceModule := "LeanAT.Compiler.OpcodeCases.Structured",sourceFiles := ["LeanAT/Compiler/OpcodeCases/Structured.lean","LeanAT/Reference/Structured.lean"]}
private def pair (op : Op) (ins : List Nat) (out : Nat) (args : List Value) (s : Seed) :=
  [build op ins out args s false,build op ins out args s true]

def cases : Except String (List OpcodeCase) := do
  let mut out := []
  let s ← seed
  out := out ++ pair .scopeNew [2] 2 [.handle s.root] s
  out := out ++ pair .spawnProcess [2,6] 4 [.handle s.root,.record []] s
  let lifecycle := build .spawnProcess [2,6] 4 [.handle s.root,.record []] s false
  out := out ++ [{lifecycle with
    variant := "process-lifecycle"
    input := {lifecycle.input with context := {lifecycle.input.context with
      environment := lifecycle.input.context.environment ++ [("structured.runChild",.bool true)]}}}]
  out := out ++ [{lifecycle with
    variant := "nonzero-connection"
    input := {lifecycle.input with context := {lifecycle.input.context with connection := 17}}}]
  let timedModel := {lifecycle.model with components := lifecycle.model.components.map (fun component =>
    {component with handlers := component.handlers.map (fun handler => {handler with context := .timedHandler,processCapacity := none})})}
  out := out ++ [{lifecycle with
    variant := "timed-process-lifecycle"
    model := timedModel
    input := {lifecycle.input with context := {lifecycle.input.context with
      kind := 0
      environment := lifecycle.input.context.environment ++ [("structured.runChild",.bool true)]}}}]
  out := out ++ pair .trySpawnProcess [2,6] 9 [.handle s.root,.record []] s
  out := out ++ pair .waitGroupNew [3,2,1,1,1,1,1] 5 [.handle s.process,.handle s.root,u 0,u 1,u 0,u 12,u 4096] s
  let s ← seed {pool with frames := 1}
  let (_,s) ← spawn s s.root
  out := out ++ pair .submitTask [2,6] 9 [.handle s.root,.record []] s
  let s ← seed {pool with overflow := 1}
  out := out ++ pair .requestTaskSlot [3,2] 11 [.handle s.process,.handle s.root] s
  let s ← seed
  let (a,s) ← childScope s s.root
  let (b,s) ← childScope s s.root
  out := out ++ pair .scopeTransfer [2,2,2] 0 [.handle a,.handle s.root,.handle b] s
  out := out ++ pair .scopeClose [2] 0 [.handle a] s
  let (nested,s) ← childScope s a
  let (_,s) ← spawn s nested
  out := out ++ pair .scopeCancel [2,7] 0 [.handle a,.bytes [9]] s
  let s ← seed
  let (task,s) ← spawn s s.root
  out := out ++ pair .cancelTask [4,7] 0 [.handle task,.bytes [9]] s
  let s ← completed s task
  out := out ++ pair .taskResultGet [4,2] 8 [.handle task,.handle s.root] s
  out := out ++ pair .taskResultRelease [4,2] 0 [.handle task,.handle s.root] s
  let (wait,task,s) ← waitSeed false
  out := out ++ pair .waitArm [5,1,1,4,2] 0 [.handle wait,u 0,u 0,.handle task,.handle s.root] s
  let (wait,_,s) ← waitSeed true
  out := out ++ pair .waitResultGet [5,2] 12 [.handle wait,.handle s.root] s
  out := out ++ pair .waitGroupRelease [5,2] 0 [.handle wait,.handle s.root] s
  pure out
end LeanAT.Compiler.OpcodeCases.Structured
