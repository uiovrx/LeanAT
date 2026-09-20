import LeanAT.Frontend.BlockSyntax
import LeanAT.ModelIR.Semantics

set_option maxRecDepth 8192
set_option maxHeartbeats 1000000
open LeanAT LeanAT.Frontend

-- A finite one-cell memory: write handler and read handler, independently callable.
at_component Memory where
  state cell : UInt8 := 0
  target bus : TlmBase 64 capacity 2 payload 1 mask 1
  on bus.beginReq tx do
    set cell := 42
    emit "write_complete"
    return

at_extern_component Traffic := {
  cppType := "example::Traffic", contract := "Caller obeys TlmBaseAtV1; assumed external contract"
  component := {program := {}, contexts := [], ports := [{name := "bus", role := .initiator, width := 64, maxOutstanding := 2, maxPayloadBytes := 1, maxByteEnableBytes := 1}]}
}

at_system MemorySystem := {
  instances := [⟨"traffic", "Traffic", Traffic.component⟩, ⟨"memory", "Memory", Memory.checked.val⟩]
  bindings := [⟨⟨0,0⟩,⟨1,0⟩⟩]
  maps := [⟨0,1,0,0⟩]
}

#eval do
  let checked ← ModelIR.validateSchema Memory.model
  let some c := checked.project.components.head? | throw "MissingComponent"
  let some h := c.handlers.head? | throw "MissingHandler"
  let run ← ModelIR.runSegment Memory.checked.val.program {values := c.states.map (fun (s : ModelIR.StateSlot) => (s.id,s.initial))} h.body
  pure run.values

