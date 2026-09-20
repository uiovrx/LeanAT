"""Core provider observations: compare owned state, retaining raw diagnostics upstream."""
try:
    from .opcode_projection_common import IdentityBijection, identity, observed_value
except ImportError:
    from opcode_projection_common import IdentityBijection, identity, observed_value


def _value(value):
    if value is None:
        return None
    if value.get("kind") == "unit" or set(value) == {"unit"}:
        return {"unit": True}
    if value.get("kind") == "bool":
        return {"bool": value["value"]}
    if set(value) == {"bool"}:
        return value
    if value.get("kind") == "variant":
        return [{"integer": str(value["tag"])}, [_value(x) for x in value["fields"]]]
    if value.get("kind") in ("record", "vec"):
        return [_value(x) for x in value.get("fields", value.get("values", []))]
    if "array" in value:
        return [_value(x) for x in value["array"]]
    return observed_value(value)


def _option(value):
    if value["kind"] != "variant" or int(value["tag"]) not in (0, 1):
        raise ValueError("core option codec")
    return None if int(value["tag"]) == 0 else value["fields"][0]


def _objects(outcome, tag):
    return [x for x in outcome["world"]["objects"] if x["tag"] == tag and x["alive"]]


def _error_agreement(model, executable, native):
    """Exact cause pairs; native messages were observed in the real Core23 run.

    Never equate a generic unsupported-provider error with a semantic failure.
    Source-location suffixes remain in raw evidence and are removed only here.
    """
    if model.get("ok") or executable.get("ok") or native.get("ok"):
        return None
    reference = executable.get("error")
    if model.get("error") != reference or not isinstance(native.get("error"), dict):
        return None
    error = native["error"]
    cause = error.get("message", "").split(" (program ", 1)[0].strip()
    pairs = {
        "TimeRegression": ("10", "past tick", "scheduled tick precedes authoritative frontier"),
        "TimeOverflow": ("1", "addition overflow", "successor turn exceeds uint64"),
        "EventAuthority": ("4", "core service handle owner", "event capability owner differs"),
        "StaleEventHandle": ("3", "event generation", "event capability generation is stale"),
        "MissingProcessContext": ("4", "current process context is not bound", "no authoritative current process"),
        "OutputPortRange": ("21", "output port", "port value exceeds uint32"),
        "TimerKind": ("11", "core wait service requires timer kind", "wait kind outside registered timer ABI"),
        "TimerProcessContext": ("4", "core service handle owner", "timer process owner differs from current process"),
        "RegisteredWaitWithoutSuspension": ("8", "registered wait lacks atomic Suspend", "registered wait cannot commit without Suspend"),
        "WaitOutcomeNotReady": ("7", "wait outcome is not successful", "owning wait outcome is not the success constructor"),
        "TransportSync": ("21", "transport sync/delay", "transport sync code exceeds legal enumeration"),
    }
    pair = pairs.get(reference)
    if pair and (str(error.get("code")), cause) == pair[:2]:
        return {"verified": True, "referenceError": reference, "nativeError": error, "rule": pair[2]}
    return None


def _discarded_wait_identity(model, executable, native, source, process_entry):
    """Witness a single allocated then rolled-back Wait without reviving it."""
    if source is None or process_entry is None:
        raise ValueError("discarded Wait requires authoritative input and process binding")
    if not _error_agreement(model, executable, native) or executable.get("error") != "RegisteredWaitWithoutSuspension":
        raise ValueError("discarded Wait requires exact post-registration commit rejection")
    initial, final = native["initialProvider"], native["provider"]
    if (initial.get("wait") is not None or final.get("wait") is not None
            or str(initial["waits"]) != "0" or str(final["waits"]) != "0"
            or initial["process"] != final["process"]):
        raise ValueError("discarded Wait is still live or changed its owning process")
    events = native["opcodeEvents"]
    if len(events) != 2 or events[0]["stage"] != "entered" or events[1]["stage"] != "completed":
        raise ValueError("discarded Wait requires the complete single registration history")
    entered, issued = events
    for key in ("program", "block", "instruction", "callDepth", "opcode", "source", "arguments", "fuelBefore"):
        if entered[key] != issued[key]:
            raise ValueError("discarded Wait instruction entry and completion disagree")
    if issued["opcode"] != 39 or issued["error"] is not None or len(issued["arguments"]) != 3:
        raise ValueError("discarded Wait was not issued by actual RegisterWait")
    mapping = IdentityBijection([process_entry])
    actual = identity(issued["result"])
    initial_counter, final_counter = initial["processCounters"], final["processCounters"]
    if (actual["kind"] != "4" or actual["domain"] != str(initial_counter["domain"])
            or actual["store"] != str(initial_counter["store"]) or actual["slot"] != "0"
            or actual["owner"] != identity(process_entry["native"])["owner"]
            or actual["generation"] != str(initial_counter["nextGeneration"])
            or int(final_counter["nextGeneration"]) != int(actual["generation"]) + 1):
        raise ValueError("discarded Wait identity is not the actual allocator issuance")
    if mapping.normalize(issued["arguments"][0], "native") != mapping.normalize({"kind": "handle", "identity": process_entry["exec"]}, "exec"):
        raise ValueError("discarded Wait was registered for another process")
    raw = native["rawSegment"]
    if raw.get("kind") != "0" or len(raw.get("values", [])) != 1 or identity(raw["values"][0]) != actual:
        raise ValueError("discarded Wait differs from the actual returned segment value")
    result = {"role": "discarded.wait", "native": actual}
    for side, outcome, operation in (("model", model, "serviceCall:leanat.core.wait.timer"), ("exec", executable, "registerWait")):
        if outcome["world"]["objects"] != source["world"]["objects"]:
            raise ValueError("discarded Wait reference did not restore original owned objects")
        rows = [row for row in outcome["trace"] if row["operation"] == operation]
        if len(rows) != 1 or len(rows[0]["results"]) != 1:
            raise ValueError("discarded Wait reference issuance history missing or duplicated")
        row = rows[0]
        if (str(row["program"]) != str(issued["program"]) or row["location"] != issued["source"]
                or mapping.normalize(row["args"], side) != mapping.normalize(issued["arguments"], "native")):
            raise ValueError("discarded Wait reference call differs from actual invocation")
        handle = identity(row["results"][0])
        rules = [rule for rule in source["world"]["allocationRules"] if int(rule["kind"]) == 4]
        if len(rules) != 1 or rules[0]["group"] != "runtime.process" or rules[0]["perSlot"] or not rules[0]["persistent"]:
            raise ValueError("discarded Wait reference allocator policy missing")
        def counter(world):
            values = [c for c in world["allocationCounters"] if c["group"] == "runtime.process" and c["slot"] is None
                      and str(c["domain"]) == handle["domain"] and c["persistent"] and not c["retired"]]
            if len(values) != 1:
                raise ValueError("discarded Wait allocator history ambiguous")
            return int(values[0]["nextGeneration"])
        if (handle["kind"] != "4" or handle["store"] != str(rules[0]["store"])
                or any(handle[key] != identity(process_entry[side])[key] for key in ("domain", "owner"))
                or handle["slot"] != "0" or int(handle["generation"]) != counter(source["world"])
                or counter(outcome["world"]) != int(handle["generation"]) + 1):
            raise ValueError("discarded Wait reference generation/authority lacks allocator witness")
        result[side] = handle
    return result


def project(modelOutcome, execOutcome, nativeOutput, input_value=None):
    outcomes = {"model": modelOutcome, "exec": execOutcome}
    provider = nativeOutput["provider"]
    entries, unmapped = [], []
    known = {"events", "eventCapacity", "eventSlots", "processCounters", "process", "wait", "frames", "waits", "frontier", "nextSequence", "nextBatch"}
    for field in provider.keys() - known:
        unmapped.append("unknown native core provider field " + field)
    for name, tag in (("process", "reference.process"), ("wait", "reference.wait")):
        refs = {side: [x for x in outcome["world"]["objects"] if x["tag"] == tag and
                      (x["alive"] or name == "process")] for side, outcome in outcomes.items()}
        native = provider.get(name)
        if any(len(items) > 1 for items in refs.values()):
            raise ValueError("core fixture permits one named " + name)
        if native and all(refs.values()):
            entries.append({"role": name, "native": native["identity"],
                            **{side: items[0]["identity"] for side, items in refs.items()}})
        elif native or any(refs.values()):
            unmapped.append("asymmetric " + name + " lifecycle")
    if execOutcome.get("error") == "RegisteredWaitWithoutSuspension":
        entries.append(_discarded_wait_identity(modelOutcome, execOutcome, nativeOutput, input_value,
                                               next((entry for entry in entries if entry["role"] == "process"), None)))
    # The fixture relocates scoped capability inputs, including deliberate owner
    # or nonce mutations. Preserve those mutations exactly across store relocation.
    def input_handles(value):
        if isinstance(value, dict):
            if value.get("kind") == "handle" or set(value) == {"handle"}:
                yield identity(value)
            else:
                for child in value.values():
                    yield from input_handles(child)
        elif isinstance(value, list):
            for child in value:
                yield from input_handles(child)
    for actual in input_handles(nativeOutput.get("preparedInputs", [])):
        for base in list(entries):
            original = identity(base["native"])
            if actual == original or any(actual[k] != original[k] for k in ("kind", "domain", "store", "slot")):
                continue
            delta = int(actual["generation"]) - int(original["generation"])
            variant = {"role": base["role"] + ".input." + actual["owner"] + "." + actual["generation"], "native": actual}
            for side in ("model", "exec"):
                ref = identity(base[side])
                variant[side] = dict(ref, owner=actual["owner"], generation=str(int(ref["generation"]) + delta))
            if not any(entry["native"] == actual for entry in entries):
                entries.append(variant)
            break
    refs_events = {side: sorted(outcome["world"]["events"], key=lambda e: int(e["sequence"]))
                   for side, outcome in outcomes.items()}
    native_events = sorted(provider["events"], key=lambda e: int(e["key"]["sequence"]))
    if len({len(native_events), *(len(v) for v in refs_events.values())}) != 1:
        unmapped.append("event count differs")
    else:
        for index, event in enumerate(native_events):
            entries.append({"role": "event." + str(index), "native": event["token"],
                            **{side: items[index]["identity"] for side, items in refs_events.items()}})
    mapping = IdentityBijection(entries)

    def reference_journal(outcome, side):
        world = outcome["world"]
        if "allocationCounters" not in world or "allocationRules" not in world:
            unmapped.append(side + " missing allocator journal")
            return None
        counters, process_next, process_domain = [], None, None
        for counter in world["allocationCounters"]:
            if not counter["persistent"]:
                unmapped.append(side + " core allocation counter is not persistent")
            if counter["group"] == "runtime.process" and counter["slot"] is None:
                if process_next is not None:
                    unmapped.append(side + " multiple process allocator domains")
                process_next = str(counter["nextGeneration"])
                process_domain = str(counter["domain"])
            elif counter["group"] == "runtime.event" and counter["slot"] is not None:
                if int(counter["nextGeneration"]) != 1:
                    counters.append({"slot": str(counter["slot"]), "nextGeneration": str(counter["nextGeneration"])})
            else:
                unmapped.append(side + " unknown allocation counter group")
        capacities = {}
        for rule in world["allocationRules"]:
            kind = int(rule["kind"])
            expected = {2: ("runtime.event", "1", True, True),
                        3: ("runtime.process", "10", False, False),
                        4: ("runtime.process", "11", False, False)}.get(kind)
            if expected is None or (rule["group"], str(rule["store"]), rule["perSlot"], rule["allowMax"]) != expected or not rule["persistent"]:
                unmapped.append(side + " incompatible core allocator rule")
            else:
                capacities[kind] = str(rule["capacity"])
        if set(capacities) != {2, 3, 4} or process_next is None:
            unmapped.append(side + " missing core allocator bounds/counter")
        if any(str(counter["domain"]) != process_domain for counter in world["allocationCounters"]):
            unmapped.append(side + " allocator domain differs")
        return {"domain": process_domain, "processNext": process_next, "events": sorted(counters, key=lambda c: int(c["slot"])),
                "nextSequence": str(world["nextSequence"]), "highWater": str(world["nextGeneration"]),
                "eventCapacity": capacities.get(2), "frameCapacity": capacities.get(3), "waitCapacity": capacities.get(4)}

    def native_journal():
        if "processCounters" not in provider or "eventSlots" not in provider:
            unmapped.append("native missing allocator journal")
            return None
        process = provider["processCounters"]
        slots, high = [], int(process["nextGeneration"])
        if len(provider["eventSlots"]) != int(provider["eventCapacity"]):
            unmapped.append("native slot journal capacity differs")
        seen = set()
        for slot in provider["eventSlots"]:
            token = identity(slot["token"])
            index = int(token["slot"])
            if not 0 <= index < int(provider["eventCapacity"]):
                unmapped.append("native journal slot outside capacity")
            if index in seen:
                unmapped.append("native duplicate journal slot")
            seen.add(index)
            if int(token["kind"]) != 2 or token["domain"] != str(process["domain"]):
                unmapped.append("native event allocator authority differs")
            state, generation = slot["state"], int(token["generation"])
            if state not in ("free", "reserved", "queued", "active", "done", "retired"):
                unmapped.append("native unknown event slot state")
            if state == "retired" and generation != 2**64-1:
                unmapped.append("native event slot retired before exhaustion")
            # Queue slots store the last issued generation even after release;
            # prepare increments it. Untouched slots therefore contain zero.
            next_generation = generation + 1
            high = max(high, next_generation)
            if next_generation != 1:
                slots.append({"slot": str(index), "nextGeneration": str(next_generation)})
        if int(process["frames"]) != int(provider["frames"]) or int(process["waits"]) != int(provider["waits"]):
            unmapped.append("native process occupancy journal differs")
        return {"domain": str(process["domain"]), "processNext": str(process["nextGeneration"]), "events": sorted(slots, key=lambda c: int(c["slot"])),
                "nextSequence": str(provider["nextSequence"]), "highWater": str(high),
                "eventCapacity": str(provider["eventCapacity"]), "frameCapacity": str(process["frameCapacity"]),
                "waitCapacity": str(process["waitCapacity"])}

    def reference(outcome, side):
        result = {"events": [], "process": None, "wait": None, "transport": [], "eventCapacity": str(outcome["world"]["maxEvents"]), "allocator": reference_journal(outcome, side)}
        for observation in outcome["world"]["observations"]:
            if observation["kind"] == "transport.prepared":
                result["transport"].extend(_value(x) for x in observation["values"])
            elif observation["kind"] == "process.completed":
                values = observation["values"]
                if len(values) != 4 or observation["opcode"] != "return" or observation["source"] != "":
                    raise ValueError("core completion observation codec")
                if result["process"] is not None:
                    raise ValueError("duplicate core completion")
                result["process"] = {"identity": _value(values[0]), "program": str(values[1]["value"]),
                    "status": "2", "retired": True, "result": _value(values[3]), "wait": None,
                    "ordinal": str(values[2]["value"]),
                    "ready": {"time": str(observation["time"]), "turn": str(observation["turn"])}}
        for event in refs_events[side]:
            result["events"].append({"time": str(event["time"]), "turn": str(event["turn"]),
                "stage": str(event["stage"]), "instance": str(event["instanceId"]),
                "sequence": str(event["sequence"]), "connection": str(event["connection"]), "owner": str(event["identity"]["owner"]),
                "cancelled": event["cancelled"], "values": [_value(x) for x in event["values"]]})
        for obj in outcome["world"]["objects"]:
            if not obj["alive"]:
                completion = result["process"]
                if obj["tag"] != "reference.process" or completion is None or not completion.get("retired") or \
                        completion["identity"] != _value({"kind": "handle", "identity": obj["identity"]}) or \
                        _value(obj["value"]) != {"unit": True}:
                    unmapped.append(side + " dead core object lacks exact completion evidence")
                continue
            if obj["tag"] == "reference.process":
                if result["process"] is not None:
                    unmapped.append(side + " completed process remains live")
                fields = obj["value"]["fields"]
                result["process"] = {"program": str(fields[0]["value"]), "status": str(fields[1]["value"]),
                    "result": _value(_option(fields[3])), "wait": _value(_option(fields[2])), "ordinal": str(fields[5]["value"])}
            elif obj["tag"] == "reference.wait":
                fields = obj["value"]["fields"]
                key = _option(fields[4])
                result["wait"] = {"kind": str(fields[1]["value"]), "deadline": str(fields[2]["value"]),
                    "outcome": _value(_option(fields[5])),
                    "ready": {"time": str(key["fields"][0]["value"]), "turn": str(key["fields"][1]["value"])} if key else None}
                if _option(fields[3]) is not None or fields[6] != {"kind": "bool", "value": False}:
                    unmapped.append(side + " timer fixture has source or claimed wait")
            elif obj["tag"] != "reference.event":
                unmapped.append(side + " unknown core object " + obj["tag"])
        return mapping.normalize(result, side)

    native = {"events": [], "process": None, "wait": None, "transport": [], "eventCapacity": str(provider["eventCapacity"]), "allocator": native_journal()}
    if nativeOutput.get("ok") and nativeOutput.get("exit") == "transportReturn":
        native["transport"] = [_value(x) for x in nativeOutput["rawSegment"]["values"]]
    for event in native_events:
        key, payload = event["key"], _value(event["value"])
        values = [payload]
        if isinstance(payload, list) and payload and payload[0] == {"integer": str(0x4c41544f)}:
            values = payload
        elif isinstance(payload, list) and payload and payload[0] == {"integer": str(0x4c415452)}:
            # Both backends retain the marker, scoped capabilities and ordinal.
            values = payload
            if len(payload) != 4 or provider.get("process", {}).get("ordinal") != payload[3].get("integer"):
                unmapped.append("resume event ordinal differs from live suspension")
        native["events"].append({"time": str(key["time"]), "turn": str(key["turn"]),
            "stage": str(key["stage"]), "instance": str(key["instance"]),
            "sequence": str(key["sequence"]), "connection": str(key["connection"]), "owner": str(event["owner"]),
            "cancelled": event["cancelled"], "values": values})
        if str(event["epoch"]) != "0":
            unmapped.append("core catalog epoch must be zero")
        if event["state"] != "queued":
            unmapped.append("core segment left an active batch member")
    process = provider.get("process")
    if process:
        if process.get("retired"):
            native["process"] = {"identity": _value({"kind": "handle", "identity": process["identity"]}),
                "program": str(process["program"]), "status": "2", "retired": True,
                "result": [_value(x) for x in process["completedValues"]], "wait": None,
                "ordinal": str(process["ordinal"]), "ready": process["completedReady"]}
            if int(provider["frames"]) != 0:
                unmapped.append("retired process still consumes a frame")
        else:
            states = {"1": "0", "2": "1", "3": "1"}
            native["process"] = {"program": str(process["program"]), "status": states.get(str(process["state"]), "invalid"), "result": None, "ordinal": str(process["ordinal"]),
                "wait": {"kind": "handle", "identity": process["wait"]} if process["wait"] else None}
            if int(provider["frames"]) != 1:
                unmapped.append("live process frame count differs")
    wait = provider.get("wait")
    if wait:
        native["wait"] = {"kind": str(wait["kind"]), "deadline": str(wait["deadline"]), "outcome": _value(wait["outcome"]), "ready": wait["ready"]}
    if int(provider["waits"]) != int(wait is not None):
        unmapped.append("wait capacity occupancy differs")
    for side, outcome in outcomes.items():
        for observation in outcome["world"]["observations"]:
            if observation["kind"] not in ("transport.prepared", "process.completed"):
                unmapped.append(side + " unmapped core observation " + observation["kind"])
    return {"model": reference(modelOutcome, "model"), "exec": reference(execOutcome, "exec"),
            "native": mapping.normalize(native, "native"), "unmapped": unmapped, "identities": entries,
            "errorAgreement": _error_agreement(modelOutcome, execOutcome, nativeOutput),
            "diagnostics": {"queueCounters": "Source setup and native queue both include the executed frontier marker history; sequence and generation journals compare directly.",
                            "transport.prepared": "Owned transport return is compared by central rawSegment/transportReturn projection.",
                            "terminalProcess": "Native host completion retires the frame; its owned returned values remain in the lifecycle observation."}}




