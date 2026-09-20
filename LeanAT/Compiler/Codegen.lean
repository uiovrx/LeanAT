import LeanAT.Compiler.Load
import LeanAT.Compiler.Facade
import LeanAT.Compiler.SourceMap
import Lean

namespace LeanAT.Compiler
open ExecIR Lean

structure GeneratedFile where
  path : String
  bytes : ByteArray
structure Artifacts where
  files : List GeneratedFile

private def textFile (path content : String) : GeneratedFile := ⟨path,content.toUTF8⟩
private def jsonObject (fields : List (String × Json)) := Json.mkObj fields
private def cppHeader := "#pragma once\n#include <leanat/descriptor.hpp>\n#include <leanat/interpreter.hpp>\n#include <leanat/systemc/adapter.hpp>\n#include <memory>\nnamespace leanat_generated {\nconst leanat::Bytes& descriptor_bytes();\nclass Component : public sc_core::sc_module {\n  leanat::systemc::RuntimeDomain& domain_;\n  std::unique_ptr<leanat::exec::ValidatedProject> descriptor_;\n  std::vector<leanat::VersionedCell> cells_;\npublic:\n  Component(sc_core::sc_module_name, leanat::systemc::RuntimeDomain&);\n  leanat::Expected<leanat::exec::SegmentResult> run(std::uint32_t program, std::uint64_t fuel);\n  std::vector<leanat::Value> state() const;\n};\nclass Top : public sc_core::sc_module {\npublic:\n  leanat::systemc::RuntimeDomain domain;\n  Component component;\n  explicit Top(sc_core::sc_module_name name);\n  ~Top() override { domain.stop(); }\n};\n}\n"
private def cppSource := "#include \"Component.hpp\"\n#include <stdexcept>\nnamespace leanat_generated {\nComponent::Component(sc_core::sc_module_name name, leanat::systemc::RuntimeDomain& domain):sc_module(name),domain_(domain) {\n  auto loaded=leanat::exec::load_descriptor(descriptor_bytes());\n  if(!loaded) throw std::runtime_error(\"generated descriptor validation failed\");\n  descriptor_=std::make_unique<leanat::exec::ValidatedProject>(std::move(loaded.value()));\n  for(const auto& value:descriptor_->get().initial_state) cells_.push_back({value,0,0,false});\n}\nleanat::Expected<leanat::exec::SegmentResult> Component::run(std::uint32_t program,std::uint64_t fuel) {\n  std::vector<leanat::VersionedCell*> state; for(auto& cell:cells_) state.push_back(&cell);\n  leanat::ExecutionContext context; context.domain=domain_.id;\n  leanat::EventTxn txn(leanat::SegmentBudget{},context);\n  leanat::exec::Interpreter interpreter(*descriptor_,state);\n  leanat::exec::FuelCounter counter{fuel};\n  auto result=interpreter.execute_segment(program,context,{},txn,counter);\n  if(!result) return result.error();\n  if(result.value().kind==leanat::exec::SegmentResult::Kind::Failed) return result;\n  auto committed=txn.commit(); if(!committed) return committed.error();\n  return result;\n}\nstd::vector<leanat::Value> Component::state() const {std::vector<leanat::Value> result; for(const auto& cell:cells_) result.push_back(cell.value); return result;}\nTop::Top(sc_core::sc_module_name name):sc_module(name),domain(\"domain\",leanat::DomainId{1}),component(\"component\",domain) {auto started=domain.start();if(!started)throw std::runtime_error(\"domain start failed\");}\n}\n"

def emitSystemC (p : ValidatedProject) (provenance : Option Compilation := none) (sourceBundle : SourceMapping.Bundle := {}) : Except String Artifacts := do
  let hierarchy := p.project.systemMetadata.isSome
  if !hierarchy && !p.project.services.isEmpty then throw "UnsupportedServiceBindings: scalar facade has no provider bindings"
  let facadeFiles ← if hierarchy then emitFacade p else pure []
  let bytes := serialize p
  let compilation := match provenance with | some compilation => compilation | none => directExecCompilation p
  if serialize compilation.validated != bytes then throw "SourceArtifactMismatch"
  let sourceMap ← buildSourceMap compilation sourceBundle
  let desc := "#include \"Component.hpp\"\nnamespace leanat_generated {\nconst leanat::Bytes& descriptor_bytes() {\nstatic const leanat::Bytes bytes = {" ++
    String.intercalate "," (bytes.data.toList.map (fun b => toString b.toNat)) ++ "};\nreturn bytes;\n}\n}\n"
  let cmake := "cmake_minimum_required(VERSION 3.20)\nproject(LeanATGenerated LANGUAGES CXX)\nfind_package(LeanAT 0.1 CONFIG REQUIRED)\nif(NOT TARGET LeanAT::SystemC)\n  message(FATAL_ERROR \"LeanAT must be installed with SystemC support\")\nendif()\nadd_library(leanat_generated src/Component.cpp src/Component_desc.cpp" ++ (if hierarchy then " src/Top.cpp" else "") ++ ")\ntarget_include_directories(leanat_generated PUBLIC include)\ntarget_link_libraries(leanat_generated PUBLIC LeanAT::Runtime LeanAT::SystemC LeanAT::Stdlib)\ntarget_compile_features(leanat_generated PUBLIC cxx_std_17)\nadd_executable(leanat_generated_smoke src/main.cpp)\ntarget_link_libraries(leanat_generated_smoke PRIVATE leanat_generated)\n"
  let first := p.project.programs.head?.map Program.id
  let runLine := if hierarchy then "" else match first with
    | none => ""
    | some id => s!"auto result=top.component.run({id},10000); if(!result) return 2; if(result.value().kind==leanat::exec::SegmentResult::Kind::Failed)return 3;"
  let main := "#include \"Component.hpp\"\nint sc_main(int,char**) { leanat_generated::Top top(\"top\"); " ++ runLine ++ " sc_core::sc_start(sc_core::SC_ZERO_TIME); return 0; }\n"
  let generated := if hierarchy then facadeFiles.map (fun (path,text) => textFile path text) else [textFile "include/Component.hpp" cppHeader,textFile "src/Component.cpp" cppSource]
  let schemaName := if p.project.schemaMajor == 1 then "LeanAT.ExecIR.v1.core16" else s!"LeanAT.ExecIR.v{p.project.schemaMajor}"
  let files := generated ++ [⟨"model.execir.bin",bytes⟩,textFile "src/Component_desc.cpp" desc,
    textFile "src/main.cpp" main,textFile "CMakeLists.txt" cmake,
    textFile "source-map.json" sourceMap.json.pretty,
    textFile "opcode-schema.json" (jsonObject [("schema",.str schemaName),("opcodes",toJson opcodeNames)]).pretty] ++
    sourceMap.documents.map (fun (path,bytes) => ⟨path,bytes⟩)
  let artifactRows := files.map (fun f => jsonObject [("id",.str f.path),("path",.str f.path),("sha256",.str (hex (computeArtifactHash f.bytes)))])
  let manifest := jsonObject [("schema",.str "leanat.manifest.v1"),("profile",.str p.project.profile),
    ("topSystemId",toJson ((p.project.systemMetadata.map ModelIR.SystemIR.id).getD 0)),
    ("optional_enabled",toJson (p.project.capabilities.filter (["raw-dmi","managed-access","extern-pure","vendor-protocol-adapter"].contains ·))),
    ("descriptorCapabilities",toJson p.project.capabilities),
    ("effectiveConfig",jsonObject [("topSystemId",toJson ((p.project.systemMetadata.map ModelIR.SystemIR.id).getD 0)),("executionSource",.str "embedded-canonical-binary"),
      ("scope",.str (if hierarchy then "selected-system-package" else "scalar-program-package")),("domainCount",toJson (1 : Nat))]),
    ("artifacts",.arr artifactRows.toArray),("claims",.arr #[]),
    ("limitations",toJson (["Release acceptance has not been established", "Constructed origins are explicitly distinguished from native source", "Native/provider dependencies require verified HostBindings"] : List String))]
  pure ⟨files ++ [textFile "manifest.json" manifest.pretty]⟩

private def portablePart (part : String) : Bool :=
  !part.isEmpty && part != "." && part != ".." &&
  !part.endsWith "." && !part.endsWith " " &&
  !(part.toList.any fun c => c.toNat < 32 || "\\:<>\"|?*".contains c) &&
  !(["con","prn","aux","nul","com1","com2","com3","com4","com5","com6","com7","com8","com9","lpt1","lpt2","lpt3","lpt4","lpt5","lpt6","lpt7","lpt8","lpt9"].contains ((part.splitOn ".").head!.toLower))

/-- A new output directory is published only after every file has been written.
The sibling stage is claimed with one exclusive mkdir; failed claimants never clean it. -/
def writeArtifacts (artifacts : Artifacts) (outputDirectory : System.FilePath) : IO Unit := do
  if artifacts.files.length > 65536 || artifacts.files.foldl (fun n f => n + f.bytes.size) 0 > 268435456 then
    throw (IO.userError "ArtifactBudget")
  let mut fileKeys : List String := []
  let mut directorySpellings : List (String × String) := []
  for file in artifacts.files do
    -- Generated artifact names are portable ASCII; original Unicode source names
    -- remain losslessly recorded inside the source map rather than as filenames.
    if file.path.toList.any (fun c => c.toNat ≥ 128) || file.path.utf8ByteSize > 4096 || (file.path.splitOn "/").length > 64 || !(file.path.splitOn "/").all portablePart then throw (IO.userError "InvalidArtifactPath")
    let key := file.path.toLower
    if fileKeys.any (fun old => old == key || old.startsWith (key ++ "/") || key.startsWith (old ++ "/")) then
      throw (IO.userError "DuplicateOrConflictingArtifactPath")
    fileKeys := key :: fileKeys
    let mut spelling := ""
    for part in (file.path.splitOn "/").dropLast do
      spelling := if spelling.isEmpty then part else spelling ++ "/" ++ part
      if let some previous := directorySpellings.lookup spelling.toLower then
        if previous != spelling then throw (IO.userError "CaseAliasedArtifactDirectory")
      else directorySpellings := (spelling.toLower,spelling) :: directorySpellings
  -- FilePath.normalize alone deliberately retains trailing separators in Lean.
  let normalized := outputDirectory.normalize.toString
  let trimmed := String.ofList (normalized.toList.reverse.dropWhile (System.FilePath.pathSeparators.contains ·)).reverse
  let output := System.FilePath.mk trimmed
  let some name := output.fileName | throw (IO.userError "InvalidOutputDirectory")
  if !portablePart name then throw (IO.userError "InvalidOutputDirectory")
  let parent := output.parent.getD "."
  IO.FS.createDirAll parent
  let parent ← IO.FS.realPath parent
  let output := parent / name
  if ← output.pathExists then throw (IO.userError "OutputExists: use a new output directory")
  let stage := parent / (name ++ ".leanat-stage")
  -- createDir reports EEXIST; unlike createDirAll this is an atomic ownership claim.
  IO.FS.createDir stage
  let mut ownedFiles : List System.FilePath := []
  let mut ownedDirs : List System.FilePath := [stage]
  try
    if ← output.pathExists then throw (IO.userError "OutputExists: concurrent publication")
    for file in artifacts.files do
      let parts := file.path.splitOn "/"
      let mut directory := stage
      for part in parts.dropLast do
        directory := directory / part
        if !ownedDirs.contains directory then
          IO.FS.createDir directory
          ownedDirs := directory :: ownedDirs
      let path := stage / file.path
      -- Track before write so a partial write is also owned and removable.
      ownedFiles := path :: ownedFiles
      IO.FS.writeBinFile path file.bytes
    if ← output.pathExists then throw (IO.userError "OutputExists: concurrent publication")
    IO.FS.rename stage output
  catch e =>
    -- Bounded cleanup touches only paths created by this owner. Unexpected files
    -- keep a directory nonempty; they are never recursively removed.
    for path in ownedFiles do
      try IO.FS.removeFile path catch _ => pure ()
    for path in ownedDirs do
      try IO.FS.removeDir path catch _ => pure ()
    throw e

end LeanAT.Compiler
