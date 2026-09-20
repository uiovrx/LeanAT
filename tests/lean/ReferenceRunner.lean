import LeanAT.Reference.E39Main

open LeanAT LeanAT.Reference

private def must (result : Except String α) : IO α :=
  match result with | .ok value => pure value | .error error => throw (IO.userError error)
private def check (ok : Bool) (message : String) : IO Unit :=
  unless ok do throw (IO.userError message)

private def project : Except String ModelIR.Project := do
  let (key,hash) ← Storage.coreIdentity .scheduleEvent
  let service : ModelIR.ServiceIR := {
    id := 0
    opcode := ExecIR.opcodeNames[ExecIR.Op.scheduleEvent.tag]!
    inputTypes := [0,1]
    resultTypes := [2]
    contextMask := 3
    effectMask := 8
    extraFuel := 0
    providerKey := key
    providerVersion := "1"
    abiHash := hash.data.toList }
  pure {
    types := [.bits 64,.unit,.handle .event,.bool]
    states := [⟨0,0,.bits 64 0⟩]
    services := [service]
    handlers := [{
      id := 0
      parameters := [⟨0,3,{}⟩]
      declaredResultTypes := some [2]
      body := [
        .writeState 0 (.literal 0 (.bits 64 42)),
        .readNow ⟨1,0,{}⟩,
        .serviceCall (some ⟨2,2,{}⟩) 0 [.local 0 1,.literal 1 .unit],
        .branch (.local 3 0) [.fail "after prepared allocation"] [],
        .ret [.local 2 2]] }] }

private def context : Context := {
  now := 5
  turn := 2
  domain := 1
  connection := 23
  owner := 9
  environment := [("profile.valueNodeBytes",.bits 64 40)] }

private def handleIn (outcome : Outcome) : Option HandleIdentity :=
  outcome.trace.flatMap (·.results) |>.findSome? (fun value => match value with | .handle h => some h | _ => none)

private def verifyBackend (name : String) (run : List Value → State → Nat → Outcome) : IO Unit := do
  let success := run [.bool false] {} 1000
  check success.ok (name ++ " successful segment: " ++ success.error)
  check (success.committed == [.bits 64 42]) (name ++ " prepared state committed")
  let [.handle allocated] := success.returned | throw (IO.userError (name ++ " returned event"))
  let [event] := success.world.events | throw (IO.userError (name ++ " full event state"))
  check (event.identity == allocated && event.time == 5 && event.turn == 3 && event.connection == 23 &&
    event.stage == 3 && event.sequence == 1 && event.values == [.unit] && !event.cancelled)
    (name ++ " event key/payload/identity")
  let cost := 1000-success.remainingFuel
  check (cost > 0 && cost < 1000) (name ++ " real consumed fuel")
  let exact := run [.bool false] {} cost
  check (exact.ok && exact.remainingFuel == 0 && exact.returned == success.returned &&
    exact.committed == success.committed && exact.world == success.world) (name ++ " exact completion boundary")
  let short := run [.bool false] {} (cost-1)
  check (!short.ok && short.error == "FuelExhausted" && short.committed == [.bits 64 0] &&
    short.world.objects.isEmpty && short.world.events.isEmpty) (name ++ " insufficient fuel atomic rollback")
  let aborted := run [.bool true] {} 1000
  check (!aborted.ok && aborted.error == "after prepared allocation" &&
    aborted.committed == [.bits 64 0] && aborted.world.objects.isEmpty && aborted.world.events.isEmpty)
    (name ++ " explicit failure rolls back all prepared state")
  let some discarded := handleIn aborted | throw (IO.userError (name ++ " actual failed prefix allocation absent"))
  check (aborted.world.nextGeneration > discarded.generation.toNat) (name ++ " allocation nonce survives rollback")
  check (aborted.world.nextSequence > 1) (name ++ " prepared event sequence survives rollback")
  let retry := run [.bool false] aborted.world 1000
  let [.handle fresh] := retry.returned | throw (IO.userError (name ++ " retry returned event"))
  check (retry.ok && fresh != discarded && fresh.generation.toNat > discarded.generation.toNat)
    (name ++ " discarded capability never aliases a later allocation")
  check ((retry.world.events.head?).any (fun e => e.sequence == aborted.world.nextSequence))
    (name ++ " discarded event sequence never reused")
  let forged := run [.bool false] {nextGeneration := 0} 1000
  check (!forged.ok && forged.trace.isEmpty && forged.remainingFuel == 1000 && forged.world.nextGeneration == 0)
    (name ++ " malformed seed rejected before execution")
  check ((JsonIO.parseBounded (JsonIO.outcomeJson aborted).compress).toOption.isSome)
    (name ++ " actual failed output serializes as bounded JSON")

def main : IO Unit := do
  let p ← must project
  let model ← must (ModelIR.validateSchema p)
  let exec ← must (Compiler.compile p)
  verifyBackend "ModelIR" (fun args world fuel => runModel model 0 args [.bits 64 0] world context fuel)
  verifyBackend "ExecIR" (fun args world fuel => runExec exec 0 args [.bits 64 0] world context fuel)
  let (source,lowered) ← E39Main.runPair p "AT-Core-1.1-draft" 0 0
    {inputs := [.bool false],committed := [.bits 64 0],context,fuel := 1000}
  check (source.ok && lowered.ok && source.returned == lowered.returned &&
    source.committed == lowered.committed && source.world == lowered.world) "independent complete segment observations"
  IO.println "ReferenceRunner: independent segments, exact fuel boundaries, full rollback and burned nonce reuse PASS"
