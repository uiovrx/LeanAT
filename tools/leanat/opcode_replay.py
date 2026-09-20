"""Actual opcode execution and strict, evidence-preserving comparisons."""
import copy
import json
import subprocess
from pathlib import Path
from .io import ToolError, canonical, digest, read_json, write_json

OPCODE_NAMES = ("const move binary makeRecord getField makeVariant variantTag variantGet makeVec vecGet vecSet selectValue "
    "loadState bufferStateWrite check trace unary compare convert callPure getNow getContextField loadInput bufferOutputWrite "
    "payloadGet bufferPayloadWrite extensionGet bufferExtensionWrite objectCall newTransaction stagePhase ackResponse cancelLocal "
    "resultGet resultRelease scheduleEvent cancelEvent spawnProcess trySpawnProcess registerWait readWaitResult setTransportReturn "
    "debugTransfer grantRawDmi denyDmi invalidateRawDmi waitGroupNew waitArm waitResultGet waitGroupRelease scopeNew scopeTransfer "
    "scopeCancel scopeClose submitTask cancelTask taskResultGet taskResultRelease requestTaskSlot requestManaged beginManagedRead "
    "beginManagedWrite invalidateManaged releaseLease callExternPure").split()


def execute_json(command, cwd, *, timeout=120):
    """Retain real exit/output; a provider rejection is not a process success."""
    from .cli import capture
    value, record = capture(command, cwd=cwd, timeout=timeout,
                            max_bytes=16 * 1024 * 1024)
    record["value"] = value
    return record


def bind_host_profile(input_value, host_info):
    if host_info.get("schema") != "leanat.opcode-host.v1":
        raise ToolError("OpcodeHostSchema", "missing actual native host information")
    size = host_info.get("valueNodeBytes")
    if not isinstance(size, str) or not size.isascii() or not size.isdecimal() or str(int(size)) != size or not 0 < int(size) <= 4096:
        raise ToolError("OpcodeHostSize", "invalid measured native Value size")
    result = copy.deepcopy(input_value)
    environment = result.setdefault("context", {}).setdefault("environment", [])
    if len({item["name"] for item in environment}) != len(environment):
        raise ToolError("OpcodeEnvironment", "duplicate environment entries")
    environment[:] = [item for item in environment if item["name"] != "profile.valueNodeBytes"]
    environment.append({"name": "profile.valueNodeBytes",
                        "value": {"kind": "bits", "width": 64, "value": size}})
    from .opcode_bounds import bind_segment_bytes, read_segment_bytes
    if any(item["name"] == "profile.segmentBytes" for item in environment):
        read_segment_bytes(result)
    else:
        result = bind_segment_bytes(result, 8 * 1024 * 1024)
    return result


def differences(left, right, path="$", limit=128):
    """Ordered structural comparison. Never sort events or erase identity fields."""
    found = []
    def walk(a, b, at):
        if len(found) >= limit:
            return
        if type(a) is not type(b):
            found.append({"path": at, "left": a, "right": b})
        elif isinstance(a, dict):
            for key in sorted(a.keys() | b.keys()):
                if key not in a or key not in b:
                    found.append({"path": f"{at}.{key}", "missing": "left" if key not in a else "right"})
                else:
                    walk(a[key], b[key], f"{at}.{key}")
        elif isinstance(a, list):
            if len(a) != len(b):
                found.append({"path": at + ".length", "left": len(a), "right": len(b)})
            for i, (x, y) in enumerate(zip(a, b)):
                walk(x, y, f"{at}[{i}]")
        elif a != b:
            found.append({"path": at, "left": a, "right": b})
    walk(left, right, path)
    return found


def actual_coverage(native, targets, expected_outcome):
    if native.get("schema") != "leanat.opcode-native-output.v1" or native.get("opcodeObservationComplete") is not True:
        return {"complete": False, "reason": "no complete actual interpreter observation", "completed": [], "errors": []}
    events = native.get("opcodeEvents", [])
    if expected_outcome == "preflight-failure":
        return {"complete": events == [], "completed": [], "errors": [],
                "observedEventCount": len(events), "reason": "preflight rejection credits no opcode execution"}
    completed = sorted({event["opcode"] for event in events if event.get("stage") == "completed"})
    errors = sorted({event["opcode"] for event in events if event.get("stage") == "error"})
    observed = set(completed if expected_outcome in ("success", "suspended") else completed + errors)
    missing = sorted(set(targets) - observed)
    return {"complete": not missing, "completed": completed, "errors": errors,
            "missing": missing, "observedEventCount": len(events)}


def validate_opcode_observations(native, summary):
    """Validate actual observer sequencing against the loaded artifact's PCs."""
    errors = []
    if not isinstance(summary, dict) or summary.get("schema") != "leanat.opcode-static.v1":
        return [{"reason": "validated descriptor instruction summary missing"}]
    if summary.get("descriptorHash") != native.get("descriptorHash"):
        return [{"reason": "observer descriptor hash differs from actual decoded artifact"}]
    locations = {}
    for program in summary["programs"]:
        for block in program["blocks"]:
            for ordinal, instruction in enumerate(block["instructions"]):
                key = (int(program["id"]), int(block["id"]), ordinal)
                if key in locations:
                    return [{"reason": "duplicate descriptor instruction address"}]
                locations[key] = instruction
    stack = []
    fields = {"stage", "program", "block", "instruction", "callDepth", "opcode", "source",
              "fuelBefore", "fuelAfter", "arguments", "result", "error"}
    for index, event in enumerate(native.get("opcodeEvents", [])):
        at = f"$.opcodeEvents[{index}]"
        if not isinstance(event, dict) or set(event) != fields:
            errors.append({"path": at, "reason": "observer fields are incomplete or unknown"})
            continue
        key = (event["program"], event["block"], event["instruction"])
        instruction = locations.get(key)
        if instruction is None:
            errors.append({"path": at, "reason": "instruction address absent from decoded artifact"})
            continue
        if event["opcode"] != int(instruction["opcode"]) or event["source"] != instruction["source"]:
            errors.append({"path": at, "reason": "opcode/source differs from decoded instruction"})
        if not isinstance(event["arguments"], list) or len(event["arguments"]) != len(instruction["args"]):
            errors.append({"path": at, "reason": "argument arity differs from decoded instruction"})
        try:
            before, after = int(event["fuelBefore"]), int(event["fuelAfter"])
            if before < 0 or after < 0 or after > before:
                raise ValueError()
        except (TypeError, ValueError):
            errors.append({"path": at, "reason": "invalid observed fuel interval"})
        if event["stage"] == "entered":
            if (event["result"] is not None or event["error"] is not None
                    or event["fuelBefore"] != event["fuelAfter"]
                    or (stack and event["callDepth"] != stack[-1]["callDepth"] + 1)
                    or (stack and stack[-1]["opcode"] != 19)
                    or (not stack and event["callDepth"] != 0)):
                errors.append({"path": at, "reason": "invalid instruction entry or nested call depth"})
            stack.append(event)
        elif event["stage"] in ("completed", "error"):
            if not stack:
                errors.append({"path": at, "reason": "completion/error has no matching entry"})
                continue
            entered = stack.pop()
            for field in ("program", "block", "instruction", "callDepth", "opcode", "source", "arguments", "fuelBefore"):
                errors.extend(differences(entered[field], event[field], at + "." + field))
            if event["stage"] == "completed" and (event["error"] is not None or ((instruction["destType"] is None) != (event["result"] is None))):
                errors.append({"path": at, "reason": "completed result/error shape differs from instruction"})
            if event["stage"] == "error" and (event["error"] is None or event["result"] is not None):
                errors.append({"path": at, "reason": "failed instruction omitted error or published result"})
        else:
            errors.append({"path": at, "reason": "unknown observer stage"})
    if stack:
        errors.append({"reason": "observer ended with unclosed instruction entries"})
    return errors


def compare_instruction_values(executable, native, identities, fuel_scopes=None):
    """Bind every actual instruction's ordered values and fuel to independent ExecIR."""
    terms = {"return", "fail", "jump", "branch", "switch", "terminator", "suspend", "transportReturn"}
    reference = [row for row in executable["trace"] if not row["operation"].startswith("enter:")
                 and row["operation"].removeprefix("error:") not in terms]
    observed, offsets, scope_stack = [], [], []
    for row in native["opcodeEvents"]:
        if not isinstance(row, dict) or not {"stage", "opcode", "program", "source", "arguments", "result", "fuelBefore", "fuelAfter"} <= row.keys():
            return [{"reason": "incomplete native instruction record"}]
        if row["stage"] == "entered":
            scope_stack.append(scope_stack[-1] if scope_stack else str(row["program"]))
            continue
        if row["stage"] not in ("completed", "error"):
            continue
        scope_program = scope_stack.pop() if scope_stack else str(row["program"])
        if fuel_scopes is not None and scope_program not in fuel_scopes:
            return [{"reason": "instruction fuel scope lacks an independent budget witness", "program": scope_program}]
        offsets.append(int(fuel_scopes[scope_program]["observerOffset"]) if fuel_scopes is not None else 0)
        opcode = row["opcode"]
        if not isinstance(opcode, int) or not 0 <= opcode < len(OPCODE_NAMES):
            return [{"reason": "unknown actual opcode in value comparison"}]
        observed.append({"operation": ("error:" if row["stage"] == "error" else "") + OPCODE_NAMES[opcode],
                         "program": str(row["program"]), "location": row["source"],
                         "args": row["arguments"], "results": [] if row["result"] is None else [row["result"]],
                         "fuelBefore": row["fuelBefore"], "fuelAfter": row["fuelAfter"]})
    fields = {"operation", "program", "location", "args", "results", "fuelBefore", "fuelAfter"}
    expected = [{key: row[key] for key in fields} for row in reference]
    for row, offset in zip(expected, offsets):
        for field in ("fuelBefore", "fuelAfter"):
            if int(row[field]) < offset:
                return [{"reason": "reference fuel below independently derived observer scope offset", "field": field}]
            row[field] = str(int(row[field]) - offset)
    errors = differences(identities.normalize(expected, "exec"), identities.normalize(observed, "native"), "$.instructionValues")
    for program, scope in (fuel_scopes or {}).items():
        if "nativeRemainingFuel" not in scope:
            continue
        rows = [row for row in executable["trace"] if str(row["program"]) == program]
        if not rows or rows[-1]["operation"] not in ("return", "fail", "suspend", "transportReturn", "error:terminator"):
            errors.append({"reason": "child fuel receipt lacks actual segment exit trace", "program": program})
            continue
        expected_remaining = int(rows[-1]["fuelAfter"]) - int(scope["observerOffset"])
        errors.extend(differences(str(expected_remaining), scope["nativeRemainingFuel"], "$.fuelScopes." + program + ".exitReceipt"))
    return errors


def instruction_fuel_scopes(case, input_value, projection):
    """Offsets come only from declared caps and authoritative invocation budgets."""
    programs = {str(program["id"]): program for program in case["descriptorSummary"]["programs"]}
    root = str(case["programId"])
    if root not in programs:
        raise ValueError("root instruction fuel scope absent from descriptor")
    def scope(program, initial, origin, native_initial=None):
        cap = int(programs[program]["instructionFuel"])
        budget = int(initial)
        native_budget = budget if native_initial is None else int(native_initial)
        if cap < 0 or budget < 0 or native_budget < 0:
            raise ValueError("negative fuel scope budget")
        available = min(native_budget, cap) if cap else native_budget
        if (min(budget, cap) if cap else budget) != available:
            raise ValueError("independent reference/native effective runtime budgets differ")
        return {"initialHostFuel": str(budget), "nativeInitialHostFuel": str(native_budget), "runtimeCap": str(cap),
                "observerOffset": str(budget - available), "origin": origin}
    scopes = {root: scope(root, input_value["fuel"], "authoritative root input and validated descriptor")}
    for witness in projection.get("fuelScopes", []):
        program = str(witness["program"])
        if program in scopes or program not in programs or witness.get("verified") is not True:
            raise ValueError("duplicate or unverified child fuel scope")
        if witness["runtimeCap"] != programs[program]["instructionFuel"] or not witness.get("evidence"):
            raise ValueError("child fuel scope budget/cap lacks matching actual evidence")
        scopes[program] = scope(program, witness["execInitialFuel"], witness["evidence"], witness["nativeInitialFuel"])
        constructor = witness["evidence"].get("nativeConstructorReceipt")
        if not isinstance(constructor, dict) or set(constructor) != {"program", "inputFuel", "runtimeCap", "remainingFuel"}:
            raise ValueError("child native fuel constructor receipt incomplete")
        if (str(constructor["program"]) != program or str(constructor["inputFuel"]) != witness["nativeInitialFuel"]
                or str(constructor["runtimeCap"]) != witness["runtimeCap"]):
            raise ValueError("child constructor receipt does not bind this invocation")
        remaining = int(constructor["remainingFuel"])
        if not 0 <= remaining <= min(int(witness["nativeInitialFuel"]), int(witness["runtimeCap"])):
            raise ValueError("child remaining fuel outside actual invocation budget")
        scopes[program]["nativeRemainingFuel"] = str(remaining)
    return scopes


def runtime_fuel_receipts(case, input_value, model, executable, native, systemc):
    """Check the program cap independently of the source evaluator's host work."""
    errors, evidence = [], {}
    def decimal(value, name):
        if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or str(int(value)) != value:
            raise ValueError(name + " must be a canonical decimal string")
        return int(value)
    for side, outcome in (("model", model), ("exec", executable)):
        if "runtimeFuelRemaining" not in outcome:
            errors.append({"path": f"$.{side}.runtimeFuelRemaining", "reason": "runtime fuel receipt missing"})
        else:
            receipt = outcome["runtimeFuelRemaining"]
            if receipt is not None:
                try:
                    decimal(receipt, "runtimeFuelRemaining")
                except ValueError as error:
                    errors.append({"path": f"$.{side}.runtimeFuelRemaining", "reason": str(error)})
            evidence[side] = receipt
    if len(evidence) == 2:
        errors.extend(differences(evidence["model"], evidence["exec"], "$.modelExec.runtimeFuelRemaining"))
    preflight = case["expectedOutcome"] in ("preflight-failure", "binding-failure")
    if preflight:
        for side, receipt in evidence.items():
            errors.extend(differences(None, receipt, f"$.{side}.preflight.runtimeFuelRemaining"))
        return errors, evidence
    try:
        programs = [program for program in case["descriptorSummary"]["programs"]
                    if int(program["id"]) == int(case["programId"])]
        if len(programs) != 1:
            raise ValueError("root program absent or duplicated in validated descriptor summary")
        cap = decimal(programs[0]["instructionFuel"], "descriptor instructionFuel")
        initial = decimal(input_value["fuel"], "input fuel")
        evidence["descriptorCap"] = str(cap)
        root_rows = [row for row in executable.get("trace", []) if int(row["program"]) == int(case["programId"])]
        if not root_rows:
            raise ValueError("root runtime receipt lacks actual ExecIR trace")
        errors.extend(differences(executable["remainingFuel"], root_rows[-1]["fuelAfter"], "$.exec.rootExitFuel"))
        for side, output in (("runtime", native), ("systemc", systemc)):
            remaining = decimal(output["remainingFuel"], "native remainingFuel")
            if remaining > initial:
                raise ValueError("native root segment fuel increased")
            spent = initial - remaining
            if cap and spent > cap:
                raise ValueError("native root segment exceeded descriptor instruction cap")
            receipt = str(cap - spent) if cap else None
            evidence[side] = {"initialHostFuel": str(initial), "remainingHostFuel": str(remaining),
                              "segmentSpent": str(spent), "runtimeFuelRemaining": receipt}
            for reference_side in ("model", "exec"):
                if reference_side in evidence:
                    errors.extend(differences(evidence[reference_side], receipt, f"$.{side}.{reference_side}.runtimeFuelRemaining"))
    except (KeyError, TypeError, ValueError) as error:
        errors.append({"path": "$.runtimeFuelRemaining", "reason": str(error)})
    return errors, evidence


def run_native(binary, root, case, input_value, output_dir):
    descriptor = Path(case["descriptor"])
    if not descriptor.is_absolute():
        descriptor = Path(root) / descriptor
    actual_hash = digest(descriptor.read_bytes())
    if actual_hash != case["descriptorHash"]:
        raise ToolError("OpcodeDescriptorHash", "catalog descriptor changed after emission")
    request = {"schema": "leanat.opcode-request.v1", "caseId": case["id"],
               "variant": case["variant"], "providerFamily": case["providerFamily"],
               "profile": case["profile"], "programId": case["programId"],
               "descriptor": str(descriptor.resolve()), "descriptorHash": actual_hash,
               "input": input_value}
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    request_path = output_dir / "request.json"
    write_json(request_path, request)
    result = execute_json([binary, request_path], root)
    result["inputHash"] = digest(canonical(input_value))
    result["binaryHash"] = digest(Path(binary).read_bytes())
    write_json(output_dir / "execution.json", result)
    return result


def execute_catalog_native(catalog_path, runtime, systemc, output_dir, root):
    """Diagnostic native pair execution; this does not evaluate E39 acceptance."""
    catalog_path = Path(catalog_path).resolve()
    base, output_dir = catalog_path.parent, Path(output_dir).resolve()
    catalog = read_json(catalog_path, max_bytes=16 * 1024 * 1024)
    if catalog.get("schema") != "leanat.opcode-catalog.v1":
        raise ToolError("OpcodeCatalogSchema", "actual source catalog required")
    binaries = {"runtime": Path(runtime).resolve(), "systemc": Path(systemc).resolve()}
    hosts = {side: execute_json([binary, "--host-info"], root) for side, binary in binaries.items()}
    if any(record["exitCode"] != 0 for record in hosts.values()) or hosts["runtime"]["value"] != hosts["systemc"]["value"]:
        raise ToolError("OpcodeHostMismatch", "native backends disagree about the actual value profile")
    report = {"schema": "leanat.opcode-native-batch.v1", "catalogHash": digest(catalog_path.read_bytes()),
              "hosts": hosts, "cases": [], "referenceExecuted": False, "release_gate": "not-evaluated"}
    seen = set()
    for case in catalog["cases"]:
        key = (case["id"], case["variant"])
        if key in seen or any(not item or item in (".", "..") or any(not (char.isascii() and (char.isalnum() or char in "-_.")) for char in item) for item in key):
            raise ToolError("OpcodeCaseIdentity", "duplicate or unsafe source case identity")
        seen.add(key)
        def bound(reference):
            path = (base / reference["path"]).resolve()
            if not path.is_relative_to(base) or digest(path.read_bytes()) != reference["sha256"]:
                raise ToolError("OpcodeCatalogHash", "catalog artifact path or hash mismatch")
            return path
        descriptor, source_input = bound(case["descriptor"]), bound(case["referenceInput"])
        prepared = bind_host_profile(read_json(source_input), hosts["runtime"]["value"])
        actual_case = {**case, "descriptor": str(descriptor), "descriptorHash": case["descriptor"]["sha256"]}
        folder = output_dir / case["id"] / case["variant"]
        write_json(folder / "input.json", prepared)
        runs = {side: run_native(binary, root, actual_case, prepared, folder / side)
                for side, binary in binaries.items()}
        report["cases"].append({"id": key[0], "variant": key[1], "providerFamily": case["providerFamily"],
                                "inputHash": digest(canonical(prepared)),
                                "executions": {side: str(folder / side / "execution.json") for side in runs},
                                "exitCodes": {side: run["exitCode"] for side, run in runs.items()},
                                "differences": differences(runs["runtime"]["value"], runs["systemc"]["value"])})
        write_json(output_dir / "native-batch.json", report)
    return report


def compare_case(case, input_value, model, exec, native, systemc, root):
    """Compare actual outcomes; unresolved semantic projections cannot pass."""
    import importlib
    import inspect
    from tests.conformance.opcode_projection_common import IdentityBijection
    failures, unmapped = [], []
    result = {"status": "NotRun", "differences": failures, "unmapped": unmapped,
              "coverage": {}, "identityMappings": {}, "projections": {}}
    targets = case["expectedOpcodeTags"]
    expected = case["expectedOutcome"]
    if expected == "binding-failure":
        module = importlib.import_module("tests.conformance.opcode_projection_" + case["providerFamily"])
        predicate = getattr(module, "project_binding_failure", None)
        if predicate is None:
            unmapped.append({"reason": "no precise binding-rejection correspondence"})
            return result
        checks = {side: predicate(model, exec, value, input_value)
                  for side, value in (("runtime", native), ("systemc", systemc))}
        result["bindingRejection"] = checks
        result["coverage"] = {side: {"complete": False, "completed": [], "errors": [],
                                     "reason": "binding rejection precedes opcode execution"}
                              for side in checks}
        receipt_errors, receipt_evidence = runtime_fuel_receipts(case, input_value, model, exec, native, systemc)
        failures.extend(receipt_errors)
        result["runtimeFuelReceipts"] = receipt_evidence
        result["status"] = "Pass" if not failures and all(item.get("accepted") is True for item in checks.values()) else "Fail"
        return result
    for side, value in (("runtime", native), ("systemc", systemc)):
        result["coverage"][side] = actual_coverage(value, targets, expected)
        if value.get("schema") == "leanat.opcode-native-output.v1":
            if "descriptorSummary" not in case:
                unmapped.append({"side": side, "reason": "actual descriptor observer binding unavailable"})
                result["coverage"][side]["complete"] = False
                result["coverage"][side]["completed"] = []
                result["coverage"][side]["errors"] = []
            else:
                observed_errors = validate_opcode_observations(value, case["descriptorSummary"])
                failures.extend({"side": side, **item} for item in observed_errors)
                if observed_errors:
                    result["coverage"][side].update(complete=False, completed=[], errors=[])
        if value.get("schema") != "leanat.opcode-native-output.v1":
            unmapped.append({"side": side, "reason": "native setup/preflight outcome has no complete VM observation", "actual": value})
    if any(value.get("schema") != "leanat.opcode-native-output.v1" for value in (native, systemc)):
        return result
    segment_entries = [entry["value"] for entry in input_value.get("context", {}).get("environment", [])
                       if entry.get("name") == "profile.segmentBytes"]
    if len(segment_entries) != 1:
        unmapped.append({"reason": "explicit staging budget missing from executed input"})
    else:
        expected_budget = {"bytes": segment_entries[0]["value"], "writes": "4096", "actions": "4096", "events": "4096"}
        for side, value in (("runtime", native), ("systemc", systemc)):
            failures.extend(differences(expected_budget, value.get("stageBudget"), f"$.{side}.stageBudget"))
    if model.get("schema") != "leanat.reference-outcome.v1" or exec.get("schema") != "leanat.reference-outcome.v1":
        raise ToolError("OpcodeReferenceSchema", "independent reference outcomes missing")
    if "descriptorSummary" in case:
        receipt_errors, receipt_evidence = runtime_fuel_receipts(case, input_value, model, exec, native, systemc)
        failures.extend(receipt_errors)
        result["runtimeFuelReceipts"] = receipt_evidence
    if expected not in ("success", "suspended", "failure", "preflight-failure"):
        raise ToolError("OpcodeExpectedOutcome", "unsupported source case outcome constraint")
    for side, value in (("model", model), ("exec", exec), ("runtime", native), ("systemc", systemc)):
        if value.get("ok") is not (expected not in ("failure", "preflight-failure")):
            failures.append({"path": f"$.{side}.ok", "expectedOutcome": expected,
                             "actual": value.get("ok")})
        if expected == "suspended" and value.get("exit") != "suspended":
            failures.append({"path": f"$.{side}.exit", "expected": "suspended", "actual": value.get("exit")})
    if expected == "preflight-failure":
        if case["providerFamily"] != "sideband":
            unmapped.append({"reason": "no reviewed preflight correspondence for this provider"})
        for side, value in (("model", model), ("exec", exec), ("runtime", native), ("systemc", systemc)):
            for field, wanted in (("remainingFuel", input_value.get("fuel")),
                                  ("committed", input_value.get("committed", [])), ("returned", [])):
                failures.extend(differences(wanted, value.get(field), f"$.{side}.preflight.{field}"))
            if side in ("model", "exec"):
                failures.extend(differences(input_value.get("world"), value.get("world"), f"$.{side}.preflight.world"))
                failures.extend(differences([], value.get("trace"), f"$.{side}.preflight.trace"))
            else:
                failures.extend(differences(value.get("initialProvider"), value.get("provider"), f"$.{side}.preflight.provider"))
                failures.extend(differences([], value.get("hostEvents"), f"$.{side}.preflight.hostEvents"))
                failures.extend(differences({"actions": [], "cursor": "0", "failed": False}, value.get("actions"), f"$.{side}.preflight.actions"))
    for field in ("ok", "returned", "committed", "world", "exit", "wait", "outcomeType"):
        failures.extend(differences(model[field], exec[field], "$.modelExec." + field))
    # Failure cause is compared between independent evaluators; native codes are
    # different enums and require the family to provide an explicit correspondence.
    if not model["ok"]:
        failures.extend(differences(model["error"], exec["error"], "$.modelExec.error"))
    family = "object" if case["providerFamily"] == "objects" else case["providerFamily"]
    if family == "sideband":
        for field in ("allocationRules", "allocationCounters"):
            if field not in exec["world"]:
                unmapped.append({"reason": "sideband allocation journal missing", "field": field})
            else:
                failures.extend(differences(input_value.get("world", {}).get(field, []),
                                            exec["world"][field], "$.sideband.unchanged." + field))
    mapper = None
    module_name = "tests.conformance.opcode_projection_" + ("pure" if family == "numeric" else family)
    try:
        mapper = importlib.import_module(module_name).project
    except ModuleNotFoundError as error:
        if error.name != module_name:
            raise
        if family not in ("pure", "numeric"):
            unmapped.append({"family": family, "reason": "complete provider semantic projection unavailable"})
    for side, value in (("runtime", native), ("systemc", systemc)):
        identities = IdentityBijection([])
        projection = {}
        if mapper:
            args = (model, exec, value, input_value) if len(inspect.signature(mapper).parameters) >= 4 else (model, exec, value)
            projection = mapper(*args)
            result["projections"][side] = projection
            if not isinstance(projection, dict) or not {"model", "exec", "native"} <= projection.keys():
                unmapped.append({"side": side, "reason": "provider did not supply a complete semantic projection"})
                projection = {}
            else:
                identities = IdentityBijection(projection.get("identities", []))
                result["identityMappings"][side] = identities.entries
                unmapped.extend({"side": side, "detail": item} for item in projection.get("unmapped", []))
                for left, right in (("model", "exec"), ("exec", "native")):
                    failures.extend(differences(identities.normalize(projection[left], left),
                                                identities.normalize(projection[right], right),
                                                f"$.{side}.provider.{left}.{right}"))
        elif family in ("pure", "numeric"):
            if value["provider"] != {} or value["initialProvider"] != {}:
                failures.append({"path": f"$.{side}.provider", "reason": "pure program acquired provider state"})
            if input_value.get("world", {}).get("objects") or input_value.get("world", {}).get("events"):
                unmapped.append({"side": side, "reason": "pure fixture has seeded world absent native provider"})
            if exec.get("world", {}).get("observations") or (value.get("rawSegment") or {}).get("traces"):
                unmapped.append({"side": side, "reason": "pure trace semantic projection unavailable"})
        for field in ("ok", "returned", "committed"):
            failures.extend(differences(identities.normalize(exec[field], "exec"),
                                        identities.normalize(value[field], "native"), f"$.{side}.{field}"))
        if "inputs" in input_value:
            failures.extend(differences(identities.normalize(input_value["inputs"], "exec"),
                                        identities.normalize(value["preparedInputs"], "native"),
                                        f"$.{side}.preparedInputs"))
        if "trace" not in exec:
            unmapped.append({"side": side, "reason": "independent ExecIR instruction trace missing"})
        elif "descriptorSummary" in case:
            try:
                scopes = instruction_fuel_scopes(case, input_value, projection) if value.get("opcodeEvents") else {}
                result.setdefault("fuelScopes", {})[side] = scopes
                failures.extend({"side": side, **item} for item in compare_instruction_values(exec, value, identities, scopes))
            except (KeyError, TypeError, ValueError) as error:
                failures.append({"side": side, "path": "$.fuelScopes", "reason": str(error)})
        expected_exit = {"transport": "transportReturn"}.get(exec["exit"], exec["exit"])
        if exec["ok"]:
            failures.extend(differences(expected_exit, value["exit"], f"$.{side}.exit"))
        elif value.get("error") in (None, ""):
            failures.append({"path": f"$.{side}.error", "reason": "failed segment omitted its actual error"})
        elif not exec["ok"] and not (isinstance(projection.get("errorAgreement"), dict) and projection["errorAgreement"].get("verified") is True):
            unmapped.append({"side": side, "reason": "native/reference failure causes need explicit semantic correspondence",
                             "referenceError": exec["error"], "nativeError": value["error"]})
        failures.extend(differences(exec["remainingFuel"], value["remainingFuel"], f"$.{side}.remainingFuel"))
        if not result["coverage"][side]["complete"]:
            unmapped.append({"side": side, "reason": "target opcode was not fully observed executing"})
    # C4/C5 execute the same native contract; preserve every native output field.
    failures.extend(differences(native, systemc, "$.runtimeSystemc"))
    result["status"] = "Fail" if failures else "NotRun" if unmapped else "Pass"
    return result
