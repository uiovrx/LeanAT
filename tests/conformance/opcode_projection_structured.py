"""Structured provider state projection; raw slot inventories remain in the evidence.

Only physical free slots, allocator nonce ranges, and the executed frontier marker
are classified as representation details. Live capabilities, ready keys, ownership,
pin counts, FIFO order, cancellation actions and outcomes are compared explicitly.
"""
try:
    from .opcode_projection_common import IdentityBijection, identity, observed_value
except ImportError:
    from opcode_projection_common import IdentityBijection, identity, observed_value


def _v(value):
    if value is None:
        return None
    if isinstance(value, list):
        return [_v(v) for v in value]
    if not isinstance(value, dict):
        return value
    kind = value.get("kind")
    if kind == "unit" or set(value) == {"unit"}:
        return {"unit": True}
    if kind == "bool":
        return {"bool": value["value"]}
    if kind == "variant":
        return [{"integer": str(value["tag"])}, [_v(v) for v in value["fields"]]]
    if kind in ("record", "vec"):
        return [_v(v) for v in value.get("fields", value.get("values", []))]
    if set(value) == {"array"}:
        return [_v(v) for v in value["array"]]
    return observed_value(value)


def _fields(value, count=None):
    if value.get("kind") not in ("record", "vec"):
        raise ValueError("structured record codec")
    fields = value.get("fields", value.get("values"))
    if count is not None and len(fields) != count:
        raise ValueError("structured record arity")
    return fields


def _num(value):
    if value.get("kind") != "bits" or value.get("width") != 64:
        raise ValueError("structured u64 codec")
    return str(value["value"])


def _bool(value):
    if value.get("kind") != "bool" or not isinstance(value.get("value"), bool):
        raise ValueError("structured bool codec")
    return value["value"]


def _option(value):
    if value.get("kind") != "variant" or int(value.get("tag", -1)) not in (0, 1):
        raise ValueError("structured option codec")
    xs = value["fields"]
    if len(xs) != int(value["tag"]):
        raise ValueError("structured option arity")
    return xs[0] if xs else None


def _h(raw):
    return None if raw is None else {"kind": "handle", "identity": identity(raw)}


def _key(raw):
    return tuple(sorted(identity(raw).items()))


def _objects(outcome, tag):
    return sorted((o for o in outcome["world"]["objects"] if (o["alive"] or tag in ("storage.result", "storage.consumer")) and o["tag"] == tag),
                  key=lambda o: int(o["identity"]["generation"]))


def _native_sorted(rows):
    return sorted(rows, key=lambda r: int(r["identity"]["generation"]))


def project(modelOutcome, execOutcome, nativeOutput):
    outcomes = {"model": modelOutcome, "exec": execOutcome}
    native_raw = nativeOutput["provider"]
    entries, unmapped = [], []
    provider_fields = {"childFuelScopes", "observations", "limits", "nextProcessGeneration", "nextTaskGeneration", "nextScopeGeneration", "nextGroupGeneration", "waitOwners", "scopes", "tasks", "tickets", "waits", "results", "consumers", "bindings", "processes", "externalWaits", "events", "frontier", "nextSequence", "nextBatch", "observers", "cancelSequence", "frames", "grants", "queue", "pending", "pins", "pinLimit", "nextResultGeneration", "nextConsumerGeneration"}
    for field in sorted(set(native_raw)-provider_fields):
        unmapped.append("unclassified native structured state field " + field)
    for process in native_raw["processes"]:
        for field in sorted(set(process)-{"identity", "alive", "program", "status", "instance", "instanceBound", "ordinal", "wait", "completed"}):
            unmapped.append("unclassified native process state field " + field)
    native_categories = {
        "scope": _native_sorted(native_raw["scopes"]),
        "task": _native_sorted(native_raw["tasks"]),
        "ticket": _native_sorted(native_raw["tickets"]),
        "wait": _native_sorted(native_raw["waits"]),
        "result": _native_sorted([r for r in native_raw["results"] if int(r["source"]["generation"]) > 0]),
        "consumer": _native_sorted([r for r in native_raw["consumers"] if int(r["result"]["generation"]) > 0]),
        "process": _native_sorted([r for r in native_raw["processes"] if r["alive"]]),
        "externalWait": _native_sorted(native_raw["externalWaits"]),
        "reservation": _native_sorted([{"identity": t["reservedTask"]} for t in native_raw["tickets"] if t["reservedTask"] is not None]),
    }
    for category, counter, delta in [("scope", "nextScopeGeneration", 0), ("task", "nextTaskGeneration", 0),
            ("ticket", "nextTaskGeneration", 0), ("reservation", "nextTaskGeneration", 0), ("wait", "nextGroupGeneration", 0),
            ("result", "nextResultGeneration", 1), ("consumer", "nextConsumerGeneration", 1),
            ("process", "nextProcessGeneration", 0), ("externalWait", "nextProcessGeneration", 0)]:
        if any(int(row["identity"]["generation"]) >= int(native_raw[counter])+delta for row in native_categories[category]):
            unmapped.append("native " + category + " identity exceeds authoritative generation journal")
    tags = {"scope": "structured.scope", "task": "structured.task", "ticket": "structured.ticket",
            "wait": "structured.wait", "result": "storage.result", "consumer": "storage.consumer",
            "process": "reference.process", "externalWait": "reference.wait", "reservation": "structured.reservation"}
    for category, tag in tags.items():
        refs = {side: _objects(outcome, tag) for side, outcome in outcomes.items()}
        native = native_categories[category]
        counts = [len(native), *(len(r) for r in refs.values())]
        if len(set(counts)) != 1:
            unmapped.append("asymmetric live " + category + " inventory " + str(counts))
        for i in range(min(counts)):
            entries.append({"role": category + "." + str(i), "native": native[i]["identity"],
                            **{side: rows[i]["identity"] for side, rows in refs.items()}})
    native_events = sorted([e for e in native_raw["events"] if e["state"] in ("queued", "active")],
                           key=lambda e: int(e["key"]["sequence"]))
    refs_events = {side: sorted(outcome["world"]["events"], key=lambda e: int(e["sequence"]))
                   for side, outcome in outcomes.items()}
    counts = [len(native_events), *(len(es) for es in refs_events.values())]
    if len(set(counts)) != 1:
        unmapped.append("asymmetric queued event inventory " + str(counts))
    for i in range(min(counts)):
        entries.append({"role": "event." + str(i), "native": native_events[i]["identity"],
                        **{side: rows[i]["identity"] for side, rows in refs_events.items()}})
    # Queued cancelled events retain owning references to retired process frames.
    # Those identities remain observable even though they are no longer live stores.
    def handles(value):
        if isinstance(value, dict):
            if value.get("kind") == "handle":
                return [value]
            return [h for child in value.values() for h in handles(child)]
        if isinstance(value, list):
            return [h for child in value for h in handles(child)]
        return []
    pending_retained = []
    for i in range(min(counts)):
        refs = {side: handles(_v(rows[i]["values"])) for side, rows in refs_events.items()}
        actual = handles(_v(native_events[i]["value"]))
        sizes = [len(actual), *(len(values) for values in refs.values())]
        if len(set(sizes)) != 1:
            unmapped.append("asymmetric retained event capabilities " + str(i))
        for j in range(min(sizes)):
            model_handle = refs["model"][j]
            if any(_key(e["model"]) == _key(model_handle) for e in entries):
                continue
            pending_retained.append(("event." + str(i) + ".retained." + str(j),
                                     {side: values[j] for side, values in refs.items()}, actual[j]))
    def retain(role, refs, actual):
        if any(_key(entry["model"]) == _key(refs["model"]) for entry in entries):
            return
        if not all(any(_key(obj["identity"]) == _key(refs[side]) for obj in outcomes[side]["world"]["objects"]) for side in refs):
            # A reused slot replaces its tombstone. Recover an older owning value
            # only through an already proven slot mapping and exact signed
            # generation displacement, never by arbitrary positional renaming.
            old_native = identity(actual)
            witnessed = False
            for entry in entries:
                new_native = identity(entry["native"])
                if not all(new_native[field] == old_native[field] for field in ("kind", "domain", "store", "slot")):
                    continue
                delta = int(new_native["generation"])-int(old_native["generation"])
                if all(all(identity(entry[side])[field] == identity(refs[side])[field] for field in ("kind", "domain", "store", "slot")) and
                       int(identity(entry[side])["generation"])-int(identity(refs[side])["generation"]) == delta for side in refs):
                    witnessed = True
                    break
            if not witnessed:
                unmapped.append(role + " lacks a proven historical slot/generation mapping")
                return
        entries.append({"role": role, "native": actual, **refs})
    for i, native_result in enumerate(native_categories["result"]):
        refs = {side: _objects(outcome, "storage.result") for side, outcome in outcomes.items()}
        if all(i < len(rows) for rows in refs.values()):
            retain("result." + str(i) + ".producer", {side: _fields(rows[i]["value"], 12)[0] for side, rows in refs.items()}, native_result["source"])
    source_observations = {side: outcome["world"]["observations"] for side, outcome in outcomes.items()}
    actual_observations = native_raw["observations"]
    lengths = [len(actual_observations), *(len(rows) for rows in source_observations.values())]
    if len(set(lengths)) != 1:
        unmapped.append("asymmetric actual lifecycle observations " + str(lengths))
    for i in range(min(lengths)):
        refs = {side: handles(_v(rows[i]["values"])) for side, rows in source_observations.items()}
        actual = handles(_v(actual_observations[i]["values"]))
        sizes = [len(actual), *(len(rows) for rows in refs.values())]
        if len(set(sizes)) != 1:
            unmapped.append("asymmetric lifecycle owning capabilities " + str(i))
        for j in range(min(sizes)):
            retain("observation." + str(i) + ".retained." + str(j), {side: rows[j] for side, rows in refs.items()}, actual[j])
    for role, refs, actual in pending_retained:
        retain(role, refs, actual)
    # Seed aliases are accepted only when both independent worlds retain the
    # exact object identity and the native initial registry contains it.
    initial = nativeOutput.get("initialProvider", {})
    initial_rows = [row for name in ("scopes", "tasks", "tickets", "waits", "results", "consumers", "processes", "externalWaits")
                    for row in initial.get(name, []) if "identity" in row]
    anchors = []
    for index, alias in enumerate(nativeOutput.get("initialIdentities", [])):
        logical, actual = identity(alias["logical"]), identity(alias["actual"])
        if not all(logical[field] == actual[field] for field in ("kind", "domain", "owner")):
            unmapped.append("seed alias changes capability authority")
            continue
        if not any(_key(row["identity"]) == _key(actual) for row in initial_rows):
            continue
        if not all(any(_key(obj["identity"]) == _key(logical) for obj in outcome["world"]["objects"])
                   for outcome in outcomes.values()):
            continue
        anchors.append((logical, actual))
        if not any(_key(entry["model"]) == _key(logical) for entry in entries):
            entries.append({"role": "initialCapability." + str(index), "model": logical, "exec": logical, "native": actual})
    operation = nativeOutput.get("caseId", "").split(".")[-1]
    argument_rows = {}
    for side, outcome in outcomes.items():
        name = "enter:leanat.ext.structured" if side == "model" else "enter:" + operation
        rows = [row for row in outcome.get("trace", []) if row.get("operation") == name]
        argument_rows[side] = handles(rows[0]["args"]) if rows else []
    actual_args = handles(nativeOutput.get("preparedInputs", []))
    if len({len(actual_args), *(len(rows) for rows in argument_rows.values())}) != 1:
        unmapped.append("asymmetric structured input capability inventory")
    for index, actual_value in enumerate(actual_args):
        if any(index >= len(rows) for rows in argument_rows.values()):
            break
        refs = {side: identity(rows[index]) for side, rows in argument_rows.items()}
        actual = identity(actual_value)
        existing = next((entry for entry in entries if _key(entry["model"]) == _key(refs["model"])), None)
        if existing is not None:
            if _key(existing["native"]) != _key(actual) or _key(existing["exec"]) != _key(refs["exec"]):
                unmapped.append("input capability contradicts authoritative identity mapping")
            continue
        witnessed = False
        for logical_anchor, actual_anchor in anchors:
            # Neither owner nor domain can be repaired by namespace translation.
            if not all(actual[field] == actual_anchor[field] for field in ("kind", "domain", "owner", "store", "slot")):
                continue
            delta = int(actual["generation"]) - int(actual_anchor["generation"])
            if all(all(ref[field] == logical_anchor[field] for field in ("kind", "domain", "owner", "store", "slot")) and
                   int(ref["generation"]) - int(logical_anchor["generation"]) == delta for ref in refs.values()):
                witnessed = True
                break
        if witnessed:
            entries.append({"role": "invalidInput." + str(index), **refs, "native": actual})
        else:
            unmapped.append("input capability lacks authoritative relative generation mapping")
    mapping = IdentityBijection(entries)

    def reference(outcome, side):
        result = {key: [] for key in ("scopes", "tasks", "tickets", "waits", "results", "consumers", "processes", "externalWaits", "events", "queue", "pending", "bindings", "observers", "waitOwners")}
        result["cancelSequence"] = "0"
        storage = {_key(o["identity"]): _fields(o["value"], 12) for o in _objects(outcome, "storage.result")}
        for o in _objects(outcome, "structured.scope"):
            parent, status, owned, reason, actions, serial = _fields(o["value"], 6)
            result["cancelSequence"] = str(max(int(result["cancelSequence"]), int(_num(serial))))
            ownership = []
            for h in _fields(owned):
                kind = int(identity(h)["kind"])
                ownership.append({"handle": _h(h), "kind": str({7: 0, 8: 1, 4: 2, 2: 3, 0: 4, 12: 5, 14: 6, 9: 7, 10: 8, 6: 9}.get(kind, 10)), "published": False})
            plan = []
            for action in _fields(actions):
                aid, child, kind, applied = _fields(action, 4)
                owned_kind = _num(kind)
                plan.append({"id": _num(aid), "scope": _h(o["identity"]), "handle": _h(child),
                             "ownedKind": owned_kind, "kind": "2" if owned_kind == "9" else "1" if owned_kind in ("0", "1") else "0", "applied": _bool(applied)})
            why = _option(reason)
            result["scopes"].append({"identity": _h(o["identity"]), "parent": _h(_option(parent)), "status": _num(status),
                                      "owned": ownership, "reason": _v(why), "actions": plan})
        for o in _objects(outcome, "structured.task"):
            scope, status, args, res, consumer, pool, start, process = _fields(o["value"], 8)
            process, start = _option(process), _option(start)
            status = _num(status)
            result["tasks"].append({"identity": _h(o["identity"]), "scope": _h(scope), "status": status, "args": _v(args),
                                     "result": _h(res), "consumer": _h(consumer), "process": _h(process), "start": _h(start), "producerReleased": int(status) >= 3})
            if status == "0":
                result["queue"].append(_h(o["identity"]))
            if process is not None:
                result["bindings"].append({"task": _h(o["identity"]), "process": _h(process)})
        for o in _objects(outcome, "structured.ticket"):
            process, scope, status, pool, reservation = _fields(o["value"], 5)
            if reservation.get("kind") != "variant" or int(reservation.get("tag", -1)) not in (0, 1):
                raise ValueError("structured ticket reservation codec")
            rs = reservation["fields"] if int(reservation["tag"]) else []
            if len(reservation["fields"]) != (3 if int(reservation["tag"]) else 0):
                raise ValueError("structured ticket reservation arity")
            if rs:
                # Ticket reservation is the public three-field variant payload.
                task, res, consumer = rs
            else:
                task = res = consumer = None
            status = _num(status)
            result["tickets"].append({"identity": _h(o["identity"]), "process": _h(process), "scope": _h(scope), "status": status,
                                       "waiting": False, "consumerReleased": False, "reservedTask": _h(task), "result": _h(res), "consumer": _h(consumer)})
            if status == "0":
                result["pending"].append(_h(o["identity"]))
        for o in _objects(outcome, "structured.wait"):
            process, scope, mode, count, policy, res, consumer, external, branches, resolved = _fields(o["value"], 10)
            result["waitOwners"].append({"external": _h(external), "process": _h(process), "scope": _h(scope), "result": _h(res), "consumer": _h(consumer)})
            metadata = storage[_key(res)]
            bs = []
            for branch in _fields(branches):
                ordinal, priority, source, br, bc, ready, value, failed, pinned, released = _fields(branch, 10)
                ready = _option(ready)
                key = None
                if ready is not None:
                    time, turn = _fields(ready, 2)
                    key = {"time": _num(time), "turn": _num(turn)}
                source_metadata = storage.get(_key(br))
                if source_metadata is None:
                    unmapped.append(side + " released branch needs retained source type metadata")
                bs.append({"ordinal": _num(ordinal), "priority": _num(priority), "source": _h(source),
                           "type": _num(source_metadata[1]) if source_metadata else None, "field": "",
                           "result": None if _bool(released) else _h(br), "consumer": None if _bool(released) else _h(bc),
                           "ready": key, "outcome": _v(_option(value)), "failed": _bool(failed), "pinned": _bool(pinned), "transferred": False})
            result["waits"].append({"identity": _h(o["identity"]), "external": _h(external), "process": _h(process), "scope": _h(scope),
                                     "mode": _num(mode), "policy": _num(policy), "count": _num(count), "resultType": _num(metadata[1]), "resultBytes": _num(metadata[2]),
                                     "result": _h(res), "consumer": _h(consumer), "committed": len(bs) == int(_num(count)), "resolved": _bool(resolved), "resumePending": False, "branches": bs})
        for o in _objects(outcome, "storage.result"):
            source, typ, limit, producer, published, value, time, turn, pins, producer_owner, initial_owner, publishing = _fields(o["value"], 12)
            consumers = [c for c in _objects(outcome, "storage.consumer") if c["alive"] and _key(_fields(c["value"], 2)[0]) == _key(o["identity"])]
            result["results"].append({"identity": _h(o["identity"]), "alive": o["alive"], "producerAlive": _bool(producer), "publishing": _bool(publishing), "source": _h(source), "producerOwner": _num(producer_owner), "initialConsumerOwner": _num(initial_owner),
                                       "type": _num(typ), "maxBytes": _num(limit), "value": _v(value) if _bool(published) else None,
                                       "ready": {"time": _num(time), "turn": _num(turn)} if _bool(published) else None, "consumerCount": str(len(consumers)), "pinCount": _num(pins)})
        for o in _objects(outcome, "storage.consumer"):
            res, dropping = _fields(o["value"], 2)
            if _bool(dropping):
                unmapped.append(side + " DropOnTerminal consumer awaiting cleanup")
            result["consumers"].append({"identity": _h(o["identity"]), "result": _h(res), "active": o["alive"]})
        for o in _objects(outcome, "reference.process"):
            program, status, wait, returned, scope, ordinal, instance = _fields(o["value"], 7)
            result["processes"].append({"identity": _h(o["identity"]), "program": _num(program), "status": _num(status),
                                         "wait": _h(_option(wait)), "ordinal": _num(ordinal), "result": _v(_option(returned)), "scope": _h(_option(scope)), "instance": _num(instance), "instanceBound": True})
        for o in _objects(outcome, "reference.wait"):
            process, kind, deadline, source, ready, value, claimed = _fields(o["value"], 7)
            if _bool(claimed):
                unmapped.append(side + " consumed single wait record remains live")
            ready = _option(ready)
            result["externalWaits"].append({"identity": _h(o["identity"]), "group": _h(_option(source)), "kind": _num(kind), "source": _h(_option(source)),
                                             "deadline": _num(deadline), "value": _v(_option(value)), "ready": None if ready is None else {"time": _num(_fields(ready, 2)[0]), "turn": _num(_fields(ready, 2)[1])}})
        for event in refs_events[side]:
            if event["kind"] not in ("structured.task.start", "runtime.resume"):
                unmapped.append(side + " unknown event kind " + event["kind"])
            result["events"].append({"identity": _h(event["identity"]), "time": str(event["time"]), "turn": str(event["turn"]), "stage": str(event["stage"]),
                                      "instance": str(event["instanceId"]), "connection": str(event["connection"]), "sequence": str(event["sequence"]),
                                      "owner": str(event["identity"]["owner"]), "cancelled": event["cancelled"], "values": [_v(v) for v in event["values"]]})
        result["frames"] = str(sum(t["status"] in ("1", "2") for t in result["tasks"]))
        result["grants"] = str(sum(t["status"] == "1" for t in result["tickets"]))
        result["pins"] = str(sum(int(r["pinCount"]) for r in result["results"]))
        result["pinLimit"] = str(outcome["world"]["maxPins"])
        result["nextSequence"] = str(outcome["world"]["nextSequence"])
        result["generationHighWater"] = str(outcome["world"]["nextGeneration"])
        groups = {"structured.scope", "structured.task", "structured.wait", "storage.result", "storage.consumer", "runtime.process", "runtime.event"}
        result["allocation"] = []
        for counter in outcome["world"]["allocationCounters"]:
            if counter["group"] not in groups:
                unmapped.append(side + " unknown allocation journal " + counter["group"])
            result["allocation"].append({"group": counter["group"], "domain": str(counter["domain"]),
                "slot": None if counter["slot"] is None else str(counter["slot"]), "nextGeneration": str(counter["nextGeneration"]),
                "persistent": counter["persistent"], "retired": counter["retired"]})
        result["allocation"].sort(key=lambda c: (c["group"], c["domain"], -1 if c["slot"] is None else int(c["slot"])))
        rules = {(int(rule["kind"]), int(rule["store"])): rule for rule in outcome["world"]["allocationRules"]}
        result["limits"] = {"events": str(outcome["world"]["maxEvents"])}
        result["allocationPolicies"] = {}
        event_rule = rules.get((2, 1), {"group": "runtime.event", "perSlot": True, "persistent": True, "allowMax": True, "capacity": outcome["world"]["maxEvents"]})
        result["allocationPolicies"]["events"] = {field: event_rule[field] for field in ("group", "perSlot", "persistent", "allowMax")}
        if str(event_rule["capacity"]) != str(outcome["world"]["maxEvents"]):
            unmapped.append(side + " event rule and world capacity disagree")
        for name, key in {"scopes": (7, 130), "tasks": (8, 131), "tickets": (12, 133), "groups": (4, 132), "results": (5, 20), "consumers": (6, 21), "processes": (3, 10), "processWaits": (4, 11)}.items():
            rule = rules.get(key)
            if rule is None or rule["capacity"] is None:
                unmapped.append(side + " missing bounded allocation rule " + name)
                result["limits"][name] = None
            else:
                result["limits"][name] = str(rule["capacity"])
                result["allocationPolicies"][name] = {field: rule[field] for field in ("group", "perSlot", "persistent", "allowMax")}
        known = set(tags.values()) | {"reference.event"}
        for o in outcome["world"]["objects"]:
            if o["tag"] not in known:
                unmapped.append(side + " unprojected object tag " + o["tag"])
        result["observations"] = [{**observation, "time": str(observation["time"]), "turn": str(observation["turn"]), "values": _v(observation["values"])} for observation in source_observations[side]]
        return mapping.normalize(result, side)

    native = {}
    native["observations"] = [{**observation, "time": str(observation["time"]), "turn": str(observation["turn"]), "values": _v(observation["values"])} for observation in actual_observations]
    native["waitOwners"] = [{key: _h(value) for key, value in owner.items()} for owner in native_raw["waitOwners"]]
    for key in ("scopes", "tasks", "tickets", "waits", "results", "consumers", "bindings", "observers"):
        native[key] = []
        rows = native_categories.get({"scopes": "scope", "tasks": "task", "tickets": "ticket", "waits": "wait", "results": "result", "consumers": "consumer"}.get(key), native_raw[key])
        for row in rows:
            record = dict(row)
            for name in ("identity", "scope", "parent", "process", "result", "consumer", "start", "external", "source", "reservedTask", "task"):
                if name in record:
                    record[name] = _h(record[name])
            for name in ("args", "value"):
                if name in record:
                    record[name] = _v(record[name])
            if key == "scopes":
                record["reason"] = None if record["reason"] is None else {"bytes": list(record["reason"].encode("utf-8"))}
                record["owned"] = [{**x, "handle": _h(x["handle"])} for x in row["owned"]]
                record["actions"] = [{**x, "scope": _h(x["scope"]), "handle": _h(x["handle"])} for x in row["actions"]]
            if key == "waits":
                record["branches"] = [{**b, "source": _h(b["source"]), "result": _h(b["result"]), "consumer": _h(b["consumer"]), "outcome": _v(b["outcome"])} for b in row["branches"]]
            native[key].append(record)
    native["processes"] = []
    for p in native_categories["process"]:
        scopes = [t["scope"] for t in native_categories["task"] if t["process"] is not None and _key(t["process"]) == _key(p["identity"])]
        if len(scopes) > 1:
            unmapped.append("native process has multiple task owner bindings")
        scope = _h(scopes[0]) if scopes else None
        if p["alive"]:
            status = {"0": "0", "1": "0", "2": "1", "3": "1"}.get(str(p["status"]), "invalid")
            native["processes"].append({"identity": _h(p["identity"]), "program": str(p["program"]), "status": status, "wait": _h(p["wait"]), "ordinal": str(p["ordinal"]), "result": None, "scope": scope, "instance": str(p["instance"]), "instanceBound": p["instanceBound"]})
        else:
            # Caller program is source program 0; completion retires its frame.
            native["processes"].append({"identity": _h(p["identity"]), "program": "0", "status": "2", "wait": None, "ordinal": "0", "result": _v(p["completed"]), "scope": scope})
    native["externalWaits"] = [{**w, "identity": _h(w["identity"]), "group": _h(w["group"]), "source": _h(w["source"]), "kind": "100", "value": _v(w["value"])} for w in native_categories["externalWait"]]
    for w in native_categories["externalWait"]:
        if str(w["kind"]) != "0":
            unmapped.append("structured group registered non-Response native wait")
    native["events"] = []
    for e in native_events:
        key = e["key"]
        native["events"].append({"identity": _h(e["identity"]), "time": str(key["time"]), "turn": str(key["turn"]), "stage": str(key["stage"]), "instance": str(key["instance"]),
                                  "connection": str(key["connection"]), "sequence": str(int(key["sequence"])-1), "owner": str(e["owner"]), "cancelled": e["cancelled"], "values": _v(e["value"])})
        if str(e["epoch"]) != "0" or e["state"] != "queued":
            unmapped.append("structured event left unsupported epoch/active batch state")
    for name in ("cancelSequence", "frames", "grants", "pins", "pinLimit"):
        native[name] = str(native_raw[name])
    for name in ("queue", "pending"):
        native[name] = _v(native_raw[name])
    native["nextSequence"] = str(int(native_raw["nextSequence"])-1)
    domain = str(native_categories["scope"][0]["identity"]["domain"])
    native["allocation"] = []
    for group, name, delta in [("structured.scope", "nextScopeGeneration", 0), ("structured.task", "nextTaskGeneration", 0),
            ("structured.wait", "nextGroupGeneration", 0), ("storage.result", "nextResultGeneration", 1),
            ("storage.consumer", "nextConsumerGeneration", 1), ("runtime.process", "nextProcessGeneration", 0)]:
        native["allocation"].append({"group": group, "domain": domain, "slot": None, "nextGeneration": str(int(native_raw[name])+delta), "persistent": True, "retired": False})
    for slot in native_raw["events"]:
        index = int(slot["identity"]["slot"])
        burned = int(slot["identity"]["generation"]) - (1 if index == 0 else 0)
        if burned < 0:
            unmapped.append("frontier marker allocation journal missing")
        if burned > 0:
            native["allocation"].append({"group": "runtime.event", "domain": domain, "slot": str(index), "nextGeneration": str(burned+1), "persistent": True, "retired": slot["state"] == "retired"})
    native["allocation"].sort(key=lambda c: (c["group"], c["domain"], -1 if c["slot"] is None else int(c["slot"])))
    native["generationHighWater"] = str(max(int(counter["nextGeneration"]) for counter in native["allocation"]))
    native["limits"] = {key: str(value) for key, value in native_raw["limits"].items() if key != "liveValues"}
    native["allocationPolicies"] = {name: {"group": group, "perSlot": False, "persistent": True, "allowMax": name in ("results", "consumers")}
        for name, group in [("scopes", "structured.scope"), ("tasks", "structured.task"), ("tickets", "structured.task"), ("groups", "structured.wait"),
                            ("results", "storage.result"), ("consumers", "storage.consumer"), ("processes", "runtime.process"), ("processWaits", "runtime.process")]}
    native["allocationPolicies"]["events"] = {"group": "runtime.event", "perSlot": True, "persistent": True, "allowMax": True}
    for name in ("nextResultGeneration", "nextConsumerGeneration"):
        if int(native_raw[name]) < 0:
            unmapped.append("invalid native generation allocator")
    fuel_scopes = []
    receipts = native_raw.get("childFuelScopes", [])
    completed = {side: [row for row in outcome["world"]["observations"] if row["kind"] == "structured.child.completed"]
                 for side, outcome in outcomes.items()}
    if len({len(receipts), *(len(rows) for rows in completed.values())}) != 1:
        unmapped.append("asymmetric child runtime fuel receipts")
    if len(receipts) > 1:
        unmapped.append("child fixture supports exactly one invocation, not inferred resume scopes")
    for index, receipt in enumerate(receipts):
        if set(receipt) != {"program", "inputFuel", "runtimeCap", "remainingFuel"}:
            unmapped.append("unclassified child fuel receipt field")
            continue
        if any(index >= len(rows) for rows in completed.values()):
            continue
        remaining = {side: _num(rows[index]["values"][-1]) for side, rows in completed.items()}
        target_programs = []
        for side, outcome in outcomes.items():
            proofs = [row for row in outcome["world"]["observations"] if row["kind"] == "process.completed"]
            if not proofs:
                unmapped.append("child budget lacks actual process completion proof")
                continue
            target_programs.append(_num(proofs[-1]["values"][1]))
        if len(target_programs) != 2 or any(program != str(receipt["program"]) for program in target_programs):
            unmapped.append("child budget program differs from actual completed process")
            continue
        actual_remaining = str(receipt["remainingFuel"])
        cap, supplied = int(receipt["runtimeCap"]), int(receipt["inputFuel"])
        if cap <= 0 or int(actual_remaining) > min(cap, supplied) or any(value != actual_remaining for value in remaining.values()):
            unmapped.append("child runtime fuel receipt differs from independent completion")
            continue
        # Runner.runExec/runModel pass parent.remainingFuel directly into the
        # child callback, and finishChild preserves that exact parent field.
        # This is the actual invocation argument, not an opcode-row estimate.
        exec_initial = str(execOutcome["remainingFuel"])
        if min(int(exec_initial), cap) != min(supplied, cap):
            unmapped.append("child actual invocation budgets have different effective runtime fuel")
            continue
        fuel_scopes.append({"program": str(receipt["program"]), "verified": True,
            "execInitialFuel": exec_initial, "nativeInitialFuel": str(supplied), "runtimeCap": str(cap),
            "evidence": {"nativeConstructorReceipt": receipt,
                "modelCallbackInputFuel": str(modelOutcome["remainingFuel"]),
                "execCallbackInputFuel": exec_initial,
                "callbackContract": "Runner passes parent.remainingFuel to the sole child invocation; finishChild preserves parent.remainingFuel.",
                "modelCompletionRuntimeFuel": remaining["model"], "execCompletionRuntimeFuel": remaining["exec"]}})
    error_agreement = None
    if not modelOutcome["ok"] and not execOutcome["ok"] and not nativeOutput["ok"]:
        errors = {modelOutcome.get("error"), execOutcome.get("error")}
        native_error = nativeOutput.get("error", {})
        causes = {"StaleReferenceHandle", "StaleWaitGroup", "SlotProcess"}
        if len(errors) == 1 and next(iter(errors)) in causes and str(native_error.get("code")) == "3":
            error_agreement = {"verified": True, "referenceError": next(iter(errors)), "nativeError": native_error,
                               "rule": "The requested complete capability generation is absent from the authoritative live registry."}
        if errors == {"StaleReferenceHandle"} and nativeOutput.get("caseId") == "structured.scopeTransfer" and str(native_error.get("code")) == "4" and native_error.get("message", "").startswith("transfer owner ("):
            inputs = nativeOutput.get("preparedInputs", [])
            if len(inputs) == 3:
                source_scope = next((scope for scope in native_raw["scopes"] if _key(scope["identity"]) == _key(inputs[1])), None)
                if source_scope is not None and not any(_key(owned["handle"]) == _key(inputs[0]) for owned in source_scope["owned"]):
                    error_agreement = {"verified": True, "referenceError": "StaleReferenceHandle", "nativeError": native_error,
                        "rule": "The source scope owns no entry for the supplied full child capability; the source registry rejects its stale generation and native transfer rejects its absent ownership entry."}
    return {"model": reference(modelOutcome, "model"), "exec": reference(execOutcome, "exec"), "native": mapping.normalize(native, "native"),
            "identities": entries, "unmapped": unmapped, "errorAgreement": error_agreement, "fuelScopes": fuel_scopes,
            "diagnostics": {"frontierMarker": "Native fixture executes exactly one seed batch; sequence is compared after its explicit one-event offset. Full ReadyKeys remain compared.",
                            "allocatorSlots": "Untouched physical slots remain raw. Allocated result/consumer tombstones, actual persistent generation journals, retained dead references, pin occupancy and FIFO order are compared.",
                            "scopePlans": "Native cancellation action IDs, ownership kinds, application flags and global serial are independently reconstructed from source scope plans.",
                            "sourceBounds": "Shared world object/byte limits and native physical registry sizes use different storage layouts; actual per-pool frame/result/queue/ticket bounds are exercised by source and native stores."}}


def negative_controls(model_outcome, exec_outcome, native_output):
    """Mutate actual evidence; a control is applicable only when its state exists."""
    from copy import deepcopy
    baseline = project(model_outcome, exec_outcome, native_output)
    if baseline["unmapped"] or baseline["model"] != baseline["exec"] or baseline["model"] != baseline["native"]:
        raise ValueError("negative controls require an actually matching structured baseline")
    controls = {}

    def check(name, mutate):
        changed = deepcopy(native_output)
        if not mutate(changed["provider"]):
            return
        try:
            observed = project(model_outcome, exec_outcome, changed)
            rejected = bool(observed["unmapped"]) or observed["model"] != observed["native"]
        except (KeyError, ValueError, TypeError):
            rejected = True
        controls[name] = rejected

    def field(name, delta):
        def mutate(provider):
            provider[name] = str(int(provider[name])+delta)
            return True
        return mutate

    def owner(provider):
        provider["scopes"][0]["identity"]["owner"] = "999"
        return True

    def generation(provider):
        provider["scopes"][0]["identity"]["generation"] = str(int(provider["nextScopeGeneration"])+100)
        return True

    def terminal(provider):
        if not provider["tasks"]:
            return False
        provider["tasks"][0]["status"] = str((int(provider["tasks"][0]["status"])+1) % 6)
        return True

    def liveness(provider):
        for result in provider["results"]:
            if int(result["source"]["generation"]) > 0:
                result["alive"] = not result["alive"]
                return True
        return False

    def order(provider):
        for scope in provider["scopes"]:
            if len(scope["owned"]) > 1:
                scope["owned"].reverse()
                return True
        return False

    def ready(provider):
        for event in provider["events"]:
            if event["state"] == "queued":
                event["key"]["turn"] = str(int(event["key"]["turn"])+1)
                return True
        return False

    def process_instance(provider):
        for process in provider["processes"]:
            if process["alive"]:
                process["instance"] = str(int(process["instance"])+1)
                return True
        return False

    def process_binding(provider):
        for process in provider["processes"]:
            if process["alive"]:
                process["instanceBound"] = not process["instanceBound"]
                return True
        return False

    def unknown(provider):
        provider["unmappedMutableState"] = {"active": True}
        return True

    for name, mutate in [("owner", owner), ("generation", generation), ("terminal", terminal),
                         ("liveness", liveness), ("ownership-order", order), ("ready-turn", ready),
                         ("allocation-counter", field("nextTaskGeneration", 1)), ("process-instance", process_instance),
                         ("process-binding", process_binding), ("unknown-state", unknown)]:
        check(name, mutate)
    # A malformed input cannot be normalized merely because its store/slot
    # resembles a seeded capability. Mutate only native authority/displacement.
    def input_handles(value):
        if isinstance(value, dict):
            if value.get("kind") == "handle":
                return [value["identity"]]
            return [h for child in value.values() for h in input_handles(child)]
        if isinstance(value, list):
            return [h for child in value for h in input_handles(child)]
        return []
    for field in ("owner", "generation"):
        changed = deepcopy(native_output)
        candidates = input_handles(changed.get("preparedInputs", []))
        if not candidates:
            continue
        candidates[0][field] = str(int(candidates[0][field])+1)
        try:
            observed = project(model_outcome, exec_outcome, changed)
            rejected = bool(observed["unmapped"]) or observed["model"] != observed["native"]
        except (KeyError, ValueError, TypeError):
            rejected = True
        controls["input-" + field] = rejected
    return controls
