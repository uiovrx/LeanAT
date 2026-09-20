import LeanAT.Compiler.Facade
import LeanAT.Frontend.BlockSyntax

open LeanAT LeanAT.Compiler

at_component FacadeTimer where
  state cell : UInt64 := 0
  process worker : Unit maxInstances 1 frameBytes 1024 results 1 do
    let saved : UInt64 := 41
    await until 5
    set cell := saved
    await after 2
    set cell := 42
    return

def facadeProcessFixture : ModelIR.Project := {
  FacadeTimer.model with
  states := []
  handlers := []
  components := [{id := ⟨0⟩,states := FacadeTimer.model.states,handlers := FacadeTimer.model.handlers}]
  topSystemId := some 0
  systems := [{id := 0,instances := [{id := ⟨0⟩,definition := ⟨0⟩}],bindings := [],runtimeDomain := 1}]
}

def facadeFixture : ModelIR.Project := {
  types := [.bits 64]
  topSystemId := some 0
  components := [{
    id := ⟨0⟩
    states := [⟨0,0,.bits 64 0⟩]
    handlers := [{id := 0,body := [.writeState 0 (.literal 0 (.bits 64 42)),.ret []]}]
  }]
  systems := [{id := 0,instances := [{id := ⟨0⟩,definition := ⟨0⟩},{id := ⟨1⟩,definition := ⟨0⟩}],bindings := [],runtimeDomain := 1}]
}

private def wireEndpoint (role : ModelIR.EndpointRole) : ModelIR.EndpointIR := {
  id := 0,role,busWidth := 32,maxBindings := 1,maxOutstanding := 4,maxPayloadBytes := 16,maxByteEnableBytes := 16
}
def facadeWireFixture : ModelIR.Project := {
  types := [.bits 64]
  topSystemId := some 0
  components := [
    {id := ⟨0⟩,states := [⟨0,0,.bits 64 0⟩],endpoints := [wireEndpoint .initiator],
      handlers := [{id := 0,endpoint := some 0,trigger := "beginResp",body := [.writeState 0 (.literal 0 (.bits 64 84)),.ret []]}]},
    {id := ⟨1⟩,states := [⟨0,0,.bits 64 0⟩],endpoints := [wireEndpoint .target],
      handlers := [{id := 0,endpoint := some 0,trigger := "beginReq",body := [.writeState 0 (.literal 0 (.bits 64 42)),.ret []]},
        {id := 1,endpoint := some 0,trigger := "endResp",body := [.ret []]}]}]
  systems := [{id := 0,instances := [{id := ⟨0⟩,definition := ⟨0⟩},{id := ⟨1⟩,definition := ⟨1⟩}],bindings := [{id := ⟨0⟩,sourceEndpoint := ⟨⟨0⟩,0,0⟩,sinkEndpoint := ⟨⟨1⟩,0,0⟩}],runtimeDomain := 1}]
}

#eval facadeCppString "quoted\"\\\n模型"

def facadeSignalFixture : ModelIR.Project := {
  facadeFixture with
  services := [{id := 10,opcode := "loadInput",inputTypes := [0],resultTypes := [0],contextMask := 3,effectMask := 0,extraFuel := 0,providerKey := "leanat.core.input.read",providerVersion := "1",abiHash := [224,132,216,99,234,243,112,221,123,128,4,148,44,23,32,250,200,240,41,119,251,158,250,141,1,186,133,24,48,18,211,15]}]
  components := facadeFixture.components.map (fun c => {c with handlers := [{id := 0,body := [.serviceCall (some ⟨0,0,{}⟩) 10 [.literal 0 (.bits 64 1)],.writeState 0 (.local 0 0),.ret []]}],sidebands := [
    {id := 0,direction := .output,typeId := 0,initial := some (.bits 64 7)},
    {id := 1,direction := .input,typeId := 0,initial := some (.bits 64 7)}]})
  systems := facadeFixture.systems.map (fun s => {s with sidebandBindings := [
    {id := 0,sourcePort := .instance ⟨0⟩ 0,sinkPort := .instance ⟨1⟩ 1},
    {id := 1,sourcePort := .instance ⟨1⟩ 0,sinkPort := .instance ⟨0⟩ 1}]})
}

def main (args : List String) : IO Unit := do
  let some outputDir := args.head? | throw (IO.userError "expected fixture output directory")
  let model := if args.contains "wire" then facadeWireFixture else if args.contains "signal" then facadeSignalFixture else if args.contains "process" then facadeProcessFixture else facadeFixture
  let selectedProfile := if args.contains "ext" then "AT-Ext-1.1-draft" else "AT-Core-1.1-draft"
  let p ← match compileForProfile model selectedProfile with | .ok p => pure p | .error e => throw (IO.userError e)
  let files ← match emitFacade p with | .ok files => pure files | .error e => throw (IO.userError e)
  for (name,content) in files do
    let path := System.FilePath.mk outputDir / name
    if let some parent := path.parent then IO.FS.createDirAll parent
    IO.FS.writeFile path content
  IO.FS.writeBinFile (System.FilePath.mk outputDir / "model.execir.bin") (serialize p)
  let bytes := String.intercalate "," ((serialize p).data.toList.map (fun b => toString b.toNat))
  IO.FS.writeFile (System.FilePath.mk outputDir / "src/Component_desc.cpp")
    ("#include \"Component.hpp\"\nnamespace leanat_generated {const leanat::Bytes& descriptor_bytes(){static const leanat::Bytes bytes={" ++ bytes ++ "};return bytes;}}\n")





