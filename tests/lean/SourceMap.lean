import LeanAT.Compiler.Main

open LeanAT LeanAT.Compiler LeanAT.ExecIR Lean

private def expectError (e : Except String α) : Bool := match e with | .error _ => true | _ => false

def main : IO Unit := do
  let check := fun b label => unless b do throw (IO.userError label)
  let .ok compilation := compileWithProvenance scalarExample | throw (IO.userError "mapped compile")
  let .ok map := buildSourceMap compilation | throw (IO.userError "constructed map")
  check ((map.json.getObjValAs? String "schema").toOption == some "leanat.source-map.v1") "source map schema"
  check ((map.json.getObjValAs? String "artifactHash").toOption == some (hex (computeArtifactHash (serialize compilation.validated)))) "artifact hash"
  let bytes := "-- λ 中\n42\n".toUTF8
  let source : SourceMapping.Bundle := {documents := [{path := "fixture.lean",content := "-- λ 中\n42\n"}],origins := [
    {nodePath := "handler/0",documentPath := "fixture.lean",startByte := 10,endByte := 12,kind := .constructed}],modelNodes := compilation.nodes.map (fun n => (n.path,n.text))}
  let .ok unicode := buildSourceMap compilation source | throw (IO.userError s!"UTF8 boundary {bytes.size}")
  check (!unicode.documents.isEmpty) "source documents packaged"
  let bad := {source with origins := source.origins.map (fun o => {o with startByte := 4})}
  check (expectError (buildSourceMap compilation bad)) "UTF8 mid-scalar rejected"
  let bad := {compilation with origins := compilation.origins.drop 1}
  check (expectError (buildSourceMap bad)) "missing location rejected"
  let bad := {compilation with origins := compilation.origins ++ compilation.origins.take 1}
  check (expectError (buildSourceMap bad)) "duplicate location rejected"
  let bad := {source with origins := source.origins.map (fun o => {o with nodePath := "unknown"})}
  check (expectError (buildSourceMap compilation bad)) "unknown ModelIR node rejected"
  let bad := {source with modelNodes := source.modelNodes.map (fun (path,text) => (path,text ++ "changed"))}
  check (match buildSourceMap compilation bad with | .error "StaleModelBinding" => true | _ => false) "stale binding rejected"
  let selfCycle := {source with origins := source.origins.map (fun o => {o with kind := .generated,ancestry := [o.nodePath]})}
  check (match buildSourceMap compilation selfCycle with | .error "SourceAncestryCycle" => true | _ => false) "self ancestry rejected"
  let cycle := {source with origins := [
    {nodePath := "handler/0",documentPath := "fixture.lean",startByte := 10,endByte := 12,kind := .generated,ancestry := ["handler/0/body/0"]},
    {nodePath := "handler/0/body/0",documentPath := "fixture.lean",startByte := 10,endByte := 12,kind := .generated,ancestry := ["handler/0"]}]}
  check (match buildSourceMap compilation cycle with | .error "SourceAncestryCycle" => true | _ => false) "two-node ancestry rejected"
  let policies : ModelIR.Project := {types := [.unit],components := [{id := ⟨1⟩,processes := [{
    id := 0,params := [],resultType := 0,body := [.ret [.literal 0 .unit]], capacity := {maxInstances := 1,frameBytesLimit := 64,resultCapacity := 1},instructionFuel := 7, ownerPolicy := "custom-owner",resultLifetimePolicy := "custom-lifetime"}]}], systems := [{id := 0,instances := [{id := ⟨1⟩,definition := ⟨1⟩}],bindings := [],runtimeDomain := 1}],topSystemId := some 0}
  match compile policies with
  | .error e => check (e.startsWith "UnsupportedProcessPolicies") "process policies explicitly rejected"
  | .ok _ => throw (IO.userError "process policies silently dropped")
  let .ok artifacts := emitSystemC compilation.validated (some compilation) | throw (IO.userError "mapped artifact")
  if !(← (System.FilePath.mk "build/source-map-tools").pathExists) then writeArtifacts artifacts "build/source-map-tools"
  let testRoot := System.FilePath.mk s!".tmp/lean_compiler/{hex (← IO.getRandomBytes 16)}"
  IO.FS.createDirAll testRoot
  let root ← IO.FS.realPath testRoot
  let payload : Artifacts := ⟨[⟨"sub/file.txt","owned".toUTF8⟩]⟩
  writeArtifacts payload (System.FilePath.mk ((root / "trailing").toString ++ "/"))
  check ((← IO.FS.readFile (root / "trailing/sub/file.txt")) == "owned") "trailing slash output works"
  let locked := root / "locked.leanat-stage"
  IO.FS.createDir locked
  IO.FS.writeFile (locked / "foreign.txt") "other owner"
  let rejected ← try writeArtifacts payload (root / "locked"); pure false catch _ => pure true
  check rejected "exclusive stage claim rejects existing owner"
  check ((← IO.FS.readFile (locked / "foreign.txt")) == "other owner") "failed claimant preserves foreign stage"
  let duplicate : Artifacts := ⟨[⟨"a.txt","one".toUTF8⟩,⟨"A.txt","two".toUTF8⟩]⟩
  let rejected ← try writeArtifacts duplicate (root / "duplicate"); pure false catch _ => pure true
  check rejected "portable case duplicate rejected"
  let conflict : Artifacts := ⟨[⟨"a","one".toUTF8⟩,⟨"a/b","two".toUTF8⟩]⟩
  let rejected ← try writeArtifacts conflict (root / "conflict"); pure false catch _ => pure true
  check rejected "file-directory path collision rejected"
  let aliased : Artifacts := ⟨[⟨"sub/a.txt","one".toUTF8⟩,⟨"SUB/b.txt","two".toUTF8⟩]⟩
  let rejected ← try writeArtifacts aliased (root / "aliased"); pure false catch _ => pure true
  check rejected "case-aliased shared directory rejected"
  check (!(← (root / "aliased.leanat-stage").pathExists)) "directory alias rejected before stage"
  let unicodeAliases : Artifacts := ⟨[⟨"Σ.txt","one".toUTF8⟩,⟨"ς.txt","two".toUTF8⟩]⟩
  let rejected ← try writeArtifacts unicodeAliases (root / "unicode"); pure false catch _ => pure true
  check rejected "artifact path ASCII policy rejects platform Unicode aliases"
  let runWriter := fun (text : String) => do
    try writeArtifacts ⟨[⟨"winner.txt",text.toUTF8⟩]⟩ (root / "concurrent"); pure true catch _ => pure false
  let first ← IO.asTask (runWriter "first")
  let second ← IO.asTask (runWriter "second")
  let a := first.get.toOption.getD false
  let b := second.get.toOption.getD false
  check (a != b) "one concurrent writer wins"
  check ((← IO.FS.readFile (root / "concurrent/winner.txt")) == (if a then "first" else "second")) "concurrent winner untouched"
  let workspaceTemp ← IO.FS.realPath ".tmp/lean_compiler"
  check (root.parent == some workspaceTemp) "test cleanup remains inside compiler scratch"
  IO.FS.removeDirAll root
  try IO.FS.removeDir workspaceTemp catch _ => pure ()
  IO.println "Source maps: complete lowering locations, descriptor/source hashes, UTF8 bounds, explicit constructed provenance, policy rejection passed"
