import tests.conformance.models.WireModels
import tests.conformance.models.MemoryModels

namespace LeanAT.Conformance.RuntimeModels
open Compiler ModelIR

def decodeHash (value : String) : List UInt8 :=
  let digit := fun c : Char => if c >= '0' && c <= '9' then c.toNat-'0'.toNat else c.toNat-'a'.toNat+10
  (List.range (value.length/2)).map fun i => UInt8.ofNat (digit (value.toList[i*2]!) * 16 + digit (value.toList[i*2+1]!))

def coreService (id : Nat) (opcode key hash : String) (inputs results : List Nat) (contexts effects fuel : Nat) : ServiceIR :=
  {id,opcode,inputTypes := inputs,resultTypes := results,contextMask := contexts,effectMask := effects,extraFuel := fuel,providerKey := key,providerVersion := "1",abiHash := decodeHash hash}

def types : TypeEnvironment := [.unit,.bool,.bits 64,.bytes 64,.handle .process,.handle .wait,.handle .hop,.record [2,3,1],.handle .event,.handle .result,.handle .consumer]
def contextService := coreService 0 "getContextField" "leanat.core.context.process" "f36277e80b561e7f2fccbe5e1fde0aa9b6599153f09130bd1f1c96f12133a0da" [] [4] 2 0 1
def timerService := coreService 1 "registerWait" "leanat.core.wait.timer" "783dda9c9ea7bd22ee2b2b075230e110219632fb463886aed1579583d2aea36f" [4,2,2] [5] 2 16 1
def responseService := coreService 1 "registerWait" "leanat.core.wait.response" "7f0dfc014f1a8c07de8d55c2e40a52b85fc7d534be8df44f48d31c2962db1a04" [4,6] [5] 2 16 1

def writeHandler (id value : Nat) : Handler := {id,body := [.writeState 0 (.literal 2 (.bits 64 value)),.ret []]}

def base : ModelIR.Project := {WireModels.wire with types,components := [
  {id := ⟨1⟩,endpoints := [WireModels.endpoint 0 .initiator]}, {id := ⟨2⟩,endpoints := [WireModels.endpoint 0 .target],states := [⟨0,2,.bits 64 7⟩],handlers := [writeHandler 0 99,writeHandler 1 7]}]}

def pending : ModelIR.Project := {base with components := base.components.map (fun c =>
    {c with endpoints := c.endpoints ++ [WireModels.endpoint 1 (if c.id.value == 1 then .initiator else .target)]}), systems := base.systems.map (fun s => {s with bindings := s.bindings ++ [{id := ⟨2⟩,sourceEndpoint := ⟨⟨1⟩,1,0⟩,sinkEndpoint := ⟨⟨2⟩,1,0⟩}]})}

def responseProcess : Handler := {id := 0,context := .process,trigger := "response-ready",parameters := [⟨0,6,{}⟩],processCapacity := some {frameBytesLimit := 2048,resultCapacity := 1},body := [
  .letVal 1 (.literal 2 (.bits 64 41)),.serviceCall (some ⟨2,4,{}⟩) 0 [], .serviceCall (some ⟨3,5,{}⟩) 1 [.local 4 2,.local 6 0],.await (.local 5 3) 4 7, .writeState 0 (.local 2 1),.ret [.local 7 4,.local 2 1]]}

def response : ModelIR.Project := {WireModels.wire with types,services := [contextService,responseService],components := [
  {id := ⟨1⟩,endpoints := [WireModels.endpoint 0 .initiator],states := [⟨0,2,.bits 64 7⟩],handlers := [responseProcess]}, {id := ⟨2⟩,endpoints := [WireModels.endpoint 0 .target]}]}

def timerProcess : Handler := {id := 0,context := .process,trigger := "ready-loop",processCapacity := some {frameBytesLimit := 32768,resultCapacity := 1},body := [
  .repeat 128 [.serviceCall (some ⟨0,4,{}⟩) 0 [],.serviceCall (some ⟨1,5,{}⟩) 1 [.local 4 0,.literal 2 (.bits 64 3),.literal 2 (.bits 64 0)], .await (.local 5 1) 2 0,.writeState 0 (.binary 2 .addWrap (.state 2 0) (.literal 2 (.bits 64 1)))],.ret [.state 2 0]]}

def timer : ModelIR.Project := {base with profile := {maxEventsPerTick := 16},services := [contextService,timerService],components := base.components.map (fun c =>
  if c.id.value == 2 then {c with handlers := [timerProcess],states := [⟨0,2,.bits 64 0⟩]} else c)}

def overflow : ModelIR.Project := {base with components := base.components.map (fun c =>
  if c.id.value == 2 then {c with handlers := [{id := 0,body := [.writeState 0 (.literal 2 (.bits 64 99)), .letVal 0 (.binary 2 .addChecked (.literal 2 (.bits 64 (2^64-1))) (.literal 2 (.bits 64 1))),.ret []]}]} else c)}

def rollback : ModelIR.Project := {WireModels.wire with types := MemoryModels.types ++ [.handle .event],services := [
  MemoryModels.serviceSignature 100 1 0 3 130 false [0,1,1] [], coreService 101 "scheduleEvent" "leanat.core.event.schedule" "bffe39a0efb9a6f7c75fbb789e564aac1286c4f9b6c00e6f2971d896c39b8705" [0,0] [3] 3 8 0], components := [{id := ⟨1⟩,endpoints := [WireModels.endpoint 0 .initiator]}, {id := ⟨2⟩,endpoints := [WireModels.endpoint 0 .target],states := [⟨0,0,.bits 64 7⟩],handlers := [{id := 0,body := [
      .serviceCall none 100 [.literal 0 (.bits 64 0),.literal 1 (.bytes [9]),.literal 1 (.bytes [])], .serviceCall (some ⟨0,3,{}⟩) 101 [.literal 0 (.bits 64 0),.literal 0 (.bits 64 0)],.ret []]}]}]}

def scenario (name : String) (model : ModelIR.Project) (obligations : List String) : Scenario :=
  {name,runner := "runtime",coreIds := [name],model,obligations,sourceFiles := ["tests/conformance/models/RuntimeModels.lean","tests/conformance/models/WireModels.lean","tests/conformance/models/MemoryModels.lean"]}

def scenarios : List Scenario := [scenario "C-T10" pending ["C-T10:bounded-pending-owned-service-vm-retirement"], scenario "C-T16" response ["C-T16:completed-before-registration-compiled-response-wait"],scenario "C-T17" timer ["C-T17:compiled-ready-wait-native-limits"], scenario "C-T18" rollback ["C-T18:compiled-object-write-schedule-atomic-rollback"],scenario "C-T19" base ["C-T19:compiled-local-prefix-real-peer-violation"], scenario "C-T27" overflow ["C-T27:runtime-counter-boundaries-compiled-value-overflow"]]

end LeanAT.Conformance.RuntimeModels
