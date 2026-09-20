"""Fail-closed pure/numeric correspondence; raw evidence stays with the caller.

ModelIR and ExecIR have distinct costs. Their complete fuel intervals are checked
independently; only ExecIR/native instruction costs are equated. No error-code,
source-path, trace-prefix, or provider-state mismatch is discarded.
"""
import re

try:
    from .opcode_projection_common import observed_value
except ImportError:
    from opcode_projection_common import observed_value


OPS = ("const", "move", "binary", "makeRecord", "getField", "makeVariant",
       "variantTag", "variantGet", "makeVec", "vecGet", "vecSet", "selectValue",
       "loadState", "bufferStateWrite", "check", "trace", "unary", "compare",
       "convert", "callPure", "getNow")
TERMS = {"return", "fail", "jump", "branch", "switch", "terminator"}
MODEL_OPS = {"literal", "local", "state", "select", "binary", "field", "index", "makeRecord",
             "makeVariant", "unary", "compare", "convert", "variantTag", "variantGet", "makeVec",
             "vecSet", "callPure", "branch", "repeat", "loop", "readNow", "letVal", "writeState",
             "check", "emit", "ret", "fail", "statement", "join"}
WORLD_DEFAULTS = {"nextGeneration": "1", "generationLimit": str(2**64), "nextSequence": "1",
                  "maxObjects": "1024", "maxPins": "256", "maxEvents": "1024",
                  "maxBytes": "1048576", "maxObservations": "100000"}


def number(value):
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise ValueError("exact integer required")
    text = str(value)
    if not re.fullmatch(r"0|[1-9][0-9]*", text):
        raise ValueError("canonical nonnegative integer required")
    return int(text)


def value(v):
    if not isinstance(v, dict):
        raise ValueError("owning typed value required")
    if v.get("kind") == "unit" or set(v) == {"unit"}:
        return {"unit": True}
    if v.get("kind") == "bool":
        return {"bool": v["value"]}
    if v.get("kind") == "variant":
        return [{"integer": str(number(v["tag"]))}, [value(x) for x in v["fields"]]]
    if v.get("kind") in ("vec", "record"):
        return [value(x) for x in v.get("fields", v.get("values"))]
    if set(v) == {"array"}:
        return [value(x) for x in v["array"]]
    return observed_value(v)


def allocation_journal(world):
    """Validate and retain all rule/counter fields; never renumber or sort them."""
    result = {}
    for key, fields in (
            ("allocationRules", {"kind", "store", "group", "perSlot", "persistent", "allowMax", "capacity"}),
            ("allocationCounters", {"group", "domain", "slot", "nextGeneration", "persistent", "retired"})):
        entries = world.get(key, [])
        if not isinstance(entries, list):
            raise ValueError("allocation journal must be a list")
        seen = set()
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != fields or not isinstance(entry["group"], str) or not entry["group"]:
                raise ValueError("complete allocation journal entry required")
            flags = ("perSlot", "persistent", "allowMax") if key == "allocationRules" else ("persistent", "retired")
            if any(type(entry[field]) is not bool for field in flags):
                raise ValueError("allocation flag must be boolean")
            if key == "allocationRules":
                if number(entry["kind"]) > 14 or number(entry["store"]) >= 2**32:
                    raise ValueError("allocation rule identity bound")
                if entry["capacity"] is not None and number(entry["capacity"]) >= 2**64:
                    raise ValueError("allocation capacity bound")
                identity = (number(entry["kind"]), number(entry["store"]))
            else:
                if number(entry["domain"]) >= 2**32 or number(entry["nextGeneration"]) > 2**64:
                    raise ValueError("allocation counter bound")
                if entry["slot"] is not None and number(entry["slot"]) >= 2**32:
                    raise ValueError("allocation slot bound")
                identity = (entry["group"], number(entry["domain"]), entry["slot"])
            if identity in seen:
                raise ValueError("duplicate allocation journal identity")
            seen.add(identity)
        result[key] = entries
    return result


def _trace_cost(outcome, initial, layer, unmapped):
    intervals = []
    branches, previous_after = [], None
    remaining = number(outcome["remainingFuel"])
    if remaining > initial:
        unmapped.append(layer + " fuel increased")
    for row in outcome["trace"]:
        if set(row) != {"layer", "program", "location", "operation", "args", "results", "fuelBefore", "fuelAfter"}:
            raise ValueError("complete execution trace row required")
        before, after = number(row["fuelBefore"]), number(row["fuelAfter"])
        operation = row["operation"].removeprefix("enter:").removeprefix("error:")
        if row["operation"].startswith("error:") and outcome.get("ok") is True:
            unmapped.append(layer + " successful outcome contains error trace")
        if operation not in (MODEL_OPS if layer == "ModelIR" else set(OPS) | TERMS):
            unmapped.append(layer + " unknown pure trace operation " + operation)
        exhausted = (outcome.get("error") == "FuelExhausted" and row["operation"].startswith("error:")
                     and before == after == remaining == 0)
        zero_join = False
        if layer == "ModelIR" and row["operation"] == "branch":
            branches.append((row["program"], row["location"]))
        if layer == "ModelIR" and operation == "join":
            key = (row["program"], row["location"])
            zero_join = (row["operation"] == "join" and branches and branches[-1] == key
                         and row["args"] == [] and row["results"] == []
                         and remaining <= before == after <= initial and previous_after == before)
            if not zero_join:
                unmapped.append("ModelIR join lacks its matching branch and exact zero-host-cost boundary")
            else:
                branches.pop()
        if layer == "ModelIR" and row["operation"] in ("ret", "fail") and branches:
            active = [(program, path) for program, path in branches if program == row["program"]]
            if not all(any(row["location"].startswith(path + arm) for arm in ("/yes/", "/no/"))
                       for _, path in active):
                unmapped.append("ModelIR branch fell through without its join trace")
            branches = [(program, path) for program, path in branches if program != row["program"]]
        if row["layer"] != layer or not row["location"] or not (remaining <= after < before <= initial or exhausted or zero_join):
            unmapped.append(layer + " invalid trace cost/location")
        intervals.append((after, before))
        previous_after = after
    cursor = remaining
    for lo, hi in sorted(intervals):
        if lo > cursor:
            unmapped.append(layer + " unobserved fuel interval " + str(cursor) + ":" + str(lo))
        cursor = max(cursor, hi)
    if cursor != initial:
        unmapped.append(layer + " incomplete final fuel interval")


def _native_rows(native, unmapped):
    stack, finished = [], []
    if native["opcodeObservationComplete"] is not True:
        unmapped.append("native opcode observer incomplete")
    for row in native["opcodeEvents"]:
        op = number(row["opcode"])
        if op >= len(OPS):
            raise ValueError("service opcode in pure family")
        key = tuple(row[k] for k in ("program", "block", "instruction", "callDepth", "opcode", "source"))
        if row["stage"] == "entered":
            if number(row["callDepth"]) != len(stack) or (stack and number(stack[-1][1]["opcode"]) != 19):
                unmapped.append("native pure call depth or parent differs")
            if row["fuelBefore"] != row["fuelAfter"] or row["result"] is not None or row["error"] is not None:
                unmapped.append("malformed native instruction entry")
            stack.append((key, row))
        elif row["stage"] in ("completed", "error"):
            if not stack or stack[-1][0] != key:
                unmapped.append("native instruction exit lacks matching entry")
            else:
                _, entry = stack.pop()
                if entry["arguments"] != row["arguments"] or entry["fuelBefore"] != row["fuelBefore"]:
                    unmapped.append("native instruction arguments or entry fuel changed")
            if number(row["fuelAfter"]) > number(row["fuelBefore"]):
                unmapped.append("native instruction fuel increased")
            if (row["stage"] == "error") != (row["error"] is not None):
                unmapped.append("native instruction error stage mismatch")
            finished.append(row)
        else:
            raise ValueError("unknown native opcode stage")
    if stack:
        unmapped.append("native unterminated instruction prefix")
    return finished


def _instruction_agreement(executable, rows, unmapped):
    reference = [r for r in executable["trace"] if not r["operation"].startswith("enter:") and r["operation"].removeprefix("error:") not in TERMS]
    if len(reference) != len(rows):
        unmapped.append("ExecIR/native executed instruction count differs")
    for ref, native in zip(reference, rows):
        operation = OPS[number(native["opcode"])]
        if native["stage"] == "error":
            operation = "error:" + operation
        location = native["source"] or "program/{}/block/{}/{}".format(native["program"], native["block"], native["instruction"])
        # Older reference error records contain the actual lowered location.
        if ref["operation"].startswith("error:") and ref["location"].startswith("program/"):
            location = "program/{}/block/{}/{}".format(native["program"], native["block"], native["instruction"])
        expected = {"operation": operation, "location": location, "program": str(number(native["program"])),
                    "args": native["arguments"], "results": [] if native["result"] is None else [native["result"]],
                    "fuelBefore": str(number(native["fuelBefore"])), "fuelAfter": str(number(native["fuelAfter"]))}
        actual = {key: ref[key] for key in expected}
        if actual != expected:
            unmapped.append({"reason": "ExecIR/native actual instruction differs", "reference": actual, "native": expected})


def _error_agreement(model, executable, native, rows):
    if model["ok"] or executable["ok"] or native["ok"] or model["error"] != executable["error"]:
        return None
    ref, error = executable["error"], native["error"]
    if isinstance(error, str):
        terminals = [r for r in executable["trace"] if r["operation"] != "error:terminator"]
        if error != ref or not terminals or terminals[-1]["operation"] != "fail":
            return None
        return {"verified": True, "referenceError": ref, "nativeError": error,
                "rule": "Explicit source and ExecIR fail terminator retain identical text."}
    failed = [row for row in rows if row["stage"] == "error"]
    if not failed or not isinstance(error, dict) or set(error) != {"code", "message"}:
        return None
    row = failed[-1]
    if row["error"] != error:
        return None
    opcode = number(row["opcode"])
    suffix = " (program {}, block {}, opcode {}{}{})".format(row["program"], row["block"], opcode,
        ", " if row["source"] else "", row["source"])
    if not error["message"].endswith(suffix):
        return None
    message = error["message"][:-len(suffix)]
    causes = {
        "ArithmeticOverflow": ("1", "ArithmeticOverflow", {18}),
        "LeanAT.Pure.Error.arithmeticOverflow": ("1", "checked addition", {2}),
        "LeanAT.Pure.Error.divisionByZero": ("0", "division by zero", {2}),
        "IndexOutOfRange": ("0", "vector index out of range", {9, 10}),
        "VariantTagMismatch": ("21", "variant tag mismatch", {7}),
        "NumericTypeMismatch": ("21", "NumericTypeMismatch", {16, 17, 18}),
    }
    rule = causes.get(ref)
    if ref == "FuelExhausted" and error["code"] == "12" and message == "instruction fuel exhausted" and row["fuelBefore"] == row["fuelAfter"] == native["remainingFuel"] == "0":
        rule = ("12", "instruction fuel exhausted", set(range(len(OPS))))
    if opcode == 14 and message == ref and error["code"] == "8" and row["arguments"] == [{"kind": "bool", "value": False}]:
        rule = ("8", ref, {14})
    if opcode == 19 and error["code"] == "8" and message == "pure call failed: " + ref:
        terminals = [r for r in executable["trace"] if r["operation"] == "fail"]
        if terminals and number(terminals[-1]["program"]) != number(row["program"]):
            rule = ("8", "pure call failed: " + ref, {19})
    if rule is None or (error["code"], message) != rule[:2] or opcode not in rule[2]:
        return None
    return {"verified": True, "referenceError": ref, "nativeError": error,
            "rule": "Exact reference cause, native code/message, executed opcode, and actual source location agree."}


def project(modelOutcome, execOutcome, nativeOutput, input_value=None):
    if input_value is None:
        raise ValueError("authoritative input/context/fuel required")
    unmapped = []
    context = input_value["context"]
    initial = number(input_value["fuel"])
    initial_journal = allocation_journal(input_value.get("world", {}))
    for side, out, layer in (("model", modelOutcome, "ModelIR"), ("exec", execOutcome, "ExecIR")):
        _trace_cost(out, initial, layer, unmapped)
        world = out["world"]
        if set(world) != set(WORLD_DEFAULTS) | {"objects", "events", "observations", "allocationRules", "allocationCounters"}:
            raise ValueError("complete pure reference world required")
        if allocation_journal(world) != initial_journal:
            unmapped.append(side + " pure allocation journal changed")
        for key, default in WORLD_DEFAULTS.items():
            expected = number(input_value.get("world", {}).get(key, default))
            if key == "generationLimit":
                expected = 2**64
            if number(world[key]) != expected:
                unmapped.append(side + " unexpected pure world bookkeeping: " + key)
        if out["world"]["objects"] or out["world"]["events"]:
            unmapped.append(side + " pure execution has owned objects/events")
    if input_value.get("world", {}).get("objects") or input_value.get("world", {}).get("events") or input_value.get("world", {}).get("observations"):
        unmapped.append("seeded pure world has no native provider correspondence")
    rows = _native_rows(nativeOutput, unmapped)
    _instruction_agreement(execOutcome, rows, unmapped)
    if execOutcome["remainingFuel"] != nativeOutput["remainingFuel"]:
        unmapped.append("ExecIR/native remaining fuel differs")
    if nativeOutput["provider"] or nativeOutput["initialProvider"] or nativeOutput["hostEvents"]:
        unmapped.append("pure execution changed provider or host state")
    if nativeOutput.get("identities") or nativeOutput.get("initialIdentities"):
        unmapped.append("pure execution has native allocation identities")
    if nativeOutput["actions"] != {"actions": [], "cursor": "0", "failed": False}:
        unmapped.append("pure execution produced committed actions")

    def reference(out):
        traces = []
        for o in out["world"]["observations"]:
            if set(o) != {"kind", "opcode", "source", "time", "turn", "values"} or o["kind"] != "trace":
                raise ValueError("unknown pure world observation")
            traces.append({"kind": o["opcode"], "source": o["source"], "ready": {"time": o["time"], "turn": o["turn"]},
                           "instance": str(number(context["instanceId"])), "connection": str(number(context["connection"])),
                           "values": [value(x) for x in o["values"]], "detail": ""})
        return {"traces": traces, "objects": out["world"]["objects"], "events": out["world"]["events"],
                "allocationJournal": allocation_journal(out["world"])}

    emitted = [r for r in rows if r["opcode"] == 15 and r["stage"] == "completed"]
    raw = nativeOutput.get("rawSegment")
    raw_traces = raw["traces"] if raw else []
    if len(emitted) != len(raw_traces):
        unmapped.append("native emitted trace prefix not fully retained")
    native_traces = []
    for trace, row in zip(raw_traces, emitted):
        if set(trace) != {"kind", "ready", "instance", "connection", "values", "detail"}:
            raise ValueError("complete native TraceEvent required")
        projected = dict(trace, source=row["source"], values=[value(x) for x in trace["values"]])
        if projected["values"] != [value(x) for x in row["arguments"]]:
            unmapped.append("native trace differs from actually executed trace operands")
        native_traces.append(projected)
    result = {"model": reference(modelOutcome), "exec": reference(execOutcome),
              "native": {"traces": native_traces, "objects": [], "events": [], "allocationJournal": initial_journal},
              "unmapped": unmapped, "identities": [],
              "costModel": "Separate complete ModelIR microstep intervals; exact ExecIR/native instruction fuel and source locations."}
    agreement = _error_agreement(modelOutcome, execOutcome, nativeOutput, rows)
    if agreement is not None:
        result["errorAgreement"] = agreement
    return result
