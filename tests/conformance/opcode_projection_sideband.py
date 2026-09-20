"""LoadInput host-state projection from independent source configuration.

The fourth argument is the actual reference input, not a backend expectation.
Raw outputs and generic VM state/counters remain the central runner's evidence.
"""
import copy

_WORLD_DEFAULTS = {"nextGeneration": "1", "generationLimit": str(2**64), "nextSequence": "1",
                   "maxObjects": "1024", "maxPins": "256", "maxEvents": "1024",
                   "maxBytes": "1048576", "maxObservations": "100000"}
_WORLD_LISTS = {"objects", "events", "observations", "allocationRules", "allocationCounters"}


def _world(world, complete=False):
    if not isinstance(world, dict) or set(world) - (set(_WORLD_DEFAULTS) | _WORLD_LISTS):
        raise ValueError("unknown sideband world field")
    if complete and set(world) != set(_WORLD_DEFAULTS) | _WORLD_LISTS:
        raise ValueError("complete sideband world required")
    result = {key: _nat(world.get(key, default)) for key, default in _WORLD_DEFAULTS.items()}
    for key in _WORLD_LISTS:
        rows = world.get(key, [])
        if not isinstance(rows, list):
            raise ValueError("world list required: " + key)
        result[key] = copy.deepcopy(rows)
    for key, fields, numbers, flags in (
        ("allocationRules", {"kind", "store", "group", "perSlot", "persistent", "allowMax", "capacity"},
         ("kind", "store", "capacity"), ("perSlot", "persistent", "allowMax")),
        ("allocationCounters", {"group", "domain", "slot", "nextGeneration", "persistent", "retired"},
         ("domain", "slot", "nextGeneration"), ("persistent", "retired"))):
        seen = set()
        for row in result[key]:
            if not isinstance(row, dict) or set(row) != fields or not isinstance(row["group"], str) or not row["group"]:
                raise ValueError("complete allocation journal row required")
            if any(type(row[flag]) is not bool for flag in flags):
                raise ValueError("allocation journal boolean required")
            for number in numbers:
                if number in ("capacity", "slot") and row[number] is None:
                    continue
                row[number] = _nat(row[number])
            identity = ((row["kind"], row["store"]) if key == "allocationRules" else
                        (row["group"], row["domain"], row["slot"]))
            if identity in seen:
                raise ValueError("duplicate allocation journal identity")
            seen.add(identity)
    return result


def _unchanged_world(outcome, input_value, side, unmapped):
    initial = _world(input_value.get("world", {}))
    actual = _world(outcome["world"], complete=True)
    for key in actual:
        if actual[key] != initial[key]:
            unmapped.append({"path": side + ".world." + key, "initial": initial[key], "actual": actual[key]})
    # Native sideband ports have no object allocator. Never manufacture a native
    # journal by copying the source journal or silently accept seeded objects.
    for key in ("objects", "events", "observations"):
        if initial[key]:
            unmapped.append({"path": "input.world." + key, "actual": initial[key]})
    return actual


def _nat(value):
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise ValueError("unsigned integer required")
    text = str(value)
    if not text.isascii() or not text.isdecimal() or str(int(text)) != text:
        raise ValueError("canonical unsigned integer required")
    return text


def _number(value):
    if not isinstance(value, dict) or set(value) != {"kind", "width", "value"}:
        raise ValueError("typed metadata integer required")
    if value["kind"] != "bits" or value["width"] != 64:
        raise ValueError("metadata requires Bits64")
    n = _nat(value["value"])
    if int(n) >= 2**64:
        raise ValueError("metadata overflow")
    return n


def _scalar(value, native=False):
    if native:
        if isinstance(value, dict) and set(value) == {"bool"} and type(value["bool"]) is bool:
            return {"bool": value["bool"]}
        if isinstance(value, dict) and set(value) == {"u64"}:
            n = _nat(value["u64"])
            if int(n) < 2**64:
                return {"integer": n}
    else:
        if isinstance(value, dict) and set(value) == {"kind", "value"} and value["kind"] == "bool" and type(value["value"]) is bool:
            return {"bool": value["value"]}
        if isinstance(value, dict) and set(value) == {"kind", "width", "value"} and value["kind"] == "bits":
            width = value["width"]
            n = _nat(value["value"])
            if type(width) is int and 1 <= width <= 64 and int(n) < 2**width:
                return {"integer": n}
    raise ValueError("complete scalar input value required")


def _rows(environment, key, count):
    value = environment[key]
    if set(value) != {"kind", "values"} or value["kind"] != "vec":
        raise ValueError("input environment requires vector")
    result = []
    for row in value["values"]:
        if set(row) != {"kind", "fields"} or row["kind"] != "record" or len(row["fields"]) != count:
            raise ValueError("input environment record arity")
        result.append(row["fields"])
    return result


def _expected(input_value):
    context = input_value["context"]
    if not isinstance(context, dict) or set(context) - {"kind", "now", "turn", "domain", "instanceId", "connection", "owner", "processIdentity", "inputs", "environment"}:
        raise ValueError("unknown sideband context field")
    environment = context["environment"]
    # JsonIO represents environment as [{name,value}], without lossy dict coercion.
    if isinstance(environment, list):
        if any(not isinstance(row, dict) or set(row) != {"name", "value"} for row in environment):
            raise ValueError("complete environment entry required")
        names = [row["name"] for row in environment]
        if len(set(names)) != len(names):
            raise ValueError("duplicate environment key")
        environment = {row["name"]: row["value"] for row in environment}
    owners = {}
    for instance, owner in _rows(environment, "runtime.owners", 2):
        instance, owner = _number(instance), _number(owner)
        if instance in owners:
            raise ValueError("duplicate owner instance")
        owners[instance] = owner
    samples = {}
    for instance, port, value in _rows(environment, "runtime.inputs", 3):
        key = (_number(instance), _number(port))
        if key in samples:
            raise ValueError("duplicate current sample")
        samples[key] = _scalar(value)
    ports = {}
    for instance, port, type_id in _rows(environment, "runtime.inputPorts", 3):
        instance, port, type_id = _number(instance), _number(port), _number(type_id)
        key = (instance, port)
        if key in ports or int(instance) >= 2**32 or int(port) >= 2**32:
            raise ValueError("invalid or duplicate input binding")
        ports[key] = {"instance": instance, "port": port, "owner": owners[instance],
                      "typeId": type_id, "ready": key in samples, "value": samples.get(key)}
    if samples.keys() - ports.keys():
        raise ValueError("sample without input binding")
    return [ports[key] for key in sorted(ports, key=lambda k: tuple(map(int, k)))]


def _native(snapshot, path, unmapped):
    if not isinstance(snapshot, dict) or "inputs" not in snapshot:
        raise ValueError("actual sideband provider snapshot required")
    for key in snapshot.keys() - {"inputs"}:
        unmapped.append({"path": path + "." + key, "actual": copy.deepcopy(snapshot[key])})
    result = []
    keys = set()
    for index, row in enumerate(snapshot["inputs"]):
        required = {"instance", "port", "owner", "typeId", "ready", "value"}
        if not required <= row.keys() or type(row["ready"]) is not bool:
            raise ValueError("complete actual input metadata required")
        for key in row.keys() - required:
            unmapped.append({"path": f"{path}.inputs[{index}].{key}", "actual": copy.deepcopy(row[key])})
        mapped = {key: _nat(row[key]) for key in ("instance", "port", "owner", "typeId")}
        identity = mapped["instance"], mapped["port"]
        if identity in keys:
            raise ValueError("duplicate actual input binding")
        keys.add(identity)
        mapped["ready"] = row["ready"]
        if row["ready"]:
            mapped["value"] = _scalar(row["value"], True)
        else:
            if row["value"] is not None:
                raise ValueError("unready input retains unexplained value")
            mapped["value"] = None
        result.append(mapped)
    return sorted(result, key=lambda row: (int(row["instance"]), int(row["port"])))


def _error_agreement(model, executable, native, source):
    if any(out.get("ok") is not False for out in (model, executable, native)) or model.get("error") != executable.get("error"):
        return None
    error = native.get("error")
    if not isinstance(error, dict) or set(error) != {"code", "message"}:
        return None
    ref = executable["error"]
    if ref == "ExecutionContextMismatch":
        # This is a rejection before entry, never evidence that opcode22 ran.
        if (error != {"code": "8", "message": "program context mismatch (program 0)"}
                or source["context"].get("kind") not in ("2", "3", "4", 2, 3, 4)
                or native.get("opcodeEvents") != [] or model.get("trace") != [] or executable.get("trace") != []
                or any(_nat(out["remainingFuel"]) != _nat(source["fuel"]) for out in (model, executable, native))):
            return None
        return {"verified": True, "referenceError": ref, "nativeError": copy.deepcopy(error),
                "opcodeExecuted": False, "rule": "Exact context preflight rejection; no opcode coverage credited."}
    pairs = {
        ("InputOwnerMismatch", "4", "input instance owner"),
        ("InputOwnerMismatch", "0", "unknown instance input port"),
        ("UnknownInputPort", "0", "unknown instance input port"),
        ("UnknownInputPort", "21", "input port index bound"),
        ("UnknownInputPort", "21", "input result type differs from port"),
        ("InputNotReady", "7", "input has no current sample"),
    }
    rows = native.get("opcodeEvents", [])
    if not rows or native.get("opcodeObservationComplete") is not True:
        return None
    row = rows[-1]
    if row.get("stage") != "error" or row.get("opcode") != 22 or row.get("error") != error or not row.get("source"):
        return None
    suffix = f" (program {row['program']}, block {row['block']}, opcode 22, {row['source']})"
    if not error["message"].endswith(suffix) or (ref, error["code"], error["message"][:-len(suffix)]) not in pairs:
        return None
    args = row.get("arguments")
    if not isinstance(args, list) or len(args) != 1:
        return None
    port = _number(args[0])
    for out, operation in ((model, "error:leanat.core.input.read"), (executable, "error:loadInput")):
        trace = out.get("trace", [])
        if not trace:
            return None
        last = trace[-1]
        if (last.get("operation") != operation or last.get("location") != row["source"]
                or _nat(last["program"]) != _nat(row["program"]) or last.get("args") != args
                or last.get("results") != [] or _nat(last["fuelAfter"]) != _nat(row["fuelAfter"])
                or _nat(out["remainingFuel"]) != _nat(row["fuelAfter"])):
            return None
    if _nat(executable["trace"][-1]["fuelBefore"]) != _nat(row["fuelBefore"]) or _nat(native["remainingFuel"]) != _nat(row["fuelAfter"]):
        return None
    context = source["context"]
    bindings = _expected(source)
    binding = next((b for b in bindings if b["instance"] == _nat(context.get("instanceId", 0)) and b["port"] == port), None)
    reason = error["message"][:-len(suffix)]
    if reason == "input instance owner" and (binding is None or binding["owner"] == _nat(context.get("owner", 0))):
        return None
    if reason == "unknown instance input port" and binding is not None:
        return None
    if reason == "input port index bound" and int(port) < 2**32:
        return None
    if reason == "input result type differs from port" and binding is None:
        return None
    if reason == "input has no current sample" and (binding is None or binding["ready"]):
        return None
    return {"verified": True, "referenceError": ref, "nativeError": copy.deepcopy(error),
            "opcodeExecuted": True, "source": row["source"], "arguments": copy.deepcopy(args),
            "rule": "Exact LoadInput cause/code, failed opcode, source, arguments and remaining fuel agree."}


def project(modelOutcome, execOutcome, nativeOutput, input_value=None):
    if input_value is None:
        raise ValueError("independent source input required for LoadInput projection")
    expected = _expected(input_value)
    unmapped = []
    # This provider allocates no world objects and schedules no events. Never
    # silently discard unexpected state introduced by a different provider.
    worlds = {side: _unchanged_world(outcome, input_value, side, unmapped)
              for side, outcome in (("model", modelOutcome), ("exec", execOutcome))}
    for key in ("hostEvents", "identities", "initialIdentities"):
        if nativeOutput.get(key):
            unmapped.append({"path": key, "actual": copy.deepcopy(nativeOutput[key])})
    actions = nativeOutput.get("actions")
    if not isinstance(actions, dict) or set(actions) != {"actions", "cursor", "failed"} or actions["actions"] != [] or _nat(actions["cursor"]) != "0" or actions["failed"] is not False:
        unmapped.append({"path": "actions", "actual": copy.deepcopy(actions)})
    central_fields = {"schema", "caseId", "variant", "descriptorHash", "ok", "exit", "error",
                      "remainingFuel", "returned", "committed", "preparedInputs", "initialProvider",
                      "provider", "initialIdentities", "identities", "actions", "hostEvents",
                      "opcodeObservationComplete", "opcodeEvents", "rawSegment", "actualValueNodeBytes", "stageBudget"}
    for key in nativeOutput.keys() - central_fields:
        unmapped.append({"path": key, "actual": copy.deepcopy(nativeOutput[key])})
    reference = {"initial": expected, "final": expected}
    return {"model": copy.deepcopy(reference), "exec": copy.deepcopy(reference),
            "native": {"initial": _native(nativeOutput["initialProvider"], "initialProvider", unmapped),
                       "final": _native(nativeOutput["provider"], "provider", unmapped)},
            "unmapped": unmapped, "identities": [],
            "allocationEvidence": {"input": _world(input_value.get("world", {})), **worlds,
                "native": {key: copy.deepcopy(nativeOutput.get(key)) for key in
                           ("initialIdentities", "identities", "actions", "hostEvents")}},
            "contextEvidence": copy.deepcopy(input_value["context"]),
            "errorAgreement": _error_agreement(modelOutcome, execOutcome, nativeOutput, input_value)}


def project_binding_failure(modelOutcome, execOutcome, nativeOutput, input_value=None):
    """Classify only the canonical input-provider ABI mismatch.

    The collector must have verified the actual descriptor hash and invoked all
    three real runners. The exact native binder and independent reference ABI
    errors are evidence; case names and arbitrary setup failures are not.
    """
    observations = {"model": copy.deepcopy(modelOutcome), "exec": copy.deepcopy(execOutcome),
                    "native": copy.deepcopy(nativeOutput)}
    if input_value is None:
        return {"accepted": False, "reason": "independent input missing", "observations": observations}
    try:
        _expected(input_value)
    except (ValueError, KeyError, TypeError):
        return {"accepted": False, "reason": "input fixture metadata invalid", "observations": observations}
    for outcome in (modelOutcome, execOutcome):
        if outcome.get("schema") != "leanat.reference-outcome.v1" or outcome.get("ok") is not False or outcome.get("error") != "RuntimeProviderABI":
            return {"accepted": False, "reason": "reference did not reject the actual ABI", "observations": observations}
        if outcome.get("returned") != [] or outcome.get("committed") != input_value.get("committed"):
            return {"accepted": False, "reason": "reference ABI failure published state/results", "observations": observations}
        try:
            changes = []
            _unchanged_world(outcome, input_value, "binding", changes)
            if changes:
                return {"accepted": False, "reason": "reference ABI failure changed allocation/world state", "observations": observations}
            if "fuel" in input_value and _nat(outcome["remainingFuel"]) != _nat(input_value["fuel"]):
                return {"accepted": False, "reason": "ABI preflight consumed instruction fuel", "observations": observations}
        except (ValueError, KeyError, TypeError):
            return {"accepted": False, "reason": "incomplete reference ABI failure world/fuel", "observations": observations}
    if set(nativeOutput) != {"schema", "error"} or nativeOutput.get("schema") != "leanat.opcode-native-error.v1" or nativeOutput.get("error") != "input provider ABI differs from descriptor":
        return {"accepted": False, "reason": "native error is not exact canonical ABI rejection", "observations": observations}
    return {"accepted": True, "reason": "independent canonical input ABI rejection; no opcode execution credited",
            "observations": observations}


def _self_test():
    """Negative controls mutate observations, never an expected-value oracle."""
    def u(n):
        return {"kind": "bits", "width": 64, "value": str(n)}
    def rows(values):
        return {"kind": "vec", "values": [{"kind": "record", "fields": v} for v in values]}
    source = {"context": {"environment": [
        {"name": "runtime.owners", "value": rows([[u(0), u(7)]])},
        {"name": "runtime.inputPorts", "value": rows([[u(0), u(2), u(1)]])},
        {"name": "runtime.inputs", "value": rows([[u(0), u(2), {"kind": "bool", "value": True}]])}]}}
    outcome = {"world": _world({})}
    row = {"instance": 0, "port": 2, "owner": 7, "typeId": 1, "ready": True, "value": {"bool": True}}
    native = {"initialProvider": {"inputs": [copy.deepcopy(row)]},
              "provider": {"inputs": [copy.deepcopy(row)]}, "identities": {}, "initialIdentities": {},
              "actions": {"actions": [], "cursor": 0, "failed": False}, "hostEvents": []}
    def check(output):
        return project(outcome, outcome, output, source)
    baseline = check(native)
    assert baseline["model"] == baseline["exec"] == baseline["native"] and not baseline["unmapped"]
    for field, value in (("port", 3), ("owner", 8), ("instance", 1), ("typeId", 9), ("value", {"bool": False})):
        changed = copy.deepcopy(native)
        changed["provider"]["inputs"][0][field] = value
        projected = check(changed)
        assert projected["model"] != projected["native"], field
    changed = copy.deepcopy(native)
    changed["provider"]["inputs"][0].update(ready=False, value=None)
    assert check(changed)["native"] != baseline["model"]
    for target in ("provider", "initialProvider"):
        changed = copy.deepcopy(native)
        changed[target]["unexpected"] = 1
        assert check(changed)["unmapped"]
    changed = copy.deepcopy(native)
    changed["provider"]["inputs"][0]["unknown"] = 1
    assert check(changed)["unmapped"]
    changed = copy.deepcopy(native)
    changed["actions"]["cursor"] = 1
    assert check(changed)["unmapped"]
    try:
        project(outcome, outcome, native)
    except ValueError:
        pass
    else:
        raise AssertionError("missing independent input accepted")
    source["committed"] = []
    failure = {"schema": "leanat.reference-outcome.v1", "ok": False,
               "error": "RuntimeProviderABI", "returned": [], "committed": [], "world": _world({})}
    binding = {"schema": "leanat.opcode-native-error.v1", "error": "input provider ABI differs from descriptor"}
    assert project_binding_failure(failure, failure, binding, source)["accepted"]
    for message in ("missing runtime.inputs", "prefix: input provider ABI differs from descriptor", "input instance owner"):
        bad = dict(binding, error=message)
        assert not project_binding_failure(failure, failure, bad, source)["accepted"]
    assert not project_binding_failure(dict(failure, error="InputNotReady"), failure, binding, source)["accepted"]
    assert not project_binding_failure(dict(failure, committed=[1]), failure, binding, source)["accepted"]
    print("sideband projection: independent input, authority/value/readiness and unknown-field controls passed")


if __name__ == "__main__":
    _self_test()
