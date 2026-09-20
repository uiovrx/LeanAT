import LeanAT.Reference.Json
import LeanAT.Reference.Runner
import LeanAT.Compiler.Load

namespace LeanAT.Reference.E39Main
open JsonIO

private def checked (result : Except String α) : IO α :=
  match result with | .ok value => pure value | .error message => throw (IO.userError message)

/-- The source factory supplies its actual checked AST, independent of the descriptor runner. -/
def runModelFile (project : ModelIR.ValidatedProject) (handlerId : Nat)
    (inputPath outputPath : System.FilePath) : IO Unit := do
  let input ← readInput inputPath
  writeOutcome outputPath (runModel project handlerId input.inputs input.committed input.world input.context input.fuel)

def runExecFile (project : ExecIR.ValidatedProject) (programId : Nat)
    (inputPath outputPath : System.FilePath) : IO Unit := do
  let input ← readInput inputPath
  writeOutcome outputPath (runExec project programId input.inputs input.committed input.world input.context input.fuel)

/-- Evaluate the actual source and its independently interpreted lowered CFG.
    The two outputs are kept separate for the external differential checker. -/
def runPair (source : ModelIR.Project) (profile : String) (handlerId programId : Nat)
    (input : Input) : IO (Outcome × Outcome) := do
  let source ← checked (ModelIR.validateSchema source)
  let compiled ← checked (Compiler.compileForProfile source.project profile)
  let sourceOutcome := runModel source handlerId input.inputs input.committed input.world input.context input.fuel
  let execOutcome := runExec compiled programId input.inputs input.committed input.world input.context input.fuel
  pure (sourceOutcome,execOutcome)

def emitPair (source : ModelIR.Project) (profile : String) (handlerId programId : Nat)
    (input : Input) (modelOutput execOutput : System.FilePath) : IO Unit := do
  let (sourceOutcome,execOutcome) ← runPair source profile handlerId programId input
  writeOutcome modelOutput sourceOutcome
  writeOutcome execOutput execOutcome

/-- Load the exact independently hashed artifact; wrong-profile admission is checked on the same bytes. -/
def runDescriptorFile (descriptor : System.FilePath) (profile digest : String) (programId : Nat)
    (inputPath outputPath : System.FilePath) : IO Unit := do
  if profile != "AT-Core-1.1-draft" && profile != "AT-Ext-1.1-draft" then throw (IO.userError "UnknownProfile")
  let handle ← IO.FS.Handle.mk descriptor .read
  let limit := ({} : Compiler.LoadPolicy).maxFileBytes
  let mut bytes := ByteArray.empty
  while bytes.size ≤ limit do
    let chunk ← handle.read (USize.ofNat (limit+1-bytes.size))
    if chunk.isEmpty then break
    bytes := bytes ++ chunk
  if bytes.size > limit then throw (IO.userError "DescriptorFileBound")
  if Compiler.hex (Compiler.computeArtifactHash bytes) != digest then throw (IO.userError "DescriptorDigestMismatch")
  let policy : Compiler.LoadPolicy := {
    expectedProfile := profile
    allowedCapabilities := ["raw-dmi","managed-access","extern-pure","vendor-protocol-adapter"] }
  let project ← checked (Compiler.deserialize bytes policy)
  let other := if profile == "AT-Core-1.1-draft" then "AT-Ext-1.1-draft" else "AT-Core-1.1-draft"
  match Compiler.deserialize bytes {policy with expectedProfile := other} with
  | .ok _ => throw (IO.userError "WrongProfileAccepted")
  | .error "ProfileMismatch" => pure ()
  | .error other => throw (IO.userError ("WrongProfileUnexpectedError: " ++ other))
  runExecFile project programId inputPath outputPath

end LeanAT.Reference.E39Main
