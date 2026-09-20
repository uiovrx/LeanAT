import LeanAT.Compiler.OpcodeCases.All
import LeanAT.Compiler.OpcodeCatalog
import LeanAT.Reference.E39Main
open Lean

private def checked (result : Except String α) : IO α :=
  match result with | .ok value => pure value | .error e => throw (IO.userError e)

private def field (row : Json) (name : String) : IO String :=
  checked (do (← row.getObjVal? name).getStr?)

private def outputKey (name : String) : IO String := do
  let path := (System.FilePath.mk name).normalize
  let some leaf := path.fileName | throw (IO.userError "BatchOutputFilename")
  if leaf == "." || leaf == ".." || !leaf.toList.all (fun c => c.toNat < 128 && (c.isAlphanum || c == '.' || c == '-' || c == '_')) then
    throw (IO.userError "BatchOutputFilename")
  let parent ← IO.FS.realPath (path.parent.getD ".")
  pure ((parent / leaf).toString.toLower)

private structure BatchEntry where
  id : String
  variant : String
  model : LeanAT.ModelIR.ValidatedProject
  handlerId : Nat
  programId : Nat
  selectedProfile : String
  inputPath : String
  modelOutput : String
  execOutput : String
  descriptor : String
  digest : String

private def diagnostic (entry : BatchEntry) (phase message : String) : IO Unit :=
  IO.eprintln (Json.mkObj [
    ("schema",.str "leanat.opcode-batch-error.v1"),
    ("id",.str entry.id),("variant",.str entry.variant),
    ("phase",.str phase),("message",.str message)]).compress

def main (args : List String) : IO Unit := do
  let [requestPath] := args | throw (IO.userError "expected batch request JSON path")
  let handle ← IO.FS.Handle.mk requestPath .read
  let limit := 16777216
  let mut bytes := ByteArray.empty
  while bytes.size ≤ limit do
    let chunk ← handle.read (USize.ofNat (limit+1-bytes.size))
    if chunk.isEmpty then break
    bytes := bytes ++ chunk
  if bytes.size > 16777216 then throw (IO.userError "BatchFileLimit")
  let some text := String.fromUTF8? bytes | throw (IO.userError "BatchUTF8")
  let request ← checked (Json.parse text)
  let schema ← field request "schema"
  if schema != "leanat.opcode-batch.v1" then throw (IO.userError "BatchSchema")
  let rows ← checked (do (← request.getObjVal? "cases").getArr?)
  if rows.size > 65536 then throw (IO.userError "BatchCaseLimit")
  let cases ← checked LeanAT.Compiler.OpcodeCases.All.cases
  let inventoryOutput ← match request.getObjVal? "inventoryOutput" with
    | .ok value => some <$> checked value.getStr?
    | .error _ => pure none
  let mut identities : List (String × String) := []
  let mut outputs : List String := []
  let mut inputs : List String := [(← IO.FS.realPath requestPath).toString.toLower]
  for path in inventoryOutput.toList do
    outputs := [← outputKey path]
  for row in rows do
    let identity := (← field row "id",← field row "variant")
    if identities.contains identity then throw (IO.userError "DuplicateBatchCase")
    identities := identity::identities
    for key in ["input","descriptor"] do
      let name ← field row key
      let path ← try
        pure (← IO.FS.realPath name).toString.toLower
      catch _ => outputKey name
      inputs := path::inputs
    for key in ["modelOutput","execOutput"] do
      let path ← outputKey (← field row key)
      if outputs.contains path then throw (IO.userError "DuplicateBatchOutput")
      outputs := path::outputs
  if outputs.any inputs.contains then throw (IO.userError "BatchOutputOverwritesInput")
  let mut sourceDigests : List (String × String) := []
  let mut authoritativeInventory : Option Json := none
  if inventoryOutput.isSome then
    let (inventory,hashes) ← checked (LeanAT.Compiler.buildOpcodeSourceInventory cases)
    authoritativeInventory := some inventory
    sourceDigests := hashes
  let mut admitted : List BatchEntry := []
  for row in rows do
    let id ← field row "id"
    let variant ← field row "variant"
    let some source := cases.find? (fun c => c.id == id && c.variant == variant)
      | throw (IO.userError ("UnknownSourceCase: " ++ id ++ "/" ++ variant))
    let selectedProfile ← field row "profile"
    let program ← checked (do (← row.getObjVal? "programId").getNat?)
    if source.profile != selectedProfile || source.programId != program then throw (IO.userError "BatchCaseIdentity")
    let inputPath ← field row "input"
    let modelOutput ← field row "modelOutput"
    let execOutput ← field row "execOutput"
    let descriptor ← field row "descriptor"
    let digest ← field row "sha256"
    let validated ← checked (LeanAT.ModelIR.validateSchema source.model)
    let sourceKey := LeanAT.Compiler.opcodeModelKey source
    let expected ← match sourceDigests.lookup sourceKey with
      | some hash => pure hash
      | none => do
        let compiled ← checked (LeanAT.Compiler.compileForProfile source.model selectedProfile)
        let hash := LeanAT.Compiler.hex (LeanAT.Compiler.computeArtifactHash (LeanAT.Compiler.serialize compiled))
        pure hash
    if expected != digest then throw (IO.userError ("SourceDescriptorMismatch: " ++ id ++ "/" ++ variant))
    if !(sourceDigests.any (fun pair => pair.1 == sourceKey)) then sourceDigests := (sourceKey,expected)::sourceDigests
    admitted := admitted ++ [{
      id := id
      variant := variant
      model := validated
      handlerId := source.handlerId
      programId := program
      selectedProfile := selectedProfile
      inputPath := inputPath
      modelOutput := modelOutput
      execOutput := execOutput
      descriptor := descriptor
      digest := digest
    }]
  match inventoryOutput,authoritativeInventory with
  | some path,some inventory => IO.FS.writeFile path inventory.pretty
  | none,none => pure ()
  | _,_ => throw (IO.userError "BatchInventoryState")
  let mut failed := 0
  for entry in admitted do
    let mut caseFailed := false
    try
      LeanAT.Reference.E39Main.runModelFile entry.model entry.handlerId entry.inputPath entry.modelOutput
    catch error =>
      caseFailed := true
      diagnostic entry "model" error.toString
    try
      LeanAT.Reference.E39Main.runDescriptorFile entry.descriptor entry.selectedProfile entry.digest entry.programId entry.inputPath entry.execOutput
    catch error =>
      caseFailed := true
      diagnostic entry "exec" error.toString
    if caseFailed then
      failed := failed+1
    else
      IO.println ("Evaluated " ++ entry.id ++ "/" ++ entry.variant)
  IO.println (Json.mkObj [
    ("schema",.str "leanat.opcode-batch-summary.v1"),
    ("attempted",toJson admitted.length),("succeeded",toJson (admitted.length-failed)),
    ("failed",toJson failed)]).compress
  if failed > 0 then throw (IO.userError s!"BatchCaseFailures: {failed}")