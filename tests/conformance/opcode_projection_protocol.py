"""Protocol provider semantic projection. Unsupported state remains a full-pass blocker."""
import copy
from .opcode_projection_common import IdentityBijection, identity


def _value(v):
    if not isinstance(v, dict):
        return v
    kind = v.get("kind")
    if kind == "bits":
        return str(v["value"])
    if kind == "bool":
        return v["value"]
    if kind == "bytes":
        return [int(x) for x in v["data"]]
    if kind == "handle":
        return {"kind": "handle", "identity": identity(v)}
    if kind == "unit":
        return None
    if kind == "record":
        return [_value(x) for x in v["fields"]]
    if kind == "vec":
        return [_value(x) for x in v["values"]]
    if set(v) == {"bytes"}:
        return [int(x) for x in v["bytes"]]
    if set(v) == {"u64"}:
        return str(v["u64"])
    if set(v) == {"bool"}:
        return v["bool"]
    if set(v) == {"unit"}:
        return None
    if set(v) == {"handle"}:
        return {"kind": "handle", "identity": identity(v)}
    return {k: _value(x) for k, x in v.items()}


def _handle(h):
    return {"kind": "handle", "identity": identity(h)}


def _objects(outcome, tag):
    return [x for x in outcome["world"]["objects"] if x["tag"] == tag]


def _roles(outcome):
    roles = {}
    for n, obj in enumerate(_objects(outcome, "runtime.transaction")):
        data = _value(obj["value"])
        roles[f"transaction.{n}"] = identity(obj["identity"])
        roles[f"hop.{n}"] = identity(data[3])
    for n, obj in enumerate(_objects(outcome, "protocol.gate")):
        roles[f"requestGate.{n}"] = identity(obj["identity"])
    results = _objects(outcome, "storage.result")
    consumers = _objects(outcome, "storage.consumer")
    if len(results) > 1 or len(consumers) > 1:
        raise ValueError("protocol result fixture supports one distinct result/consumer")
    if results:
        roles["result"] = identity(results[0]["identity"])
        roles["resultSource"] = identity(_value(results[0]["value"])[0])
    if consumers:
        roles["consumer"] = identity(consumers[0]["identity"])
    return roles


def _reference(outcome):
    transactions, results, consumers = [], [], []
    for obj in _objects(outcome, "runtime.transaction"):
        d = _value(obj["value"])
        transactions.append({"identity": _handle(obj["identity"]), "alive": obj["alive"],
            "connection": d[0], "transport": d[1], "generation": d[2], "hop": d[3],
            "phase": d[5], "acknowledged": d[6], "cancelled": d[7]})
    for obj in _objects(outcome, "storage.result"):
        d = _value(obj["value"])
        results.append({"identity": _handle(obj["identity"]), "alive": obj["alive"],
            "source": d[0], "type": d[1], "maxBytes": d[2], "producerAlive": d[3],
            "published": d[4], "value": d[5] if d[4] else None,
            "ready": {"time": d[6], "turn": d[7]} if d[4] else None, "pins": d[8]})
    for obj in _objects(outcome, "storage.consumer"):
        d = _value(obj["value"])
        consumers.append({"identity": _handle(obj["identity"]), "result": d[0],
                          "active": obj["alive"], "dropOnTerminal": d[1]})
    return {"transactions": transactions, "results": results, "consumers": consumers,
            "pinLimit": str(outcome["world"]["maxPins"])}


def _native(native):
    p = native["provider"]
    transactions, results, consumers = [], [], []
    for row in p["ledgers"]:
        a, wire = row["admission"], row["wire"]
        if "error" in a or "error" in wire:
            transactions.append({"admissionError": copy.deepcopy(a), "wireError": copy.deepcopy(wire)})
            continue
        w = wire["value"]
        transactions.append({"identity": _handle(a["txn"]), "alive": True,
            "connection": str(a["connection"]), "transport": str(a["transport"]),
            "generation": str(a["transportGeneration"]), "hop": _handle(a["hop"]),
            "phase": str(w["state"]),
            "acknowledged": _value(p["ack"]["value"]) if p["ack"] else False,
            "cancelled": any(d["cancelled"] and identity(d["transaction"]) == identity(a["txn"])
                             for d in p["drains"])})
    for s in p["resultStore"]["results"]:
        c = s["create"]
        results.append({"identity": _handle(s["identity"]), "alive": s["alive"],
            "source": _handle(c["source"]), "type": str(c["type"]), "maxBytes": str(c["maxBytes"]),
            "producerAlive": s["producerAlive"], "published": s["ready"] is not None,
            "value": _value(s["value"]), "ready": s["ready"], "pins": str(s["pinCount"])})
    for c in p["resultStore"]["consumers"]:
        consumers.append({"identity": _handle(c["identity"]), "result": _handle(c["result"]),
                          "active": c["active"], "dropOnTerminal": False})
    return {"transactions": transactions, "results": results, "consumers": consumers,
            "pinLimit": str(p["resultStore"]["pinLimit"])}


def project(modelOutcome, execOutcome, nativeOutput, input_value=None):
    model_roles, exec_roles = _roles(modelOutcome), _roles(execOutcome)
    native_roles = {x["name"]: identity(x["identity"]) for x in nativeOutput["identities"]}
    if len(native_roles) != len(nativeOutput["identities"]):
        raise ValueError("duplicate native semantic role")
    initial_roles = {x["name"]: identity(x["identity"]) for x in nativeOutput["initialIdentities"]}
    if len(initial_roles) != len(nativeOutput["initialIdentities"]):
        raise ValueError("duplicate initial native semantic role")
    for role in initial_roles.keys() & native_roles.keys():
        if initial_roles[role] != native_roles[role]:
            raise ValueError("existing semantic role changed incarnation: " + role)
    shared = model_roles.keys() & exec_roles.keys() & native_roles.keys()
    entries = [{"role": role, "model": model_roles[role], "exec": exec_roles[role],
                "native": native_roles[role]} for role in sorted(shared)]
    if input_value is not None:
        for index, (logical_value, prepared_value) in enumerate(zip(input_value["inputs"], nativeOutput.get("preparedInputs", []))):
            if logical_value.get("kind") != "handle":
                continue
            logical, prepared = identity(logical_value), identity(prepared_value)
            if logical in model_roles.values():
                continue
            candidates = [entry for entry in entries if all(logical[k] == entry["model"][k]
                for k in ("kind", "domain", "owner", "store", "slot"))]
            if len(candidates) != 1:
                continue
            base = candidates[0]
            for field in ("kind", "domain", "owner"):
                if prepared[field] != logical[field]:
                    raise ValueError("prepared invalid capability changed authority")
            for field in ("store", "slot", "generation"):
                if int(prepared[field]) - int(base["native"][field]) != int(logical[field]) - int(base["model"][field]):
                    raise ValueError("prepared invalid capability lost relative identity displacement")
            entries.append({"role": f"invalidInput.{index}", "model": logical, "exec": logical, "native": prepared})
    mapping = IdentityBijection(entries)
    unmapped = [{"path": "identities." + role, "reason": "semantic identity missing from one backend"}
               for role in sorted((model_roles.keys() | exec_roles.keys() | native_roles.keys()) - shared)]
    # These are retained explicitly. They cannot be inferred from a transaction's phase.
    for key in ("protocolCounters", "runtimeCounters", "admissionStore", "setup", "ledgers", "drains", "drainOutstanding", "activeServices", "intents",
                "result", "resultSlots", "ack", "stopped", "stopDetail", "wake"):
        if key in nativeOutput["provider"]:
            unmapped.append({"path": "provider." + key,
                "reason": "full native state has no complete independent reference codec yet",
                "actual": copy.deepcopy(nativeOutput["provider"][key])})
    for key in ("nextResultGeneration", "nextConsumerGeneration", "pinCount"):
        unmapped.append({"path": "provider.resultStore." + key,
            "reason": "pool-specific allocation/capacity accounting absent in reference world",
            "actual": copy.deepcopy(nativeOutput["provider"]["resultStore"][key])})
    unmapped.append({"path": "provider.resultStore", "reason":
        "publishing locks, aggregate consumer counts and producer/consumer create ownership require independent codec",
        "actual": copy.deepcopy(nativeOutput["provider"]["resultStore"])})
    for key in ("actions", "hostEvents", "initialProvider"):
        unmapped.append({"path": key, "reason": "independent causal/lifecycle projection not complete",
                        "actual": copy.deepcopy(nativeOutput[key])})
    result = {"model": mapping.normalize(_reference(modelOutcome), "model"),
            "exec": mapping.normalize(_reference(execOutcome), "exec"),
            "native": mapping.normalize(_native(nativeOutput), "native"),
            "identities": entries, "unmapped": unmapped}
    typed = _objects(modelOutcome, "protocol.control")
    if typed and len(_value(typed[0]["value"])) == 23 and input_value is not None:
        from .opcode_projection_protocol_state import (
            protocol_reference, protocol_native, result_reference, result_native, host_reference, result_query_reference, canonical, setup_reference)
        ctx = input_value["context"]
        def reference_state(outcome):
            return {**protocol_reference(outcome["world"], ctx),
                    "resultStore": result_reference(outcome["world"]),
                    "result": result_query_reference(outcome["world"]),
                    "setup": setup_reference(outcome["world"]),
                    "resultSlots": str(sum(o["alive"] for o in _objects(outcome, "storage.result")))}
        def native_state(output):
            return {**protocol_native(output, ctx),
                    "resultStore": result_native(output["provider"], ctx),
                    "result": canonical(output["provider"]["result"]),
                    "setup": canonical(output["provider"]["setup"]),
                    "resultSlots": output["provider"]["resultSlots"]}
        initial_native = {**nativeOutput, "provider": nativeOutput["initialProvider"],
                          "identities": nativeOutput["initialIdentities"]}
        initial = reference_state({"world": input_value["world"]})
        result["model"] = mapping.normalize({"final": reference_state(modelOutcome), "initial": initial, "hostEvents": host_reference(modelOutcome["world"], input_value["world"]), "actions": {"actions": modelOutcome["committed"], "cursor": str(len(modelOutcome["committed"])), "failed": False}}, "model")
        result["exec"] = mapping.normalize({"final": reference_state(execOutcome), "initial": initial, "hostEvents": host_reference(execOutcome["world"], input_value["world"]), "actions": {"actions": execOutcome["committed"], "cursor": str(len(execOutcome["committed"])), "failed": False}}, "exec")
        result["native"] = mapping.normalize({"final": native_state(nativeOutput),
                                                "initial": native_state(initial_native), "hostEvents": copy.deepcopy(nativeOutput["hostEvents"]), "actions": copy.deepcopy(nativeOutput["actions"])}, "native")
        covered = set(reference_state(modelOutcome))
        result["unmapped"] = [entry for entry in result["unmapped"]
            if not (entry["path"] in ("initialProvider", "hostEvents", "actions") or any(
                entry["path"] == "provider." + key or entry["path"].startswith("provider." + key + ".")
                for key in covered))]
        for key in nativeOutput["provider"].keys() - covered:
            if not any(entry["path"] == "provider." + key for entry in result["unmapped"]):
                result["unmapped"].append({"path": "provider." + key, "reason":
                    "unknown native provider state cannot be omitted", "actual": copy.deepcopy(nativeOutput["provider"][key])})
        for key in nativeOutput["initialProvider"].keys() - covered:
            result["unmapped"].append({"path": "initialProvider." + key, "reason":
                "independent query or callback codec not yet compared",
                "actual": copy.deepcopy(nativeOutput["initialProvider"][key])})
    if not modelOutcome.get("ok", True) and not nativeOutput.get("ok", True):
        native_error = nativeOutput.get("error")
        reference_error = modelOutcome.get("error")
        if isinstance(native_error, dict):
            message = native_error.get("message", "").split(" (program ", 1)[0]
            pairs = {
                ("TransactionOperands", "0", "new transaction finite operands"):
                    "both providers reject the finite NewTransaction operand tuple",
                ("CancelReason", "0", "cancel reason"):
                    "both canonical cancellation providers reject reason outside 0..3",
                ("ResultNotReady", "7", "result reserved"):
                    "a valid consumer references an unpublished result",
                ("ResponseNotAckable", "8", "hop has no response awaiting acknowledgement"):
                    "the ledger is not in the response acknowledgement state",
            }
            rule = pairs.get((reference_error, str(native_error.get("code")), message))
            if rule is None and input_value is not None:
                actual_objects = input_value["world"]["objects"]
                handles = [identity(v) for v in input_value["inputs"] if v.get("kind") == "handle"]
                stale = any(any(all(identity(o["identity"])[k] == h[k] for k in ("kind", "domain", "owner", "store", "slot"))
                    and identity(o["identity"])["generation"] != h["generation"] for o in actual_objects) for h in handles)
                inactive_consumer = any(h["kind"] == "6" and any(identity(o["identity"]) == h and not o["alive"]
                    for o in actual_objects) for h in handles)
                wrong_connection = False
                if len(input_value["inputs"]) >= 2 and handles:
                    txn = next((o for o in actual_objects if o["tag"] == "runtime.transaction" and identity(o["identity"]) == handles[0]), None)
                    wrong_connection = txn is not None and _value(txn["value"])[0] != _value(input_value["inputs"][1])
                code = str(native_error.get("code"))
                if reference_error == "StaleReferenceHandle" and code == "3" and stale and message in (
                    "transaction connection has no local hop", "consumer generation"):
                    rule = "both reject the input incarnation displaced from the actual typed live slot"
                elif reference_error == "StaleReferenceHandle" and code == "6" and inactive_consumer and message == "consumer consumed":
                    rule = "both reject the exact inactive consumer retained in the input world"
                elif reference_error == "PhaseOperands" and code == "3" and wrong_connection and message == "transaction connection has no local hop":
                    rule = "both reject a transaction paired with a different connection than its typed admission record"
            if rule and execOutcome.get("error") == reference_error:
                result["errorAgreement"] = {"verified": True, "referenceError": reference_error,
                    "nativeError": copy.deepcopy(native_error), "rule": rule}
    if input_value is not None and not (typed and len(_value(typed[0]["value"])) == 23):
        result["unmapped"].append({"path": "input.world", "reason":
            "initial typed protocol provider projection awaits current state codec validation",
            "actual": copy.deepcopy(input_value["world"])})
    return result
