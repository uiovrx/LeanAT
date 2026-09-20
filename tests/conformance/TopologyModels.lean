import LeanAT.Compiler.Catalog

open LeanAT
namespace LeanAT.Conformance.TopologyModels

def endpoint (id : Nat) (role : ModelIR.EndpointRole) (bindings : Nat := 1) : ModelIR.EndpointIR := {
  id, role, busWidth := 32, maxBindings := bindings, maxOutstanding := 8,
  maxPayloadBytes := 256, maxByteEnableBytes := 16
}

/-- The native crossbar consumes this checked hierarchy and address-map row.
    Its admission observer is an actual compiled handler with instance-local state. -/
def routed : ModelIR.Project := {
  types := [.bits 64]
  components := [
    {id := ⟨1⟩, endpoints := [endpoint 0 .initiator]},
    {id := ⟨2⟩, states := [⟨0,0,.bits 64 0⟩],
      endpoints := [endpoint 0 .target 2, endpoint 1 .initiator],
      handlers := [{id := 0,body := [
        .writeState 0 (.binary 0 .addWrap (.state 0 0) (.literal 0 (.bits 64 1))),
        .emit "route-admitted" [.state 0 0], .ret [.state 0 0]]}]},
    {id := ⟨3⟩, endpoints := [endpoint 0 .target]}]
  systems := [{id := 1,runtimeDomain := 7, instances := [{id := ⟨20⟩,definition := ⟨2⟩},{id := ⟨10⟩,definition := ⟨1⟩}, {id := ⟨30⟩,definition := ⟨1⟩},{id := ⟨40⟩,definition := ⟨3⟩}], bindings := [ {id := ⟨1⟩,sourceEndpoint := ⟨⟨10⟩,0,0⟩,sinkEndpoint := ⟨⟨20⟩,0,0⟩}, {id := ⟨2⟩,sourceEndpoint := ⟨⟨20⟩,1,0⟩,sinkEndpoint := ⟨⟨40⟩,0,0⟩}, {id := ⟨3⟩,sourceEndpoint := ⟨⟨30⟩,0,0⟩,sinkEndpoint := ⟨⟨20⟩,0,1⟩}], addressMaps := [{id := 0,decoder := ⟨20⟩,outputEndpoint := 1,bindingIndex := 0, sourceStart := 4096,size := 256,targetStart := 0}]}]
  topSystemId := some 1
}

/-- Each externally supplied callback order runs this same selected model.
    The profile changes compiler policy, never the callback sequence. -/
def callbackOrders : ModelIR.Project := routed

def scenarios : List Compiler.Scenario := [
  {name := "topology",runner := "routing",coreIds := ["C-T29"],model := routed,
   obligations := ["C-T29:ooo-routes"],sourceFiles := ["tests/conformance/TopologyModels.lean"]},
  {name := "callback-orders",runner := "routing",coreIds := ["C-T24"],model := callbackOrders,
   obligations := ["C-T24:callback-order-forward","C-T24:callback-order-reverse"],
   sourceFiles := ["tests/conformance/TopologyModels.lean"]}]
end LeanAT.Conformance.TopologyModels
