import LeanAT.Compiler.Codegen

namespace LeanAT.Compiler
open Lean ExecIR

/-- One actual source model and the native observations its loaded topology/programs drive. -/
structure Scenario where
  name : String
  runner : String
  coreIds : List String
  model : ModelIR.Project
  sourceBundle : SourceMapping.Bundle := {}
  obligations : List String := []
  sourceFiles : List String := []

def compileForProfileWithProvenance (model : ModelIR.Project) (profile : String) : Except String Compilation := do
  let compilation ← compileWithProvenance model
  let validated ← compileForProfile model profile
  pure {compilation with validated}

private def catalogText (path text : String) : GeneratedFile := ⟨path,text.toUTF8⟩
private def hashBytes (bytes : ByteArray) := hex (computeArtifactHash bytes)

private def metadataJson (project : ExecProject) : Json := Json.mkObj [
  ("schemaMajor",toJson project.schemaMajor),
  ("topSystemId",toJson (project.systemMetadata.map (·.id))),
  ("runtimeDomain",toJson (project.systemMetadata.map (·.runtimeDomain))),
  ("instances",.arr (project.instances.map (fun inst => Json.mkObj [
    ("id",toJson inst.id),("definition",toJson inst.definition),("stateBase",toJson inst.stateBase),("stateCount",toJson inst.stateCount),
    ("handlers",.arr (inst.handlers.map (fun h => Json.mkObj [("localId",toJson h.localId),("programId",toJson h.programId),("context",toJson h.context),("trigger",.str h.trigger)])).toArray)])).toArray),
  ("programs",.arr (project.programs.map (fun p => Json.mkObj [("id",toJson p.id),("context",toJson p.context),("inputTypes",toJson p.inputTypes),("resultTypes",toJson p.resultTypes),("effects",toJson p.effectMask)])).toArray),
  ("connections",toJson ((project.systemMetadata.map (fun s => s.bindings.map (·.id.value))).getD [])),
  ("services",.arr (project.services.map (fun s => Json.mkObj [("id",toJson s.id),("opcode",toJson s.op.tag),("provider",.str s.providerKey),("version",.str s.providerVersion),("abiHash",.str (hex s.abiHash))])).toArray)]

/-- Builds source-bound paired artifacts. Catalog files are published by the same owned-stage writer. -/
def emitScenarioCatalog (scenarios : List Scenario) : IO Artifacts := do
  if scenarios.isEmpty || (scenarios.map (·.name)).eraseDups.length != scenarios.length then throw (IO.userError "EmptyOrDuplicateScenario")
  let mut files : List GeneratedFile := []
  let mut rows : List Json := []
  for scenario in scenarios do
    if scenario.name.isEmpty || !scenario.name.toList.all (fun c => c.isAlphanum && c.toNat < 128 || c == '-') then throw (IO.userError "InvalidScenarioName")
    if scenario.runner.isEmpty || scenario.coreIds.isEmpty || scenario.obligations.isEmpty || scenario.sourceFiles.isEmpty then throw (IO.userError "MissingScenarioContract")
    let modelBytes := (reprStr scenario.model).toUTF8
    let modelPath := s!"{scenario.name}/model-ir.txt"
    files := files ++ [⟨modelPath,modelBytes⟩]
    let mut sources := []
    for sourcePath in scenario.sourceFiles do
      let bytes ← IO.FS.readBinFile sourcePath
      let some _ := String.fromUTF8? bytes | throw (IO.userError "ScenarioSourceNotUTF8")
      let path := s!"sources/{hashBytes bytes}.lean"
      if !files.any (fun f => f.path == path) then files := files ++ [⟨path,bytes⟩]
      sources := sources ++ [Json.mkObj [("originalPath",.str sourcePath),("path",.str path),("sha256",.str (hashBytes bytes))]]
    let mut profiles := []
    let mut metadata := Json.null
    for (short,profile) in [("core","AT-Core-1.1-draft"),("ext","AT-Ext-1.1-draft")] do
      let compilation ← match compileForProfileWithProvenance scenario.model profile with
        | .ok c => pure c | .error e => throw (IO.userError s!"{scenario.name}/{short}: {e}")
      if compilation.validated.project.systemMetadata.isNone then throw (IO.userError "ScenarioRequiresActualHierarchy")
      let bytes := serialize compilation.validated
      let map ← match buildSourceMap compilation scenario.sourceBundle with
        | .ok map => pure map | .error e => throw (IO.userError e)
      let path := s!"{scenario.name}/{short}.execir.bin"
      let sourceMapPath := s!"{scenario.name}/{short}.source-map.json"
      -- Source-map paths are relative to the catalog root, like all catalog paths.
      files := files ++ [⟨path,bytes⟩,catalogText sourceMapPath map.json.pretty]
      for (path,bytes) in map.documents do
        if !files.any (fun f => f.path == path) then files := files ++ [⟨path,bytes⟩]
      profiles := profiles ++ [(short,Json.mkObj [("profile",.str profile),("path",.str path),("sha256",.str (hashBytes bytes)),
        ("sourceMap",.str sourceMapPath),("sourceMapSha256",.str (hashBytes map.json.pretty.toUTF8))])]
      metadata := metadataJson compilation.validated.project
    rows := rows ++ [Json.mkObj [("name",.str scenario.name),("runner",.str scenario.runner),("coreIds",toJson scenario.coreIds),
      ("obligations",toJson scenario.obligations),("artifacts",Json.mkObj profiles),("descriptor",metadata),
      ("model",Json.mkObj [("path",.str modelPath),("sha256",.str (hashBytes modelBytes)),("provenance",.str "canonical-ModelIR")]),
      ("sources",.arr sources.toArray),("hostBudget",Json.mkObj [("instructionFuel",toJson scenario.model.profile.instructionFuel),
        ("eventCapacity",toJson scenario.model.profile.eventCapacity),("maxEventsPerTick",toJson scenario.model.profile.maxEventsPerTick)])]]
  let catalog := Json.mkObj [("schema",.str "leanat.scenario-catalog.v1"),("scenarios",.arr rows.toArray)]
  pure ⟨files ++ [catalogText "catalog.json" catalog.pretty]⟩

def writeScenarioCatalog (scenarios : List Scenario) (output : System.FilePath) : IO Unit := do
  writeArtifacts (← emitScenarioCatalog scenarios) output

end LeanAT.Compiler
