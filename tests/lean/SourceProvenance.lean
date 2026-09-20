import LeanAT.Frontend.BlockSyntax
import LeanAT.Compiler.Provenance
import LeanAT.Compiler.SourceMap

open LeanAT
set_option maxRecDepth 8192
set_option maxHeartbeats 1000000

-- 中文与 λ precede the declarations: character offsets would fail these checks.
at_component SourceProcess where
  state cell : UInt64 := 0
  process worker : Unit maxInstances 1 frameBytes 1024 results 1 do
    let saved : UInt64 := 41
    await until 5
    set cell := saved
    let timer ← registerWait until 7
    let outcome ← await timer
    set cell := 42
    return

def sourceSlice (bundle : SourceMapping.Bundle) (path : String) : String := Id.run do
  let some origin := bundle.origins.find? (·.nodePath == path) | return "missing-node"
  let some document := bundle.documents.find? (·.path == origin.documentPath) | return "missing-document"
  return String.fromUTF8! (document.content.toUTF8.extract origin.startByte origin.endByte)

def fullyCovered (bundle : SourceMapping.Bundle) (nodes : List Compiler.ModelNode) : Bool :=
  bundle.origins.length == nodes.length &&
  bundle.modelNodes.length == nodes.length &&
  nodes.all (fun node => bundle.modelNodes.lookup node.path == some node.text) &&
  (bundle.origins.map (·.nodePath)).eraseDups.length == nodes.length &&
  nodes.all (fun node => bundle.origins.any (·.nodePath == node.path)) &&
  bundle.origins.all (fun origin => origin.startByte < origin.endByte &&
    (origin.kind != .generated || (!origin.ancestry.isEmpty && origin.ancestry.all
      (fun ancestor => bundle.origins.any (·.nodePath == ancestor)))))

#guard fullyCovered SourceProcess.sourceBundle
  (SourceProcess.model.handlers.flatMap fun h => Compiler.handlerNodes s!"handler/{h.id}" h)
#guard sourceSlice SourceProcess.sourceBundle "handler/0/body/0" == "let saved : UInt64 := 41"
#guard sourceSlice SourceProcess.sourceBundle "handler/0/body/0/expr/0" == "41"
#guard sourceSlice SourceProcess.sourceBundle "handler/0/body/2/expr/2" == "5"
#guard sourceSlice SourceProcess.sourceBundle "handler/0/body/4/expr/0" == "saved"
#guard sourceSlice SourceProcess.sourceBundle "handler/0/body/6/expr/2" == "7"
#guard sourceSlice SourceProcess.sourceBundle "handler/0/body/7/expr/0" == "timer"
#guard (SourceProcess.sourceBundle.origins.find? (·.nodePath == "handler/0/body/1")).map (·.kind) == some .generated

at_component SourceSignal where
  output irq : Bool := false
  on internal.raise event do
    set output irq := true
    return

at_system SourceSystem where
  instance signal := SourceSignal
  instance worker :=  SourceProcess
  output irq : Bool
  connect signal.irq => top.irq

#guard fullyCovered SourceSystem.sourceBundle
  (SourceSystem.model.components.flatMap fun c => c.handlers.flatMap fun h =>
    Compiler.handlerNodes s!"component/{c.id.value}/handler/{h.id}" h)
#guard sourceSlice SourceSystem.sourceBundle "component/1/handler/0/body/0/expr/1" == "true"
#guard SourceSystem.sourceBundle.documents.length == 1

at_component SourceTransport where
  target bus : TlmBase 64 capacity 1 payload 8 mask 8
  on bus.transport fw beginReq tx do
    returnTransport accepted

#guard fullyCovered SourceTransport.sourceBundle
  (SourceTransport.model.components.flatMap fun c => c.handlers.flatMap fun h =>
    Compiler.handlerNodes s!"component/{c.id.value}/handler/{h.id}" h)
#guard sourceSlice SourceTransport.sourceBundle "component/0/handler/0/body/0/expr/0" == "accepted"

namespace SourceNamespace
at_component Nested where
  on internal.event e do
    return
at_system NestedSystem where
  instance nested := Nested
#guard fullyCovered NestedSystem.sourceBundle
  (NestedSystem.model.components.flatMap fun c => c.handlers.flatMap fun h =>
    Compiler.handlerNodes s!"component/{c.id.value}/handler/{h.id}" h)
end SourceNamespace

open SourceNamespace
at_system OpenNamespaceSystem where
  instance nested := Nested
#guard fullyCovered OpenNamespaceSystem.sourceBundle
  (OpenNamespaceSystem.model.components.flatMap fun c => c.handlers.flatMap fun h =>
    Compiler.handlerNodes s!"component/{c.id.value}/handler/{h.id}" h)

def checkNativeMap (model : ModelIR.Project) (bundle : SourceMapping.Bundle) : IO Unit := do
  let compilation ← match Compiler.compileWithProvenance model with
    | .ok value => pure value
    | .error message => throw (IO.userError message)
  match Compiler.buildSourceMap compilation bundle with
  | .ok _ => pure ()
  | .error message => throw (IO.userError message)

#eval checkNativeMap SourceProcess.model SourceProcess.sourceBundle
#eval checkNativeMap SourceSystem.model SourceSystem.sourceBundle

at_component SourceInitiator where
  initiator bus : TlmBase 64 capacity 1 payload 8 mask 8
at_system SourceTransportSystem where
  instance master := SourceInitiator
  instance device := SourceTransport
  bind master.bus => device.bus
#eval checkNativeMap SourceTransportSystem.model SourceTransportSystem.sourceBundle

def changedLiteralModel : ModelIR.Project :=
  { SourceProcess.model with handlers := SourceProcess.model.handlers.map fun handler =>
      { handler with body := handler.body.map fun statement => match statement with
        | .letVal id (.literal 5 (.bits 64 41)) => .letVal id (.literal 5 (.bits 64 40))
        | other => other } }

def expectMapError (model : ModelIR.Project) (bundle : SourceMapping.Bundle) (expected : String) : IO Unit := do
  let compilation ← match Compiler.compileWithProvenance model with
    | .ok value => pure value
    | .error message => throw (IO.userError ("UnexpectedCompileFailure: " ++ message))
  match Compiler.buildSourceMap compilation bundle with
  | .error message => if message != expected then throw (IO.userError ("WrongDiagnostic: " ++ message))
  | .ok _ => throw (IO.userError ("ExpectedError: " ++ expected))

-- Same node paths and valid types; only a literal changed after exporting the bundle.
#eval expectMapError changedLiteralModel SourceProcess.sourceBundle "StaleModelBinding"
#eval expectMapError SourceProcess.model {SourceProcess.sourceBundle with modelNodes := []} "MissingModelBinding"

