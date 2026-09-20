from pathlib import Path
from .io import ToolError, digest, read_json, require

OPTIONAL = {"raw-dmi", "managed-access", "extern-pure", "vendor-protocol-adapter"}
EXT_REQUIRED = {"finite-exchange-protocols", "structured-wait", "cancel-scope-drain", "bounded-task-results"}
ALIASES = {"pure-ffi":"extern-pure", "protocol-adapter":"vendor-protocol-adapter"}
BASE_GATES = ("dsl-context-schema-positive-negative", "real-examples", "base-matrix-interop", "lifecycle", "capacity-backpressure", "locked-ci", "evidence-levels", "independent-testbench", "three-layer-full-trace", "auditable-assumptions", "standards-review")


def normalize_capabilities(values):
    normalized, migrations = set(), []
    for item in values:
        name = ALIASES.get(item, item)
        if name not in OPTIONAL: raise ToolError("UnsupportedCapability", str(item))
        normalized.add(name)
        if name != item: migrations.append({"from":item, "to":name})
    return sorted(normalized), migrations


def validate_manifest(manifest, root, trusted=None, max_entries=4096, max_artifact_bytes=67_108_864):
    require(manifest, ("schema", "profile", "topSystemId", "optional_enabled", "artifacts", "effectiveConfig", "claims"), "manifest")
    if manifest["schema"] != "leanat.manifest.v1": raise ToolError("SchemaMismatch", "manifest schema")
    if manifest["profile"] not in {"AT-Core-1.1-draft", "AT-Ext-1.1-draft"}: raise ToolError("SchemaMismatch", "profile")
    enabled, migrations = normalize_capabilities(manifest["optional_enabled"])
    if manifest["profile"].startswith("AT-Core") and enabled: raise ToolError("UnsupportedCapability", "Core cannot enable Ext capabilities")
    if manifest["effectiveConfig"].get("topSystemId") != manifest["topSystemId"]: raise ToolError("ConfigurationMismatch", "top selection mismatch")
    if trusted is not None:
        if not isinstance(trusted,dict) or not {"schema","profile","runtimeVersion","providerVersion","abi","toolchainLock"}.issubset(trusted) or any(value is None or value=="" or value=={} for value in trusted.values()):raise ToolError("HostPolicyIncomplete","trusted policy must explicitly pin schema, profile, runtime, provider, ABI and toolchain")
        for key, expected in trusted.items():
            if manifest.get(key) != expected: raise ToolError("ToolchainMismatch", key)
    artifacts = manifest["artifacts"]
    if not isinstance(artifacts, list) or len(artifacts)>max_entries: raise ToolError("ReadLimit", "artifact count")
    root = Path(root).resolve()
    hashes, paths, artifact_paths = {}, set(), {}
    for artifact in artifacts:
        require(artifact, ("id", "path", "sha256"), "artifact")
        path = (root / artifact["path"]).resolve()
        if not path.is_relative_to(root) or path in paths or artifact["id"] in hashes: raise ToolError("InvalidArtifact", "duplicate or escaping artifact path")
        paths.add(path)
        if not path.is_file(): raise ToolError("MissingArtifact", artifact["path"])
        with path.open("rb") as stream: data=stream.read(max_artifact_bytes+1)
        if len(data)>max_artifact_bytes: raise ToolError("ReadLimit", "artifact bytes")
        if digest(data)!=artifact["sha256"]: raise ToolError("HashMismatch", artifact["path"])
        hashes[artifact["id"]] = artifact["sha256"]
        artifact_paths[artifact["id"]]=path
    proof_audits=[]
    for claim in manifest["claims"]:
        require(claim, ("property", "scope", "assumptions", "evidence", "coveredArtifacts"), "claim")
        if not isinstance(claim["evidence"],str) or claim["evidence"] not in {"planned","proved","checked","tested","runtime-monitored","assumed","out-of-scope"}:raise ToolError("InvalidEvidenceLabel","unsupported evidence class")
        if not claim["property"] or not claim["scope"] or not isinstance(claim["assumptions"],list) or not isinstance(claim["coveredArtifacts"],dict):raise ToolError("InvalidClaim","property, scope, assumptions and covered artifacts must be explicit")
        for identity, hash_value in claim["coveredArtifacts"].items():
            if hashes.get(identity)!=hash_value: raise ToolError("HashMismatch", "stale claim artifact " + identity)
        if claim["evidence"] == "proved":
            from .proof import validate_proof_claim
            try:proof_audits.append(validate_proof_claim(claim,artifact_paths,hashes))
            except ToolError as exc:
                if exc.code=="SchemaMismatch":raise ToolError("UnverifiedProofClaim","proved claim lacks fields needed for a real kernel audit") from exc
                raise
        if claim["evidence"] in {"checked","tested","runtime-monitored"}:
            if not claim.get("evidenceArtifacts") or not claim["coveredArtifacts"]:raise ToolError("MissingEvidence","claim requires concrete artifact-bound observations")
            for reference in claim["evidenceArtifacts"]:
                if reference not in artifact_paths:raise ToolError("MissingArtifact","claim evidence")
                report=read_json(artifact_paths[reference])
                if report.get("property")!=claim["property"] or report.get("scope")!=claim["scope"] or report.get("coveredArtifacts")!=claim["coveredArtifacts"] or report.get("status")!="pass":raise ToolError("ClaimScopeMismatch","actual evidence does not match claimed property/scope/artifacts")
    for test in manifest.get("tests",[]):
        if test.get("status")!="pass":continue
        reference=test.get("reportArtifact")
        if reference not in artifact_paths:raise ToolError("MissingArtifact","test report "+str(reference))
        report=read_json(artifact_paths[reference])
        matches=[row for row in report.get("tests",[]) if row.get("id")==test.get("id")]
        if len(matches)!=1:raise ToolError("InvalidTestEvidence","missing/duplicate report test identity")
        row=matches[0]
        if row.get("status")!="pass" or not row.get("expectedJSON") or row.get("actual")!=row.get("expectedJSON") or row.get("execution",{}).get("exitCode")!=row.get("expectedExit") or not row.get("execution",{}).get("command"):
            raise ToolError("InvalidTestEvidence","test lacks matching actual values and successful precise assertion")
    for gate,row in manifest.get("gates",{}).items():
        if row.get("status")!="pass":continue
        if gate=="standards-review":raise ToolError("UnverifiedGateEvidence","formal standards review has no trusted audit adapter here; implementation-test JSON cannot satisfy it")
        if not row.get("scope") or not row.get("evidenceArtifacts"):raise ToolError("InvalidGateEvidence","gate scope/evidence missing")
        for reference in row["evidenceArtifacts"]:
            if reference not in artifact_paths:raise ToolError("MissingArtifact","gate report")
            report=read_json(artifact_paths[reference])
            if report.get("schema")!="leanat.gate-report.v1" or report.get("gate")!=gate or report.get("scope")!=row["scope"] or report.get("profile")!=manifest["profile"] or report.get("status")!="pass" or not report.get("coveredArtifacts") or not report.get("evidence"):
                raise ToolError("InvalidGateEvidence","gate report content/scope/profile mismatch")
            if any(hashes.get(identity)!=value for identity,value in report["coveredArtifacts"].items()):raise ToolError("HashMismatch","gate report covered artifacts")
            for item in report["evidence"]:
                require(item,("artifactId","scope","method"),"gate evidence")
                if item["artifactId"] not in artifact_paths or item["scope"]!=row["scope"]:raise ToolError("InvalidGateEvidence","gate evidence source/scope mismatch")
                if item["method"]!="implementation-test":raise ToolError("UnverifiedGateEvidence","manual/standards/proof review requires an external trusted audit; JSON labels are insufficient")
                evidence=read_json(artifact_paths[item["artifactId"]])
                if evidence.get("schema")!="leanat.suite-report.v1" or evidence.get("status")!="pass" or not evidence.get("tests"):raise ToolError("InvalidGateEvidence","expected actual implementation suite report")
    return {"valid":True, "hashes":hashes, "capabilities":enabled, "migrations":migrations, "proofAudits":proof_audits,"compatibility":"checked" if trusted else "not-checked: trusted host policy not supplied"}


def release_gate(manifest, artifact_validation=False, host_compatibility=False):
    enabled, migrations = normalize_capabilities(manifest.get("optional_enabled", []))
    enabled = set(enabled)
    ext = manifest.get("profile") == "AT-Ext-1.1-draft"
    required = [f"C-T{i:02}" for i in range(1,31)]
    groups=[]
    if ext:
        required += [f"E-T{i:02}" for i in [*range(1,21), *range(35,41)]]
        for label, trigger, ids in (("native-adapter", {"extern-pure","vendor-protocol-adapter"},range(21,26)), ("raw-managed", {"raw-dmi","managed-access"},range(26,35))):
            active=bool(enabled & trigger)
            groups.append({"group":label, "applicability":"required" if active else "N/A", "reason":"capability enabled" if active else "entire optional group disabled"})
            if active: required += [f"E-T{i:02}" for i in ids]
    tests=manifest.get("tests", [])
    indexed={}
    for test in tests:
        if test.get("id") in indexed: raise ToolError("DuplicateTest", test["id"])
        indexed[test.get("id")]=test
    blockers=[]
    if not artifact_validation:blockers.append("artifact-validation-required")
    if not host_compatibility:blockers.append("trusted-host-compatibility-required")
    for identity in required:
        row=indexed.get(identity,{})
        if row.get("status")!="pass" or row.get("evidenceKind")!="implementation-test" or not row.get("reportArtifact") or row.get("applicability","required")!="required": blockers.append(identity)
    for gate in BASE_GATES:
        row=manifest.get("gates",{}).get(gate,{})
        if row.get("status")!="pass" or not row.get("evidenceArtifacts"): blockers.append(gate)
    if ext and not EXT_REQUIRED.issubset(manifest.get("requires",[])): blockers.append("Ext-required-capabilities")
    basis=manifest.get("conformanceBasis",{})
    if not basis.get("edition") or not basis.get("clauseMappings") or any(row.get("status")!="checked" for row in basis.get("clauseMappings",[])): blockers.append("standards-conformance-basis")
    if not manifest.get("toolchainLock"): blockers.append("toolchain-lock")
    if manifest.get("strictEvidence") and any(row.get("unsafe") for row in manifest.get("components",[])): blockers.append("unsafe-opaque-component")
    # A declared result is not enough: every gate/report must reference verified artifact entries.
    artifact_ids={row.get("id") for row in manifest.get("artifacts",[])}
    for identity,row in indexed.items():
        if row.get("status")=="pass" and row.get("reportArtifact") not in artifact_ids: blockers.append(identity+":missing-report-artifact")
    for gate,row in manifest.get("gates",{}).items():
        if any(ref not in artifact_ids for ref in row.get("evidenceArtifacts",[])): blockers.append(gate+":missing-evidence-artifact")
    return {"release_gate":"unsatisfied" if blockers else "satisfied", "requiredTests":required, "optionalGroups":groups, "blockers":blockers, "migrations":migrations}
