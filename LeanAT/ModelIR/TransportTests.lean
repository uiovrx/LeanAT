import LeanAT.ModelIR.Transport
namespace LeanAT.ModelIR.Transport.Tests
private def config : Config := ⟨4,64,64,32,32⟩
private def payload : Payload := ⟨0,0,[1,2],2,2,[],0,false,[]⟩
private def call : WireCall := ⟨0,0,0,.forward,.beginReq,100,10,payload⟩
private def completed : WireReturn := ⟨.completed,some .beginReq,25,some {payload with status := 1}⟩
#guard (runFrom config none 6 {} [.call call,.reply 0 completed]).reason == .quiescent
#guard (runFrom config none 6 {} [.call call,.reply 0 completed]).state.trace.filterMap (fun r => match r with | .timed e => some e.time | _ => none) == [110,125,125,125]
#guard let r := runFrom config (some 110) 20 {} [.call call,.reply 0 completed]
       r.reason == .horizonReached && r.state.events.length == 3 && r.remaining.isEmpty && r.state.hops.all (fun h => h.state == .terminal)
#guard let r := runFrom config none 2 {} [.call call,.reply 0 completed]
       r.reason == .runBudgetReached && r.state.trace.length == 2 && r.state.events.length == 4
#guard let r := runFrom config none 20 {} [.call call,.reply 0 ⟨.accepted,some .endResp,2^80,none⟩]
       r.reason == .waitingForEnvironment && r.state.trace.length == 3
#guard let r := runFrom config none 20 {} [.call call,.reply 0 completed,.reply 0 completed]
       r.reason == .failed "UnknownOrConsumedCallTicket" && r.state.trace.length == 3
#guard let r := runFrom config none 20 {} [.call call,.reply 0 ⟨.updated,some .endResp,25,none⟩]
       r.reason == .failed "IllegalUpdatedPhase" && r.state.trace.length == 2 && r.state.pending.length == 1
#guard let r := runFrom {config with maxEventsPerTick := 1} none 20 {} [.call call,.reply 0 completed]
       r.reason == .failed "ZenoDetected" && r.state.events.length == 2
#guard let second := {call with id := 1, transaction := 1, callTime := 101}
       let r := runFrom config none 20 {} [.call call,.reply 0 ⟨.accepted,none,0,none⟩,.call second]
       r.reason == .failed "RequestLaneBusy"
#guard let second := {call with id := 1, connection := 1, transaction := 1, callTime := 101}
       let r := runFrom config none 20 {} [.call call,.reply 0 ⟨.accepted,none,0,none⟩,.call second,.reply 1 ⟨.accepted,none,0,none⟩]
       r.reason == .waitingForEnvironment && r.state.hops.length == 2
#guard let intent : SendIntent := ⟨0,0,0,.forward,.beginReq,100,10,payload⟩
       match queueIntent config {} intent with
       | .error _ => false
       | .ok s => (runFrom config none 20 s [.outbound {call with payload := {payload with address := 9}},.reply 0 completed]).reason == .failed "EnvironmentViolation: outbound intent mismatch"
#guard match startCall config {lastEventTime := some 110,lastEventTurn := 2} {call with callTime := 110,incomingDelay := 0} with
       | .ok next => next.events.head?.any (fun event => event.turn == 3)
       | .error _ => false
#guard match startCall config {lastEventTime := some 111} call with
       | .error error => error == "TimeRegression" | _ => false
#guard match startCall config {lastEventTime := some 110,lastEventTurn := 2^64-1} call with
       | .error error => error == "TurnExhausted" | _ => false
private def responseEntry : WireCall := {call with id := 1,phase := .beginResp,flow := .backward,callTime := 120,incomingDelay := 10,payload := {payload with status := 1}}
private def responsePrefix : List TranscriptRecord := [.call call,.reply 0 ⟨.accepted,none,0,none⟩,.call responseEntry]
private def terminalReturnCase (sync : Sync) : RunResult :=
  runFrom config none 32 {} (responsePrefix ++ [.reply 1 ⟨sync,some .endResp,25,some {payload with data := [9,9],status := 1}⟩])
#guard [.updated,.completed].all (fun sync =>
  let result := terminalReturnCase sync
  let milestones := result.state.trace.filterMap (fun record => match record with
    | .timed event => match event.kind with
      | .requestReleased | .responseReady | .terminal => some (event.time,event.snapshot.data)
      | _ => none
    | _ => none)
  result.reason == .quiescent && milestones == [(130,[1,2]),(130,[1,2]),(145,[1,2])])
#guard (runFrom config none 32 {} (responsePrefix.take 2 ++ [.call {responseEntry with payload := {payload with data := [1],length := 1,streamingWidth := 1,status := 1}}])).reason == .failed "InvalidResponseSnapshot"
#guard [.updated,.completed].all (fun sync =>
  (runFrom config none 32 {} [.call call,.reply 0 ⟨sync,some .beginResp,25,some {payload with data := [1],length := 1,streamingWidth := 1,status := 1}⟩]).reason == .failed "InvalidResponseSnapshot")
#guard (runFrom config none 0 {} []).reason == .runBudgetReached
#guard let waiting := (runFrom config none 8 {} [.call call,.reply 0 ⟨.accepted,none,0,none⟩]).state
       (runFrom config none 0 waiting []).reason == .runBudgetReached && (runFrom config none 1 waiting []).reason == .waitingForEnvironment
#guard (runFrom config none 1 {} []).reason == .quiescent
end LeanAT.ModelIR.Transport.Tests



