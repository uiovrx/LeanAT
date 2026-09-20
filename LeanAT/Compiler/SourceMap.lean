import LeanAT.Compiler.Descriptor
import LeanAT.Frontend.Source
import Lean

namespace LeanAT.Compiler
open Lean ExecIR SourceMapping

structure SourceMapArtifact where
  json : Json
  documents : List (String × ByteArray)

private def location (m : LoweredOrigin) : Json := Json.mkObj (
  [("ir",.str "exec"),("program",toJson m.program),("kind",.str m.kind)] ++
  (m.block.toList.map fun n => ("block",toJson n)) ++
  (m.instruction.toList.map fun n => ("instruction",toJson n)))

private def expectedLocations (p : ExecProject) : List LoweredOrigin :=
  p.programs.flatMap fun program =>
    {program := program.id,kind := "program",node := program.source} ::
    program.blocks.flatMap fun block =>
      [{program := program.id,kind := "block",block := some block.id,node := program.source},
       {program := program.id,kind := "terminator",block := some block.id,node := program.source}] ++
      block.instructions.zipIdx.map fun (inst,i) =>
        {program := program.id,kind := "instruction",block := some block.id,instruction := some i,node := inst.source}

private def kindText : OriginKind → String
  | .native => "native" | .constructed => "constructed-model" | .generated => "generated"

private def makeSpan (document : SourceDocument) (origin : NodeOrigin) (constructedExec : Bool := false) : Except String Json := do
  let bytes := document.content.toUTF8
  if bytes.size > 8388608 then throw "SourceDocumentLimit"
  if origin.startByte > origin.endByte || origin.endByte > bytes.size then throw "SourceRange"
  let some before := String.fromUTF8? (bytes.extract 0 origin.startByte) | throw "SourceStartSplitsUTF8"
  let some _ := String.fromUTF8? (bytes.extract origin.startByte origin.endByte) | throw "SourceEndSplitsUTF8"
  let hash := hex (computeArtifactHash bytes)
  let lines := before.splitOn "\n"
  pure (Json.mkObj [("path",.str s!"sources/{hash}.lean"),("originalPath",.str document.path),
    ("hash",.str hash),("startByte",toJson origin.startByte),("endByte",toJson origin.endByte),
    ("line",toJson lines.length),("column",toJson ((lines.getLast?.getD "").length+1)),
    ("ancestry",toJson origin.ancestry),("provenance",.str (if constructedExec then "constructed-exec" else kindText origin.kind))])

/-- The constructor representation is an explicit generated source document, not a native DSL span. -/
private def constructedBundle (nodes : List ModelNode) : Bundle := Id.run do
  let mut content := ""
  let mut origins := []
  for node in nodes do
    let startByte := content.utf8ByteSize
    content := content ++ node.text ++ "\n"
    origins := origins ++ [{nodePath := node.path,documentPath := "constructed-model.lean",startByte,endByte := content.utf8ByteSize-1,kind := .constructed,ancestry := []}]
  return {documents := [{path := "constructed-model.lean",content}],origins}

def buildSourceMap (compilation : Compilation) (provided : Bundle := {}) : Except String SourceMapArtifact := do
  if provided.origins.length > 65536 || provided.modelNodes.length > 65536 || provided.documents.length > 1024 ||
      provided.origins.foldl (fun n o => n + o.ancestry.length) 0 > 65536 then throw "SourceMapBudget"
  let p := compilation.validated.project
  let expected := expectedLocations p
  let keys := compilation.origins.map (fun o => (location o).compress)
  if keys.eraseDups.length != keys.length || keys.length != expected.length then throw "SourceLocationCoverage"
  for e in expected do
    let some actual := compilation.origins.find? (fun o => location o == location e) | throw "MissingSourceLocation"
    if (e.kind == "instruction" || e.kind == "program") && actual.node != e.node then throw "SourceTokenMismatch"
  if (compilation.nodes.map ModelNode.path).eraseDups.length != compilation.nodes.length then throw "DuplicateModelNode"
  if (provided.documents.map SourceDocument.path).eraseDups.length != provided.documents.length then throw "DuplicateSourceDocument"
  if (provided.origins.map NodeOrigin.nodePath).eraseDups.length != provided.origins.length then throw "DuplicateSourceOrigin"
  if (provided.modelNodes.map Prod.fst).eraseDups.length != provided.modelNodes.length then throw "DuplicateModelBinding"
  for (path,text) in provided.modelNodes do
    let some node := compilation.nodes.find? (fun n => n.path == path) | throw "UnknownModelBinding"
    if text != node.text then throw "StaleModelBinding"
  for o in provided.origins do
    let some node := compilation.nodes.find? (fun n => n.path == o.nodePath) | throw "UnknownSourceNode"
    if provided.modelNodes.lookup o.nodePath != some node.text then throw "MissingModelBinding"
    if o.kind == .native && o.startByte == o.endByte then throw "EmptyNativeSourceRange"
    if !o.ancestry.all (fun ancestor => compilation.nodes.any (fun n => n.path == ancestor)) then throw "UnknownSourceAncestry"
  -- Topological elimination is bounded independently of caller supplied ancestry.
  let mut remaining := provided.origins
  let mut work := 0
  for _ in [:65] do
    if remaining.isEmpty then break
    let mut next := []
    for origin in remaining do
      work := work + 1 + origin.ancestry.length * remaining.length
      if work > 1000000 then throw "SourceAncestryWorkLimit"
      if origin.ancestry.any (fun ancestor => remaining.any (fun o => o.nodePath == ancestor)) then
        next := next ++ [origin]
    if next.length == remaining.length then throw "SourceAncestryCycle"
    remaining := next
  if !remaining.isEmpty then throw "SourceAncestryDepthLimit"
  let fallback := constructedBundle compilation.nodes
  let mut spans : List (String × Json) := []
  let mut documents : List (String × ByteArray) := []
  for node in compilation.nodes do
    let origin ← match provided.origins.find? (fun o => o.nodePath == node.path) with
      | some origin => pure origin
      | none => match fallback.origins.find? (fun o => o.nodePath == node.path) with
        | some origin => pure origin | none => throw "MissingConstructedOrigin"
    let sourceDocuments := if provided.origins.any (fun o => o.nodePath == node.path) then provided.documents else fallback.documents
    let some document := sourceDocuments.find? (fun d => d.path == origin.documentPath) | throw "MissingSourceDocument"
    if origin.kind == .generated && origin.ancestry.isEmpty then throw "MissingGeneratedAncestry"
    let span ← makeSpan document origin (node.kind == "constructed-exec")
    spans := spans ++ [(node.path,span)]
    let path := s!"sources/{hex (computeArtifactHash document.content.toUTF8)}.lean"
    if !documents.any (fun d => d.1 == path) then documents := documents ++ [(path,document.content.toUTF8)]
  let mut entries := []
  for node in compilation.nodes do
    let some span := spans.lookup node.path | throw "MissingModelSpan"
    if node.kind != "constructed-exec" then
      entries := entries ++ [Json.mkObj [("location",Json.mkObj [("ir",.str "model"),("node",.str node.path)]),
        ("sourceSpan",span),("modelNode",.str node.path),("nodeKind",.str node.kind),
        ("lowering",.str (if compilation.origins.any (fun o => o.node == node.path) then "emitted" else "no-direct-instruction"))]]
  for origin in compilation.origins do
    let some span := spans.lookup origin.node | throw "MissingLoweredNodeOrigin"
    let direct := compilation.nodes.any (fun n => n.path == origin.node && n.kind == "constructed-exec")
    entries := entries ++ [Json.mkObj ([("location",location origin),("sourceSpan",span)] ++
      (if direct then [] else [("modelNode",.str origin.node)]))]
  pure ⟨Json.mkObj [("schema",.str "leanat.source-map.v1"),
    ("artifactHash",.str (hex (computeArtifactHash (serialize compilation.validated)))),
    ("entries",.arr entries.toArray)],documents⟩

/-- Direct descriptor emission has only constructed ExecIR provenance; it cannot invent ModelIR nodes. -/
def directExecCompilation (validated : ValidatedProject) : Compilation := Id.run do
  let project := validated.project
  -- Keep the descriptor unchanged; existing source strings are references, not trusted source ranges.
  let origins := expectedLocations project
  let mut nodes := []
  for origin in origins do
    if !nodes.any (fun n : ModelNode => n.path == origin.node) then
      nodes := nodes ++ [⟨origin.node,"constructed-exec",reprStr project⟩]
  return ⟨validated,origins,nodes⟩

end LeanAT.Compiler
