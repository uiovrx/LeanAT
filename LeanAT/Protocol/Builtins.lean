import LeanAT.Protocol.Validation
namespace LeanAT.Protocol

def ProbeV1 : Package := {
  name := "LeanAT.ProbeV1", version := "1.0.0"
  phases := [⟨"BEGIN_PROBE", .forward⟩, ⟨"END_PROBE", .backward⟩,
    ⟨"BEGIN_PROBE_RESP", .backward⟩, ⟨"END_PROBE_RESP", .forward⟩]
  states := ["idle", "request", "waitingResponse", "response", "terminal"]
  terminal := [4], lanes := [1,1]
  rules := [
    {pre:=0,flow:=.forward,phase:=0,sync:=.accepted,entryLanes:=[0],callActions:=[.openHop],returnActions:=[.acquireLane 0],post:=1},
    {pre:=0,flow:=.forward,phase:=0,sync:=.updated,returned:=some 1,entryLanes:=[0],callActions:=[.openHop],returnActions:=[.requestReleased],post:=2},
    {pre:=1,flow:=.backward,phase:=1,sync:=.accepted,returnActions:=[.releaseLane 0,.requestReleased],post:=2},
    {pre:=2,flow:=.backward,phase:=2,sync:=.accepted,entryLanes:=[1],returnActions:=[.acquireLane 1,.responseReady],post:=3},
    {pre:=2,flow:=.backward,phase:=2,sync:=.updated,returned:=some 3,entryLanes:=[1],returnActions:=[.responseReady,.closeHop],post:=4},
    {pre:=2,flow:=.backward,phase:=2,sync:=.completed,entryLanes:=[1],returnActions:=[.responseReady,.closeHop],post:=4},
    {pre:=3,flow:=.forward,phase:=3,sync:=.accepted,returnActions:=[.releaseLane 1,.closeHop],post:=4},
    {pre:=3,flow:=.forward,phase:=3,sync:=.completed,returnActions:=[.releaseLane 1,.closeHop],post:=4}]
}
def TraceMarkerV1 : Package := {
  name := "LeanAT.TraceMarkerV1", version := "1.0.0", kind := .ignorableBaseExtension
  phases := [⟨"TRACE_MARKER", .forward⟩], states := ["active", "terminal"]
  terminal := [1], lanes := []
  rules := [{pre:=0,flow:=.forward,phase:=0,sync:=.accepted,returnActions:=[.traceTag "marker"],post:=0}]
}

-- Base's additional BEGIN_REQ response/completion shortcuts are explicit rows.
def BaseBuiltin : Package := {
  ProbeV1 with
  name := "LeanAT.TlmBaseAtV1", kind := .baseBuiltin
  phases := [⟨"BEGIN_REQ", .forward⟩, ⟨"END_REQ", .backward⟩,
    ⟨"BEGIN_RESP", .backward⟩, ⟨"END_RESP", .forward⟩]
  rules := ProbeV1.rules ++ [
    {pre:=0,flow:=.forward,phase:=0,sync:=.updated,returned:=some 2,entryLanes:=[0],callActions:=[.openHop],returnActions:=[.responseReady,.acquireLane 1],post:=3},
    {pre:=0,flow:=.forward,phase:=0,sync:=.completed,entryLanes:=[0],callActions:=[.openHop],returnActions:=[.responseReady,.closeHop],post:=4}]
}
end LeanAT.Protocol


