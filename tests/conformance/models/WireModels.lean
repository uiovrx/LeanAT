import LeanAT.Compiler.Catalog

namespace LeanAT.Conformance.WireModels
open Compiler ModelIR

def endpoint (id : Nat) (role : EndpointRole) : EndpointIR := {
  id,role,busWidth := 64,maxBindings := 1,maxOutstanding := 16,maxPayloadBytes := 64,maxByteEnableBytes := 64}

/-- The native wire rules exercise exactly this loaded domain and connection. -/
def wire : ModelIR.Project := {
  types := [.bits 64]
  components := [{id := ⟨1⟩,endpoints := [endpoint 0 .initiator]}, {id := ⟨2⟩,endpoints := [endpoint 0 .target]}]
  systems := [{id := 1,runtimeDomain := 1,instances := [{id := ⟨1⟩,definition := ⟨1⟩},{id := ⟨2⟩,definition := ⟨2⟩}], bindings := [{id := ⟨1⟩,sourceEndpoint := {instanceId := ⟨1⟩,endpoint := 0,bindingIndex := 0},sinkEndpoint := {instanceId := ⟨2⟩,endpoint := 0,bindingIndex := 0}}]}]
  topSystemId := some 1
}

def descriptor : ModelIR.Project := {wire with components := [
  {id := ⟨1⟩,endpoints := [endpoint 0 .initiator]}, {id := ⟨2⟩,endpoints := [endpoint 0 .target],states := [⟨0,0,.bits 64 7⟩], handlers := [{id := 0,body := [.ret [.state 0 0]]}]}]}

def scenarios : List Scenario := [
  {name := "wire",runner := "wire",coreIds := ["C-T01","C-T02","C-T03","C-T04","C-T05","C-T06","C-T07","C-T08","C-T25"],model := wire, obligations := ["C-T01:native-wire","C-T02:native-accepted-ignored-fields","C-T03:native-wire","C-T04:native-wire","C-T05:native-wire","C-T06:native-wire","C-T07:native-wire","C-T08:native-wire","C-T25:native-wire"], sourceFiles := ["tests/conformance/models/WireModels.lean"]}, {name := "descriptor",runner := "descriptor",coreIds := ["C-T26"],model := descriptor,obligations := ["C-T26:version","C-T26:type","C-T26:tag","C-T26:index","C-T26:valid-execution-control"], sourceFiles := ["tests/conformance/models/WireModels.lean"]}]

end LeanAT.Conformance.WireModels
