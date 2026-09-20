"""Bounded base-protocol transport capture and replay through actual runtime drivers."""
from pathlib import Path
import tempfile
from .io import ToolError, canonical, digest, read_json, write_json

CONFIG = {"profile": "AT-Core-1.1-draft", "scope": "two-connection-base-nb-v1",
          "domain": 1, "instance": 1, "connections": [1, 2],
          "descriptorIdentity": "base-transport-replay-v1", "timeResolutionUnitsPerTick": 1,
          "eventCapacity": 256, "pumpBudget": 64, "maxCallbacks": 4,
          "runtimeCapacities": {"instances":64,"ledgers":128,"calls":256,"callsPerLedger":64,"intents":128,"frames":64,"waits":128,"live":64,"drains":128,"hopsPerTransaction":8,"results":128,"consumers":256,"pins":128,"maxEvents":100000,"maxEventsPerTick":10000,"ingressBytes":65536,"nesting":16,"blocking":128},
          "rawDmi": False, "managedAccess": False, "extensions": []}


def fixture(reverse=True):
    import copy
    order = [2, 1] if reverse else [1, 2]
    return {"schema": "leanat.transport-fixture.v1", "effectiveConfig": copy.deepcopy(CONFIG),
            "requests": [{"data": [9, 9, 9, 9], "byteEnable": [255, 0]},
                         {"data": [8, 8, 8, 8], "byteEnable": [0, 255]}],
            "callbacks": [{"time": str(time), "connection": conn, "phase": phase,
                           "delay": "1", "data": ([9]*4 if conn == 1 else [8]*4)
                           if phase == 2 else ([10, 9, 30, 9] if conn == 1 else [8, 20, 8, 40])}
                          for time, phase in ((3, 2), (5, 3)) for conn in order],
            "feedback": [{"connection": c, "phase": p, "sync": 0 if p == 1 else 2,
                          "delay": "0"} for p, cs in ((1, [1, 2]), (4, [1, 2])) for c in cs]}


def _error(message):
    raise ToolError("InvalidTransportCapture", message)


def script(plan):
    def keys(obj, expected):
        if not isinstance(obj, dict) or set(obj) != set(expected):
            _error("missing or unsupported fixture fields")
    keys(plan, ("schema", "effectiveConfig", "requests", "callbacks", "feedback"))
    if plan.get("schema") != "leanat.transport-fixture.v1" or canonical(plan.get("effectiveConfig")) != canonical(CONFIG):
        _error("unsupported schema/configuration; no capability downgrade")
    requests, callbacks, feedback = (plan.get(k) for k in ("requests", "callbacks", "feedback"))
    if not isinstance(requests, list) or len(requests) != 2 or not isinstance(callbacks, list) or len(callbacks) != 4 or not isinstance(feedback, list) or len(feedback) != 4:
        _error("exactly two requests, four callbacks and four outgoing returns are required")
    def byte_line(values, count):
        if not isinstance(values, list) or len(values) != count or any(type(b) is not int or not 0 <= b <= 255 for b in values):
            _error("full bounded byte arrays required")
        return str(count) + " " + " ".join(map(str, values))
    def nat(value, limit):
        if not isinstance(value, str) or not value.isascii() or not value.isdecimal() or str(int(value)) != value or int(value) > limit:
            _error("canonical bounded decimal required")
        return value
    lines = ["LEANAT_TRANSPORT_V1"]
    for req in requests:
        keys(req, ("data", "byteEnable"))
        lines.extend([byte_line(req.get("data"), 4), byte_line(req.get("byteEnable"), 2)])
    lines.append("4")
    order = []
    for j, callback in enumerate(callbacks):
        keys(callback, ("time", "connection", "phase", "delay", "data"))
        conn, phase = callback.get("connection"), callback.get("phase")
        if type(conn) is not int or type(phase) is not int or conn not in (1, 2) or phase != (2 if j < 2 else 3):
            _error("only END_REQ then BEGIN_RESP callbacks are supported")
        if callback.get("time") != ("3" if j < 2 else "5") or callback.get("delay") != "1":
            _error("this scope requires same-tick callbacks at 3/5 with delay 1")
        order.append(conn)
        lines.append(f"{nat(callback['time'], 100)} {conn} {phase} {nat(callback['delay'], 10)} " + byte_line(callback.get("data"), 4))
    if sorted(order[:2]) != [1, 2] or order[:2] != order[2:]:
        _error("each connection must appear once per phase with stable observed order")
    for j, ret in enumerate(feedback):
        keys(ret, ("connection", "phase", "sync", "delay"))
        expected = (j+1, 1, 0) if j < 2 else (j-1, 4, 2)
        if any(type(ret.get(k)) is not int for k in ("connection", "phase", "sync")) or (ret.get("connection"), ret.get("phase"), ret.get("sync")) != expected or ret.get("delay") != "0":
            _error("missing or unsupported outgoing feedback")
        lines.append(f"{expected[0]} {expected[1]} {expected[2]} 0")
    return "\n".join(lines) + "\n"


def execute(plan, binary, output_dir):
    from .cli import capture
    source = script(plan)
    binary = Path(binary).resolve()
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="transport-", dir=output_dir) as temp:
        path = Path(temp) / "input.txt"
        path.write_text(source, encoding="ascii")
        result, command = capture([binary, path], timeout=30)
    if command["exitCode"] or result.get("schema") != "leanat.transport-run.v1" or result.get("complete") is not True or result.get("stop") != "Quiescent":
        raise ToolError("TransportExecutionFailed", str(command), 4)
    if len(result.get("records", [])) != 16 or len(result.get("semantic", [])) != 4 or len(result.get("milestones", [])) != 6:
        _error("missing actual boundary records or semantic events")
    command["binarySha256"] = digest(binary.read_bytes())
    command["scriptSha256"] = digest(source.encode("ascii"))
    return result, command


def capture_transport(plan, runtime, systemc, output):
    output = Path(output)
    native, ncmd = execute(plan, runtime, output.parent)
    kernel, kcmd = execute(plan, systemc, output.parent)
    if native != kernel:
        raise ToolError("TransportBackendMismatch", "actual runtime and SystemC boundaries differ", 4)
    oracle = transport_oracle(kernel, output.with_suffix(".oracle.json"))
    result = {"schema": "leanat.transport-capture.v1", "coverage": "FullForDeclaredScope",
              "effectiveConfig": CONFIG.copy(), "initialState": {"ledgers": "Idle", "requests": plan["requests"]},
              "environment": plan, "observed": kernel, "runtimeSystemCStatus": "Pass",
              "provenance": {"runtime": ncmd, "systemc": kcmd}, "independentOracle": oracle,
              "limitations": ["Two independent connections; this does not establish same-socket OOO routing",
                              "Fixed four-byte masked reads and scripted responder bytes",
                              "No blocking, DMI, extensions, reentrancy or arbitrary model loading"]}
    result["contentSha256"] = digest(canonical(result))
    write_json(output, result)
    if oracle["status"] != "Pass":
        raise ToolError("TransportOracleMismatch", "Independent trace differences retained in " + str(output.with_suffix(".oracle.json")), 4)
    return result


def replay_transport(captured, runtime, systemc, report):
    if not isinstance(captured, dict):
        _error("capture must be an object")
    if captured.get("schema") != "leanat.transport-capture.v1" or canonical(captured.get("effectiveConfig")) != canonical(CONFIG) or captured.get("coverage") != "FullForDeclaredScope":
        _error("unsupported capture scope/configuration")
    body = {k: v for k, v in captured.items() if k != "contentSha256"}
    if captured.get("contentSha256") != digest(canonical(body)):
        _error("capture content hash mismatch")
    plan = captured.get("environment", {})
    script(plan)
    if captured.get("initialState") != {"ledgers": "Idle", "requests": plan["requests"]}:
        _error("initial state mismatch")
    # Reconstruct every environment callback and outgoing return from actual capture.
    records = captured.get("observed", {}).get("records", [])
    if not isinstance(records, list) or len(records) != 16 or any(not isinstance(r, dict) for r in records):
        _error("exactly sixteen complete boundary records required")
    for ordinal, record in enumerate(records):
        if record.get("observedOrdinal") != str(ordinal) or record.get("callOrdinal") != str(ordinal//2+1):
            _error("missing or reordered observed/call ordinal")
        expected_kind = ("outgoing" if ordinal < 4 or ordinal >= 12 else "callback") + ("-call" if ordinal % 2 == 0 else "-return")
        if record.get("kind") != expected_kind:
            _error("missing call/return boundary")
        required = {"connection", "phase", "delay", "payload", "time"} if ordinal % 2 == 0 else {"connection", "callPhase", "sync", "phase", "delay", "response"}
        if not required <= record.keys():
            _error("incomplete boundary record")
        if ordinal % 2 == 0 and (not isinstance(record["payload"], dict) or "data" not in record["payload"]):
            _error("full payload missing")
    callbacks = [r for r in records if r.get("kind") == "callback-call"]
    feedback = [r for r in records if r.get("kind") == "outgoing-return"]
    replay_plan = dict(plan)
    replay_plan["callbacks"] = [{"time": r["time"], "connection": r["connection"], "phase": r["phase"], "delay": r["delay"], "data": r["payload"]["data"]} for r in callbacks]
    replay_plan["feedback"] = [{"connection": r["connection"], "phase": r["callPhase"], "sync": r["sync"], "delay": r["delay"]} for r in feedback]
    if replay_plan != plan:
        _error("observed callbacks/feedback do not bind declared environment")
    observations = {}
    for backend, binary in (("runtime", runtime), ("systemc", systemc)):
        actual, command = execute(replay_plan, binary, Path(report).parent)
        if actual != captured["observed"]:
            raise ToolError("TransportReplayMismatch", backend + " boundary/semantic trace differs", 4)
        observations[backend] = {"actual": actual, "command": command}
    oracle = transport_oracle(actual, Path(report).with_suffix(".oracle.json"))
    result = {"schema": "leanat.transport-replay-report.v1", "status": oracle["status"], "independentOracle": oracle,
              "captureSha256": captured["contentSha256"], "effectiveConfig": CONFIG,
              "scope": CONFIG["scope"], "executions": observations, "runtimeSystemCStatus": "Pass"}
    write_json(report, result)
    if oracle["status"] != "Pass":
        raise ToolError("TransportOracleMismatch", "Independent trace differences retained in " + str(Path(report).with_suffix(".oracle.json")), 4)
    return result


def conformance(runtime, systemc, output_dir):
    """Execute both observed callback orders; retain each capture and actual replay."""
    import copy
    output = Path(output_dir)
    output.mkdir(parents=True, exist_ok=True)
    captures, replays = {}, {}
    for name, reverse in (("forward", False), ("reverse", True)):
        capture_path, replay_path = output / f"{name}.capture.json", output / f"{name}.replay.json"
        try:
            captures[name] = capture_transport(fixture(reverse), runtime, systemc, capture_path)
        except ToolError as error:
            if error.code != "TransportOracleMismatch":
                raise
            captures[name] = read_json(capture_path)
        try:
            replays[name] = replay_transport(captures[name], runtime, systemc, replay_path)
        except ToolError as error:
            if error.code != "TransportOracleMismatch":
                raise
            replays[name] = read_json(replay_path)
    distinct = captures["forward"]["observed"]["records"] != captures["reverse"]["observed"]["records"]
    corrupted = copy.deepcopy(captures["reverse"])
    corrupted["observed"]["semantic"][0]["time"] = "99"
    corrupted["contentSha256"] = digest(canonical({k: v for k, v in corrupted.items() if k != "contentSha256"}))
    rejected = None
    try:
        replay_transport(corrupted, runtime, systemc, output / "unexpected-tamper-success.json")
    except ToolError as error:
        rejected = error.code
    rows = []
    divergent = divergent_outbound_check(runtime, output)
    for identity in ("C-T24", "E-T35"):
        independent_passed = all(r["status"] == "Pass" for r in replays.values())
        passed = distinct and rejected == "TransportReplayMismatch" and independent_passed and divergent["status"] == "Pass"
        rows.append({"id": identity, "backend": "runtime-systemc-transport",
                     "input": {"forward": fixture(False), "reverse": fixture(True)},
                     "expected": {"nonemptyCaptureReplay": True, "externalOrderDistinct": True,
                                  "semanticTraceTamperRejected": "TransportReplayMismatch"},
                     "actual": {"externalOrderDistinct": distinct, "semanticTraceTamperRejection": rejected,
                                "runtimeSystemCReplayStatus": "Pass", "independentOracleStatus": "Pass" if independent_passed else "Fail",
                                "divergentOutgoingRejectedBeforeFeedback": divergent,
                                "captures": {k: {"path": str(output / f"{k}.capture.json"),
                                                 "sha256": v["contentSha256"], "boundaryRecords": len(v["observed"]["records"])}
                                             for k, v in captures.items()},
                                "replays": replays},
                     "status": "Pass" if passed else "Fail",
                     "stop": "BoundedTransportReplayCompleted" if passed else "IndependentOracleMismatch" if not independent_passed else "TransportReplayAssertionFailure",
                     "scope": CONFIG["scope"], "limitations": captures["reverse"]["limitations"]})
    return rows


def model_input(observed):
    """Lossless admitted-boundary translation to the independent Lean transport oracle."""
    phases = {1: "beginReq", 2: "endReq", 3: "beginResp", 4: "endResp"}
    records = []
    for ordinal, record in enumerate(observed["records"]):
        common = {"id": str(ordinal//2+1), "ordinal": str(ordinal), "domain": "1", "observedKind": record["kind"],
                  "instance": "1", "localSide": "initiator", "connection": record["connection"]}
        if record["kind"].endswith("-call"):
            p = record["payload"]
            payload = dict(p, command={"Read": 0, "Write": 1, "Ignore": 2}[p["command"]],
                           length=len(p["data"]), streamingWidth=int(p["streamingWidth"]))
            records.append(dict(common, kind="call", transaction=record["transport"],
                                flow="forward" if record["flow"] == "fw" else "backward",
                                phase=phases[record["phase"]], callTime=record["time"],
                                incomingDelay=record["delay"], payload=payload))
        else:
            records.append(dict(common, kind="return", sync={0: "accepted", 1: "updated", 2: "completed"}[record["sync"]],
                                phase=None if record["phase"] is None else phases[record["phase"]],
                                outgoingDelay=record["delay"], response=record["response"]))
    return {"schema": "leanat.model-transport-input.v1", "config": {"maxOutstanding": 128,
            "maxPayloadBytes": 64, "maxByteEnableBytes": 64, "eventCapacity": 256, "maxEventsPerTick": 10000},
            "fuel": "100000", "horizon": None, "records": records}


def compare_model(observed, model):
    """Compare parsed wire fields and timed payloads, preserving both trace orders."""
    source = model_input(observed)
    errors = []
    if model.get("schema") != "leanat.model-transport-output.v1" or model.get("stopReason") != {"kind": "Quiescent"}:
        errors.append({"field": "stopReason", "expected": {"kind": "Quiescent"}, "actual": model.get("stopReason")})
    for name in ("remainingEvents", "remainingRecords", "pendingCalls"):
        if model.get(name) != []:
            errors.append({"field": name, "expected": [], "actual": model.get(name)})
    def normalized(record):
        result = dict(record)
        for key in ("connection", "domain", "instance", "id", "ordinal", "transaction", "callTime", "incomingDelay", "outgoingDelay"):
            if key in result:
                result[key] = str(result[key])
        if isinstance(result.get("payload"), dict):
            result["payload"] = dict(result["payload"])
            for key in ("address", "length", "streamingWidth"):
                result["payload"][key] = str(result["payload"][key])
        return result
    wire = [r for r in model.get("trace", []) if r.get("kind") in ("call", "return")]
    if len(wire) != len(source["records"]):
        errors.append({"field": "wireRecords", "expected": len(source["records"]), "actual": len(wire)})
    for index, (expected, actual) in enumerate(zip(source["records"], wire)):
        if actual.get("raw") != expected:
            errors.append({"field": f"wire[{index}].raw", "expected": expected, "actual": actual.get("raw")})
        expected_fields = normalized({k: v for k, v in expected.items() if k != "observedKind"})
        actual_fields = {k: actual.get(k) for k in expected_fields}
        if actual_fields != expected_fields:
            errors.append({"field": f"wire[{index}].parsed", "expected": expected_fields, "actual": actual_fields})
    timed = [r for r in model.get("trace", []) if r.get("kind") == "timed"]
    expected_timed = {}
    for cause in source["records"]:
        call = next(r for r in source["records"] if r["id"] == cause["id"] and r["kind"] == "call")
        event_kind = "input" if cause["kind"] == "call" else {"endReq": "requestReleased", "beginResp": "responseReady", "endResp": "terminal"}.get(call["phase"])
        if event_kind:
            expected_timed[(cause["ordinal"], event_kind)] = {"sequence": str(len(expected_timed)),
                "phase": call["phase"] if event_kind == "input" else None}
    actual_timed_keys = [(e.get("causedByOrdinal"), e.get("event")) for e in timed]
    if len(actual_timed_keys) != len(set(actual_timed_keys)) or set(actual_timed_keys) != set(expected_timed):
        errors.append({"field": "timedEventIdentities", "expected": list(expected_timed), "actual": actual_timed_keys})
    projected = []
    for event in timed:
        ordinal = event.get("causedByOrdinal")
        cause = next((r for r in source["records"] if r["ordinal"] == ordinal), None)
        call = next((r for r in source["records"] if cause and r["id"] == cause["id"] and r["kind"] == "call"), None)
        if not call:
            errors.append({"field": "timed.cause", "actual": event})
            continue
        expected_time = str(int(call["callTime"]) + int(call["incomingDelay"]))
        expected_payload = normalized(call)["payload"]
        expected_envelope = {"domain": call["domain"], "instance": call["instance"], "localSide": call["localSide"],
                             "connection": str(call["connection"]), "transaction": call["transaction"],
                             "causedByCallId": call["id"], "time": expected_time, "payload": expected_payload}
        expected_envelope.update(expected_timed.get((ordinal, event.get("event")), {}))
        expected_envelope.update(turn="0", stage="2" if event.get("event") == "input" else "3",
                                 keyConnection=str(call["connection"]) if event.get("event") == "input" else "0")
        if any(event.get(key) != value for key, value in expected_envelope.items()):
            errors.append({"field": "timed.envelope-payload-time", "expected": expected_envelope, "actual": event})
        if event.get("event") == "input" and call["observedKind"] == "callback-call":
            # RuntimeHostAdapter.snapshot_call exposes only callback-relevant fields.
            p = {"command": "Ignore", "address": "0", "data": [], "byteEnable": [],
                 "streamingWidth": "1", "status": 0, "dmiHint": False, "extensions": {}}
            if call["phase"] == "beginResp":
                p.update(data=event["payload"]["data"], status=event["payload"]["status"],
                         dmiHint=event["payload"]["dmiHint"], extensions=event["payload"]["extensions"])
            projected.append({"time": event["time"], "turn": event.get("turn"), "connection": int(event["connection"]),
                              "phase": {"endReq": 2, "beginResp": 3}[call["phase"]], "payload": p})
    if projected != observed["semantic"]:
        errors.append({"field": "callbackSemanticOrderAndValues", "expected": projected, "actual": observed["semantic"]})
    ready_keys = [tuple(int(e.get(k, -1)) for k in ("time", "turn", "stage", "keyConnection", "sequence")) for e in timed]
    if any(left >= right for left, right in zip(ready_keys, ready_keys[1:])):
        errors.append({"field": "timedEventKeyOrder", "actual": ready_keys})
    expected_hops = []
    for call in source["records"]:
        if call.get("phase") == "beginResp":
            expected_hops.append({"connection": str(call["connection"]), "transaction": call["transaction"], "state": "terminal",
                                  "response": normalized(call)["payload"], "requestReleasedRecorded": True,
                                  "responseReadyRecorded": True, "terminalRecorded": True})
    if sorted(model.get("hops", []), key=lambda h: h["connection"]) != sorted(expected_hops, key=lambda h: h["connection"]):
        errors.append({"field": "terminalHopStates", "expected": expected_hops, "actual": model.get("hops")})
    if sorted(observed.get("terminalTransports", [])) != sorted(h["transaction"] for h in expected_hops):
        errors.append({"field": "runtimeTerminalTransports", "expected": [h["transaction"] for h in expected_hops], "actual": observed.get("terminalTransports")})
    milestone_comparison = compare_milestones(observed.get("milestones", []), timed)
    errors.extend(milestone_comparison["differences"])
    return {"status": "Pass" if not errors else "Fail", "differences": errors,
            "wireFieldsCompared": len(wire), "timedSnapshotsCompared": len(timed),
            "projectedCallbackSemantics": projected,
            "dutMilestones": milestone_comparison,
            "projection": "Keep actual wire order; timed callback inputs use RuntimeHostAdapter snapshot_call payload projection; retain timed input order."}


def compare_milestones(actual, model_timed):
    reference = [e for e in model_timed if e.get("event") != "input"]
    differences, sequence_map = [], []
    if len(actual) != len(reference) or len(actual) != 6:
        differences.append({"field": "actualRuntimeMilestoneCoverage", "expected": 6, "actual": len(actual)})
    keys, tokens = [], []
    for index, (dut, model) in enumerate(zip(actual, reference)):
        response = {k: model["payload"][k] for k in ("status", "data", "dmiHint", "extensions")} if model["event"] == "responseReady" else None
        expected_key = {"time": model["time"], "turn": model["turn"], "stage": int(model["stage"]),
                        "instance": int(model["instance"]), "connection": int(model["keyConnection"])}
        expected = {"kind": model["event"], "key": expected_key, "domain": int(model["domain"]),
                    "instance": int(model["instance"]), "connection": int(model["connection"]),
                    "transport": model["transaction"], "transportGeneration": "1", "callId": model["causedByCallId"],
                    "milestoneTime": model["time"], "implicit": False, "response": response}
        projected = {k: dut.get(k) for k in expected}
        raw_key = dut.get("key", {})
        projected["key"] = {k: raw_key.get(k) for k in expected_key}
        if projected != expected:
            differences.append({"field": f"runtimeMilestones[{index}].key-cause-snapshot", "expected": expected, "actual": dut})
        try:
            sequence = raw_key["sequence"]
            if not isinstance(sequence, str) or str(int(sequence)) != sequence or not 0 <= int(sequence) < 2**64:
                raise ValueError("noncanonical sequence")
            keys.append(tuple(int(raw_key[k]) for k in ("time", "turn", "stage", "instance", "connection", "sequence")))
            token = dut["event"]
            if set(token) != {"kind", "domain", "store", "slot", "generation", "owner"} or token["domain"] != dut["domain"] or int(token["generation"]) < 1:
                raise ValueError("invalid actual event token")
            tokens.append(canonical(token))
            sequence_map.append({"cause": {"callId": dut["callId"], "kind": dut["kind"], "connection": dut["connection"]},
                                 "runtimeSequence": sequence, "modelSequence": model["sequence"]})
        except (KeyError, TypeError, ValueError):
            differences.append({"field": f"runtimeMilestones[{index}].raw-key-token", "actual": dut})
    if len(set(tokens)) != len(tokens) or len({row["runtimeSequence"] for row in sequence_map}) != len(sequence_map):
        differences.append({"field": "runtimeMilestoneTokenOrSequenceReuse", "actual": actual})
    if any(left >= right for left, right in zip(keys, keys[1:])):
        differences.append({"field": "actualRuntimeMilestoneKeyOrder", "actual": keys})
    return {"status": "Pass" if not differences else "Fail", "differences": differences,
            "actual": actual, "causalSequenceMap": sequence_map,
            "sequencePolicy": "Raw allocator-local sequence and event handles retained. Compare all other key fields exactly and preserve full-key order under the explicit causal sequence bijection; raw sequence numbers are not claimed equal."}


def divergent_outbound_check(binary, output_dir):
    from .cli import capture
    output_dir = Path(output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="outbound-negative-", dir=output_dir) as temporary:
        path = Path(temporary) / "input.txt"
        path.write_text(script(fixture(False)), encoding="ascii")
        _, command = capture([Path(binary).resolve(), path, "--divergent-outbound"], parse_json=False)
    passed = command["exitCode"] != 0 and "outgoing full payload/time diverged before replay feedback" in command["stderr"] and "feedbackConsumed=0" in command["stderr"]
    return {"status": "Pass" if passed else "Fail", "expected": "Actual published outgoing data differs from unchanged environment; reject before any feedback",
            "actual": command, "binarySha256": digest(Path(binary).read_bytes())}


def transport_oracle(observed, output):
    from .cli import capture, lean_argv, ROOT
    from .proof import semantic_sources
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    lake = lean_argv("LeanAT.ModelIR.TransportMain", "check")[0]
    _, build = capture([lake, "build", "LeanAT.ModelIR.TransportCLI"], parse_json=False)
    if build["exitCode"]:
        raise ToolError("TransportOracleBuildFailed", str(build), 3)
    sources = semantic_sources()
    input_path = output.with_suffix(".input.json")
    actual_path = output.with_suffix(".actual.json")
    write_json(input_path, model_input(observed))
    _, command = capture([lake, "env", "lean", "--run", ROOT / "LeanAT/ModelIR/TransportMain.lean", input_path, actual_path], parse_json=False)
    if command["exitCode"]:
        raise ToolError("TransportOracleFailed", str(command), 4)
    if semantic_sources() != sources:
        raise ToolError("ArtifactChanged", "Lean transport sources changed during oracle execution", 4)
    actual = read_json(actual_path)
    comparison = compare_model(observed, actual)
    result = {"schema": "leanat.transport-oracle-report.v1", **comparison, "actual": actual,
              "input": model_input(observed), "build": build, "command": command,
              "semanticSources": sources, "semanticSourcesSha256": digest(canonical(sources)),
              "coverage": "Independent Lean protocol transitions and timed payload snapshots; does not generate DUT intents or compute responder memory"}
    write_json(output, result)
    return result
