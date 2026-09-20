"""Full owned-payload projection for E39; never supplies expected provider values.

Reference-only global allocation leases and host bounds are retained as diagnostics:
the payload shadow uses a composite key and has no corresponding global object store.
Actual payload/extension bounds and every authority field remain in the comparison.
"""
from copy import deepcopy

_FIELDS = ("transaction", "hop", "instance", "localSide", "target", "writable", "command",
           "address", "streamingWidth", "baseline", "data", "byteEnable", "status", "dmiHint",
           "maxBytes", "extensions", "connection")
_WORLD = {"objects", "events", "observations", "allocationRules", "allocationCounters", "nextGeneration", "generationLimit", "nextSequence",
          "maxObjects", "maxPins", "maxEvents", "maxBytes", "maxObservations"}
_ID = {"kind", "domain", "store", "slot", "generation", "owner"}


def _integer(value, bits=64):
    if isinstance(value, bool) or not isinstance(value, (int, str)):
        raise ValueError("integer type")
    text = str(value)
    if not text.isascii() or not text.isdecimal() or str(int(text)) != text:
        raise ValueError("noncanonical unsigned integer")
    number = int(text)
    if not 0 <= number < 2 ** bits:
        raise ValueError("integer range")
    return number


def _handle(value):
    if not isinstance(value, dict) or set(value) != _ID:
        raise ValueError("complete raw handle required")
    return {key: str(_integer(value[key], 32 if key in ("kind", "domain", "store", "slot") else 64))
            for key in sorted(_ID)}


def _value(value, native=False):
    if not isinstance(value, dict):
        raise ValueError("owned value object required")
    if native:
        if len(value) != 1:
            raise ValueError("unknown native value field")
        key, item = next(iter(value.items()))
        if key == "array" and isinstance(item, list):
            return [_value(x, True) for x in item]
        if key == "u64":
            return _integer(item)
        if key == "bytes" and isinstance(item, list):
            return bytes(_integer(x, 8) for x in item)
        if key == "bool" and isinstance(item, bool):
            return item
        if key == "unit" and item is None:
            return None
        if key == "handle":
            return _handle(item)
        raise ValueError("unsupported native value variant")
    kind = value.get("kind")
    expected = {"unit": {"kind"}, "bool": {"kind", "value"}, "bits": {"kind", "width", "value"},
                "bytes": {"kind", "data"}, "handle": {"kind", "identity"},
                "record": {"kind", "fields"}, "vec": {"kind", "values"}}
    if kind not in expected or set(value) != expected[kind]:
        raise ValueError("unknown reference value fields")
    if kind == "unit":
        return None
    if kind == "bool" and isinstance(value["value"], bool):
        return value["value"]
    if kind == "bits" and value["width"] == 64:
        return _integer(value["value"])
    if kind == "bytes" and isinstance(value["data"], list):
        return bytes(_integer(x, 8) for x in value["data"])
    if kind == "handle":
        return _handle(value["identity"])
    if kind in ("record", "vec"):
        return [_value(x) for x in value["fields" if kind == "record" else "values"]]
    raise ValueError("payload scalar type")


def _payload(value, native=False):
    values = _value(value, native)
    if not isinstance(values, list) or len(values) != len(_FIELDS):
        raise ValueError("payload codec requires 17 fields")
    p = dict(zip(_FIELDS, values))
    for key in ("transaction", "hop"):
        p[key] = _handle(p[key])
    if p["transaction"]["kind"] != "0" or p["hop"]["kind"] != "1":
        raise ValueError("payload identity kinds")
    for key in ("instance", "localSide", "connection"):
        p[key] = _integer(p[key], 32)
    for key in ("command", "address", "streamingWidth", "status", "maxBytes"):
        p[key] = _integer(p[key])
    if p["command"] > 2 or p["status"] > 6 or not p["streamingWidth"]:
        raise ValueError("payload field range")
    for key in ("target", "writable", "dmiHint"):
        if not isinstance(p[key], bool):
            raise ValueError("payload flag type")
    for key in ("baseline", "data", "byteEnable"):
        if not isinstance(p[key], bytes):
            raise ValueError("payload buffer type")
        p[key] = list(p[key])
    if len(p["data"]) != len(p["baseline"]):
        raise ValueError("payload snapshot length")
    extensions, names = [], set()
    if not isinstance(p["extensions"], list):
        raise ValueError("extension registry type")
    for extension in p["extensions"]:
        if not isinstance(extension, list) or len(extension) != 6:
            raise ValueError("extension codec")
        name, bound, request, response, writable, data = extension
        if not isinstance(name, bytes) or not name:
            raise ValueError("extension name")
        name = name.decode("utf-8", errors="strict")
        if name in names:
            raise ValueError("duplicate extension")
        names.add(name)
        bound = _integer(bound)
        if not all(isinstance(flag, bool) for flag in (request, response, writable)):
            raise ValueError("extension permission type")
        if writable and not response:
            raise ValueError("extension permission invariant")
        if data is not None and (not isinstance(data, bytes) or len(data) > bound):
            raise ValueError("extension value capacity")
        extensions.append({"name": name, "maxBytes": bound, "requestAllowed": request,
                           "responseAllowed": response, "responseWritable": writable,
                           "value": None if data is None else list(data)})
    p["extensions"] = sorted(extensions, key=lambda e: e["name"])
    aggregate = len(p["data"]) + len(p["byteEnable"]) + sum(
        (len(e["name"].encode("utf-8")) + len(e["value"])) if e["value"] is not None else 0 for e in extensions)
    if aggregate > p["maxBytes"]:
        raise ValueError("payload aggregate capacity")
    return p


def _reference(outcome, side, unmapped):
    world = outcome.get("world")
    if not isinstance(world, dict) or set(world) != _WORLD:
        raise ValueError(side + ": unknown or missing world field")
    entries = world["objects"]
    if not isinstance(entries, list) or len(entries) != 1:
        raise ValueError(side + ": payload fixture must expose exactly one view")
    entry = entries[0]
    if set(entry) != {"identity", "tag", "value", "alive"} or entry["tag"] != "storage.payload" or entry["alive"] is not True:
        raise ValueError(side + ": unmapped world object")
    view = _handle(entry["identity"])
    payload = _payload(entry["value"])
    if view["domain"] != payload["transaction"]["domain"] or view["owner"] != payload["transaction"]["owner"]:
        raise ValueError(side + ": view authority mismatch")
    if world["events"]:
        unmapped.append(side + ": payload program produced reference events")
    if world["observations"]:
        unmapped.append(side + ": payload program produced reference observations")
    diagnostic = {key: deepcopy(value) for key, value in world.items() if key not in ("objects", "events", "observations")}
    diagnostic["viewIdentity"] = view
    return {"payload": payload, "events": deepcopy(world["events"]),
            "observations": deepcopy(world["observations"]), "actions": [], "actionCursor": 0,
            "actionFailed": False}, diagnostic


def _error_agreement(model, executed, native):
    if model.get("ok") is not False or executed.get("ok") is not False or native.get("ok") is not False:
        return None
    reference = model.get("error")
    if not isinstance(reference, str) or executed.get("error") != reference:
        return None
    actual = native.get("error")
    if reference == "intentional post-write rollback" and actual == reference:
        return {"verified": True, "referenceError": reference, "nativeError": actual,
                "rule": "Identical explicit source fail after staged write; both discard the segment."}
    if not isinstance(actual, dict) or set(actual) != {"code", "message"}:
        return None
    rules = {
        ("PayloadWrongLocalView", "4", "payload transaction is not current view"):
            "Exact transaction identity differs from the bound local view, including stale generation.",
        ("PayloadWrongLocalView", "4", "payload local view"):
            "Execution connection/instance/owner differs from the stored payload-view context.",
        ("PayloadDataLength", "21", "payload data shape"):
            "READ response byte count differs from the frozen request byte count.",
        ("PayloadWriteCommand", "8", "illegal payload data write"):
            "Data mutation requires a target writable READ request; WRITE data is frozen.",
        ("PayloadWritePermission", "8", "payload response write permission"):
            "Payload field write requires the bound response-write permit and target phase.",
        ("PayloadWritePermission", "8", "extension response write permission"):
            "Extension write requires the bound response-write permit and target phase.",
    }
    message = actual["message"]
    # The VM appends the actual instruction site. Remove it only when the
    # native error observation and both independent traces witness that site.
    if isinstance(message, str) and " (program " in message:
        errors = [event for event in native.get("opcodeEvents", []) if event.get("stage") == "error"]
        if len(errors) != 1 or errors[0].get("error") != actual:
            return None
        event = errors[0]
        opcode_names = {24: "payloadGet", 25: "bufferPayloadWrite", 26: "extensionGet", 27: "bufferExtensionWrite"}
        opcode = event.get("opcode")
        if opcode not in opcode_names:
            return None
        suffix = f" (program {event.get('program')}, block {event.get('block')}, opcode {opcode}, {event.get('source')})"
        if not message.endswith(suffix):
            return None
        for outcome in (model, executed):
            trace = outcome.get("trace", [])
            if not trace:
                return None
            witness = trace[-1]
            if (str(witness.get("program")) != str(event.get("program")) or
                    witness.get("location") != event.get("source") or
                    not str(witness.get("operation", "")).startswith("error:")):
                return None
        if executed["trace"][-1]["operation"] != "error:" + opcode_names[opcode]:
            return None
        prefix, ending = {24: ("error:leanat.payload.", ".get"),
                          25: ("error:leanat.payload.", ".write"),
                          26: ("error:leanat.extension.", ".get"),
                          27: ("error:leanat.extension.", ".write")}[opcode]
        source_operation = model["trace"][-1]["operation"]
        if not source_operation.startswith(prefix) or not source_operation.endswith(ending):
            return None
        message = message[:-len(suffix)]
    rule = rules.get((reference, str(actual["code"]), message))
    if rule is None:
        return None
    return {"verified": True, "referenceError": reference, "nativeError": deepcopy(actual), "rule": rule}

def project(modelOutcome, execOutcome, nativeOutput):
    """Return computed semantic worlds; malformed/unknown observations fail closed."""
    out = {"model": None, "exec": None, "native": None, "unmapped": [], "identities": [], "diagnostics": {}}
    try:
        out["model"], model_diag = _reference(modelOutcome, "model", out["unmapped"])
        out["exec"], exec_diag = _reference(execOutcome, "exec", out["unmapped"])
        out["diagnostics"] = {"referenceRepresentation": {
            "reason": "Global reference allocation leases/host bounds and private view handles have no native PayloadShadow store counterpart; per-view and extension bounds are compared.",
            "model": model_diag, "exec": exec_diag}}
        if model_diag != exec_diag:
            out["unmapped"].append("reference allocation/host bookkeeping differs across interpreters")
        provider = nativeOutput.get("provider")
        if not isinstance(provider, dict) or set(provider) != {"payload", "events"}:
            raise ValueError("unknown or missing native provider field")
        native = _payload(provider["payload"], True)
        if not isinstance(provider["events"], list):
            raise ValueError("native queue snapshot type")
        if provider["events"]:
            out["unmapped"].append("payload program produced native pending events")
        actions = nativeOutput.get("actions")
        if not isinstance(actions, dict) or set(actions) != {"actions", "cursor", "failed"}:
            raise ValueError("unknown or missing native actions field")
        if not isinstance(actions["actions"], list) or not isinstance(actions["failed"], bool):
            raise ValueError("native actions type")
        if actions["actions"]:
            out["unmapped"].append("payload program produced native send actions")
        host = nativeOutput.get("hostEvents")
        if not isinstance(host, list):
            raise ValueError("native host events missing")
        if host:
            out["unmapped"].append("payload program produced native host observations")
        out["native"] = {"payload": native, "events": deepcopy(provider["events"]),
                         "observations": deepcopy(host), "actions": deepcopy(actions["actions"]),
                         "actionCursor": _integer(actions["cursor"]), "actionFailed": actions["failed"]}
        ids = nativeOutput.get("identities")
        if not isinstance(ids, dict) or set(ids) != {"transaction", "hop"}:
            raise ValueError("unknown or missing native identity field")
        for role in ("transaction", "hop"):
            raw = _handle(ids[role])
            if raw != native[role]:
                raise ValueError("native identity map differs from actual payload: " + role)
            # These are injected source identities, not fresh native allocations.
            # Full generation/slot/store equality must hold, never normalize it away.
            if raw != out["model"]["payload"][role] or raw != out["exec"]["payload"][role]:
                out["unmapped"].append("injected payload identity differs: " + role)
            else:
                out["identities"].append({"role": "payload." + role, "model": deepcopy(raw),
                                          "exec": deepcopy(raw), "native": deepcopy(raw)})
    except (ValueError, TypeError, KeyError, UnicodeError, OverflowError) as error:
        out["unmapped"].append(str(error))
    agreement = _error_agreement(modelOutcome, execOutcome, nativeOutput)
    if agreement is not None:
        out["errorAgreement"] = agreement
    return out



