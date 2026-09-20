import LeanAT.Compiler.Catalog
import LeanAT.Compiler.OpcodeCase

namespace LeanAT.Compiler
open Lean

private def opcodeText (path text : String) : GeneratedFile := ⟨path,text.toUTF8⟩
private def digest (bytes : ByteArray) := hex (computeArtifactHash bytes)
private def fileReference (file : GeneratedFile) := Json.mkObj [("path",.str file.path),("sha256",.str (digest file.bytes))]
private def safePart (s : String) := !s.isEmpty && s != "." && s != ".." && s.toList.all (fun c => c.toNat < 128 && (c.isAlphanum || c == '-' || c == '_' || c == '.'))

private def caseWrapper (c : OpcodeCase) : String :=
  "import " ++ c.sourceModule ++ "\nimport LeanAT.Reference.E39Main\n\ndef main (args : List String) : IO Unit := do\n" ++
  "  let cases ← match " ++ c.sourceModule ++ ".cases with | .ok xs => pure xs | .error e => throw (IO.userError e)\n" ++
  "  let some c := cases.find? (fun c => c.id == " ++ reprStr c.id ++ " && c.variant == " ++ reprStr c.variant ++ ") | throw (IO.userError \"MissingSourceCase\")\n" ++
  "  match args with\n" ++
  "  | [\"pair\",inputPath,modelOutput,execOutput] =>\n" ++
  "    let request ← LeanAT.Reference.JsonIO.readInput inputPath\n" ++
  "    LeanAT.Reference.E39Main.emitPair c.model c.profile c.handlerId c.programId request modelOutput execOutput\n" ++
  "  | [\"model\",inputPath,outputPath] =>\n" ++
  "    let p ← match LeanAT.ModelIR.validateSchema c.model with | .ok p => pure p | .error e => throw (IO.userError e)\n" ++
  "    LeanAT.Reference.E39Main.runModelFile p c.handlerId inputPath outputPath\n" ++
  "  | [\"exec\",inputPath,outputPath] =>\n" ++
  "    let p ← match LeanAT.Compiler.compileForProfile c.model c.profile with | .ok p => pure p | .error e => throw (IO.userError e)\n" ++
  "    LeanAT.Reference.E39Main.runExecFile p c.programId inputPath outputPath\n" ++
  "  | _ => throw (IO.userError \"expected model|exec INPUT OUTPUT\")\n"

def opcodeNodeInventory (compilation : Compilation) : List Json :=
  compilation.nodes.map fun node => Json.mkObj [
    ("path",.str node.path),("kind",.str node.kind),
    ("contentHash",.str (digest node.text.toUTF8)),
    ("locations",.arr ((compilation.origins.filter (fun o => o.node == node.path)).map (fun o =>
      Json.mkObj ([("program",toJson o.program),("kind",.str o.kind)] ++
      o.block.toList.map (fun n => ("block",toJson n)) ++
      o.instruction.toList.map (fun n => ("instruction",toJson n))))).toArray)]

/-- Source factory identity, independent of any supplied catalog or batch metadata. -/
def opcodeModelKey (c : OpcodeCase) : String := c.profile ++ "\n" ++ reprStr c.model

def buildOpcodeSourceInventory (cases : List OpcodeCase) : Except String (Json × List (String × String)) := do
  if cases.isEmpty || (cases.map (fun c => (c.id,c.variant))).eraseDups.length != cases.length then throw "EmptyOrDuplicateOpcodeCase"
  let mut compiled : List (String × Compilation) := []
  let mut maps : List (String × Json) := []
  let mut hashes : List (String × String) := []
  let mut rows := []
  for c in cases do
    let key := opcodeModelKey c
    let compilation ← match compiled.lookup key with
      | some value => pure value
      | none => compileForProfileWithProvenance c.model c.profile
    if !(compiled.any (fun pair => pair.1 == key)) then compiled := (key,compilation)::compiled
    let descriptorHash := digest (serialize compilation.validated)
    if !(hashes.any (fun pair => pair.1 == key)) then hashes := (key,descriptorHash)::hashes
    let mapKey := key ++ "\n" ++ reprStr c.sourceBundle
    let sourceMap ← match maps.lookup mapKey with
      | some value => pure value
      | none => do
        let built ← buildSourceMap compilation c.sourceBundle
        pure built.json
    if !(maps.any (fun pair => pair.1 == mapKey)) then maps := (mapKey,sourceMap)::maps
    rows := rows ++ [Json.mkObj [
      ("id",.str c.id),("variant",.str c.variant),("profile",.str c.profile),
      ("handlerId",toJson c.handlerId),("programId",toJson c.programId),
      ("providerFamily",.str c.providerFamily),("expectedOutcome",.str c.expectedOutcome),
      ("expectedOpcodeTags",toJson c.expectedOpcodeTags),
      ("sourceInputSha256",.str (digest (Reference.JsonIO.inputJson c.input).pretty.toUTF8)),
      ("descriptorSha256",.str descriptorHash),
      ("sourceMapSha256",.str (digest sourceMap.pretty.toUTF8)),
      ("modelSha256",.str (digest (reprStr c.model).toUTF8)),
      ("sourceWrapperSha256",.str (digest (caseWrapper c).toUTF8)),
      ("sourceModule",.str c.sourceModule),("sourceFiles",toJson c.sourceFiles),
      ("modelNodeInventory",.arr (opcodeNodeInventory compilation).toArray)]]
  pure (Json.mkObj [("schema",.str "leanat.opcode-source-inventory.v1"),
    ("canonicalInputEncoding",.str "UTF8(Reference.JsonIO.inputJson(case.input).pretty), no trailing newline"),
    ("cases",.arr rows.toArray)],hashes)
def emitOpcodeCatalog (cases : List OpcodeCase) : IO Artifacts := do
  if cases.isEmpty || (cases.map (fun c => (c.id,c.variant))).eraseDups.length != cases.length then throw (IO.userError "EmptyOrDuplicateOpcodeCase")
  let mut files : List GeneratedFile := []
  let mut rows := []
  for c in cases do
    if !safePart c.id || !safePart c.variant || c.sourceFiles.isEmpty then throw (IO.userError s!"InvalidOpcodeCase: {c.id}/{c.variant}")
    if !(c.sourceModule.splitOn ".").all (fun s => !s.isEmpty && s.toList.all (fun ch => ch.toNat < 128 && (ch.isAlphanum || ch == '_'))) then throw (IO.userError "InvalidSourceModule")
    let compilation ← match compileForProfileWithProvenance c.model c.profile with
      | .ok value => pure value | .error e => throw (IO.userError s!"{c.id}/{c.variant}: {e}")
    let emitted := compilation.validated.project.programs.flatMap (fun p => p.blocks.flatMap (fun b => b.instructions.map (fun i => i.op.tag)))
    if !c.expectedOpcodeTags.all emitted.contains then throw (IO.userError s!"MissingTargetOpcode: {c.id}/{c.variant}")
    let map ← match buildSourceMap compilation c.sourceBundle with
      | .ok value => pure value | .error e => throw (IO.userError e)
    let base := c.id ++ "/" ++ c.variant
    let descriptor := (⟨base ++ "/model.execir.bin",serialize compilation.validated⟩ : GeneratedFile)
    let input := opcodeText (base ++ "/input.json") (Reference.JsonIO.inputJson c.input).pretty
    let sourceMap := opcodeText (base ++ "/source-map.json") map.json.pretty
    let wrapper := opcodeText (base ++ "/Source.lean") (caseWrapper c)
    let model := opcodeText (base ++ "/model-ir.txt") (reprStr c.model)
    files := files ++ [descriptor,input,sourceMap,wrapper,model]
    for (path,bytes) in map.documents do
      if !files.any (fun f => f.path == path) then files := files ++ [⟨path,bytes⟩]
    let mut sources := []
    for original in c.sourceFiles do
      let bytes ← IO.FS.readBinFile original
      let path := s!"sources/{digest bytes}.lean"
      if !files.any (fun f => f.path == path) then files := files ++ [⟨path,bytes⟩]
      sources := sources ++ [Json.mkObj [("originalPath",.str original),("path",.str path),("sha256",.str (digest bytes))]]
    let inventory := opcodeNodeInventory compilation
    rows := rows ++ [Json.mkObj [("id",.str c.id),("variant",.str c.variant),("providerFamily",.str c.providerFamily),
      ("profile",.str c.profile),("handlerId",toJson c.handlerId),("programId",toJson c.programId),
      ("modelNodeInventory",.arr inventory.toArray),("expectedOpcodeTags",toJson c.expectedOpcodeTags),("expectedOutcome",.str c.expectedOutcome),
      ("descriptor",fileReference descriptor),("referenceInput",fileReference input),("sourceMap",fileReference sourceMap),
      ("sourceWrapper",fileReference wrapper),("model",fileReference model),("sources",.arr sources.toArray)]]
  let catalog := Json.mkObj [("schema",.str "leanat.opcode-catalog.v1"),("coverageStatus",.str "requires-observed-execution"),("cases",.arr rows.toArray)]
  pure ⟨files ++ [opcodeText "catalog.json" catalog.pretty]⟩

def writeOpcodeCatalog (cases : List OpcodeCase) (output : System.FilePath) : IO Unit :=
  emitOpcodeCatalog cases >>= fun artifacts => writeArtifacts artifacts output

end LeanAT.Compiler
