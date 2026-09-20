import LeanAT.Compiler.Catalog

namespace LeanAT.Conformance.MemoryModels
open LeanAT Compiler

def types : TypeEnvironment := [.bits 64,.bytes 64,.record [0,1]]
private def u64 (n : Nat) : List UInt8 := (List.range 8).map (fun i => UInt8.ofNat (n / 256^i % 256))
private def scalarType (kind bound : Nat) := u64 kind ++ u64 bound ++ u64 0 ++ u64 0
private def encodeType : Nat → List UInt8
  | 0 => scalarType 2 64
  | 1 => scalarType 8 64
  | _ => u64 4 ++ u64 0 ++ u64 2 ++ scalarType 2 64 ++ scalarType 8 64 ++ u64 0

/-- Exact standard object ABI, independently checked by ObjectServices::bind_existing. -/
def serviceSignature (id objectId kind method effects : Nat) (debug : Bool) (args results : List Nat) : ModelIR.ServiceIR :=
  let mask := if debug then 8 else 3
  let canonical := u64 kind ++ u64 method ++ u64 mask ++ u64 effects ++ u64 73 ++ args.flatMap encodeType ++ [255] ++ results.flatMap encodeType
  {id,opcode := if debug then "debugTransfer" else "objectCall",inputTypes := args,resultTypes := results,
   contextMask := mask,effectMask := effects,extraFuel := 73,providerKey := s!"leanat.stdlib.object.{objectId}.method.{method}",
   providerVersion := "2",abiHash := (computeArtifactHash ⟨canonical.toArray⟩).data.toList}

def service (id objectId kind method : Nat) (debug : Bool) (args : List Nat) :=
  serviceSignature id objectId kind method 131 debug args [2]

def handler (id serviceId : Nat) (debug : Bool) (args : List Nat) : ModelIR.Handler := {
  id,context := if debug then .debugEntry else .timedHandler
  endpoint := some 0
  trigger := if debug then "memory.debug" else "memory.transfer"
  parameters := args.zipIdx |>.map (fun (t,i) => ⟨i,t,{}⟩)
  declaredResultTypes := some [2]
  body := [.serviceCall (some ⟨args.length,2,{}⟩) serviceId (args.zipIdx |>.map (fun (t,i) => .local t i))] ++
    (if debug then [] else [.writeState 0 (.binary 0 .addWrap (.state 0 0) (.literal 0 (.bits 64 1)))]) ++
    [.ret [.local 2 args.length]]
}

def model : ModelIR.Project := {
  types
  services := [service 100 1 0 1 false [0,0,1,0,1],service 101 1 0 6 true [0,0,1],
    service 102 2 1 1 false [0,0,1,0,1],service 103 2 1 6 true [0,0,1]]
  topSystemId := some 0
  components := [
    {id := ⟨0⟩,endpoints := [{id := 0,role := .initiator,busWidth := 32,maxBindings := 1,maxOutstanding := 4,maxPayloadBytes := 64,maxByteEnableBytes := 64}]},
    {id := ⟨1⟩,states := [⟨0,0,.bits 64 0⟩],endpoints := [{id := 0,role := .target,busWidth := 32,maxBindings := 1,maxOutstanding := 4,maxPayloadBytes := 64,maxByteEnableBytes := 64}],
     handlers := [handler 0 100 false [0,0,1,0,1],handler 1 101 true [0,0,1],handler 2 102 false [0,0,1,0,1],handler 3 103 true [0,0,1]]}]
  systems := [{id := 0,runtimeDomain := 1,instances := [{id := ⟨1⟩,definition := ⟨0⟩},{id := ⟨2⟩,definition := ⟨1⟩}],bindings := [{id := ⟨1⟩,sourceEndpoint := ⟨⟨1⟩,0,0⟩,sinkEndpoint := ⟨⟨2⟩,0,0⟩}]}]
}

def scenarios : List Compiler.Scenario := [{
  name := "memory"
  runner := "memory"
  coreIds := ["C-T20","C-T21","C-T22","C-T23"]
  model
  obligations := ["C-T20:masked-streaming-bytes","C-T21:ignore-overflow-zero","C-T22:debug-rc-partial-noirq","C-T23:mixed-sockets-one-backing"]
  sourceFiles := ["tests/conformance/models/MemoryModels.lean"]
}]
end LeanAT.Conformance.MemoryModels

