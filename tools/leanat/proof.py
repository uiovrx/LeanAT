"""Fresh Lean-kernel theorem lookup; JSON certificate labels never authorize proofs."""
import json
import re
import tempfile
from pathlib import Path
from .io import ToolError,require,digest,canonical,read_json,unique_pairs

ROOT=Path(__file__).resolve().parents[2]
NAME=re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*")


def semantic_sources():
    paths=sorted((ROOT/"LeanAT").rglob("*.lean"))
    if len(paths)>10000:raise ToolError("ReadLimit","proof source count exceeds host limit")
    total=0;result={}
    for path in paths:
        size=path.stat().st_size;total+=size
        if size>8_388_608 or total>67_108_864:raise ToolError("ReadLimit","proof source bytes exceed host limit")
        result[path.relative_to(ROOT).as_posix()]=digest(path.read_bytes())
    return result


def run_proof_audit(module,theorem):
    if not NAME.fullmatch(module) or not NAME.fullmatch(theorem):raise ToolError("InvalidTheorem","module and theorem must be Lean qualified identifiers")
    from .cli import capture,lean_argv
    lake=lean_argv(module,"check")[0]
    _,build=capture([lake,"build",module,"LeanAT.ModelIR.ProofAuditCommand"],parse_json=False)
    if build["exitCode"]:raise ToolError("ProofBuildFailed",json.dumps(build),3)
    before=semantic_sources()
    scratch=ROOT/".tmp/tools";scratch.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w",encoding="utf-8",suffix=".lean",prefix="proof-audit-",dir=scratch,delete=False) as stream:
        path=Path(stream.name)
        audit_source="import "+module+"\nimport LeanAT.ModelIR.ProofAuditCommand\n#leanat_audit "+theorem+"\n"
        stream.write(audit_source)
    try:
        stdout,execution=capture([lake,"env","lean",str(path)],parse_json=False)
    finally:path.unlink(missing_ok=True)
    if execution["exitCode"]:raise ToolError("KernelAuditFailed",json.dumps(execution),3)
    lines=[line.split("LEANAT_PROOF_AUDIT_JSON:",1)[1].strip() for line in stdout.splitlines() if line.startswith("LEANAT_PROOF_AUDIT_JSON:")]
    if len(lines)!=1:raise ToolError("KernelAuditFailed","missing or ambiguous real kernel audit output",3)
    actual=json.loads(lines[0],object_pairs_hook=unique_pairs)
    require(actual,("schema","theorem","theoremType","axioms","usesNativeEvaluation","leanVersion"),"kernel audit")
    if actual["schema"]!="leanat.proof-audit.v1" or actual["theorem"]!=theorem or "sorryAx" in actual["axioms"] or actual["usesNativeEvaluation"]:raise ToolError("UnverifiedProofClaim","theorem uses sorry/native evaluation or mismatched audit schema")
    if semantic_sources()!=before:raise ToolError("ArtifactChanged","Lean sources changed while auditing proof")
    source=ROOT/(module.replace(".","/")+".lean")
    certificate=ROOT/".lake/build/lib/lean"/(module.replace(".","/")+".olean")
    return {"actual":actual,"execution":execution,"build":build,"auditSource":audit_source,"moduleHash":digest(source.read_bytes()),"certificateHash":digest(certificate.read_bytes()),"semanticsHash":digest(canonical(before)),"semanticSources":before}


def validate_proof_claim(claim,artifact_paths,hashes):
    require(claim,("module","theorem","moduleArtifact","certificateArtifact","kernelRecord","leanVersion","semanticsVersion","semanticsHash","axioms"),"proof claim")
    for field in ("moduleArtifact","certificateArtifact","kernelRecord"):
        if claim[field] not in artifact_paths:raise ToolError("MissingArtifact","proof "+field)
    fresh=run_proof_audit(claim["module"],claim["theorem"])
    actual=fresh["actual"]
    if claim["property"]!=actual["theorem"] or claim["scope"]!=actual["theoremType"]:raise ToolError("ProofScopeMismatch","claim must name the audited theorem and its exact proposition; broad relabeling is forbidden")
    if claim["axioms"]!=actual["axioms"] or claim["leanVersion"]!=actual["leanVersion"]:raise ToolError("ProofDependencyMismatch","actual kernel axioms/Lean version differ from claim")
    expected=read_json(artifact_paths[claim["kernelRecord"]])
    if expected!=actual:raise ToolError("KernelRecordMismatch","saved record differs from fresh theorem/axiom audit")
    if claim["semanticsVersion"]!="LeanAT.source-set.v1" or claim["semanticsHash"]!=fresh["semanticsHash"]:raise ToolError("HashMismatch","proof semantic source set differs")
    if hashes[claim["moduleArtifact"]]!=fresh["moduleHash"] or hashes[claim["certificateArtifact"]]!=fresh["certificateHash"]:raise ToolError("HashMismatch","claimed module/certificate does not match actual fresh Lean import")
    return fresh
