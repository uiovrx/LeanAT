"""Actual source/serialized ExecIR/native/SystemC opcode conformance orchestration."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid
import traceback
import copy
sys.dont_write_bytecode = True


def _hash(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _validate_targets(targets):
    # A return-only program legitimately has no instruction opcodes.
    if not isinstance(targets, list) or any(type(tag) is not int or not 0 <= tag < 65 for tag in targets):
        raise ValueError("Invalid declared opcode targets")


def _validate_source_inventory(catalog, inventory):
    if inventory.get("schema") != "leanat.opcode-source-inventory.v1":
        raise ValueError("Missing authoritative current source inventory")
    fields = ("id", "variant", "profile", "handlerId", "programId", "expectedOutcome", "expectedOpcodeTags", "providerFamily")
    def index(rows, source):
        result = {}
        for case in rows:
            identity = (case["id"], case["variant"])
            if identity in result:
                raise ValueError("Duplicate source inventory identity")
            item = {key: case[key] for key in fields}
            item["sourceInputSha256"] = case["sourceInputSha256"] if source else case["referenceInput"]["sha256"]
            item["descriptorSha256"] = case["descriptorSha256"] if source else case["descriptor"]["sha256"]
            for field, reference in (("sourceMapSha256", "sourceMap"), ("modelSha256", "model"), ("sourceWrapperSha256", "sourceWrapper")):
                item[field] = case[field] if source else case[reference]["sha256"]
            item["modelNodeInventory"] = case["modelNodeInventory"]
            item["sourceFiles"] = case["sourceFiles"] if source else [entry["originalPath"] for entry in case["sources"]]
            result[identity] = item
        return result
    actual, authoritative = index(catalog["cases"], False), index(inventory["cases"], True)
    if actual.keys() != authoritative.keys():
        raise ValueError(f"Catalog source inventory mismatch: missing={sorted(authoritative.keys() - actual.keys())}, extra={sorted(actual.keys() - authoritative.keys())}")
    for identity in actual:
        if json.dumps(actual[identity], sort_keys=True) != json.dumps(authoritative[identity], sort_keys=True):
            raise ValueError(f"Catalog source metadata/input/descriptor mismatch: {identity}")
    return {"caseCount": len(actual), "exactMatch": True}


def _return_mutation_control(replay, case, input_value, model, executable, native, systemc, root):
    tampered = copy.deepcopy(native)
    tampered["returned"] = list(tampered["returned"]) + [{"kind": "bits", "width": 1, "value": "0"}]
    result = replay.compare_case(case, input_value, model, executable, tampered, systemc, root)
    return_mismatches = [item for item in result.get("differences", [])
                         if item.get("path", "").startswith(("$.runtime.returned", "$.runtimeSystemc.returned"))]
    if result.get("status") == "Pass" or not return_mismatches:
        raise ValueError("Production opcode comparator did not reject the actual corrupted return")
    return {"mutation": "append actual returned value", "comparison": result, "returnMismatches": return_mismatches}


def _sources(root):
    files = {}
    for folder in ("LeanAT", "runtime", "stdlib", "systemc", "tools", "tests/conformance", "tests/lean", "tests/support"):
        for path in sorted((root / folder).rglob("*")):
            if path.is_file() and path.suffix in (".lean", ".hpp", ".cpp", ".py", ".json", ".txt"):
                files[path.relative_to(root).as_posix()] = _hash(path)
    for name in ("lakefile.lean", "lean-toolchain", "CMakeLists.txt", "LeanAT.lean"):
        files[name] = _hash(root / name)
    return files


def run(root, output_dir, opcode_runtime=None, opcode_systemc=None):
    root = Path(root).resolve()
    directory = Path(output_dir).resolve() / "opcode_cases"
    directory.mkdir(parents=True, exist_ok=True)
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from tools.leanat import opcode_replay as replay
    from tools.leanat.io import ToolError, read_json, write_json
    from tools.leanat.trace import lookup_source
    from tools.leanat.toolchain import select_toolchain_lock
    row = {"id": "E-T39", "backend": "source-exec-native-systemc", "status": "NotRun", "stop": "IncompleteOpcodeCorpus",
           "input": {"requiredOpcodes": list(range(65)), "comparison": "actual independent source and serialized ExecIR outcomes, provider state, committed values and native instruction observations"},
           "expected": "All 65 opcodes execute positively with source locations; declared failure cases retain actual errors and equal full outcomes across four backends",
           "actual": {"commands": [], "cases": [], "coverage": {}}, "reason": "Actual four-backend corpus has not completed"}
    before = _sources(root)
    row["source_sha256"] = before
    lock = None
    lakes = [Path(os.environ["LEANAT_LAKE"])] if os.environ.get("LEANAT_LAKE") else []
    lakes += sorted((root / ".deps").glob("lean*/bin/lake.exe" if os.name == "nt" else "lean*/bin/lake"))
    if shutil.which("lake"):
        lakes.append(Path(shutil.which("lake")))
    lake = next((path for path in lakes if path.is_file()), None)
    binaries = {"runtime": Path(opcode_runtime).resolve() if opcode_runtime else None,
                "systemc": Path(opcode_systemc).resolve() if opcode_systemc else None}

    def command(arguments, timeout=180):
        record = {"command": [str(value) for value in arguments]}
        row["actual"]["commands"].append(record)
        try:
            result = subprocess.run(record["command"], cwd=root, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=timeout)
            record.update(exitCode=result.returncode, stdout=result.stdout, stderr=result.stderr)
            return result.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            record.update(exitCode=None, error=str(error))
            raise

    try:
        lock_values, lock_provenance = select_toolchain_lock(root)
        lock = root / lock_provenance["path"]
        row["actual"]["toolchainLock"] = {**lock_provenance, "values": lock_values}
        if lake is None or any(path is None or not path.is_file() for path in binaries.values()):
            row["reason"] = "Actual Lean toolchain and both registered opcode replay binaries required"
        else:
            hashes = {side: _hash(path) for side, path in binaries.items()}
            row["actual"]["binarySha256"] = hashes
            host = {}
            for side, binary in binaries.items():
                measured = replay.execute_json([binary, "--host-info"], root)
                row["actual"]["commands"].append(measured)
                if measured["exitCode"] != 0:
                    raise ValueError("Actual native host measurement failed")
                host[side] = measured["value"]
            if replay.differences(host["runtime"], host["systemc"]):
                raise ValueError("Runtime and SystemC host value layout differs")
            row["actual"]["measuredHost"] = host
            provided_catalog = os.environ.get("LEANAT_OPCODE_CATALOG")
            if provided_catalog:
                catalog_path = Path(provided_catalog).resolve()
                catalog_directory = catalog_path.parent
                row["actual"]["catalogMode"] = "explicit source-validated precompiled catalog; no build launched"
            else:
                catalog_directory = directory / ("catalog-" + uuid.uuid4().hex)
                catalog_path = catalog_directory / "catalog.json"
                if command([lake, "build", "tests.conformance.OpcodeCatalog", "tests.conformance.OpcodeBatch"], timeout=300) != 0:
                    raise ValueError("Ordered opcode compiler/reference build failed")
                if command([lake, "env", "lean", "--run", root / "tests/conformance/OpcodeCatalog.lean", catalog_directory], timeout=300) != 0:
                    raise ValueError("Fresh actual source opcode catalog emission failed")
                row["actual"]["catalogMode"] = "fresh independently compiled source cases"
            catalog = read_json(catalog_path, max_bytes=32 * 1024 * 1024)
            row["actual"].update(catalogPath=str(catalog_path), catalogSha256=_hash(catalog_path), catalog=catalog)
            if catalog.get("schema") != "leanat.opcode-catalog.v1" or not catalog.get("cases"):
                raise ValueError("Missing actual opcode case catalog")
            auxiliary = []
            for fixture in ("ModelNodeCoverage", "OpcodeCoverage", "SourceMap"):
                source = root / f"tests/lean/{fixture}.lean"
                code = command([lake, "env", "lean", "--run", source])
                auxiliary.append({"fixture": fixture, "sourceSha256": _hash(source), "exitCode": code})
            row["actual"]["auxiliaryCoverageExecutions"] = auxiliary

            def artifact(reference):
                path = (catalog_directory / reference["path"]).resolve()
                if not path.is_relative_to(catalog_directory) or _hash(path) != reference["sha256"]:
                    raise ValueError("Opcode catalog artifact path/hash mismatch")
                return path

            seen = set()
            positive = {side: set() for side in binaries}
            errors = {side: set() for side in binaries}
            declared_tags = set()
            prepared = []
            batch_requests = []
            for case in catalog["cases"]:
                sample = {"id": case.get("id"), "variant": case.get("variant"), "status": "NotRun", "executions": {}}
                row["actual"]["cases"].append(sample)
                try:
                    identity = (case["id"], case["variant"])
                    if identity in seen or any(not part or part in (".", "..") or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_." for c in part) for part in identity):
                        raise ValueError("Invalid or duplicate opcode case identity")
                    seen.add(identity)
                    targets = case["expectedOpcodeTags"]
                    _validate_targets(targets)
                    declared_tags.update(targets)
                    paths = {key: artifact(case[key]) for key in ("descriptor", "referenceInput", "sourceMap", "sourceWrapper", "model")}
                    if not case.get("sources"):
                        raise ValueError("Opcode case has no canonical source provenance")
                    for source in case["sources"]:
                        copied = artifact(source)
                        original = (root / source["originalPath"]).resolve()
                        if not original.is_relative_to(root) or _hash(original) != _hash(copied):
                            raise ValueError("Opcode source changed since catalog compilation")
                    case_directory = directory / "executions" / case["id"] / case["variant"]
                    case_directory.mkdir(parents=True, exist_ok=True)
                    input_value = replay.bind_host_profile(read_json(paths["referenceInput"]), host["runtime"])
                    input_path = case_directory / "measured-input.json"
                    write_json(input_path, input_value)
                    sample["input"] = input_value
                    model_output, exec_output = case_directory / "model.json", case_directory / "exec.json"
                    for output in (model_output, exec_output):
                        output.unlink(missing_ok=True)
                    prepared.append((case, sample, paths, case_directory, input_value, model_output, exec_output))
                    batch_requests.append({"id": case["id"], "variant": case["variant"], "input": str(input_path),
                                           "modelOutput": str(model_output), "execOutput": str(exec_output),
                                           "descriptor": str(paths["descriptor"]), "profile": case["profile"],
                                           "sha256": case["descriptor"]["sha256"], "programId": case["programId"]})
                except (OSError, subprocess.TimeoutExpired) as error:
                    sample.update(status="NotRun", reason=str(error))
                except (ValueError, KeyError, TypeError, IndexError, RuntimeError, AssertionError) as error:
                    sample.update(status="Fail", reason=str(error))
            batch_path = directory / "reference-batch.json"
            inventory_path = directory / "source-inventory.json"
            inventory_path.unlink(missing_ok=True)
            write_json(batch_path, {"schema": "leanat.opcode-batch.v1", "cases": batch_requests, "inventoryOutput": str(inventory_path)})
            batch_code = command([lake, "env", "lean", "--run", root / "tests/conformance/OpcodeBatch.lean", batch_path], timeout=600)
            row["actual"]["referenceBatch"] = {"path": str(batch_path), "sha256": _hash(batch_path),
                                                 "caseCount": len(batch_requests), "exitCode": batch_code}
            inventory_error = None
            try:
                inventory = read_json(inventory_path, max_bytes=32 * 1024 * 1024)
                row["actual"]["sourceInventory"] = {"path": str(inventory_path), "sha256": _hash(inventory_path), "value": inventory}
                row["actual"]["sourceInventory"]["validation"] = _validate_source_inventory(catalog, inventory)
            except (OSError, ValueError, KeyError, TypeError) as error:
                inventory_error = str(error)
                row["actual"]["sourceInventoryError"] = inventory_error
            for case, sample, paths, case_directory, input_value, model_output, exec_output in prepared:
                try:
                    if not model_output.is_file() or not exec_output.is_file():
                        raise ValueError("Reference batch did not produce both fresh actual case outcomes")
                    model, executable = read_json(model_output), read_json(exec_output)
                    sample["executions"].update(model=model, exec=executable)
                    native_case = {**case, "descriptor": str(paths["descriptor"]), "descriptorHash": case["descriptor"]["sha256"]}
                    native_results = {}
                    summaries = {}
                    for side, binary in binaries.items():
                        record = replay.run_native(binary, root, native_case, input_value, case_directory / side)
                        sample["executions"][side] = record
                        if record.get("exitCode") != 0 and record["value"].get("schema") == "leanat.opcode-native-output.v1":
                            raise ValueError("Native process failed after producing a semantic outcome")
                        native_results[side] = record["value"]
                        description = replay.execute_json([binary, "--describe", case_directory / side / "request.json"], root)
                        sample["executions"][side + "Description"] = description
                        if description["exitCode"] != 0:
                            raise ValueError("Actual descriptor instruction description failed")
                        summaries[side] = description["value"]
                    if replay.differences(summaries["runtime"], summaries["systemc"]):
                        raise ValueError("Native backends disagree on actual descriptor instruction mapping")
                    comparison_case = {**case, "descriptorSummary": summaries["runtime"]}
                    comparison = replay.compare_case(comparison_case, input_value, model, executable,
                                                     native_results["runtime"], native_results["systemc"], root)
                    sample["comparison"] = comparison
                    sample["status"] = comparison["status"]
                    source_map = read_json(paths["sourceMap"], max_bytes=32 * 1024 * 1024)
                    locations = {}
                    for side, value in native_results.items():
                        resolved = []
                        if not case["expectedOpcodeTags"] and value.get("opcodeEvents", []):
                            raise ValueError("Declared terminator-only case unexpectedly executed instruction opcodes")
                        for event in value.get("opcodeEvents", []):
                            location = {"ir": "exec", "program": event["program"], "kind": "instruction", "block": event["block"], "instruction": event["instruction"]}
                            span = lookup_source(source_map, paths["descriptor"].read_bytes(), location, catalog_directory)
                            resolved.append({"location": location, "stage": event["stage"], "opcode": event["opcode"], "sourceSpan": span})
                        locations[side] = resolved
                        if sample["status"] == "Pass":
                            if case["expectedOutcome"] in ("success", "suspended") and model.get("ok") is True:
                                positive[side].update(e["opcode"] for e in value.get("opcodeEvents", []) if e["stage"] == "completed")
                            errors[side].update(e["opcode"] for e in value.get("opcodeEvents", []) if e["stage"] == "error")
                    sample["actualSourceLocations"] = locations
                    # The production comparison must detect a changed actual return.
                    if sample["status"] == "Pass" and native_results["runtime"].get("schema") == "leanat.opcode-native-output.v1":
                        sample["negativeComparatorControl"] = _return_mutation_control(
                            replay, comparison_case, input_value, model, executable, native_results["runtime"], native_results["systemc"], root)
                except (OSError, subprocess.TimeoutExpired) as error:
                    sample.update(status="NotRun", reason=str(error))
                except (ValueError, KeyError, TypeError, IndexError, RuntimeError, AssertionError) as error:
                    sample.update(status="Fail", reason=str(error))
                except Exception as error:
                    sample.update(status="Fail", reason=str(error), exceptionType=type(error).__name__, traceback=traceback.format_exc())
                write_json(directory / "progress.json", {"cases": row["actual"]["cases"]})
            row["actual"]["coverage"] = {"declared": sorted(declared_tags), "positiveExecuted": {k: sorted(v) for k, v in positive.items()},
                                         "runtimeErrors": {k: sorted(v) for k, v in errors.items()},
                                         "missingPositive": {k: sorted(set(range(65)) - v) for k, v in positive.items()}}
            failures = [case for case in row["actual"]["cases"] if case["status"] == "Fail"]
            incomplete = [case for case in row["actual"]["cases"] if case["status"] != "Pass"]
            auxiliary_failures = [item["fixture"] for item in auxiliary if item["exitCode"] != 0]
            if failures or auxiliary_failures or batch_code != 0 or inventory_error is not None:
                row.update(status="Fail", stop="OpcodeDifferentialFailure", reason=f"{len(failures)} actual opcode cases failed; reference batch exit: {batch_code}; inventory error: {inventory_error}; auxiliary failures: {auxiliary_failures}; full evidence retained")
            elif incomplete or any(set(range(65)) - tags for tags in positive.values()) or any(not tags for tags in errors.values()):
                row["reason"] = "Some declared cases or positive opcode/error/source coverage remain incomplete; see exact case evidence"
            else:
                row.update(status="Pass", stop="OpcodeCorpusCompleted", reason="")
            if any(_hash(binary) != hashes[side] for side, binary in binaries.items()):
                raise ValueError("Opcode executable changed during corpus execution")
    except (OSError, subprocess.TimeoutExpired) as error:
        row.update(status="NotRun", stop="ExecutionUnavailable", reason=str(error))
    except (ValueError, KeyError, TypeError, RuntimeError, AssertionError) as error:
        row.update(status="Fail", stop="OpcodeCorpusFailure", reason=str(error))
    row["actual"]["sourceStable"] = _sources(root) == before
    if not row["actual"]["sourceStable"]:
        row["actual"]["priorOutcome"] = {"status": row["status"], "reason": row["reason"]}
        row.update(status="Fail", stop="ChangedSources", reason="Opcode sources changed during actual execution")
    if lock is not None and (not lock.is_file() or _hash(lock) != row["actual"].get("toolchainLock", {}).get("sha256")):
        row.update(status="Fail", stop="ChangedToolchainLock", reason="Actual toolchain lock changed during opcode execution")
    evidence = directory / "E-T39.json"
    write_json(evidence, row)
    row.update(evidence_path=str(evidence), evidence_sha256=_hash(evidence))
    return [row]


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Execute the actual four-backend opcode corpus")
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--systemc", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, help="Use an explicitly emitted source-validated catalog; skip catalog build/emission")
    args = parser.parse_args()
    if args.catalog:
        os.environ["LEANAT_OPCODE_CATALOG"] = str(args.catalog.resolve())
    outcome = run(args.root, args.output, args.runtime, args.systemc)[0]
    print(json.dumps({"status": outcome["status"], "reason": outcome["reason"], "coverage": outcome["actual"]["coverage"], "report": outcome["evidence_path"]}))
    sys.exit(0 if outcome["status"] == "Pass" else 1)
