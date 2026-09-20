import LeanAT.Reference.Json

open Lean LeanAT LeanAT.Reference LeanAT.Reference.JsonIO

private def check (ok : Bool) (message : String) : IO Unit :=
  unless ok do throw (IO.userError message)

private def identity : HandleIdentity :=
  ⟨.consumer,4294967295,4,7,18446744073709551615,9007199254740993⟩

def main : IO Unit := do
  let samples : List Value := [
    .unit,.bool false,.bool true,.bits 1 1,.bits 64 (2^64-1),
    .bytes [0,1,127,128,255],.record [.bits 8 255,.bool false],
    .vec [.unit,.record []],.variant 7 [.bytes [],.handle identity],.handle identity]
  for sample in samples do
    check ((valueOfJson (valueJson sample)).toOption == some sample) "typed value round trip"
  let context : Context := {
    kind := 2
    now := 2^64-1
    turn := 17
    domain := 2^32-1
    instanceId := 19
    connection := 3
    owner := 9007199254740993
    processIdentity := some {identity with kind := .process}
    inputs := [(7,.bits 32 42)]
    environment := [("quoted\\\"key",.vec samples)] }
  check ((contextOfJson (contextJson context)).toOption == some context) "context round trip"
  let h : HandleIdentity := ⟨.event,1,1,0,1,9⟩
  let world : State := {
    objects := [⟨h,"reference.event",.bits 64 1,true⟩]
    events := [⟨h,7,2,1,8,3,1,"timer",samples,"fixture:7",false⟩]
    observations := [⟨"trace","ReadNow","fixture:1",7,2,samples⟩]
    allocationRules := [{kind := .event,store := 1,group := "runtime.event",perSlot := true,capacity := some 1024}]
    allocationCounters := [{group := "runtime.event",domain := 1,slot := some 0,nextGeneration := 2}]
    nextGeneration := 2
    nextSequence := 2
    maxPins := 19 }
  check ((stateOfJson (stateJson world)).toOption == some world) "owned state round trip"
  let input : Input := {inputs := samples,committed := [.bits 64 9],world,context,fuel := 2^64-1}
  let parsed := parseBounded (inputJson input).compress >>= inputOfJson
  check (parsed.toOption == some input) "bounded input round trip"
  let outcome : Outcome := {
    ok := false
    error := "CheckedOverflow"
    committed := [.bits 64 9]
    world
    remainingFuel := 9007199254740993
    runtimeFuelRemaining := some 0
    exit := "failed"
    wait := some (.handle h)
    resumeBlock := some 3
    outcomeType := some 7
    live := samples
    trace := [⟨"exec",2,"block/0/op/1","AddChecked",[.bits 64 (2^64-1),.bits 64 1],[],7,6⟩]}
  let emitted := outcomeJson outcome
  check ((emitted.getObjValAs? String "remainingFuel").toOption == some "9007199254740993") "exact uint64 output"
  check ((emitted.getObjValAs? String "runtimeFuelRemaining").toOption == some "0") "separate exact runtime fuel receipt"
  check ((emitted.getObjValAs? Bool "ok").toOption == some false) "actual failed outcome retained"
  check ((emitted.getObjVal? "world").toOption == some (stateJson world)) "full world retained"
  for malformed in [
      "{\"kind\":\"bits\",\"width\":64,\"value\":\"18446744073709551616\"}",
      "{\"kind\":\"bits\",\"width\":64,\"value\":\"01\"}",
      "{\"kind\":\"bits\",\"width\":64,\"value\":7}",
      "{\"kind\":\"bits\",\"width\":0,\"value\":\"0\"}",
      "{\"kind\":\"bits\",\"width\":8,\"value\":\"256\"}",
      "{\"kind\":\"bytes\",\"data\":[256]}",
      "{\"kind\":\"unit\",\"hidden\":true}",
      "{\"kind\":\"bogus\"}",
      "{\"kind\":\"bool\",\"value\":\"true\"}"] do
    check ((parseBounded malformed >>= valueOfJson).toOption.isNone) "malformed typed value rejected"
  for malformed in ["{\"a\":1,\"a\":2}","{\"a\":1,\"\\u0061\":2}",
      "[1e999999999]","[1E999999999]","[1.0]","[-1]","[184467440737095516160]",
      "{\"a\":[]", "[\"unterminated]"] do
    check ((parseBounded malformed).toOption.isNone) "malformed document rejected before evaluation"
  let tooDeep := String.ofList (List.replicate 161 '[' ++ List.replicate 161 ']')
  check ((parseBounded tooDeep).toOption.isNone) "parser depth bound"
  let nested := (List.range 64).foldl (fun v _ => Value.vec [v]) .unit
  check ((valueOfJson (valueJson nested)).toOption.isNone) "typed nesting bound"
  let tooMany := Json.mkObj [("kind",.str "vec"),("values",.arr (Array.replicate 65537 (valueJson .unit)))]
  check ((valueOfJson tooMany).toOption.isNone) "container bound"
  let badIdentity := Json.mkObj [("kind",toJson (kindNumber identity.kind)),("domain",.str "4294967296"),
    ("store",.str "4"),("slot",.str "7"),("generation",.str "1"),("owner",.str "0")]
  check ((identityOfJson badIdentity).toOption.isNone) "identity conversion never wraps"
  let exhaustedCounter : AllocationCounter := {group := "runtime.event",domain := 1,slot := some 9,nextGeneration := 2^64,retired := true}
  check ((allocationCounterOfJson (allocationCounterJson exhaustedCounter)).toOption == some exhaustedCounter)
    "allocator exhaustion marker and retired slot survive JSON"
  let invalidCounter := Json.mkObj [("group",.str "runtime.event"),("domain",.str "1"),("slot",.str "4294967296")]
  check ((allocationCounterOfJson invalidCounter).toOption.isNone) "allocator slot conversion never wraps"
  check ((contextOfJson (Json.mkObj [("inputs",toJson [Json.mkObj [("index",toJson (0:Nat)),("value",valueJson .unit)],
    Json.mkObj [("index",toJson (0:Nat)),("value",valueJson .unit)]])])).toOption.isNone) "duplicate binding rejected"
  IO.println "ReferenceJson: typed round trips, complete outcome, exact integers and bounded malformed input PASS"
