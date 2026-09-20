"""Execute the descriptor-bound original Core corpus independently in both profiles."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid
import sys
try:
    from .profile_observation_schema import validate_observations, symmetric_deletion_control
except ImportError:
    from profile_observation_schema import validate_observations, symmetric_deletion_control

PROFILES = {"core": "AT-Core-1.1-draft", "ext": "AT-Ext-1.1-draft"}
REQUIRED = {f"C-T{i:02}" for i in range(1, 31)}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def canonical(value):
    # Keep JSON types and every field: Python's True == 1 is not semantic equality.
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False)


def source_hashes(root):
    result = {}
    for name in ("CMakeLists.txt", "lakefile.lean", "lean-toolchain", "LeanAT.lean"):
        if (root / name).is_file():
            result[name] = digest(root / name)
    for folder in ("LeanAT", "runtime", "stdlib", "systemc", "tests/conformance", "tests/support"):
        for path in sorted((root / folder).rglob("*")):
            if path.is_file() and path.suffix in {".lean", ".cpp", ".hpp", ".py", ".json", ".txt"}:
                result[path.relative_to(root).as_posix()] = digest(path)
    return result


def validate_descriptor_projection(row, descriptor, output):
    """Check the declared C26 physical-location projection against actual bytes."""
    provenance = row.get("artifactProvenance", {})
    if provenance.get("schema") != "leanat.descriptor-diagnostic-projection.v1":
        raise ValueError("C26 requires explicit validated artifact-location projection")
    original = descriptor.read_bytes()
    offset = 104

    def skip(count):
        nonlocal offset
        if count < 0 or offset + count > len(original):
            raise ValueError("C26 diagnostic offset exceeds actual descriptor")
        offset += count

    def word():
        nonlocal offset
        skip(4)
        return int.from_bytes(original[offset - 4:offset], "little")

    skip(word())
    types = word()
    type_offset = offset
    for _ in range(types):
        skip(9)
        skip(word() * 4)
        for _ in range(word()):
            skip(word() * 4)
    states = word()
    state_offset = offset
    skip(states * 4)
    values = word()
    value_offset = offset
    if not types or not states or not values:
        raise ValueError("C26 actual descriptor lacks required first type/state")
    expected = {
        "version": (4, 2, 65535, "11", "schema version", "header.major", None),
        "type": (type_offset, 1, 255, "17", "unknown type", "types[0].kind", value_offset + 1),
        "tag": (value_offset, 1, 255, "17", "bits width", "initialState[0].tag", value_offset + 1),
        "index": (state_offset, 4, 4294967295, "17", "literal type ID", "stateTypes[0]", value_offset + 1)}
    diagnostics = provenance.get("diagnostics", [])
    if len(diagnostics) != 4 or {d.get("branch") for d in diagnostics} != set(expected):
        raise ValueError("C26 projection omits a malformed artifact")
    semantic = {r["value"]["branch"]: r["value"] for r in row["observations"]["records"] if r["kind"] == "rejectedMutation"}
    for diagnostic in diagnostics:
        branch = diagnostic["branch"]
        at, width, value, code, reason, field, reader = expected[branch]
        if diagnostic.get("projectionValidated") is not True or any(diagnostic.get("negativeControls", {}).get(k) is not True for k in ("wrongCodeRejected", "wrongReasonRejected", "wrongPathRejected", "wrongOffsetRejected")):
            raise ValueError("C26 projection negative controls did not execute")
        if diagnostic["mutationByteOffset"] != str(at) or diagnostic["readerByteOffset"] != (str(reader) if reader is not None else None):
            raise ValueError("C26 reported location differs from independently parsed artifact field")
        if diagnostic["rawDiagnostic"] != {"code": code, "message": reason + (" at byte " + str(reader) if reader is not None else "")}:
            raise ValueError("C26 actual raw diagnostic does not map exactly to declared semantic reason")
        if semantic[branch]["error"] != {"code": code, "reason": reason, "fieldPath": field}:
            raise ValueError("C26 semantic diagnostic code/reason/path changed")
        artifact = Path(diagnostic["artifactFile"]).resolve()
        if artifact != Path(str(output) + "." + branch + ".bin").resolve():
            raise ValueError("C26 corrupted artifact identity mismatch")
        actual = artifact.read_bytes()
        planned = bytearray(original)
        planned[at:at + width] = value.to_bytes(width, "little")
        planned[72:104] = b"\0" * 32
        planned[72:104] = hashlib.sha256(planned).digest()
        if actual != bytes(planned) or digest(artifact) != diagnostic["artifactSha256"]:
            raise ValueError("C26 mutation or integrity evidence differs from actual specified bytes")


def run(root, output_dir, runners):
    root, directory = Path(root).resolve(), Path(output_dir).resolve() / "original_profile_corpus"
    directory.mkdir(parents=True, exist_ok=True)
    # The compiler publishes atomically into a new directory and rejects existing
    # output. Keep each emitted catalog as the reproducible input of this run.
    catalog_directory = directory / ("catalog-" + uuid.uuid4().hex)
    report = {"schema": "leanat.original-profile-corpus.v1", "status": "NotRun", "commands": [], "cases": [],
              "missingIds": sorted(REQUIRED), "reason": "Descriptor-bound corpus has not executed",
              "comparePolicy": {"version": 1, "dataMode": "FullValue", "ignoredObservationFields": [],
                                "includeFuel": True, "compareInputs": True,
                                "nestedScenarioSchemas": {"C-T24": {"version": 1, "requiredSubruns": ["forward", "reverse"], "eachRequires": ["records", "stop", "fuel"], "comparison": "complete unchanged nested observations"}},
                                "artifactLocationProjection": {"C-T26": "Validated actual byte offset maps to typed descriptor field; raw diagnostics and physical offsets retained as artifact provenance, all non-location error fields compared"}}}
    before = source_hashes(root)
    report["sourceSha256"] = before
    lock_path = root / ((".deps/linux-toolchain-lock.json" if (root / ".deps/linux-toolchain-lock.json").is_file() else "docs/toolchain-linux-lock.json") if sys.platform.startswith("linux") else "docs/toolchain-lock.json")
    if lock_path.is_file():
        report["toolchainLock"] = {"path": str(lock_path.relative_to(root)), "sha256": digest(lock_path), "values": json.loads(lock_path.read_text(encoding="utf-8"))}
    catalog_source = root / "tests/conformance/ScenarioCatalog.lean"
    lake_options = [Path(os.environ["LEANAT_LAKE"])] if os.environ.get("LEANAT_LAKE") else []
    lake_options += sorted((root / ".deps").glob("lean*/bin/lake.exe" if os.name == "nt" else "lean*/bin/lake"))
    if shutil.which("lake"):
        lake_options.append(Path(shutil.which("lake")))
    lake = next((p for p in lake_options if p.is_file()), None)

    def command(args):
        args = [str(arg) for arg in args]
        item = {"command": args}
        report["commands"].append(item)
        try:
            done = subprocess.run(args, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=180)
            item.update(exitCode=done.returncode, stdout=done.stdout, stderr=done.stderr)
            return done.returncode
        except (OSError, subprocess.TimeoutExpired) as exc:
            item.update(exitCode=None, error=str(exc))
            raise

    def artifact(row, key="path", hash_key="sha256"):
        path = (catalog_directory / row[key]).resolve()
        if not path.is_relative_to(catalog_directory):
            raise ValueError("Catalog artifact outside fresh output directory")
        if digest(path) != row[hash_key]:
            raise ValueError("Catalog artifact hash mismatch: " + str(path))
        return path

    try:
        if lake is None or not catalog_source.is_file() or not runners:
            report["reason"] = "Actual scenario catalog compiler and registered descriptor-bound runners required"
        else:
            catalog_path = catalog_directory / "catalog.json"
            if command([lake, "build", "tests.conformance.ScenarioCatalog"]) != 0:
                raise ValueError("Catalog compiler build failed")
            if command([lake, "env", "lean", "--run", catalog_source, catalog_directory]) != 0:
                raise ValueError("Actual independent profile catalog compilation failed")
            catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
            report["catalog"] = catalog
            report["catalogDirectory"] = str(catalog_directory)
            report["catalogSha256"] = digest(catalog_path)
            if catalog.get("schema") != "leanat.scenario-catalog.v1" or not catalog.get("scenarios"):
                raise ValueError("Missing/invalid scenario catalog")
            seen_names, covered = set(), set()
            for scenario in catalog["scenarios"]:
                pair = None
                try:
                    name = scenario["name"]
                    if not isinstance(name, str) or not name or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in name) or name in seen_names:
                        raise ValueError("Invalid/duplicate scenario name")
                    seen_names.add(name)
                    ids = scenario["coreIds"]
                    if not ids or len(ids) != len(set(ids)) or not set(ids).issubset(REQUIRED):
                        raise ValueError("Invalid scenario original Core identities")
                    artifact(scenario["model"])
                    if not scenario.get("sources"):
                        raise ValueError("Scenario source provenance missing")
                    for source in scenario["sources"]:
                        copied = artifact(source)
                        original = (root / source["originalPath"]).resolve()
                        if not original.is_relative_to(root) or digest(original) != digest(copied):
                            raise ValueError("Catalog model source differs from executed compiler source")
                    runner_name = scenario["runner"]
                    binary = Path(runners[runner_name]).resolve() if runner_name in runners else None
                    pair = {"name": name, "runner": runner_name, "coreIds": ids, "executions": {}, "rawExecutions": {}, "status": "NotRun"}
                    report["cases"].append(pair)
                    if binary is None or not binary.is_file():
                        pair["reason"] = "Required native runner unavailable: " + runner_name
                        continue
                    binary_hash = digest(binary)
                    pair["binarySha256"] = binary_hash
                    for suffix, profile in PROFILES.items():
                        compiled = scenario["artifacts"][suffix]
                        if compiled["profile"] != profile:
                            raise ValueError("Compiler artifact has incorrect selected profile")
                        descriptor = artifact(compiled)
                        if "sourceMap" in compiled:
                            artifact(compiled, "sourceMap", "sourceMapSha256")
                        output = directory / f"{name}.{suffix}.jsonl"
                        output.unlink(missing_ok=True)
                        exit_code = command([binary, output, descriptor, profile, compiled["sha256"], name])
                        rows = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()] if output.exists() else []
                        pair["rawExecutions"][suffix] = rows
                        pair.setdefault("executionArtifacts", {})[suffix] = [
                            {"path": str(path.relative_to(directory)), "sha256": digest(path)}
                            for path in sorted(directory.glob(output.name + ".*.bin")) if path.is_file()]
                        if output.exists():
                            output.unlink()
                        if exit_code != 0:
                            raise ValueError(f"{name}/{suffix}: actual scenario runner failed ({exit_code})")
                        if len(rows) != len(ids) or {r.get("id") for r in rows} != set(ids):
                            raise ValueError(f"{name}/{suffix}: missing/duplicate original observations")
                        for row in rows:
                            if row.get("schema") != "leanat.profile-case.v1" or row.get("profile") != profile or row.get("descriptorSha256") != compiled["sha256"]:
                                raise ValueError("Scenario not bound to the selected compiled artifact")
                            if row.get("wrongProfileRejected") is not True or row.get("assertionsPassed") is not True or row.get("coverage") not in ("Complete", "Partial"):
                                raise ValueError("Scenario lacks complete original assertions or actual wrong-profile control")
                            if not isinstance(row.get("input"), (dict, list)) or not row["input"] or not isinstance(row.get("observations"), dict) or not row["observations"]:
                                raise ValueError("Typed full input/observations required; fixed prose cannot establish equivalence")
                            validate_observations(row["id"], row["observations"])
                            if row["id"] == "C-T24":
                                if set(row["observations"]) != {"forward", "reverse"}:
                                    raise ValueError("C24 requires both explicitly ordered callback subruns")
                                for direction in ("forward", "reverse"):
                                    subrun = row["observations"][direction]
                                    if not isinstance(subrun, dict) or not isinstance(subrun.get("records"), list) or not subrun["records"] or "fuel" not in subrun or "stop" not in subrun:
                                        raise ValueError("C24 actual subrun stop/fuel/full records missing")
                            elif "fuel" not in row["observations"] or "stop" not in row["observations"]:
                                raise ValueError("Actual stop and fuel observation required (zero for host-only scenarios)")
                            if row["id"] != "C-T24":
                                events = row["observations"].get("records", row["observations"].get("events"))
                                if not isinstance(events, list) or not events or any(not isinstance(event, dict) for event in events):
                                    raise ValueError("Nonempty typed actual event records required; stop/fuel alone cannot establish conformance")
                            if not row.get("branches") or len(row["branches"]) != len(set(row["branches"])):
                                raise ValueError("Scenario requires distinct explicit observed branches")
                            obligations = scenario.get("obligations", [])
                            required_branches = [b.split(":", 1)[1] for b in obligations if b.startswith(row["id"] + ":")]
                            if not required_branches or not set(required_branches).issubset(row["branches"]):
                                raise ValueError("Original scenario branch obligations absent or not all exercised: " + row["id"])
                            if row["id"] == "C-T26":
                                validate_descriptor_projection(row, descriptor, output)
                        pair["executions"][suffix] = {r["id"]: r for r in rows}
                    if digest(binary) != binary_hash:
                        raise ValueError("Scenario executable changed during profile comparison")
                    for identity in ids:
                        core, ext = (pair["executions"][p][identity] for p in PROFILES)
                        pair.setdefault("symmetricDeletionControls", {})[identity] = {
                            profile: symmetric_deletion_control(identity, pair["executions"][profile][identity]["observations"])
                            for profile in PROFILES}
                        for field in ("input", "observations", "branches"):
                            if canonical(core[field]) != canonical(ext[field]):
                                pair["firstDifference"] = {"id": identity, "field": field, "core": core[field], "ext": ext[field]}
                                raise ValueError(f"{identity}: Core/Ext full {field} differ")
                        # Probe the very same comparator using a real observed object.
                        tampered = json.loads(canonical(ext["observations"]))
                        if identity == "C-T24":
                            tampered["forward"]["fuel"] = {"deliberatelyCorrupted": tampered["forward"]["fuel"]}
                        else:
                            tampered["fuel"] = {"deliberatelyCorrupted": ext["observations"]["fuel"]}
                        if canonical(core["observations"]) == canonical(tampered):
                            raise ValueError("Comparator accepted deliberate actual-observation corruption")
                    pair.update(status="Pass", comparatorNegativeControl="corrupted observed fuel rejected")
                    covered.update(ids)
                except (OSError, subprocess.TimeoutExpired, ValueError, KeyError, TypeError) as error:
                    if pair is None:
                        pair = {"name": scenario.get("name", "invalid"), "executions": {}}
                        report["cases"].append(pair)
                    pair.update(status="NotRun" if isinstance(error, (OSError, subprocess.TimeoutExpired)) else "Fail", reason=str(error))
            # Multiple complementary backend branches may implement one original
            # requirement. A passing native fragment cannot discharge its GP half.
            requirements_path = root / "tests/conformance/profile_requirements.json"
            requirements = json.loads(requirements_path.read_text(encoding="utf-8")) if requirements_path.is_file() else {}
            report["requirements"] = requirements
            report["requirementsSha256"] = digest(requirements_path) if requirements_path.is_file() else None
            covered = set()
            for identity in REQUIRED:
                needs = requirements.get(identity, [])
                if not needs:
                    continue
                satisfied = True
                for need in needs:
                    pair = next((p for p in report["cases"] if p["name"] == need["scenario"]), None)
                    if pair is None or pair["status"] != "Pass":
                        satisfied = False
                        break
                    for profile in PROFILES:
                        observed = pair["executions"][profile].get(identity)
                        if not observed or not need["branches"] or not set(need["branches"]).issubset(observed["branches"]):
                            satisfied = False
                if satisfied:
                    covered.add(identity)
            report["missingIds"] = sorted(REQUIRED - covered)
            if report["missingIds"]:
                report["reason"] = "Required descriptor-bound paired original Core scenarios are missing: " + ", ".join(report["missingIds"])
            else:
                report.update(status="Pass", reason="All thirty original Core scenarios have complete paired descriptor-bound observations")
            failed_pairs = [p for p in report["cases"] if p["status"] == "Fail"]
            if failed_pairs:
                report.update(status="Fail", reason="; ".join(p["name"] + ": " + p["reason"] for p in failed_pairs))
    except (OSError, subprocess.TimeoutExpired) as exc:
        report.update(status="NotRun", reason="Actual corpus execution unavailable: " + str(exc))
    except (ValueError, KeyError, TypeError) as exc:
        report.update(status="Fail", reason=str(exc))
        if report["cases"] and report["cases"][-1]["status"] != "Pass":
            report["cases"][-1].update(status="Fail", reason=str(exc))
    report["sourceStable"] = source_hashes(root) == before
    if "toolchainLock" in report and digest(lock_path) != report["toolchainLock"]["sha256"]:
        report["sourceStable"] = False
    if not report["sourceStable"]:
        report["priorOutcome"] = {"status": report["status"], "reason": report["reason"]}
        report.update(status="Fail", reason="Scenario/compiler/runtime sources changed during corpus execution")
    evidence = directory / "E-T40-corpus.json"
    evidence.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    report.update(evidencePath=str(evidence), evidenceSha256=digest(evidence))
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Run the complete descriptor-bound Core/Ext corpus")
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    suffix = ".exe" if os.name == "nt" else ""
    targets = {"wire": "leanat_conformance_profile_wire", "descriptor": "leanat_profile_descriptor",
               "runtime": "leanat_profile_runtime", "routing": "leanat_profile_topology",
               "memory": "leanat_profile_memory", "systemc": "leanat_profile_systemc"}
    result = run(args.root, args.output, {name: args.build / "tests/conformance" / (target + suffix) for name, target in targets.items()})
    print(json.dumps({"status": result["status"], "reason": result["reason"], "missingIds": result["missingIds"], "report": result["evidencePath"]}))
    sys.exit(0 if result["status"] == "Pass" else 1)
