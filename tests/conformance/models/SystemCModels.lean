import tests.conformance.models.RuntimeModels

namespace LeanAT.Conformance.SystemCModels
open Compiler ModelIR RuntimeModels

def base : ModelIR.Project := {RuntimeModels.base with components := RuntimeModels.base.components.map (fun c =>
  {c with endpoints := c.endpoints.map (fun e => {e with busWidth := 32})})}

def repeated : ModelIR.Project := {base with components := base.components.map (fun c => if c.id.value == 1 then
    {c with endpoints := c.endpoints ++ [{WireModels.endpoint 1 .initiator with busWidth := 32}]} else c), systems := [{id := 1,runtimeDomain := 1,instances := [{id := ⟨1⟩,definition := ⟨1⟩},{id := ⟨2⟩,definition := ⟨2⟩},{id := ⟨3⟩,definition := ⟨2⟩}], bindings := [{id := ⟨1⟩,sourceEndpoint := ⟨⟨1⟩,0,0⟩,sinkEndpoint := ⟨⟨2⟩,0,0⟩}, {id := ⟨2⟩,sourceEndpoint := ⟨⟨1⟩,1,0⟩,sinkEndpoint := ⟨⟨3⟩,0,0⟩}]}]}

def resultGet := coreService 0 "resultGet" "leanat.core.result.get" "f40fcd8a22c18448a9b8b9bef88c7eca9dc150e28444ab9e4b2465ee6de587d0" [9,10] [3] 3 64 0
def resultRelease := coreService 1 "resultRelease" "leanat.core.result.release" "21c278f5ed404ee7e67528ae212eab663d65b69ea2c08440980a21a244d673e7" [9,10] [0] 3 64 0
def consumer : Handler := {id := 2,parameters := [⟨0,9,{}⟩,⟨1,10,{}⟩],body := [
  .serviceCall (some ⟨2,3,{}⟩) 0 [.local 9 0,.local 10 1], .serviceCall (some ⟨3,0,{}⟩) 1 [.local 9 0,.local 10 1],.ret [.local 3 2]]}
def durable : ModelIR.Project := {base with services := [resultGet,resultRelease],components := base.components.map (fun c =>
  if c.id.value == 2 then {c with handlers := c.handlers ++ [consumer]} else c)}

def scenario (id : String) (model : ModelIR.Project) (branches : List String) : Scenario :=
  {name := id,runner := "systemc",coreIds := [id],model,obligations := branches.map (fun b => id ++ ":" ++ b), sourceFiles := ["tests/conformance/models/SystemCModels.lean","tests/conformance/models/RuntimeModels.lean","tests/conformance/models/WireModels.lean"]}

def scenarios : List Scenario := [
  scenario "C-T02" base ["real-gp-accepted-mutation","native_accepted_phase_mutation_ignored","native_accepted_delay_mutation_ignored","annotated_input_milestone"], scenario "C-T09" repeated ["same_component_multiple_instances","independent_connection_ledgers","shared_domain_call_identity"], scenario "C-T11" base ["nb_no_mm_rejected","blocking_no_mm_owned"], scenario "C-T12" base ["callee_releases_owner_in_callback","call_pin_protects_return_snapshot","future_owned_response"], scenario "C-T13" base ["external_buffer_recycled","call_pin_protects_return_snapshot","future_owned_response"], scenario "C-T14" base ["timeout_remains_local","late_response_wire_ack","pin_retained_until_terminal"], scenario "C-T15" base ["reset_request","reset_service","reset_response"], scenario "C-T28" base ["actual_host_endian","actual_socket_width_rejected","declared_big_endian_rejected"], scenario "C-T30" durable ["native_gp_freed_before_consume","owned_result_published","compiled_result_get_release"], {name := "C-T17-systemc",runner := "systemc",coreIds := ["C-T17"],model := RuntimeModels.timer, obligations := ["C-T17:compiled-ready-wait-systemc-yield"],sourceFiles := ["tests/conformance/models/SystemCModels.lean","tests/conformance/models/RuntimeModels.lean"]}]

end LeanAT.Conformance.SystemCModels
