import LeanAT.Compiler.Main

open LeanAT LeanAT.ExecIR LeanAT.Compiler

def Except.isError (e : Except α β) : Bool := match e with | .error _ => true | .ok _ => false

def main : IO Unit := do
  let check := fun (b : Bool) (label : String) => if b then pure () else throw (IO.userError label)
  check (hex (computeArtifactHash ByteArray.empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") "SHA256 empty"
  check (hex (computeArtifactHash "abc".toUTF8) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") "SHA256 abc"
  let .ok p := compile scalarExample | throw (IO.userError "compile")
  let .ok result := evalExecSegment p 0 [] {state := p.project.initialState} 6 | throw (IO.userError "lazy select execution")
  check (result.returned == [.bits 64 42] && commit result.txn == [.bits 64 42]) "lazy CFG result/state"
  check (result.remainingFuel == 0) "exact terminator fuel"
  check ((evalExecSegment p 0 [] {state := p.project.initialState} 5).isError) "fuel rejection"
  let bytes := serialize p
  let .ok loaded := deserialize bytes | throw (IO.userError "roundtrip load")
  check (serialize loaded == bytes) "canonical roundtrip"
  check ((deserialize (bytes.extract 0 (bytes.size-1))).isError) "truncated descriptor"
  check ((deserialize (bytes.push 0)).isError) "trailing descriptor"
  check ((deserialize (bytes.set! 110 (bytes[110]! ^^^ 1))).isError) "integrity tamper"
  check ((deserialize bytes {maxFileBytes := 10}).isError) "trusted load limit"
  let bad := {p.project with programs := [{id := 0, blocks := [⟨0,[],[],.ret [⟨999,1⟩]⟩]}]}
  check ((validateExec bad).isError) "undefined register"
  let badConstant := {p.project with programs := [{id := 0,resultTypes := [1],blocks := [⟨0,[],[{op := .const,dest := some ⟨0,1⟩,value := .bits 64 (2^64)}],.ret [⟨0,1⟩]⟩]}]}
  check ((validateExec badConstant).isError) "out of range constant"
  let stagedFailure : ModelIR.Project := {types := [.bool,.bits 64],states := [⟨0,1,.bits 64 0⟩],handlers := [{id := 0,body := [.writeState 0 (.literal 1 (.bits 64 12)),.check (.literal 0 (.bool false)) "deliberate"]}]}
  let .ok f := compile stagedFailure | throw (IO.userError "failure compile")
  check ((evalExecSegment f 0 [] {state := f.project.initialState} 20).isError) "segment discards staged writes on failure"
  let deepRepeat : ModelIR.Project := {types := [.bits 64],handlers := [{id := 0,body := [.repeat 4096 [.repeat 4096 [.emit "bounded" []]]]}]}
  check ((compile deepRepeat).isError) "shared lowering expansion budget"
  let v2 : ExecProject := {schemaMajor := 2,types := [.bytes 4,.handle .task,.bits 64],stateTypes := [],initialState := [],programs := [{id := 0,resultTypes := [0,1,2],blocks := [⟨0,[],[
    {op := .const,dest := some ⟨0,0⟩,value := .bytes [1,2,3]},
    {op := .const,dest := some ⟨1,1⟩,value := .handle ⟨.task,1,2,3,4,5⟩},
    {op := .getNow,dest := some ⟨2,2⟩}],.ret [⟨0,0⟩,⟨1,1⟩,⟨2,2⟩]⟩]}]}
  let .ok v2 := validateExec v2 | throw (IO.userError "v2 validate")
  let .ok v2round := deserialize (serialize v2) | throw (IO.userError "v2 codec")
  check (serialize v2round == serialize v2) "v2 bytes handle canonical roundtrip"
  let .ok v2result := evalExecSegment v2 0 [] {state := []} 4 {now := 55} | throw (IO.userError "v2 execute")
  check (v2result.returned.getLast? == some (.bits 64 55)) "getNow explicit context"
  let suspended : ExecProject := {schemaMajor := 2,types := [.handle .wait,.bits 64,.bool],stateTypes := [],initialState := [],programs := [{id := 0,context := 1,effectMask := 16,inputTypes := [0,1],resultTypes := [1],frame := [⟨1,0,8,0⟩],frameBytes := 64,blocks := [
    ⟨0,[⟨0,0⟩,⟨1,1⟩],[],.suspend ⟨0,0⟩ 1 [⟨1,1⟩]⟩,
    ⟨1,[⟨2,2⟩,⟨3,1⟩],[],.ret [⟨3,1⟩]⟩]}]}
  let .ok suspended := validateExec suspended | throw (IO.userError "suspend validate")
  let .ok sr := evalExecSegment suspended 0 [.handle ⟨.wait,1,1,1,1,1⟩,.bits 64 7] {state := []} 1 {kind := 1} | throw (IO.userError "suspend execute")
  check (match sr.exit with | .suspended _ 1 [.bits 64 7] 2 => true | _ => false) "prepared owning suspension result"
  IO.FS.createDirAll "build/compiler-fixtures"
  IO.FS.writeBinFile "build/compiler-fixtures/v2.execir.bin" (serialize v2)
  IO.FS.writeBinFile "build/compiler-fixtures/suspend.execir.bin" (serialize suspended)
  let pureCall : ExecProject := {schemaMajor := 2,types := [.bits 64],stateTypes := [],initialState := [],programs := [
    {id := 0,inputTypes := [0],resultTypes := [0],blocks := [⟨0,[⟨0,0⟩],[{op := .callPure,args := [⟨0,0⟩],dest := some ⟨1,0⟩,immediate := 1}],.ret [⟨1,0⟩]⟩]},
    {id := 1,inputTypes := [0],resultTypes := [0],blocks := [⟨0,[⟨0,0⟩],[{op := .binary,args := [⟨0,0⟩,⟨0,0⟩],dest := some ⟨1,0⟩,operator := .addWrap}],.ret [⟨1,0⟩]⟩]}]}
  let .ok pureCall := validateExec pureCall | throw (IO.userError "pure call validate")
  let .ok doubled := evalExecSegment pureCall 0 [.bits 64 21] {state := []} 4 | throw (IO.userError "nested pure shared fuel")
  check (doubled.returned == [.bits 64 42] && doubled.remainingFuel == 0) "pure call result/exact cost"
  check ((evalExecSegment pureCall 0 [.bits 64 21] {state := []} 3).isError) "pure shared budget failure"
  IO.FS.writeBinFile "build/compiler-fixtures/pure-call.execir.bin" (serialize pureCall)
  let twoAwaits : ModelIR.Project := {types := [.handle .wait,.bits 64,.bool],handlers := [{id := 0,context := .process,processCapacity := some {frameBytesLimit := 1024,resultCapacity := 1},body := [
    .letVal 0 (.literal 0 (.handle ⟨.wait,1,1,1,1,1⟩)),.letVal 1 (.literal 1 (.bits 64 7)),
    .await (.local 0 0) 2 2,.letVal 3 (.literal 1 (.bits 64 9)),.await (.local 0 0) 4 2,
    .ret [.local 1 1,.local 1 3]]}]}
  let .ok two := compile twoAwaits | throw (IO.userError "two await lower")
  let .ok first := evalExecSegment two 0 [] {state := []} 3 {kind := 1} | throw (IO.userError "first suspend")
  let .ok second := evalPreparedResume two 0 first (.bool true) {state := []} 2 {kind := 1} | throw (IO.userError "second suspend")
  let .ok returned := evalPreparedResume two 0 second (.bool false) {state := []} 1 {kind := 1} | throw (IO.userError "final resume")
  check (returned.returned == [.bits 64 7,.bits 64 9]) "two await live mapping"
  IO.FS.writeBinFile "build/compiler-fixtures/two-awaits.execir.bin" (serialize two)
  IO.println "Compiler: SHA256, lazy CFG, SSA validation, fuel, transactional failure, canonical bounded codec passed"


