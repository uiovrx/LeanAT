"""Execute original-regression scenarios and fail the gate on missing evidence."""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import subprocess
import sys
sys.dont_write_bytecode = True

REASONS = {
    "C-T09": "Full multi-binding repeated-component-instance fixture is not yet implemented; isolated ledger independence is insufficient.",
    "C-T14": "Timeout controller exists, but fixture lacks Runtime drain executor connected to real late-response ACK and immutable local timeout result.",
    "C-T15": "No complete request/service/response-stage reset fixture with real GP lifetime and stale business suppression is installed.",
    "C-T17": "Ready-wait loop plus real SystemC pump yielding/fuel-stop scenario is not yet wired end to end.",
    "C-T19": "Host violation path exists, but fixture does not yet verify durable fatal trace plus retained externally visible prefix.",
    "C-T24": "Full native callback transcript recorder/replayer is not connected to this harness; entry-order unit observations cannot substitute.",
    "C-T26": "Harness has no complete corrupted descriptor version/type/tag/index corpus bound to load-before-execute checks yet.",
    "C-T27": "No supported test injection for generation and sequence exhaustion; time-overflow-only checks would not satisfy all required observations.",
    "C-T28": "Cross-host endian and BUSWIDTH configuration rejection/codec fixtures are not yet connected.",
    "C-T29": "Crossbar helper lacks complete real-GP multihop OOO transport fixture and address-view bridge; static route lookup alone is insufficient.",
    "E-T01": "Both completed task and COMPLETED response late-registration branches need integrated latch fixtures; only Core single-process deadline latch is exercised.",
    "E-T04": "Loser late notification plus recycled WaitGroup slot generation fixture is not yet installed.",
    "E-T05": "AwaitAll failure/cancel/duplicate/empty matrix with consumer/pin ledger checks is not yet installed.",
    "E-T06": "Child cancellation plan exists; executing controller continuation outside cancellation closure not yet wired in this harness.",
    "E-T07": "Compile-time Lean negative fixture for timed-handler awaitSlot is not yet installed; runtime rejection cannot count as compiler evidence.",
    "E-T09": "No complete late WRITE response drain ACK fixture; timeout controller alone cannot certify wire cleanup/no duplicate write.",
    "E-T10": "Same-pool recursive dependency graph deadlock diagnostic is not implemented by TaskPool's capacity rejection alone.",
    "E-T11": "Finite custom-protocol full exchange selection fixture is pending; base matrix is not substituted for this Ext regression.",
    "E-T13": "Reverse-direction nested real socket scenario and failure evidence not yet installed.",
    "E-T14": "Unknown ignorable/mandatory custom phase C5 matrix is pending; Core registered phase test covers only one branch.",
    "E-T15": "Lean static protocol-trait bind mismatch/explicit adapter fixture not yet installed.",
    "E-T16": "Stable phase key codec across separately compiled translation units/plugins is not implemented by fixed numeric PhaseCodec.",
    "E-T17": "Compile-time protocol release-GP grammar rejection fixture not yet installed.",
    "E-T18": "Cancel silent-terminal validation plus executing drainOnly replacement fixture not yet installed.",
    "E-T19": "Lean kernel-checked overlapping-guard/priority negative corpus not yet connected.",
    "E-T20": "Custom response invalid borrowed-buffer scenario requires supported sanitizer/monitor path; this harness does not perform unsafe dereferences.",
    "E-T24": "Manifest proof-grade promotion rejection fixture not yet connected to certificate checker.",
    "E-T25": "Unknown vendor stateful semantic mismatch fixture not yet connected; phase-renaming alone is not accepted evidence.",
    "E-T28": "Combined raw per-byte latency versus managed shared service charge fixture not yet installed.",
    "E-T30": "Managed replacement with in-flight old backing pin fixture not yet installed.",
    "E-T31": "Shared AT/managed Resource arrival/commit fixture not yet installed.",
    "E-T32": "Managed unadmitted/admitted/committed cancellation matrix not yet installed.",
    "E-T33": "Real cross-bus clipped DMI grant and synchronous invalidation fanout fixture not yet installed.",
    "E-T34": "Timing-strict/raw-alias manifest downgrade or rejection fixture not yet connected.",
    "E-T35": "Full open-SystemC transcript replay and semantic trace comparator integration is absent.",
    "E-T36": "Closed-batch cause chain plus real pump budget/yield integration fixture is pending.",
    "E-T37": "Repeated Runtime reset with outstanding real-GP drain backlog fixture is absent.",
    "E-T38": "Complete effective config/schema/artifact hash negative corpus is not yet connected.",
    "E-T39": "Complete two-IR node/opcode/source-map implementation coverage is not available; scalar backend coverage cannot certify this ID.",
    "E-T40": "Complete Core semantic trace corpus has not passed, so Core-only versus Ext full-trace equivalence cannot be established.",
}


def catalog(root):
    definitions = json.loads((root / "tests/conformance/requirements.json").read_text(encoding="utf-8"))
    if definitions.get("schema") != "leanat.requirements.v1":
        raise RuntimeError("unsupported requirement catalog")
    source_rows = definitions["regressions"]
    rows = {row["id"]: {"scenario": row["scenario"], "requirement": row["requirement"]} for row in source_rows}
    if len(source_rows) != len(rows):
        raise RuntimeError("duplicate original regression ID")
    required = {f"C-T{i:02}" for i in range(1, 31)} | {f"E-T{i:02}" for i in range(1, 41)}
    if set(rows) != required:
        raise RuntimeError("original regression catalog does not contain exactly 70 IDs")
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--native", type=pathlib.Path, required=True)
    parser.add_argument("--systemc", type=pathlib.Path)
    parser.add_argument("--compiler", type=pathlib.Path)
    parser.add_argument("--phase-registry", type=pathlib.Path)
    parser.add_argument("--systemc-lifecycle", type=pathlib.Path)
    parser.add_argument("--transport-runtime", type=pathlib.Path)
    parser.add_argument("--transport-systemc", type=pathlib.Path)
    parser.add_argument("--profile-driver", type=pathlib.Path)
    parser.add_argument("--facade-results", type=pathlib.Path)
    parser.add_argument("--profile-runner", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--opcode-runtime", type=pathlib.Path)
    parser.add_argument("--opcode-systemc", type=pathlib.Path)
    parser.add_argument("--systemc-include", type=pathlib.Path)
    parser.add_argument("--toolchain-lock", type=pathlib.Path, help="actual executing platform toolchain lock; defaults to the Linux or Windows workspace lock")
    parser.add_argument("--allow-incomplete", action="store_true", help="collect reports without treating NotRun as a passing release gate")
    args = parser.parse_args()
    root = args.root.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    definitions = catalog(root)
    observed = {}
    executions = []
    runners = [("native", args.native, []), ("systemc", args.systemc, [])]
    if args.systemc_lifecycle:
        runners.append(("systemc_lifecycle", args.systemc_lifecycle, []))
    if args.phase_registry:
        runners += [("phases-ab", args.phase_registry, ["ab"]), ("phases-ba", args.phase_registry, ["ba"])]
    for kind, binary, extra in runners:
        if binary is None:
            continue
        partial = args.output.with_suffix(f".{kind}.jsonl")
        if partial.exists():
            partial.unlink()
        command = [str(binary.resolve()), str(partial.resolve()), *extra]
        binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest() if binary.exists() else None
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
            executions.append({"backend": kind, "command": command, "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
        except subprocess.TimeoutExpired as exc:
            executions.append({"backend": kind, "command": command, "exit_code": None, "error": "120 second scenario executable timeout", "stdout": str(exc.stdout or ""), "stderr": str(exc.stderr or "")})
        except OSError as exc:
            executions.append({"backend": kind, "command": command, "exit_code": None, "error": str(exc), "stdout": "", "stderr": ""})
        executions[-1]["executable_sha256"] = binary_hash
        source_name = "phase_registry" if kind.startswith("phases-") else kind
        executions[-1]["declared_scenarios"] = sorted(set(re.findall(r'"([CE]-T\d{2})"', (root / f"tests/conformance/{source_name}.cpp").read_text(encoding="utf-8"))))
        if partial.exists():
            seen = set()
            for line in partial.read_text(encoding="utf-8").splitlines():
                row = json.loads(line)
                if row["id"] not in definitions or row["id"] in seen or row["status"] not in ("Pass", "Fail", "NotRun"):
                    raise RuntimeError(f"invalid/duplicate scenario output: {row}")
                seen.add(row["id"])
                row["backend"] = kind
                observed.setdefault(row["id"], []).append(row)
            partial.unlink()
    if args.compiler and args.systemc_include:
        outcomes = []
        for name in ("control", "mismatch"):
            source = root / f"tests/conformance/protocol_bind_{name}.cpp"
            command = [str(args.compiler), "-std=c++17", "-fsyntax-only", "-I" + str(args.systemc_include), str(source)]
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                outcomes.append({"command": command, "exit_code": result.returncode, "diagnostics": result.stderr, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
            except (OSError, subprocess.TimeoutExpired) as exc:
                outcomes.append({"command": command, "exit_code": None, "diagnostics": str(exc)})
        valid = outcomes[0]["exit_code"] == 0 and outcomes[1]["exit_code"] not in (0, None) and "bind" in outcomes[1]["diagnostics"]
        status = "Pass" if valid else "NotRun" if any(o["exit_code"] is None for o in outcomes) else "Fail"
        observed["E-T15"] = [{"id": "E-T15", "backend": "compiler", "input": "same protocol positive control; distinct protocol traits direct socket bind", "expected": "positive compiles; distinct traits bind rejected statically", "actual": outcomes, "status": status, "stop": "CompilerDiagnostics"}]
    import importlib.util
    for module_name in ("evidence_cases", "lean_cases", "profile_cases", "opcode_cases"):
        module_path = root / f"tests/conformance/{module_name}.py"
        if module_path.exists():
            spec = importlib.util.spec_from_file_location(module_name, module_path)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            kwargs = {"transport_runtime": args.transport_runtime, "transport_systemc": args.transport_systemc} if module_name == "evidence_cases" else {}
            if module_name == "profile_cases":
                profile_runners = {}
                for item in args.profile_runner:
                    name, separator, path = item.partition("=")
                    if not separator or not name or not path or name in profile_runners:
                        raise RuntimeError("profile runner must be a distinct NAME=PATH")
                    profile_runners[name] = pathlib.Path(path)
                kwargs = {"profile_driver": args.profile_driver, "facade_results": args.facade_results, "profile_runners": profile_runners}
            elif module_name == "opcode_cases":
                kwargs = {"opcode_runtime": args.opcode_runtime, "opcode_systemc": args.opcode_systemc}
            for row in module.run(root, args.output.parent, **kwargs):
                if row["id"] not in definitions or row["status"] not in ("Pass", "Fail", "NotRun"):
                    raise RuntimeError(f"invalid scenario row from {module_name}: {row}")
                observed.setdefault(row["id"], []).append(row)
    cases = []
    for identity, definition in sorted(definitions.items()):
        samples = observed.get(identity, [])
        failed = any(s["status"] == "Fail" for s in samples)
        passed = any(s["status"] == "Pass" for s in samples)
        if identity == "E-T16" and args.phase_registry:
            passed = all(any(s["backend"] == backend and s["status"] == "Pass" for s in samples) for backend in ("phases-ab", "phases-ba"))
        status = "Fail" if failed else "Pass" if passed else "NotRun"
        reason = "" if status != "NotRun" else REASONS.get(identity, "Required scenario executable/backend has not supplied complete observations; no pass inferred.")
        blocked = [e for e in executions if identity in e["declared_scenarios"] and e["exit_code"] != 0 and not any(s["backend"] == e["backend"] for s in samples)]
        if status == "NotRun" and blocked:
            reason = "; ".join(f"{e['backend']} scenario present but not executed: {e.get('error', 'runner stopped before this scenario')}" for e in blocked)
        elif status == "NotRun" and any(s.get("reason") for s in samples):
            reason = "; ".join(s["reason"] for s in samples if s.get("reason"))
        cases.append({"id": identity, **definition, "status": status,
                      "input": [s["input"] for s in samples], "config": {"language": "C++17", "systemc_enabled": args.systemc is not None, "extern_pure_enabled": True, "managed_enabled": True, "raw_dmi_enabled": args.systemc is not None},
                      "expected": definition["requirement"], "actual": samples,
                      "stop": "ScenarioCompleted" if status == "Pass" else "AssertionFailure" if status == "Fail" else "NotRun", "reason": reason})
    hashes = {}
    for name in ("CMakeLists.txt", "lakefile.lean", "lean-toolchain", "LeanAT.lean"):
        path = root / name
        if path.is_file():
            hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    catalog_path = root / "tests/conformance/requirements.json"
    hashes[catalog_path.relative_to(root).as_posix()] = hashlib.sha256(catalog_path.read_bytes()).hexdigest()
    lock = args.toolchain_lock or root / (".deps/linux-toolchain-lock.json" if sys.platform.startswith("linux") else "docs/toolchain-lock.json")
    lock = lock.resolve()
    lock_bytes = lock.read_bytes()
    lock_hash = hashlib.sha256(lock_bytes).hexdigest()
    lock_label = lock.relative_to(root).as_posix() if lock.is_relative_to(root) else str(lock)
    hashes[lock_label] = lock_hash
    actual_toolchain = {"path": lock_label, "sha256": lock_hash, "platform": sys.platform, "values": json.loads(lock_bytes)}
    for directory in ("runtime", "stdlib", "systemc", "LeanAT", "tools", "tests/support", "tests/conformance"):
        for path in sorted((root / directory).rglob("*")):
            if path.is_file() and path.suffix in (".cpp", ".hpp", ".lean", ".py", ".txt", ".md"):
                hashes[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    counts = {status: sum(c["status"] == status for c in cases) for status in ("Pass", "Fail", "NotRun")}
    execution_ok = all(e["exit_code"] == 0 for e in executions)
    report = {"schema": "leanat.original-conformance.v1", "generated_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "gate_pass": counts["Pass"] == 70 and execution_ok, "counts": counts, "capability_policy": "All original IDs retained; enabled-group missing paths are NotRun, never N/A", "toolchain_lock": actual_toolchain, "executions": executions, "source_sha256": hashes, "cases": cases}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"report": str(args.output.resolve()), "gate_pass": report["gate_pass"], **counts}))
    if counts["Fail"] or not execution_ok:
        return 1
    return 0 if args.allow_incomplete or report["gate_pass"] else 2


if __name__ == "__main__":
    sys.exit(main())
