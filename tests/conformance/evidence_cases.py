"""Original evidence-policy scenarios; missing open-environment replay stays NotRun.

The system under test here is the implemented Python manifest/evidence validator.
Fixture data is never advertised as C++/SystemC execution or as a proof certificate.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def _manifest(payload: bytes):
    return {
        "schema": "leanat.manifest.v1",
        "profile": "AT-Ext-1.1-draft",
        "topSystemId": 7,
        "runtimeVersion": "evidence-fixture-runtime-v1",
        "providerVersion": "evidence-fixture-provider-v1",
        "abi": "fixture-owned-bytes-v1",
        "toolchainLock": {"fixtureVersion": 1},
        "requires": ["finite-exchange-protocols", "structured-wait",
                     "cancel-scope-drain", "bounded-task-results"],
        "optional_enabled": ["extern-pure"],
        "effectiveConfig": {"topSystemId": 7, "runtime_checks": "full"},
        "artifacts": [{"id": "fixture-input", "path": "input.bin", "sha256": _sha(payload)}],
        "claims": [],
    }


def _expect_error(operation, expected, tool_error):
    try:
        result = operation()
    except tool_error as error:
        return {"expectedError": expected, "actualError": error.code,
                "message": str(error), "matched": error.code == expected}
    except Exception as error:
        return {"expectedError": expected, "actualError": type(error).__name__,
                "message": str(error), "matched": False}
    return {"expectedError": expected, "actualError": None, "returned": result,
            "matched": False}


def _tested_not_proved(directory, validate_manifest, release_gate, tool_error):
    identity = "E-T24"
    payload = b"finite test evidence is not a universal proof"
    (directory / "input.bin").write_bytes(payload)
    manifest = _manifest(payload)
    assumptions = ["external provider obeys its declared input contract",
                   "the finite input set does not cover all possible values"]
    # These are concrete fixture observations, not a fabricated native-backend run.
    samples = [b"", b"\x00", b"\x00\x7f\xff", payload]
    observed = [{"inputHex": sample.hex(), "actualHex": bytes.fromhex(sample.hex()).hex()}
                for sample in samples]
    property_name = "fixture-finite-byte-roundtrip"
    scope = "only the four explicitly listed fixture byte strings"
    covered = {"fixture-input": _sha(payload)}
    test_evidence = {
        "schema": "leanat.fixture-observation.v1", "property": property_name,
        "scope": scope, "coveredArtifacts": covered,
        "status": "pass" if all(row["inputHex"] == row["actualHex"] for row in observed) else "fail",
        "method": "executed Python bytes.fromhex fixture roundtrips",
        "observations": observed, "notClaimed": ["native backend equivalence", "universal proof"],
    }
    evidence_bytes = _json_bytes(test_evidence)
    (directory / "tested.json").write_bytes(evidence_bytes)
    manifest["artifacts"].append({"id": "finite-tests", "path": "tested.json",
                                  "sha256": _sha(evidence_bytes)})
    claim = {"property": property_name, "scope": scope, "assumptions": assumptions,
             "evidence": "tested", "coveredArtifacts": covered,
             "evidenceArtifacts": ["finite-tests"]}
    manifest["claims"] = [claim]
    before = _json_bytes(manifest)
    control = validate_manifest(manifest, directory)
    promoted = copy.deepcopy(manifest)
    promoted["claims"][0]["evidence"] = "proved"
    promotion = _expect_error(lambda: validate_manifest(promoted, directory),
                              "UnverifiedProofClaim", tool_error)
    # An old blanket label must not bypass the explicit evidence enumeration either.
    blanket = copy.deepcopy(manifest)
    blanket["claims"][0]["evidence"] = "verified"
    blanket_result = _expect_error(lambda: validate_manifest(blanket, directory),
                                   "InvalidEvidenceLabel", tool_error)
    unchanged = _json_bytes(manifest) == before and manifest["claims"][0]["assumptions"] == assumptions
    still_tested = manifest["claims"][0]["evidence"] == "tested" and control["proofAudits"] == []
    gate = release_gate(manifest)
    passed = (control["valid"] and promotion["matched"] and blanket_result["matched"]
              and unchanged and still_tested and gate["release_gate"] == "unsatisfied")
    return {
        "id": identity, "backend": "python-evidence",
        "input": {"manifest": manifest, "files": {"input.bin": {"hex": payload.hex()},
                                                     "tested.json": test_evidence}},
        "expected": {"testedControlAccepted": True, "promotionError": "UnverifiedProofClaim",
                     "assumptionsPreserved": assumptions, "proofAudits": [],
                     "releaseGate": "unsatisfied"},
        "actual": {"control": control, "promotion": promotion, "blanketLabel": blanket_result,
                   "originalManifestUnchanged": unchanged, "remainsTested": still_tested,
                   "gate": gate},
        "status": "Pass" if passed else "Fail",
        "stop": "EvidencePolicyAssertionsCompleted" if passed else "EvidencePolicyAssertionFailure",
        "scope": "real manifest-validator grade-promotion rejection; no runtime/FFI conformance inferred",
    }


def _mismatches(directory, validate_manifest, tool_error):
    payload = bytes(range(32))
    path = directory / "input.bin"
    path.write_bytes(payload)
    baseline = _manifest(payload)
    trusted = {key: copy.deepcopy(baseline[key]) for key in
               ("schema", "profile", "runtimeVersion", "providerVersion", "abi", "toolchainLock")}
    control = validate_manifest(baseline, directory, trusted)
    observations = []
    scenarios = [
        ("resolved-top-config", "ConfigurationMismatch", lambda m: m["effectiveConfig"].update(topSystemId=8), False),
        ("manifest-schema", "SchemaMismatch", lambda m: m.update(schema="leanat.manifest.unsupported-v999"), False),
        ("declared-artifact-hash", "HashMismatch", lambda m: m["artifacts"][0].update(sha256="0" * 64), False),
        ("host-abi", "ToolchainMismatch", lambda m: m.update(abi="incompatible-fixture-abi"), True),
        ("host-runtime", "ToolchainMismatch", lambda m: m.update(runtimeVersion="different-runtime"), True),
        ("host-provider", "ToolchainMismatch", lambda m: m.update(providerVersion="different-provider"), True),
        ("host-lock", "ToolchainMismatch", lambda m: m.update(toolchainLock={"fixtureVersion": 2}), True),
    ]
    for name, expected, mutate, use_trusted in scenarios:
        candidate = copy.deepcopy(baseline)
        mutate(candidate)
        before = _json_bytes(candidate)
        observed = _expect_error(
            lambda: validate_manifest(candidate, directory, trusted if use_trusted else None),
            expected, tool_error,
        )
        observed.update(case=name, inputManifest=candidate,
                        inputUnchanged=_json_bytes(candidate) == before)
        observations.append(observed)
    corrupt = bytes([payload[0] ^ 1]) + payload[1:]
    path.write_bytes(corrupt)
    mutated = _expect_error(lambda: validate_manifest(baseline, directory), "HashMismatch", tool_error)
    mutated.update(case="changed-artifact-bytes", actualArtifactHex=corrupt.hex(), inputUnchanged=True)
    observations.append(mutated)
    path.write_bytes(payload)
    restored = validate_manifest(baseline, directory, trusted)
    passed = (control["valid"] and restored["valid"] and
              all(row["matched"] and row["inputUnchanged"] for row in observations))
    return {
        "id": "E-T38", "backend": "python-evidence",
        "input": {"manifest": baseline, "trustedHostPolicy": trusted,
                  "files": {"input.bin": {"hex": payload.hex()}}},
        "expected": {"controlAccepted": True,
                     "mismatchClasses": ["config", "schema", "artifact bytes/hash", "host ABI/runtime/provider/lock"],
                     "silentDowngrade": False},
        "actual": {"control": control, "negativeCases": observations, "restoredControl": restored},
        "status": "Pass" if passed else "Fail",
        "stop": "ManifestLoadRejectionsCompleted" if passed else "ManifestLoadAssertionFailure",
        "scope": "actual manifest loading/configuration compatibility; binary descriptor codec corpus is separate",
    }


def _replay_unavailable(identity, scalar_plan, tool_error):
    probe = {"schema": "leanat.transcript.v1", "profile": "AT-Core-1.1-draft",
             "segment": {"scope": "scalar-segment", "programId": 0, "inputs": []},
             "records": [{"recordKind": "event", "apiKind": "sideband",
                          "observedOrdinal": "0", "domain": 1, "time": "100",
                          "role": "environment", "body": {"value": {"line": "irq", "level": True}}}]}
    rejected = _expect_error(lambda: scalar_plan(probe), "UnsupportedReplayScope", tool_error)
    reason = (
        "Native/SystemC callback capture is not connected to a complete runtime replay adapter. "
        "The available live runner supports closed scalar segments and rejects real environment records. "
        + ("No same-Tick callback permutations have been replayed through the actual runtime; parser order preservation is insufficient."
           if identity == "C-T24" else
           "No open-SystemC call/return/callback/config transcript has driven two actual full semantic runs; scalar equivalence is not substituted.")
    )
    return {"id": identity, "backend": "python-evidence",
            "input": {"capabilityProbe": probe},
            "expected": "actual full boundary capture and runtime replay for the original regression",
            "actual": {"scalarRunnerBoundaryProbe": rejected, "fullReplayExecuted": False},
            "status": "NotRun", "stop": "MissingFullCaptureReplayAdapter", "reason": reason}


def run(root, output_dir, transport_runtime=None, transport_systemc=None):
    """Return one original-regression row per assigned ID and retain full observations."""
    root = Path(root).resolve()
    output_dir = Path(output_dir).resolve()
    evidence_dir = output_dir / "evidence_cases"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from tools.leanat.evidence import validate_manifest, release_gate
    from tools.leanat.cli import scalar_plan
    from tools.leanat.io import ToolError

    rows = []
    with tempfile.TemporaryDirectory(prefix="fixtures-", dir=evidence_dir) as temporary:
        fixture_root = Path(temporary).resolve()
        if not fixture_root.is_relative_to(evidence_dir.resolve()):
            raise RuntimeError("fixture directory escaped its designated output root")
        for identity, scenario in (("E-T24", _tested_not_proved), ("E-T38", _mismatches)):
            directory = fixture_root / identity
            directory.mkdir()
            try:
                row = (scenario(directory, validate_manifest, release_gate, ToolError)
                       if identity == "E-T24" else scenario(directory, validate_manifest, ToolError))
            except Exception as error:
                row = {"id": identity, "backend": "python-evidence", "input": "scenario fixture setup/execution",
                       "expected": "all precise evidence-policy assertions complete",
                       "actual": {"exception": type(error).__name__, "message": str(error)},
                       "status": "Fail", "stop": "UnexpectedScenarioFailure"}
            rows.append(row)
    def executable(explicit, relative):
        if explicit:
            return Path(explicit)
        for suffix in ("", ".exe"):
            candidate = root / "build/integration" / (relative + suffix)
            if candidate.is_file():
                return candidate
        return None
    native = executable(transport_runtime, "leanat_transport_replay")
    kernel = executable(transport_systemc, "systemc/leanat_systemc_transport_replay")
    if native and kernel:
        from tools.leanat.transport_replay import conformance
        try:
            rows.extend(conformance(native, kernel, evidence_dir / "transport"))
        except Exception as error:
            rows.extend({"id": identity, "backend": "runtime-systemc-transport", "input": "actual transport fixtures",
                         "expected": "bounded full capture and replay", "actual": {"error": str(error)},
                         "status": "Fail", "stop": "TransportScenarioFailure"} for identity in ("C-T24", "E-T35"))
    else:
        for identity in ("C-T24", "E-T35"):
            row = _replay_unavailable(identity, scalar_plan, ToolError)
            row["reason"] = "The real bounded transport capture/replay implementation is present, but this build lacks leanat_transport_replay and/or leanat_systemc_transport_replay; no live transport execution was substituted."
            row["stop"] = "MissingTransportReplayExecutable"
            rows.append(row)
    provenance = {"python": sys.version, "scenarioSourceSha256": _sha(Path(__file__).read_bytes()),
                  "validatorSourceSha256": _sha((root / "tools/leanat/evidence.py").read_bytes()),
                  "proofSourceSha256": _sha((root / "tools/leanat/proof.py").read_bytes())}
    for row in rows:
        row["provenance"] = provenance
        path = evidence_dir / (row["id"] + ".json")
        content = _json_bytes(row)
        path.write_bytes(content)
        row["evidence_path"] = str(path)
        row["evidence_sha256"] = _sha(content)
    return rows
