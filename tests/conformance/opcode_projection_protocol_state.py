"""Decode committed protocol and result-store state; never select expected case outputs."""
from .opcode_projection_common import identity
from .opcode_projection_protocol import _value, _handle


def objects(world, tag):
    return [o for o in world["objects"] if o["tag"] == tag]


def canonical(value):
    if isinstance(value, dict):
        if set(value) == {"kind", "domain", "store", "slot", "generation", "owner"}:
            return _handle(value)
        if set(value) in ({"bytes"}, {"bool"}, {"u64"}, {"unit"}, {"handle"}):
            return _value(value)
        return {key: canonical(item) for key, item in value.items()}
    if isinstance(value, list):
        return [canonical(item) for item in value]
    return value


def payload(record):
    return {"command": record[6], "address": record[7],
            "data": [str(x) for x in record[9]], "streamingWidth": record[8],
            "byteEnable": [str(x) for x in record[11]], "status": record[12],
            "dmiHint": record[13], "extensions": record[15]}


def result_reference(world):
    rules = {r["group"]: r for r in world["allocationRules"]}
    capacities = [int(rules[group]["capacity"]) for group in ("storage.result", "storage.consumer")]
    counters = {c["group"]: c for c in world["allocationCounters"]}
    results, consumers = [], []
    for obj in objects(world, "storage.consumer"):
        d = _value(obj["value"])
        consumers.append({"identity": _handle(obj["identity"]), "result": d[0], "active": obj["alive"]})
    for obj in objects(world, "storage.result"):
        d = _value(obj["value"])
        if len(d) != 12:
            raise ValueError("complete twelve-field ResultRecord required")
        count = sum(c["active"] and identity(c["result"]) == identity(obj["identity"]) for c in consumers)
        results.append({"identity": _handle(obj["identity"]), "alive": obj["alive"],
            "producerAlive": d[3], "publishing": d[11], "value": d[5] if d[4] else None,
            "ready": {"time": d[6], "turn": d[7]} if d[4] else None,
            "consumerCount": str(count), "pinCount": d[8],
            "create": {"source": d[0], "type": d[1], "maxBytes": d[2],
                       "producerOwner": d[9], "consumerOwner": d[10]}})
    results.sort(key=lambda r: int(identity(r["identity"])["slot"]))
    consumers.sort(key=lambda r: int(identity(r["identity"])["slot"]))
    occupied = [{int(identity(r["identity"])["slot"]) for r in rows} for rows in (results, consumers)]
    return {"results": results, "consumers": consumers,
        "resultCapacity": str(capacities[0]), "consumerCapacity": str(capacities[1]),
        "unusedResultSlots": [str(n) for n in range(capacities[0]) if n not in occupied[0]],
        "unusedConsumerSlots": [str(n) for n in range(capacities[1]) if n not in occupied[1]],
        "unusedSlotViolations": [],
        "nextResultGeneration": str(int(counters["storage.result"]["nextGeneration"]) - 1),
        "nextConsumerGeneration": str(int(counters["storage.consumer"]["nextGeneration"]) - 1),
        "pinCount": str(sum(int(r["pinCount"]) for r in results)), "pinLimit": str(world["maxPins"])}


def result_native(provider, context):
    original = provider["resultStore"]
    results, consumers, unused_results, unused_consumers, violations = [], [], [], [], []
    stores = {str(row["identity"]["store"]) for key in ("results", "consumers") for row in original[key]}
    if len(stores) != 1:
        violations.append("result/consumer physical store coherence")
    zero = dict(kind="0", domain="0", store="0", slot="0", generation="0", owner="0")
    for kind, key, retained, unused in (("5", "results", results, unused_results), ("6", "consumers", consumers, unused_consumers)):
        for slot, row in enumerate(original[key]):
            h = identity(row["identity"])
            if h["kind"] != kind or h["domain"] != str(context["domain"]) or int(h["slot"]) != slot:
                violations.append(f"{key}/{slot}/identity")
            if h["generation"] != "0":
                retained.append(canonical(row))
                continue
            unused.append(str(slot))
            if h["owner"] != "0":
                violations.append(f"{key}/{slot}/unallocatedOwner")
            if key == "results":
                blank = {"alive": False, "producerAlive": False, "publishing": False,
                    "value": None, "ready": None, "consumerCount": "0", "pinCount": "0",
                    "create": {"source": zero, "type": "0", "maxBytes": "4096", "producerOwner": "0", "consumerOwner": "0"}}
            else:
                blank = {"result": zero, "active": False}
            if {k: v for k, v in row.items() if k != "identity"} != blank:
                violations.append(f"{key}/{slot}/unallocatedState")
    return {"results": results, "consumers": consumers,
        "resultCapacity": str(len(original["results"])), "consumerCapacity": str(len(original["consumers"])),
        "unusedResultSlots": unused_results, "unusedConsumerSlots": unused_consumers,
        "unusedSlotViolations": violations,
        **{k: original[k] for k in ("nextResultGeneration", "nextConsumerGeneration", "pinCount", "pinLimit")}}


def protocol_reference(world, context):
    entries = objects(world, "protocol.control")
    if len(entries) != 1 or not entries[0]["alive"]:
        raise ValueError("one live typed protocol control required")
    c = _value(entries[0]["value"])
    if len(c) != 23:
        raise ValueError("complete typed protocol control required")
    transactions = objects(world, "runtime.transaction")
    payloads = {tuple(sorted(identity(p["identity"]).items())): _value(p["value"]) for p in objects(world, "storage.payload")}
    ledgers, drains, ack_cells = [], [], []
    for entry in transactions:
        t = _value(entry["value"])
        a, ledger, drain, ack = t[8:12]
        if not entry["alive"]:
            ledgers.append({"transaction": _handle(entry["identity"]), "hop": t[3], "alive": False})
            continue
        request = payloads[tuple(sorted(identity(t[4]).items()))]
        admission = {"txn": _handle(entry["identity"]), "hop": t[3], "connection": t[0], "transport": t[1],
            "transportGeneration": t[2], "inTime": a[0], "request": payload(request),
            "wireTerminal": a[6], "semanticTerminal": a[7], "pending": a[8], "servicing": a[9],
            "sequence": a[1], "resetDeferred": a[10],
            "route": {"upstream": a[4], "downstream": a[5], "originalAddress": a[2], "localAddress": a[3]}}
        wire = {"identity": {"domain": str(context["domain"]), "localSide": str(context["instanceId"]),
            "connection": t[0], "transport": t[1], "transportGeneration": t[2]},
            "state": ledger[0], "lastTiming": ledger[1], "faulted": ledger[2], "pending": ledger[3],
            "callOrdinal": ledger[4], "protocolState": ledger[5]}
        ledgers.append({"transaction": _handle(entry["identity"]), "hop": t[3], "alive": True,
                        "admission": admission, "wire": wire})
        complete = drain[7] and (not drain[2] or (drain[3] and drain[4] and drain[5] and not drain[6]))
        if drain[0] and not complete:
            hops = [{"hop": t[3], "wireTerminal": drain[3], "timingConsumed": drain[4],
                     "cleanupReturned": drain[5], "callPin": drain[6]}] if drain[2] else []
            drains.append({"transaction": _handle(entry["identity"]), "instance": str(context["instanceId"]),
                "epoch": drain[1], "parent": None, "hops": hops, "localFinished": drain[7], "cancelled": drain[8]})
        if ack[0]:
            ack_cells.append({"value": ack[1], "version": ack[2], "epoch": ack[3],
                              "stableBytes": ack[4], "epochIndependent": False})
    gates = []
    for entry in objects(world, "protocol.gate"):
        if not entry["alive"]:
            continue
        d = _value(entry["value"])
        gates.append({"handle": _handle(entry["identity"]), "transaction": d[0], "connection": d[1],
                      "state": d[2], "ready": {"time": d[3], "turn": d[4]}, "sequence": d[5]})
    intents = [{"connection": i[1], "transaction": i[0], "flow": "1" if i[3] in ("2", "3") else "0",
                "phase": i[3], "notBefore": i[4], "callId": i[5], "transport": i[2], "payload": payload(i[6])} for i in c[14]]
    live = [t for t in transactions if t["alive"]]
    return {"drainCounters": {"nextReceipt": c[3], "nextReset": c[4],
            "receiptCount": str(sum(int(_value(t["value"])[10][9]) > 0 and not _value(t["value"])[10][10] for t in transactions)), "capacity": c[11],
            "hopLimit": c[21], "responsibilities": str(sum(_value(t["value"])[10][0] for t in transactions))},
        "runtimeCounters": {"nextCall": c[2], "wakeGeneration": c[13], "allocatedCalls": str(len(intents)),
            "intentCapacity": c[18], "callCapacity": c[19], "ledgerCapacity": c[10], "drainCapacity": c[11],
            "preparedIntents": "0", "committedIntents": str(len(intents))},
        "protocolCounters": {"nextGeneration": c[17], "ledgers": str(len(live)), "pendingCalls": "0",
            "preparedBindings": "0", "preparedRetirements": "0", "ledgerCapacity": c[10],
            "ticketCapacity": c[19], "callsPerLedger": c[20],
            "requestLaneFree": not any(_value(t["value"])[9][6] for t in live),
            "responseLaneFree": not any(_value(t["value"])[9][7] for t in live)},
        "admissionStore": {"nextGeneration": c[0], "nextSequence": c[1], "revision": c[15], "prepared": False,
            "activeServices": str(sum(_value(t["value"])[8][9] for t in live)), "reservedResponses": "0",
            "responseOrder": c[22], "limits": {"transactions": c[5], "hops": c[6], "services": c[7], "responses": c[9], "gates": c[8]},
            "gates": gates, "responses": [], "wireBusy": [{"connection": p[0], "busy": p[1]} for p in sorted(c[16], key=lambda p: int(p[0]))]},
        "ledgers": ledgers, "drains": drains, "drainOutstanding": str(len(drains)),
        "activeServices": str(sum(_value(t["value"])[8][9] for t in live)), "intents": intents,
        "ack": ack_cells[0] if len(ack_cells) == 1 else None,
        "stopped": False, "stopDetail": "", "wake": {"time": str(max(int(context["now"]), min(int(i["notBefore"]) for i in intents))), "turn": "0"} if intents else None}


def protocol_native(native, context):
    p = native["provider"]
    ids = {entry["name"]: entry["identity"] for entry in native["identities"]}
    ledgers = []
    for index, row in enumerate(p["ledgers"]):
        t, h = _handle(ids[f"transaction.{index}"]), _handle(ids[f"hop.{index}"])
        if "error" in row["admission"] or "error" in row["wire"]:
            valid = all(str(row[key]["error"]["code"]) == "3" for key in ("admission", "wire"))
            ledgers.append({"transaction": t, "hop": h, "alive": False,
                            **({} if valid else {"retirementErrors": canonical(row)})})
        else:
            ledgers.append({"transaction": t, "hop": h, "alive": True,
                            "admission": canonical(row["admission"]), "wire": canonical(row["wire"]["value"])})
    return {**{key: canonical(p[key]) for key in ("drainCounters", "runtimeCounters", "protocolCounters", "admissionStore",
        "drains", "drainOutstanding", "activeServices", "intents", "ack", "stopped", "stopDetail", "wake")}, "ledgers": ledgers}

def host_reference(world, initial_world):
    before = initial_world.get("observations", [])
    after = world.get("observations", [])
    if after[:len(before)] != before:
        raise ValueError("initial reference observation prefix was rewritten")
    records = []
    for event in after[len(before):]:
        if event["kind"] == "protocol.host.arm":
            values = [_value(v) for v in event["values"]]
            if len(values) != 3:
                raise ValueError("invalid typed host arm observation")
            records.append({"ordinal": str(len(records)), "kind": "arm",
                            "value": dict(zip(("time", "turn", "generation"), values))})
    return records
def result_query_reference(world):
    pool = result_reference(world)
    if not pool["results"]:
        return None
    if len(pool["results"]) != 1 or len(pool["consumers"]) != 1:
        raise ValueError("query fixture requires its one held owner and consumer")
    row, consumer = pool["results"][0], pool["consumers"][0]
    if not row["alive"]:
        error = {"error": {"code": "3", "message": "result generation"}}
    elif not consumer["active"]:
        error = {"error": {"code": "6", "message": "consumer consumed"}}
    elif row["ready"] is None:
        error = {"error": {"code": "7", "message": "result reserved"}}
    else:
        error = None
    ownership = ({"consumers": row["consumerCount"], "pins": row["pinCount"],
        "published": row["ready"] is not None, "ownerReleased": not row["producerAlive"],
        "publishing": row["publishing"]} if row["alive"] else
        {"error": {"code": "3", "message": "result generation"}})
    return {"read": error if error else {"value": row["value"]},
            "ready": error if error else {"value": row["ready"]},
            "ownership": ownership, "alive": row["alive"] and row["producerAlive"]}
def setup_reference(world):
    def response(row):
        if row is None:
            return None
        if len(row) != 4:
            raise ValueError("response snapshot codec")
        return {"status": row[0], "data": [str(x) for x in row[1]],
                "dmiHint": row[2], "extensions": row[3]}
    records = []
    for event in world.get("observations", []):
        if event["kind"] != "protocol.setup.exchange":
            continue
        values = [_value(v) for v in event["values"]]
        if len(values) != 3:
            raise ValueError("full actual setup exchange codec required")
        call, returned, exchange = values
        if len(call) != 8 or len(returned) != 4 or len(exchange) != 8:
            raise ValueError("setup wire record arity")
        p = call[7]
        request = {"command": p[0], "address": p[1], "data": [str(x) for x in p[2]],
            "streamingWidth": p[3], "byteEnable": [str(x) for x in p[4]],
            "status": p[5], "dmiHint": p[6], "extensions": p[7]}
        a = dict(zip(("id", "connection", "transport", "flow", "phase", "callTime", "incomingDelay"), call[:7]))
        a["request"] = request
        b = {"sync": returned[0], "phase": returned[1], "outgoingDelay": returned[2], "response": response(returned[3])}
        c = {"callId": exchange[0], "hop": exchange[1], "nextState": exchange[2],
             "milestones": [{"kind": m[0], "time": m[1], "implicit": m[2], "response": response(m[3])} for m in exchange[3]],
             "traceTags": exchange[4], "needsAck": exchange[5], "wireTerminal": exchange[6], "ignored": exchange[7]}
        for name, row in (("call", a), ("return", b), ("exchange", c)):
            records.append({"ordinal": str(len(records)), "kind": name, "value": row})
    return records