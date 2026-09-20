import LeanAT.ModelIR.TransportCLI
open Lean LeanAT.ModelIR LeanAT.ModelIR.TransportCLI
private def must {α : Type} (value : Except String α) : IO α :=
  match value with | .ok value => pure value | .error error => throw (IO.userError error)
private def check (condition : Bool) (label : String) : IO Unit :=
  unless condition do throw (IO.userError label)
private def replace (json : Json) (key : String) (value : Json) : Json :=
  Json.mkObj (((json.getObj?.toOption.getD {}).toList.filter (fun pair => pair.1 != key)) ++ [(key,value)])
private def badRows (json : Json) (index : Nat) (key : String) (value : Json) : Json :=
  let rows := (json.getObjValAs? (Array Json) "records").toOption.getD #[]
  replace json "records" (toJson (rows.modify index (fun row => replace row key value)))
def main : IO Unit := do
  let source ← IO.FS.readFile "tests/fixtures/model-transport/two-connections.json"
  let json ← must (Json.parse source)
  let input ← must (parseInput json)
  let actual := evaluate input
  check (actual.result.reason == .quiescent) "expected completed protocol"
  check (actual.result.state.trace.length == 30) "expected 16 wire and 14 timed observations"
  for fuel in List.range 45 do
    for horizon in [none,some 0,some 1,some 4,some 6,some 7] do
      let bounded := {input with fuel,horizon}
      check ((evaluate bounded).result == Transport.runFrom input.config horizon fuel {} input.records) "step adapter changed reference semantics"
      let _ ← must (resultJson bounded (evaluate bounded))
  let output ← must (resultJson input actual)
  let expected ← must (Json.parse (← IO.FS.readFile "tests/fixtures/model-transport/two-connections.expected.json"))
  check (output == expected) "actual golden trace changed"
  check ((parseInput (badRows json 1 "ordinal" (toJson (1 : Nat)))).toOption.isNone) "duplicate ordinal accepted"
  check ((parseInput (badRows json 1 "connection" (toJson (3 : Nat)))).toOption.isNone) "mismatched reply ledger accepted"
  check ((parseInput (badRows json 2 "domain" (toJson (2 : Nat)))).toOption.isNone) "multiple domains accepted"
  check ((parseInput (badRows json 4 "instance" (toJson (2 : Nat)))).toOption.isNone) "mirrored local ledger accepted"
  let ignored := badRows json 1 "phase" (toJson "deliberately ignored phase")
  let ignoredInput ← must (parseInput ignored)
  check ((evaluate ignoredInput).result == actual.result) "Accepted mutation was not ignored"
  let ignoredOutput ← must (runJson ignored)
  let observations ← must (ignoredOutput.getObjValAs? (Array Json) "trace")
  check ((observations[1]?.bind (fun row => (row.getObjValAs? String "phase").toOption)) == some "deliberately ignored phase") "ignored return mutation lost from trace"
  let reverseJson ← must (Json.parse (← IO.FS.readFile "tests/fixtures/model-transport/reverse-callbacks.json"))
  let reverseInput ← must (parseInput reverseJson)
  let reverseResult := evaluate reverseInput
  check (reverseResult.result.reason == .quiescent && reverseResult.result.state.trace.length == 30) "reverse callbacks failed"
  let wireConnections := reverseResult.result.state.trace.filterMap (fun record => match record with | .call call => some call.connection | _ => none)
  check (wireConnections == [1,2,2,1,2,1,1,2]) "wire boundary order was rewritten"
  let atFour := reverseResult.result.state.trace.filterMap (fun record => match record with | .timed event => if event.time == 4 then some (event.stage,event.connection) else none | _ => none)
  check (atFour == [(2,1),(2,2),(3,2),(3,1)]) "stage/connection/sequence event order incorrect"
  let reverseOutput ← must (resultJson reverseInput reverseResult)
  let reverseGolden ← must (Json.parse (← IO.FS.readFile "tests/fixtures/model-transport/reverse-callbacks.expected.json"))
  check (reverseOutput == reverseGolden) "reverse trace golden changed"
  for fuel in List.range 45 do
    for horizon in [none,some 0,some 1,some 4,some 6,some 7] do
      let bounded := {reverseInput with fuel,horizon}
      check ((evaluate bounded).result == Transport.runFrom bounded.config horizon fuel {} bounded.records) "reverse step semantics changed"
  let stdout ← IO.getStdout
  stdout.putStrLn "Transport JSON oracle: 540 bounded equivalence cases, forward/reverse actual traces, trace golden and rejection cases passed."



