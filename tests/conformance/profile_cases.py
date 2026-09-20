"""Real profile compilation/load/VM differential, with explicit Core corpus gaps."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import importlib.util

PROFILES = {"core": "AT-Core-1.1-draft", "ext": "AT-Ext-1.1-draft"}
CASES = ("lazy-state", "trace-state", "rollback")
FACADE_CASES = ("scalar", "wire", "signal", "process")


def facade_pairs(root, result_dir):
    """Consume source-bound actual run evidence, never infer runs from old JSON."""
    result_dir = Path(result_dir)
    manifest_path = result_dir / "runs.json"
    if not manifest_path.is_file():
        return {"status": "NotRun", "reason": "Missing source-bound eight-run facade manifest: " + str(manifest_path), "cases": []}
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != "leanat.facade-profile-runs.v1":
        raise RuntimeError("Unsupported facade run manifest schema")

    def check_hash(path, expected):
        if not isinstance(expected, str) or len(expected) != 64 or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise RuntimeError("Facade evidence hash mismatch: " + str(path))

    source_hashes = manifest.get("sourceSha256", {})
    required_sources = {"tests/lean/Facade.lean", "tests/lean/facade_observations.hpp", "scripts/test-facade.sh",
                        "LeanAT/Compiler/Facade.lean", "LeanAT/Compiler/Lowering.lean"}
    if not required_sources.issubset(source_hashes):
        raise RuntimeError("Facade evidence lacks required compiler/fixture provenance")
    for path, expected in source_hashes.items():
        resolved = (root / path).resolve()
        if not resolved.is_relative_to(root.resolve()):
            raise RuntimeError("Facade source evidence path outside workspace")
        check_hash(resolved, expected)
    lock = manifest["toolchainLock"]
    expected_lock = (".deps/linux-toolchain-lock.json" if (root / ".deps/linux-toolchain-lock.json").is_file() else "docs/toolchain-linux-lock.json") if sys.platform.startswith("linux") else "docs/toolchain-lock.json"
    if lock["path"] != expected_lock:
        raise RuntimeError("Facade evidence belongs to another platform lock")
    check_hash(root / lock["path"], lock["sha256"])
    runs = manifest.get("runs", [])
    identities = [(r.get("profile"), r.get("scenario")) for r in runs]
    if len(identities) != 8 or set(identities) != {(p, s) for p in PROFILES for s in FACADE_CASES}:
        raise RuntimeError("Facade evidence requires exactly eight distinct actual runs")
    samples = {}
    for run in runs:
        profile, scenario = run["profile"], run["scenario"]
        if run.get("exitCode") != 0 or not run.get("command") or len(run.get("binarySha256", "")) != 64:
            raise RuntimeError("Facade execution failed or lacks executable provenance")
        if run.get("generateExitCode") != 0 or run.get("compileExitCode") != 0 or not run.get("generateCommand") or not run.get("compileCommand"):
            raise RuntimeError("Facade compilation/generation did not actually succeed")
        if not run.get("generatedSourceSha256") or not run.get("linkedInputSha256"):
            raise RuntimeError("Facade lacks generated source or linked library provenance")
        for path, expected in run["generatedSourceSha256"].items():
            resolved = (result_dir / path).resolve()
            if not resolved.is_relative_to(result_dir.resolve()):
                raise RuntimeError("Generated facade source outside evidence directory")
            check_hash(resolved, expected)
        for path, expected in run["linkedInputSha256"].items():
            check_hash(Path(path), expected)
        result_path = result_dir / f"{profile}-{scenario}.json"
        descriptor_path = result_dir / f"{profile}-{scenario}.execir.bin"
        check_hash(result_path, run["resultSha256"])
        check_hash(descriptor_path, run["descriptorSha256"])
        payload = json.loads(result_path.read_text(encoding="utf-8"))
        if payload.get("profile") != PROFILES[profile] or payload.get("scenario") != scenario or payload.get("wrongProfileRejected") is not True:
            raise RuntimeError("Facade profile selection/control identity mismatch")
        if payload.get("descriptorHash") != run["descriptorSha256"]:
            raise RuntimeError("Facade running descriptor differs from retained compiled descriptor")
        if not isinstance(payload.get("observations"), dict) or not payload["observations"]:
            raise RuntimeError("Facade has no actual observations")
        samples[(profile, scenario)] = {"run": run, "result": payload}
    pairs = []
    for scenario in FACADE_CASES:
        core, ext = (samples[(p, scenario)]["result"]["observations"] for p in PROFILES)
        if core != ext:
            raise RuntimeError("Facade full observation mismatch: " + scenario)
        pairs.append({"name": scenario, "status": "Pass", "scope": "all fixture-recorded observations; not complete Core semantic trace",
                      "executions": {p: samples[(p, scenario)] for p in PROFILES}})
    return {"status": "Pass", "cases": pairs, "manifest": manifest, "manifestSha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
            "limitations": ["Wire records phase/tick/instance/firstByte, not full call/return payload/status/delay/ownership trace",
                            "Process records two resumed states and final time, not all wait-registration/ReadyKey/commit events",
                            "Scalar and signal record final state/ports, not complete intermediate commits"]}


def run(root, output_dir, profile_driver=None, facade_results=None, profile_runners=None):
    root, output_dir = Path(root), Path(output_dir)
    directory = output_dir / "profile_cases"
    directory.mkdir(parents=True, exist_ok=True)
    source = root / "tests/conformance/ProfileCorpus.lean"
    driver = Path(profile_driver) if profile_driver else None
    candidates = [Path(os.environ["LEANAT_LAKE"])] if os.environ.get("LEANAT_LAKE") else []
    candidates += sorted((root / ".deps").glob("lean*/bin/lake.exe" if os.name == "nt" else "lean*/bin/lake"))
    if shutil.which("lake"):
        candidates.append(Path(shutil.which("lake")))
    lake = next((p for p in candidates if p.is_file()), None)
    # Every original Core observation remains required; segment subcases do not
    # silently discharge transport, process, lifecycle or full-profile obligations.
    definitions = json.loads((root / "tests/conformance/requirements.json").read_text(encoding="utf-8"))
    coverage = [{"id": item["id"], "scenario": item["scenario"], "requiredObservation": item["requirement"],
                 "status": "NotRun", "reason": "Complete profile-selected execution is required"}
                for item in definitions["regressions"] if item["id"].startswith("C-T")]
    if len(coverage) != 30 or {r["id"] for r in coverage} != {f"C-T{i:02}" for i in range(1,31)}:
        raise RuntimeError("Core profile inventory missing or duplicated")
    row = {"id": "E-T40", "backend": "compiler-profile-native", "status": "NotRun", "stop": "IncompleteCorpus",
           "input": {"source": source.read_text(encoding="utf-8"), "cases": list(CASES), "profiles": PROFILES,
                     "parameters": [], "context": {"time": 11, "turn": 2, "instance": 1, "connection": 2}, "fuel": 1000,
                     "loadPolicy": {"maxFileBytes": 16777216, "maxStringBytes": 65536, "memoryBudget": 67108864,
                                    "maxRows": 65536, "maxDepth": 64, "maxWork": 1000000, "maxValueElements": 1048576, "maxRegisters": 65536, "allowedCapabilities": []},
                     "segmentBudget": {"writes": 128, "actions": 128, "bytes": 65536, "events": 128},
                     "observation": "complete returned/failed segment, all committed state and ordered emitted TraceEvent fields", "externalEnvironment": []},
           "expected": "Real profile compilation and independent policy load; full semantic values equal; full Core corpus required for E-T40 Pass",
           "actual": {"commands": [], "cases": [], "coreCoverage": coverage},
           "reason": "Complete Core profile-selected corpus is not implemented; finite segment comparisons are recorded without upgrading E-T40"}

    def invoke(command):
        result = subprocess.run([str(x) for x in command], cwd=root, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=120)
        record = {"command": [str(x) for x in command], "exit_code": result.returncode,
                  "stdout": result.stdout, "stderr": result.stderr}
        row["actual"]["commands"].append(record)
        return record

    try:
        if lake is None or driver is None or not driver.is_file():
            row.update(stop="MissingDependency", reason="Actual Lean compiler and explicit profile-driver binary required")
        else:
            build = invoke([lake, "build", "LeanAT.Compiler.Main"])
            if build["exit_code"] != 0:
                raise RuntimeError("Compiler module build failed; actual diagnostics retained")
            emitted = invoke([lake, "env", "lean", "--run", source, directory.resolve()])
            if emitted["exit_code"] != 0:
                raise RuntimeError("Actual compileForProfile/Lean loader controls failed")
            for name in CASES:
                sample = {"name": name, "scope": "closed segment only", "executions": {}, "status": "Fail"}
                row["actual"]["cases"].append(sample)
                for suffix, profile in PROFILES.items():
                    descriptor = directory / f"{name}.{suffix}.bin"
                    good = invoke([driver.resolve(), descriptor.resolve(), profile])
                    if good["exit_code"] != 0:
                        raise RuntimeError(f"{name}/{suffix}: actual native profile execution failed")
                    payload = json.loads(good["stdout"])
                    if payload.get("loadedProfile") != profile:
                        raise RuntimeError("Loaded artifact profile differs from selected compiler profile")
                    wrong = invoke([driver.resolve(), descriptor.resolve(), PROFILES["ext" if suffix == "core" else "core"]])
                    if wrong["exit_code"] != 3 or "profile" not in wrong["stdout"].lower():
                        raise RuntimeError("Wrong native loader policy was not specifically rejected")
                    sample["executions"][suffix] = {"descriptor_sha256": hashlib.sha256(descriptor.read_bytes()).hexdigest(), **payload}
                core, ext = (sample["executions"][p]["semantic"] for p in ("core", "ext"))
                if core != ext:
                    raise RuntimeError(f"{name}: full semantic value difference")
                if sample["executions"]["core"]["fuelUsed"] != sample["executions"]["ext"]["fuelUsed"]:
                    raise RuntimeError(f"{name}: actual Core/Ext instruction fuel differs")
                expected_state = "42" if name == "lazy-state" else "7" if name == "rollback" else "18446744073709551615"
                if core["state"] != [{"u64": expected_state}]:
                    raise RuntimeError(f"{name}: independent expected committed state mismatch")
                if name == "rollback":
                    if core["stop"] != "Failed" or core["trace"] or "profile rollback" not in core["error"]:
                        raise RuntimeError("Rollback did not retain exact failed-segment semantics")
                elif core["stop"] != "Completed" or core["result"] != [{"u64": expected_state}]:
                    raise RuntimeError("Successful fixture did not reach required complete result")
                if name == "trace-state" and len(core["trace"]) != 2:
                    raise RuntimeError("Full ordered trace missing")
                if name == "trace-state":
                    for event, tag, expected in zip(core["trace"], ("before", "after"), ("3", expected_state)):
                        if event["kind"] != tag or event["values"] != [{"u64": expected}] or event["time"] != 11 or event["turn"] != 2 or event["instance"] != 1 or event["connection"] != 2:
                            raise RuntimeError("Independent ordered emitted-value/context expectation failed")
                tampered = copy.deepcopy(ext)
                tampered["state"][0]["u64"] = "corrupted"
                if core == tampered:
                    raise RuntimeError("Full-value comparator accepted intentional corruption")
                sample.update(status="Pass", comparatorNegativeControl="mutated full state rejected")
            row["actual"]["binary_sha256"] = hashlib.sha256(driver.read_bytes()).hexdigest()
    except (OSError, subprocess.TimeoutExpired) as error:
        row.update(stop="ExecutionUnavailable", reason=str(error))
    except (RuntimeError, ValueError, KeyError) as error:
        row.update(status="Fail", stop="ProfileDifferentialFailure", reason=str(error))
    try:
        facade = facade_pairs(root, facade_results or os.environ.get("LEANAT_FACADE_RESULTS") or root / "build/facade-profiles")
        row["actual"]["generatedFacade"] = facade
        partial = {"C-T09": ["scalar", "wire", "signal"], "C-T16": ["process"], "C-T23": ["wire"], "C-T26": list(FACADE_CASES)}
        for entry in coverage:
            if entry["id"] in partial:
                entry["relatedPartialFixtures"] = partial[entry["id"]]
                entry["reason"] += "; paired facade observations provide related partial evidence only, not this original observation in full"
    except (OSError, RuntimeError, ValueError, KeyError, TypeError) as error:
        row["actual"]["generatedFacade"] = {"status": "Fail", "reason": str(error)}
        row.update(status="Fail", stop="FacadeProfileEvidenceFailure", reason=str(error))
    corpus_path = root / "tests/conformance/profile_corpus.py"
    spec = importlib.util.spec_from_file_location("profile_corpus", corpus_path)
    corpus_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(corpus_module)
    corpus = corpus_module.run(root, output_dir, profile_runners or {})
    row["actual"]["originalCorpus"] = corpus
    if corpus["status"] == "Fail":
        row.update(status="Fail", stop="OriginalProfileCorpusFailure", reason=corpus["reason"])
    elif corpus["status"] == "Pass" and row["status"] != "Fail":
        row.update(status="Pass", stop="OriginalProfileCorpusCompleted", reason="")
        for entry in coverage:
            entry.update(status="Pass", reason="All registered original branches executed under both loaded profiles and full observations matched")
    row["source_sha256"] = {str(source.relative_to(root)): hashlib.sha256(source.read_bytes()).hexdigest(),
                            "tests/conformance/profile_driver.cpp": hashlib.sha256((root / "tests/conformance/profile_driver.cpp").read_bytes()).hexdigest()}
    evidence = directory / "E-T40.json"
    evidence.write_text(json.dumps(row, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    row.update(evidence_path=str(evidence.resolve()), evidence_sha256=hashlib.sha256(evidence.read_bytes()).hexdigest())
    return [row]
