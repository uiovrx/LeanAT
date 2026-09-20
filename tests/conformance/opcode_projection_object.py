"""Object-family projection from observed state, with explicit unsupported mappings.

No expected returned value or state byte is used to construct the native projection.
Native owned Value arrays are decoded using the versioned object record schemas.
Allocator limits that do not yet share a cross-backend contract remain unmapped.
"""
from copy import deepcopy
import re
try:
    from .opcode_projection_common import IdentityBijection, identity
except ImportError:
    from opcode_projection_common import IdentityBijection, identity

U64 = "u64"
BOOL = "bool"
BYTES = "bytes"
HANDLE = "handle"
UNIT = "unit"

def rec(*fields):
    return ("record", fields)

def vec(element):
    return ("vec", element)

def variant(*constructors):
    return ("variant", constructors)

def option(element):
    return variant((), (element,))

GRANT_ROW = rec(BOOL, HANDLE, U64, U64, U64, U64, BOOL)
FIELD = rec(U64, U64, U64, U64)
REGISTER = rec(U64, U64, vec(U64), U64, BOOL, vec(FIELD))
# The source object opcode catalog declares its ValueOnly queue element as UInt64.
QUEUE_ROW = rec(U64, option(U64), option(HANDLE), BOOL, option(rec(HANDLE, HANDLE)))
SCHEMAS = {
    "reference.object.memory": rec(U64, BYTES, BYTES, U64, BOOL),
    "reference.object.register": rec(U64, BYTES, BYTES, vec(REGISTER)),
    "reference.object.resource": rec(*([U64] * 10), vec(U64), vec(GRANT_ROW)),
    "reference.object.queue": rec(*([U64] * 6), vec(QUEUE_ROW)),
    "reference.object.pipeline": rec(*([U64] * 10), vec(U64), vec(GRANT_ROW)),
}
KINDS = {name: i for i, name in enumerate(SCHEMAS)}


def decimal(value):
    text = str(value)
    if not text.isascii() or not text.isdecimal() or str(int(text)) != text:
        raise ValueError("noncanonical unsigned integer")
    return text


def bits(value):
    value = decimal(value)
    if int(value) >= 2**64:
        raise ValueError("UInt64 range")
    return {"kind": "bits", "width": 64, "value": value}


def boolean(value):
    if type(value) is not bool:
        raise ValueError("expected bool")
    return {"kind": "bool", "value": value}


def record(fields):
    return {"kind": "record", "fields": fields}


def vector(values):
    return {"kind": "vec", "values": values}


def handle(value):
    return {"kind": "handle", "identity": identity(value)}


def native_value(value, schema):
    """Decode native data without consulting the corresponding reference value."""
    if schema == U64:
        if set(value) != {"u64"}:
            raise ValueError("native UInt64 representation")
        return bits(value["u64"])
    if schema == BOOL:
        if set(value) != {"bool"}:
            raise ValueError("native Bool representation")
        return boolean(value["bool"])
    if schema == BYTES:
        if set(value) != {"bytes"}:
            raise ValueError("native Bytes representation")
        data = [int(decimal(x)) for x in value["bytes"]]
        if any(x > 255 for x in data):
            raise ValueError("native byte range")
        return {"kind": "bytes", "data": data}
    if schema == HANDLE:
        return handle(value)
    if schema == UNIT:
        if set(value) != {"unit"}:
            raise ValueError("native Unit representation")
        return {"kind": "unit"}
    if not isinstance(value, dict) or set(value) != {"array"}:
        raise ValueError("native aggregate representation")
    values = value["array"]
    if not isinstance(values, list):
        raise ValueError("native aggregate is not a list")
    kind, fields = schema
    if kind == "vec":
        return vector([native_value(v, fields) for v in values])
    if kind == "record":
        if len(values) != len(fields):
            raise ValueError("native record arity")
        return record([native_value(v, t) for v, t in zip(values, fields)])
    if kind == "variant":
        if len(values) != 2:
            raise ValueError("native variant arity")
        tag = int(native_value(values[0], U64)["value"])
        if tag >= len(fields) or set(values[1]) != {"array"}:
            raise ValueError("native variant tag")
        payload = values[1]["array"]
        if len(payload) != len(fields[tag]):
            raise ValueError("native variant constructor arity")
        return {"kind": "variant", "tag": tag,
                "fields": [native_value(v, t) for v, t in zip(payload, fields[tag])]}
    raise ValueError("unknown object schema")


def reference_value(value):
    """Preserve source record/vec/variant distinctions and all literal values."""
    v = deepcopy(value)
    kind = v.get("kind")
    if kind == "handle":
        return handle(v)
    if kind == "bits":
        v["value"] = decimal(v["value"])
    if kind in ("record", "variant"):
        v["fields"] = [reference_value(f) for f in v["fields"]]
    elif kind == "vec":
        v["values"] = [reference_value(f) for f in v["values"]]
    return v


def semantic_object(entry, native=False):
    tag = entry["tag"]
    if tag not in SCHEMAS:
        raise ValueError("unknown object tag: " + tag)
    if native:
        if entry.get("valueSchema") != tag:
            raise ValueError("native object schema witness")
        desc = {"id": decimal(entry["objectId"]), "domain": decimal(entry["domain"]),
                "instance": decimal(entry["instance"]), "providerKind": decimal(entry["providerKind"])}
        value = native_value(entry["value"], SCHEMAS[tag])
    else:
        raw = identity(entry["identity"])
        value = reference_value(entry["value"])
        desc = {"id": raw["slot"], "domain": raw["domain"],
                "instance": value["fields"][0]["value"], "providerKind": str(KINDS[tag])}
    if desc["providerKind"] != str(KINDS[tag]) or value["fields"][0] != bits(desc["instance"]):
        raise ValueError("object descriptor/value instance mismatch")
    if type(entry["alive"]) is not bool:
        raise ValueError("object lifetime flag")
    return {"object": desc, "tag": tag, "alive": entry["alive"], "value": value}


def _tickets(objects):
    result = {}
    for obj in objects:
        if obj["tag"] in ("reference.object.resource", "reference.object.pipeline"):
            rows = obj["value"]["fields"][11]["values"]
            for index, row in enumerate(rows):
                result[f"object.{obj['object']['id']}.ticket.{index}"] = row["fields"][1]
    return result


def _reference_events(world):
    events = []
    for event in world["events"]:
        if event["kind"] != "object.pipeline.ready":
            raise ValueError("object projection does not own event family " + event["kind"])
        events.append({"identity": handle(event["identity"]),
                       **{k: decimal(event[k]) for k in ("time", "turn", "stage", "instanceId", "connection", "sequence")},
                       "kind": event["kind"], "values": [reference_value(v) for v in event["values"]],
                       "source": event["source"], "cancelled": event["cancelled"]})
    return events


def _native_events(provider, unmapped):
    events = []
    for slot in provider["events"]:
        state = slot["state"]
        queued = slot["event"]
        if state in ("free", "retired"):
            if int(identity(queued["token"])["generation"]) != 0:
                unmapped.append("retired/free native event generation requires retirement projection")
            continue
        if state not in ("queued", "active"):
            unmapped.append("native event slot state " + state)
            continue
        draft = queued["event"]
        key = draft["key"]
        ticket = native_value(draft["value"], HANDLE)
        if ticket["identity"]["kind"] != "11":
            raise ValueError("non-pipeline internal event")
        if identity(queued["token"])["owner"] != decimal(draft["owner"]):
            raise ValueError("event token/draft owner disagreement")
        events.append({"identity": handle(queued["token"]),
                       "time": decimal(key["time"]), "turn": decimal(key["turn"]),
                       "stage": decimal(key["stage"]), "instanceId": decimal(key["instance"]),
                       "connection": decimal(key["connection"]), "sequence": decimal(key["sequence"]),
                       "kind": "object.pipeline.ready", "values": [ticket, bits(draft["epoch"])],
                       "source": "", "cancelled": queued["cancelled"]})
        if state == "active":
            unmapped.append("active batch cursor needs scheduler projection")
    # The native slot array is physical order; event semantics use its complete key.
    events.sort(key=lambda e: tuple(int(e[k]) for k in ("time", "turn", "stage", "instanceId", "connection", "sequence")))
    return events


def _reference_world(outcome, unmapped):
    world = outcome["world"]
    objects, resources = [], []
    for entry in world["objects"]:
        if entry["tag"] in SCHEMAS:
            objects.append(semantic_object(entry))
        elif entry["tag"] == "reference.event":
            matching = [e for e in world["events"] if identity(e["identity"]) == identity(entry["identity"])]
            if len(matching) != 1 or not entry["alive"] or entry["value"] != bits(matching[0]["sequence"]):
                unmapped.append("reference event allocation metadata differs from event row")
        else:
            resources.append({"identity": handle(entry["identity"]), "tag": entry["tag"],
                              "alive": entry["alive"], "value": reference_value(entry["value"])})
    if any(r["tag"] not in ("structured.scope", "storage.result", "storage.consumer") for r in resources):
        unmapped.append("unknown auxiliary object schema")
    if world["observations"]:
        unmapped.append("reference observations require host observation projection")
    counters = deepcopy(world.get("allocationCounters", []))
    rules = deepcopy(world.get("allocationRules", []))
    domain = objects[0]["object"]["domain"] if objects else "0"
    for rule in rules:
        if rule["group"] in ("storage.result", "storage.consumer") and not any(c["group"] == rule["group"] for c in counters):
            counters.append({"group": rule["group"], "domain": domain, "slot": None,
                             "nextGeneration": "1", "persistent": True, "retired": False})
    if any(c["group"] == "runtime.event" for c in counters) and not any(r["group"] == "runtime.event" for r in rules):
        rules.append({"kind": 2, "store": "1", "group": "runtime.event", "perSlot": True,
                      "persistent": True, "allowMax": True, "capacity": world["maxEvents"]})
    return {"objects": objects, "events": _reference_events(world),
            "eventCapacity": decimal(world["maxEvents"]), "nextSequence": decimal(world["nextSequence"]),
            "pinLimit": decimal(world["maxPins"]), "auxiliaryObjects": resources,
            "allocationCounters": allocation_counters(counters, unmapped),
            "allocationRules": allocation_rules(rules, unmapped)}


def allocation_rules(rules, unmapped, native=False):
    normalized = []
    kinds = {"runtime.event": "2", "storage.result": "5", "storage.consumer": "6", "structured.scope": "7"}
    for rule in rules:
        if set(rule) != {"kind", "store", "group", "perSlot", "persistent", "allowMax", "capacity"}:
            raise ValueError("allocator rule schema")
        group = rule["group"]
        if group not in kinds:
            unmapped.append("allocator rule requires its owner projection: " + group)
            normalized.append(deepcopy(rule))
            continue
        if decimal(rule["kind"]) != kinds[group] or rule["capacity"] is None:
            raise ValueError("native allocator group kind/capacity contract missing")
        expected_store = {"runtime.event": "1", "storage.result": "20", "storage.consumer": "21", "structured.scope": "130"}[group]
        if not native and decimal(rule["store"]) != expected_store:
            raise ValueError("reference allocator rule uses another provider store")
        normalized.append({"kind": kinds[group], "storeNamespace": group, "group": group,
                           "perSlot": boolean(rule["perSlot"])["value"],
                           "persistent": boolean(rule["persistent"])["value"],
                           "allowMax": boolean(rule["allowMax"])["value"],
                           "capacity": decimal(rule["capacity"])})
    return sorted(normalized, key=lambda r: r["group"])


def allocation_counters(counters, unmapped):
    normalized = []
    for counter in counters:
        if set(counter) != {"group", "domain", "slot", "nextGeneration", "persistent", "retired"}:
            raise ValueError("allocator counter schema")
        group = counter["group"]
        if group not in ("runtime.event", "storage.result", "storage.consumer", "structured.scope"):
            unmapped.append("allocator group requires its owner projection: " + group)
        normalized.append({"group": group, "domain": decimal(counter["domain"]),
                           "slot": None if counter["slot"] is None else decimal(counter["slot"]),
                           "nextGeneration": decimal(counter["nextGeneration"]),
                           "persistent": boolean(counter["persistent"])["value"],
                           "retired": boolean(counter["retired"])["value"]})
    return sorted(normalized, key=lambda c: (c["group"], int(c["domain"]), -1 if c["slot"] is None else int(c["slot"])))


def _native_world(provider, unmapped):
    known = {"objects", "scopes", "scopeMetadata", "events", "eventOccupied", "frontier", "batch", "batchReady", "members",
             "cursor", "resolverCursor", "reclaimCursor", "resolverTotal", "nextBatch", "nextSequence", "maxEventBytes",
             "results", "consumers", "nextResultGeneration", "nextConsumerGeneration", "resultPins", "resultPinLimit", "protocol", "protocolIdentities", "drainReceipt"}
    if provider.get("protocol") is not None or provider.get("drainReceipt") is not None:
        unmapped.append("composed protocol drain lifecycle requires protocol projection")
    for key in set(provider) - known:
        unmapped.append("unknown native provider field: " + key)
    if provider.get("batch") is not None or provider.get("members") or int(provider.get("cursor", "0")):
        unmapped.append("native live batch metadata needs scheduler mapping")
    auxiliary = []
    if provider.get("scopes"):
        metadata = provider["scopeMetadata"]
        if metadata["observers"] or int(metadata["sequence"]) != 0:
            raise ValueError("object scope fixture supports initial scope authority only")
        for scope in provider["scopes"]:
            if scope["hasPlan"] or scope["plan"] is not None or scope["owned"] or int(scope["state"]) != 0:
                raise ValueError("object scope fixture unexpected cancellation lifecycle")
            parent = identity(scope["parent"])
            if int(parent["generation"]) != 0:
                raise ValueError("object scope fixture requires root scope")
            none = {"kind": "variant", "tag": 0, "fields": []}
            auxiliary.append({"identity": handle(scope["identity"]), "tag": "structured.scope", "alive": True,
                              "value": record([none, bits(scope["state"]), vector([]), deepcopy(none),
                                               vector([]), bits(metadata["sequence"])])})
    result_slots = provider.get("results", [])
    consumers = provider.get("consumers", [])
    for result in result_slots:
        raw = identity(result["identity"])
        if int(raw["generation"]) == 0:
            if result["alive"]:
                raise ValueError("live native result has zero generation")
            continue
        published = result["value"] is not None
        if published != (result["ready"] is not None):
            raise ValueError("result value/ready publication disagreement")
        if raw["owner"] != decimal(result["producerOwner"]):
            raise ValueError("result producer identity disagrees with creation metadata")
        if int(result["consumerCount"]) != sum(c["active"] and identity(c["result"]) == raw for c in consumers):
            raise ValueError("result consumer count disagrees with actual active capabilities")
        if published and int(result["type"]) != 0:
            raise ValueError("object fixture result type is outside its UInt64 host schema")
        value = native_value(result["value"], U64) if published else {"kind": "unit"}
        ready = result["ready"] if published else {"time": 0, "turn": 0}
        auxiliary.append({"identity": handle(raw), "tag": "storage.result", "alive": result["alive"],
                          "value": record([handle(result["source"]), bits(result["type"]), bits(result["maxBytes"]),
                                           boolean(result["producerAlive"]), boolean(published), value,
                                           bits(ready["time"]), bits(ready["turn"]), bits(result["pinCount"]),
                                           bits(result["producerOwner"]), bits(result["consumerOwner"]),
                                           boolean(result["publishing"])])})
    for consumer in consumers:
        raw = identity(consumer["identity"])
        if int(raw["generation"]) == 0:
            if consumer["active"]:
                raise ValueError("active native consumer has zero generation")
            continue
        # Native ResultStore has no deferred DropOnTerminal state; its release deactivates immediately.
        auxiliary.append({"identity": handle(raw), "tag": "storage.consumer", "alive": consumer["active"],
                          "value": record([handle(consumer["result"]), boolean(False)])})
    if sum(int(r["pinCount"]) for r in result_slots) != int(provider.get("resultPins", 0)):
        raise ValueError("result pin total disagrees with slot counts")
    counters = []
    if provider.get("scopes"):
        counters.append({"group": "structured.scope", "domain": identity(provider["scopes"][0]["identity"])["domain"],
                         "slot": None, "nextGeneration": provider["scopeMetadata"]["nextAllocationGeneration"],
                         "persistent": True, "retired": False})
    for slot in provider["events"]:
        raw = identity(slot["event"]["token"])
        if int(raw["generation"]):
            counters.append({"group": "runtime.event", "domain": raw["domain"], "slot": raw["slot"],
                             "nextGeneration": str(int(raw["generation"]) + 1), "persistent": True,
                             "retired": slot["state"] == "retired"})
    result_allocator_used = (int(provider.get("nextResultGeneration", 0)) or int(provider.get("nextConsumerGeneration", 0))
                             or provider.get("protocol") is not None)
    for group, key, slots in (("storage.result", "nextResultGeneration", result_slots),
                             ("storage.consumer", "nextConsumerGeneration", consumers)):
        last = int(provider.get(key, 0))
        if result_allocator_used:
            if not slots:
                raise ValueError("allocator counter has no physical pool")
            counters.append({"group": group, "domain": identity(slots[0]["identity"])["domain"], "slot": None,
                             "nextGeneration": str(last + 1), "persistent": True, "retired": False})
    rules = []
    for group in sorted({c["group"] for c in counters}):
        kind, capacity = {"runtime.event": (2, len(provider["events"])),
                          "storage.result": (5, len(result_slots)),
                          "storage.consumer": (6, len(consumers)),
                          "structured.scope": (7, provider["scopeMetadata"]["capacity"] if provider.get("scopeMetadata") else 0)}[group]
        pool_identity = (provider["events"][0]["event"]["token"] if group == "runtime.event" else
                         result_slots[0]["identity"] if group == "storage.result" else
                         consumers[0]["identity"] if group == "storage.consumer" else provider["scopes"][0]["identity"])
        rules.append({"kind": kind, "store": identity(pool_identity)["store"], "group": group, "perSlot": group == "runtime.event",
                      "persistent": True, "allowMax": group != "structured.scope", "capacity": str(capacity)})
    events = _native_events(provider, unmapped)
    if int(provider["eventOccupied"]) != len(events):
        unmapped.append("native event occupancy includes unprojected slots")
    return {"objects": [semantic_object(entry, True) for entry in provider["objects"]],
            "events": events, "eventCapacity": str(len(provider["events"])),
            "nextSequence": decimal(provider["nextSequence"]), "pinLimit": decimal(provider["resultPinLimit"]),
            "auxiliaryObjects": auxiliary, "allocationCounters": allocation_counters(counters, unmapped),
            "allocationRules": allocation_rules(rules, unmapped, native=True)}


def error_agreement(model, executable, native):
    if any(side.get("ok") for side in (model, executable, native)):
        return None
    cause = model.get("error")
    if not isinstance(cause, str) or cause != executable.get("error"):
        return None
    observed = native.get("error")
    rule = None
    if cause == "ObjectRollback" and observed == cause:
        rule = "explicit source Fail message is preserved by both VM backends"
    elif isinstance(observed, dict) and set(observed) == {"code", "message"}:
        message = observed["message"]
        suffix = re.fullmatch(r"(.*) \(program [0-9]+, block [0-9]+, opcode [0-9]+, [^\n]*\)", message)
        base = suffix.group(1) if suffix else message
        exact = {
            ("RegisterFieldWidth", "21", "field value width"): "register field byte width or unused high bits rejected",
            ("ResourceOwner", "4", "resource caller owner"): "transaction caller ownership rejected before reservation",
            ("PipelineActiveEventRequired", "7", "event is not current batch cursor"): "Ready requires the authoritative active dispatch cursor",
            ("PipelineEventMissing", "3", "event generation"): "received event generation is stale",
        }
        rule = exact.get((cause, str(observed["code"]), base))
        if cause == "MemoryRangeOrMask" and str(observed["code"]) == "0":
            args = native.get("preparedInputs", [])
            objects = native.get("initialProvider", {}).get("objects", [])
            if len(args) == 3 and len(objects) == 1 and objects[0].get("tag") == "reference.object.memory":
                initial = semantic_object(objects[0], True)["value"]["fields"][1]["data"]
                address = int(args[0]["value"])
                data, mask = args[1]["data"], args[2]["data"]
                invalid_range = bool(data) and (address >= len(initial) or len(data) > len(initial) - address)
                invalid_mask = bool(data) and any(b not in (0, 255) for b in mask)
                if base == "memory range" and invalid_range:
                    rule = "observed memory input fails contiguous bounds before mask validation"
                elif base == "memory byte enable" and not invalid_range and invalid_mask:
                    rule = "observed in-range memory input contains a non-00/FF byte enable"
    if rule is None:
        return None
    return {"verified": True, "referenceError": cause, "nativeError": deepcopy(observed), "rule": rule}


def active_pipeline_witness(source, native_output):
    """Check the catalog's single seeded dispatch against the actual queue cursor."""
    if source is None:
        return None
    environment = {row["name"]: row["value"] for row in source["context"]["environment"]}
    if "objects.pipelineSeed" not in environment:
        return None
    seed = environment["objects.pipelineSeed"]
    if seed.get("kind") != "record" or len(seed.get("fields", [])) != 5:
        raise ValueError("pipeline source setup codec")
    pending = seed["fields"][4]
    if pending.get("kind") != "record" or len(pending.get("fields", [])) != 4:
        raise ValueError("pipeline source pending ticket codec")
    context = source["context"]
    expected = source["world"]["events"]
    if len(expected) != 1 or identity(environment["objects.activeEvent"]) != identity(expected[0]["identity"]):
        raise ValueError("pipeline setup must name its sole source event")
    event = expected[0]
    if identity(pending["fields"][2]) != identity(event["identity"]):
        raise ValueError("pipeline setup ticket names another event")
    if decimal(event["time"]) != decimal(context["now"]) or decimal(event["turn"]) != decimal(context["turn"]):
        raise ValueError("pipeline source active event is not at context cursor")
    metadata = ("frontier", "batch", "batchReady", "members", "cursor", "resolverCursor",
                "reclaimCursor", "resolverTotal", "nextBatch")
    initial, final = native_output["initialProvider"], native_output["provider"]
    if any(initial[key] != final[key] for key in metadata) or initial["events"] != final["events"]:
        raise ValueError("pipeline Ready unexpectedly changed scheduler state")
    active = [(index, slot) for index, slot in enumerate(final["events"]) if slot["state"] == "active"]
    if len(active) != 1 or any(slot["state"] not in ("free", "active") for slot in final["events"]):
        raise ValueError("pipeline setup requires exactly one active slot")
    index, slot = active[0]
    actual = slot["event"]
    token = identity(actual["token"])
    logical = identity(event["identity"])
    if any(token[key] != logical[key] for key in ("kind", "domain", "slot", "generation", "owner")):
        raise ValueError("pipeline active capability authority mismatch")
    key = actual["event"]["key"]
    for native_key, source_key in (("time", "time"), ("turn", "turn"), ("stage", "stage"),
                                   ("instance", "instanceId"), ("connection", "connection"), ("sequence", "sequence")):
        if decimal(key[native_key]) != decimal(event[source_key]):
            raise ValueError("pipeline active event key mismatch")
    ready = {"time": decimal(context["now"]), "turn": decimal(context["turn"])}
    if (final["frontier"] != ready or final["batchReady"] != ready or
            list(map(decimal, final["members"])) != [str(index)] or
            any(int(final[k]) != 0 for k in ("cursor", "resolverCursor", "reclaimCursor")) or
            final["resolverTotal"] is not None or int(final["batch"]) != 1 or int(final["nextBatch"]) != 2):
        raise ValueError("pipeline native batch cursor does not match single-dispatch setup")
    return {"verified": True, "rule": "single seeded Pipeline event remains at the authoritative dispatch cursor",
            "sourceEvent": deepcopy(event), "actualToken": deepcopy(actual["token"]),
            "actualScheduler": {key: deepcopy(final[key]) for key in metadata}}


def discarded_pipeline_witness(source, model, executable, native):
    if source is None:
        return None
    burned = [slot for slot in native["provider"]["events"] if slot["state"] in ("free", "retired")
              and int(identity(slot["event"]["token"])["generation"]) != 0]
    if not burned:
        return None
    if len(burned) != 1 or any(o["ok"] for o in (model, executable, native)):
        return None
    if source["world"]["events"] or model["world"]["events"] or executable["world"]["events"]:
        return None
    if any(int(identity(s["event"]["token"])["generation"]) != 0 for s in native["initialProvider"]["events"]):
        return None
    traces = []
    for outcome, operation in ((model, "serviceCall:"), (executable, "objectCall")):
        rows = [row for row in outcome["trace"] if row["operation"].startswith(operation)
                and len(row["results"]) == 1 and row["results"][0].get("kind") == "record"
                and len(row["results"][0].get("fields", [])) == 4]
        if len(rows) != 1:
            return None
        traces.append(rows[0])
    native_rows = [row for row in native["opcodeEvents"] if row["stage"] == "completed" and row["opcode"] == 28
                   and row.get("result") is not None and row["source"] == traces[0]["location"]]
    if len(native_rows) != 1 or traces[0]["location"] != traces[1]["location"]:
        return None
    values = [traces[0]["results"][0], traces[1]["results"][0], native_rows[0]["result"]]
    aliases = IdentityBijection([{"role": role, **{side: identity(value["fields"][path]["fields"][3]
                  if path == 0 else value["fields"][path]) for side, value in zip(("model", "exec", "native"), values)}}
                for role, path in (("discarded.resource", 0), ("discarded.event", 2))])
    for entry in aliases.entries:
        if len({(entry[side]["slot"], entry[side]["generation"]) for side in ("model", "exec", "native")}) != 1:
            raise ValueError("discarded capability slot/generation history disagreement")
    normalized = [aliases.normalize(value, side) for side, value in zip(("model", "exec", "native"), values)]
    if normalized[0] != normalized[1] or normalized[0] != normalized[2]:
        raise ValueError("discarded Pipeline ticket trace disagreement")
    slot = burned[0]
    token = identity(slot["event"]["token"])
    if token != identity(values[2]["fields"][2]):
        raise ValueError("discarded native event differs from issued capability")
    for outcome, value in zip((model, executable), values):
        logical = identity(value["fields"][2])
        matches = [counter for counter in outcome["world"]["allocationCounters"] if counter["group"] == "runtime.event"
                   and decimal(counter["domain"]) == logical["domain"] and decimal(counter["slot"]) == logical["slot"]]
        if len(matches) != 1 or int(matches[0]["nextGeneration"]) != int(logical["generation"])+1:
            raise ValueError("discarded event generation burn missing from reference journal")
    draft = slot["event"]["event"]
    finish = decimal(values[0]["fields"][0]["fields"][1]["value"])
    context = source["context"]
    expected_key = {"time": finish, "turn": str(int(context["turn"])+1) if finish == decimal(context["now"]) else "0",
                    "stage": "3", "instance": decimal(context["instanceId"]), "connection": decimal(context["connection"]),
                    "sequence": decimal(source["world"]["nextSequence"])}
    if draft["key"] != expected_key or identity(draft["value"]) != identity(values[2]["fields"][0]["fields"][3]):
        raise ValueError("discarded event retained key/value disagreement")
    if decimal(draft["owner"]) != token["owner"] or decimal(draft["epoch"]) != decimal(values[2]["fields"][3]["value"]) or slot["event"]["cancelled"]:
        raise ValueError("discarded event retained authority/epoch disagreement")
    if slot["state"] != "free" or int(token["generation"]) >= 2**64-1:
        return None
    return {"verified": True, "rule": "failed segment releases its single reservation while preserving issued generation and draft authority",
            "actualSlot": deepcopy(slot), "modelIssued": deepcopy(values[0]), "execIssued": deepcopy(values[1]),
            "nativeIssued": deepcopy(values[2]), "identities": deepcopy(aliases.entries)}


def composed_protocol(source, model, executable, native):
    if native["provider"].get("protocol") is None:
        return None
    if source is None:
        raise ValueError("composed protocol requires source context and initial world")
    from .opcode_projection_protocol_state import (protocol_reference, protocol_native, result_reference,
        result_native, result_query_reference, setup_reference, canonical, host_reference)
    from .opcode_projection_protocol import _roles, _value
    context = source["context"]
    tags = {"protocol.control", "runtime.transaction", "runtime.hop", "storage.payload", "protocol.receipt"}
    journals = []
    def reference(world):
        transactions = [o for o in world["objects"] if o["tag"] == "runtime.transaction"]
        if len(transactions) != 1 or not transactions[0]["alive"]:
            raise ValueError("queue composition requires one live transaction")
        transaction = transactions[0]
        data = _value(transaction["value"])
        if len(data) != 12 or data[5] != data[9][0] or data[6] != data[11][1] or data[7] != data[10][8]:
            raise ValueError("composed transaction redundant state disagrees with ledger")
        hop_rows = [o for o in world["objects"] if o["tag"] == "runtime.hop"]
        expected_hop = [handle(transaction["identity"]), str(context["instanceId"]), data[0], data[1], data[2], data[5]]
        if len(hop_rows) != 1 or not hop_rows[0]["alive"] or identity(hop_rows[0]["identity"]) != identity(data[3]) or _value(hop_rows[0]["value"]) != expected_hop:
            raise ValueError("composed hop backing disagrees with transaction/ledger")
        payload_rows = [o for o in world["objects"] if o["tag"] == "storage.payload"]
        if len(payload_rows) != 1 or not payload_rows[0]["alive"] or identity(payload_rows[0]["identity"]) != identity(data[4]):
            raise ValueError("composed payload backing identity/lifetime")
        payload = _value(payload_rows[0]["value"])
        controls = [o for o in world["objects"] if o["tag"] == "protocol.control"]
        if (len(payload) != 17 or len(controls) != 1 or identity(payload[0]) != identity(transaction["identity"]) or
                identity(payload[1]) != identity(data[3]) or payload[2:6] != [str(context["instanceId"]), str(context["instanceId"]), False, False] or
                payload[9] != payload[10] or payload[14] != _value(controls[0]["value"])[12] or payload[16] != data[0]):
            raise ValueError("composed payload authority/baseline/configuration disagreement")
        protocol = {**protocol_reference(world, context), "resultStore": result_reference(world),
                    "result": result_query_reference(world), "setup": setup_reference(world),
                    "resultSlots": str(sum(o["alive"] for o in world["objects"] if o["tag"] == "storage.result"))}
        receipts = [o for o in world["objects"] if o["tag"] == "protocol.receipt"]
        if len(receipts) != 1 or not receipts[0]["alive"]:
            raise ValueError("queue composition requires one live owned receipt")
        txn, reason, state, rows = _value(receipts[0]["value"])
        receipt_identity = identity(receipts[0]["identity"])
        if (receipt_identity["kind"] != "13" or receipt_identity["store"] != "15" or receipt_identity["slot"] != "0" or
                receipt_identity["domain"] != str(context["domain"]) or receipt_identity["owner"] != str(context["owner"]) or
                int(receipt_identity["generation"])+1 != int(protocol["drainCounters"]["nextReceipt"])):
            raise ValueError("source owned receipt incarnation differs from its allocator journal")
        if identity(txn) != identity(transaction["identity"]) or len(rows) != 1 or rows[0][1] != data[10] or identity(rows[0][0]) != identity(data[3]):
            raise ValueError("owned receipt differs from its transaction responsibility")
        hops = []
        for hop, status in rows:
            if len(status) != 12 or not status[0] or not status[2] or not status[7] or not status[8] or status[10] or status[11] != reason:
                raise ValueError("owned receipt responsibility codec/lifecycle")
            hops.append({"hop": hop, **dict(zip(("wireTerminal", "timingConsumed", "cleanupReturned", "callPin"), status[3:7]))})
        return {"state": protocol, "receipt": {"identity": handle(receipts[0]["identity"]),
                "transaction": txn, "reason": reason, "state": state, "hops": hops}}
    def actual(provider):
        child = provider["protocol"]
        result = {**protocol_native({"provider": child, "identities": provider["protocolIdentities"]}, context),
                  "resultStore": result_native(child, context), "result": canonical(child["result"]),
                  "setup": canonical(child["setup"]), "resultSlots": child["resultSlots"]}
        if set(result) != set(child):
            raise ValueError("unknown composed native protocol field")
        receipt = canonical(provider["drainReceipt"])
        raw_identity = identity(provider["drainReceipt"]["identity"])
        receipt_counter = int(result["drainCounters"]["nextReceipt"])-1
        receipt_capacity = int(result["drainCounters"]["capacity"])
        if (raw_identity["kind"] != "13" or raw_identity["domain"] != str(context["domain"]) or
                raw_identity["owner"] != str(context["owner"]) or int(raw_identity["generation"]) != receipt_counter or
                receipt_capacity <= 0 or int(raw_identity["slot"]) != receipt_counter % receipt_capacity):
            raise ValueError("native owned receipt incarnation differs from its allocator journal")
        receipt["identity"] = handle(provider["drainReceipt"]["identity"])
        receipt["transaction"] = handle(provider["drainReceipt"]["transaction"])
        for row in receipt["hops"]:
            row["hop"] = handle(row["hop"])
        return {"state": result, "receipt": receipt}
    values = [reference(model["world"]), reference(executable["world"]), actual(native["provider"])]
    initial = [reference(source["world"]), reference(source["world"]), actual(native["initialProvider"])]
    roles = [_roles(model), _roles(executable), {r["name"]: identity(r["identity"]) for r in native["provider"]["protocolIdentities"]}]
    entries = []
    if roles[0].keys() != roles[1].keys() or roles[0].keys() != roles[2].keys():
        raise ValueError("composed protocol identity roles mismatch")
    for role in roles[0]:
        entries.append({"role": "queue.protocol."+role, **{side: identities[role] for side, identities in zip(("model", "exec", "native"), roles)}})
    entries.append({"role": "queue.protocol.receipt", **{side: identity(value["receipt"]["identity"])
                    for side, value in zip(("model", "exec", "native"), values)}})
    filtered = []
    for outcome, observed in zip((model, executable), values):
        clone = deepcopy(outcome)
        world = clone["world"]
        logical = [c for c in world["allocationCounters"] if c["group"].startswith("logical.") or c["group"] == "storage.payload-view"]
        if any(c["group"] not in ("logical.14.LeanAT.HandleKind.access", "storage.payload-view") for c in logical):
            raise ValueError("unknown logical backing allocator in protocol composition")
        if len(logical) != 2:
            raise ValueError("composed protocol logical backing allocator count")
        for counter in logical:
            tag = "protocol.control" if counter["group"].startswith("logical.14.") else "storage.payload"
            backing = [o for o in world["objects"] if o["tag"] == tag]
            if len(backing) != 1 or not backing[0]["alive"]:
                raise ValueError("composed protocol backing lifetime")
            incarnation = identity(backing[0]["identity"])
            persistent = counter["group"].startswith("logical.14.")
            if (counter["slot"] is not None or counter["retired"] or counter["persistent"] != persistent or
                    decimal(counter["domain"]) != incarnation["domain"] or
                    int(counter["nextGeneration"]) != int(incarnation["generation"])+1):
                raise ValueError("logical backing incarnation journal does not describe its owned record")
        if any(o["kind"] not in ("protocol.setup.exchange", "protocol.host.arm") for o in world["observations"]):
            raise ValueError("unknown composed protocol observation")
        journals.append(logical)
        view_rules = [r for r in world["allocationRules"] if r["group"] == "storage.payload-view"]
        expected_view_rule = {"kind": 1, "store": "22", "group": "storage.payload-view", "perSlot": False,
                              "persistent": False, "allowMax": True, "capacity": "128"}
        if view_rules != [expected_view_rule]:
            raise ValueError("composed payload-view policy differs from the owned request codec")
        protocol_rules = [r for r in world["allocationRules"] if r["group"].startswith("protocol.")]
        expected_rules = {(0, "12", "protocol.admission"): observed["state"]["admissionStore"]["limits"]["transactions"],
                          (1, "13", "protocol.admission"): observed["state"]["admissionStore"]["limits"]["hops"],
                          (14, "12", "protocol.admission"): observed["state"]["admissionStore"]["limits"]["gates"],
                          (13, "15", "protocol.receipt"): observed["state"]["drainCounters"]["capacity"]}
        if len(protocol_rules) != len(expected_rules):
            raise ValueError("composed protocol allocator policy count")
        for rule in protocol_rules:
            key = (int(rule["kind"]), decimal(rule["store"]), rule["group"])
            if key not in expected_rules or decimal(rule["capacity"]) != decimal(expected_rules[key]) or rule["perSlot"] or not rule["persistent"] or rule["allowMax"]:
                raise ValueError("composed protocol allocator policy/capacity mismatch")
        for group, native_counter in (("protocol.admission", observed["state"]["admissionStore"]["nextGeneration"]),
                                      ("protocol.receipt", observed["state"]["drainCounters"]["nextReceipt"])):
            counters = [c for c in world["allocationCounters"] if c["group"] == group]
            if len(counters) != 1 or decimal(counters[0]["domain"]) != decimal(context["domain"]) or decimal(counters[0]["nextGeneration"]) != decimal(native_counter) or counters[0]["slot"] is not None or not counters[0]["persistent"] or counters[0]["retired"]:
                raise ValueError("composed protocol persistent journal mismatch")
        world["objects"] = [o for o in world["objects"] if o["tag"] not in tags]
        world["observations"] = []
        world["allocationCounters"] = [c for c in world["allocationCounters"] if not c["group"].startswith(("protocol.", "logical.")) and c["group"] != "storage.payload-view"]
        world["allocationRules"] = [r for r in world["allocationRules"] if not r["group"].startswith("protocol.") and r["group"] != "storage.payload-view"]
        filtered.append(clone)
    if journals[0] != journals[1]:
        raise ValueError("source/Exec logical backing incarnation journal mismatch")
    host = [host_reference(o["world"], {"observations": []}) for o in (model, executable)] + [deepcopy(native["hostEvents"])]
    return filtered, [{"final": value, "initial": before, "hostEvents": events} for value, before, events in zip(values, initial, host)], entries, journals


def project(modelOutcome, execOutcome, nativeOutput, sourceInput=None):
    unmapped, entries = [], []
    raw = (modelOutcome, execOutcome, nativeOutput)
    try:
        composition = composed_protocol(sourceInput, modelOutcome, execOutcome, nativeOutput)
        model = _reference_world(composition[0][0] if composition else modelOutcome, unmapped)
        executable = _reference_world(composition[0][1] if composition else execOutcome, unmapped)
        native = _native_world(nativeOutput["provider"], unmapped)
        if composition:
            entries.extend(composition[2])
            for world, child in zip((model, executable, native), composition[1]):
                world["protocol"] = child
            unmapped = [item for item in unmapped if item != "composed protocol drain lifecycle requires protocol projection"]
        if sourceInput is not None:
            initial_world = deepcopy(sourceInput["world"])
            if composition:
                protocol_tags = {"protocol.control", "runtime.transaction", "runtime.hop", "storage.payload", "protocol.receipt"}
                initial_world["objects"] = [o for o in initial_world["objects"] if o["tag"] not in protocol_tags]
                initial_world["observations"] = []
                initial_world["allocationRules"] = [r for r in initial_world["allocationRules"] if not r["group"].startswith("protocol.") and r["group"] != "storage.payload-view"]
                initial_world["allocationCounters"] = [c for c in initial_world["allocationCounters"] if not c["group"].startswith(("protocol.", "logical.")) and c["group"] != "storage.payload-view"]
            initial_reference = _reference_world({"world": initial_world}, unmapped)
            initial_native = _native_world(nativeOutput["initialProvider"], unmapped)
            if composition:
                unmapped = [item for item in unmapped if item != "composed protocol drain lifecycle requires protocol projection"]
            for initial_state in (initial_reference, initial_native):
                initial_state["auxiliaryObjects"].sort(key=lambda row: (row["tag"], int(identity(row["identity"])["slot"])))
            model["initial"] = deepcopy(initial_reference)
            executable["initial"] = deepcopy(initial_reference)
            native["initial"] = initial_native
        scheduler_witness = active_pipeline_witness(sourceInput, nativeOutput)
        if scheduler_witness is not None:
            unmapped = [item for item in unmapped if item not in
                        ("active batch cursor needs scheduler projection", "native live batch metadata needs scheduler mapping")]
        discarded_witness = discarded_pipeline_witness(sourceInput, modelOutcome, execOutcome, nativeOutput)
        if discarded_witness is not None:
            entries.extend(discarded_witness["identities"])
            unmapped = [item for item in unmapped if item != "retired/free native event generation requires retirement projection"]
        for world in (model, executable, native):
            world["auxiliaryObjects"].sort(key=lambda r: (r["tag"], int(identity(r["identity"])["slot"])))
        for tag in ("structured.scope", "storage.result", "storage.consumer"):
            rows = [[r for r in w["auxiliaryObjects"] if r["tag"] == tag] for w in (model, executable, native)]
            if len({len(s) for s in rows}) != 1:
                unmapped.append(tag + " lifecycle counts differ")
                continue
            for index in range(len(rows[0])):
                parsed = [identity(items[index]["identity"]) for items in rows]
                if tag.startswith("storage.") and len({(h["slot"], h["generation"]) for h in parsed}) != 1:
                    raise ValueError("storage slot/generation disagreement: " + tag)
                entries.append({"role": f"{tag}.{index}", **dict(zip(("model", "exec", "native"), parsed))})
        tickets = [_tickets(world["objects"]) for world in (model, executable, native)]
        if not (tickets[0].keys() == tickets[1].keys() == tickets[2].keys()):
            unmapped.append("resource ticket lifecycle counts differ")
        for role in tickets[0].keys() & tickets[1].keys() & tickets[2].keys():
            ids = [identity(items[role]) for items in tickets]
            # Resource generation and slot algorithms are identical; only store incarnation differs.
            if len({(h["slot"], h["generation"]) for h in ids}) != 1:
                raise ValueError("resource ticket slot/generation disagreement: " + role)
            entries.append(dict(role=role, **dict(zip(("model", "exec", "native"), ids))))
        event_lists = [world["events"] for world in (model, executable, native)]
        if len({len(events) for events in event_lists}) != 1:
            unmapped.append("event lifecycle counts differ")
        else:
            for index in range(len(event_lists[0])):
                entries.append({"role": f"pipeline.event.{index}", **{
                    side: identity(events[index]["identity"])
                    for side, events in zip(("model", "exec", "native"), event_lists)}})
        aliases = IdentityBijection(entries)
        for side, world in zip(("model", "exec", "native"), (model, executable, native)):
            # Normalize a Resource's dedicated store field by the real object's composite ID.
            for obj in world["objects"] + world.get("initial", {}).get("objects", []):
                if obj["tag"] in ("reference.object.resource", "reference.object.pipeline"):
                    raw_store = obj["value"]["fields"][6]["value"]
                    role_prefix = f"object.{obj['object']['id']}.ticket."
                    owned_tickets = [e for e in entries if e["role"].startswith(role_prefix)]
                    if any(e[side]["store"] != raw_store for e in owned_tickets):
                        raise ValueError("resource ticket belongs to another object store")
                    obj["value"]["fields"][6] = {"resourceStoreOf": obj["object"]}
            world.update(ok=raw[("model", "exec", "native").index(side)]["ok"],
                         exit=raw[("model", "exec", "native").index(side)]["exit"],
                         returned=[reference_value(v) for v in raw[("model", "exec", "native").index(side)]["returned"]],
                         committed=[reference_value(v) for v in raw[("model", "exec", "native").index(side)]["committed"]])
        agreement = error_agreement(*raw)
        if not all(o["ok"] for o in raw) and agreement is None:
            unmapped.append("failure error-code/message translation requires explicit provider error contract")
        reference_limits = {side: {key: outcome["world"][key] for key in
                            ("maxObjects", "maxBytes", "nextGeneration")}
                            for side, outcome in (("model", modelOutcome), ("exec", execOutcome))}
        if reference_limits["model"] != reference_limits["exec"]:
            raise ValueError("source/Exec global envelope or allocation highwater disagreement")
        return {"model": aliases.normalize(model, "model"), "exec": aliases.normalize(executable, "exec"),
                "native": aliases.normalize(native, "native"), "unmapped": sorted(set(unmapped)), "identities": entries,
                "errorAgreement": agreement,
                "schedulerWitness": scheduler_witness,
                "discardedEventWitness": discarded_witness,
                "referenceEnvelope": reference_limits,
                "referenceLogicalJournal": composition[3] if composition else None,
                "centralVerifiedFields": ["exec-native fuel", "opcode trace", "returned", "committed", "outcome", "source-map"]}
    except (KeyError, ValueError, TypeError, IndexError) as error:
        # The enclosing evidence retains all raw Outcomes. Never accidentally compare their
        # different interpreter microsteps as if they were canonical provider state.
        return {"model": {"projectionAvailable": False}, "exec": {"projectionAvailable": False},
                "native": {"projectionAvailable": False},
                "unmapped": ["object projection rejected malformed or unsupported observation: " + str(error)],
                "projectionRejection": str(error), "identities": entries}


def _self_test():
    import unittest
    class Controls(unittest.TestCase):
        def test_active_pipeline_cursor_witness(self):
            token = {"kind": "2", "domain": "1", "store": "1", "slot": "0", "generation": "1", "owner": "7"}
            event = {"identity": token, "time": "0", "turn": "1", "stage": "3", "instanceId": "2",
                     "connection": "9", "sequence": "1"}
            source = {"context": {"now": "0", "turn": "1", "environment": [
                {"name": "objects.pipelineSeed", "value": record([bits(0), bits(0), bits(0), bits(0),
                    record([bits(0), bits(0), handle(token), bits(0)])])},
                {"name": "objects.activeEvent", "value": handle(token)}]}, "world": {"events": [event]}}
            provider = {"frontier": {"time": "0", "turn": "1"}, "batchReady": {"time": "0", "turn": "1"},
                        "batch": "1", "nextBatch": "2", "members": ["0"], "cursor": "0",
                        "resolverCursor": "0", "reclaimCursor": "0", "resolverTotal": None,
                        "events": [{"state": "active", "event": {"token": token, "event": {"key": {
                            "time": "0", "turn": "1", "stage": "3", "instance": "2", "connection": "9", "sequence": "1"}}}}]}
            native = {"provider": provider, "initialProvider": deepcopy(provider)}
            self.assertTrue(active_pipeline_witness(source, native)["verified"])
            for field, bad in (("cursor", "1"), ("members", []), ("frontier", {"time": "0", "turn": "0"})):
                altered = deepcopy(native)
                altered["provider"][field] = bad
                with self.assertRaises(ValueError): active_pipeline_witness(source, altered)
            altered = deepcopy(native)
            for side in ("provider", "initialProvider"):
                altered[side]["events"][0]["event"]["event"]["key"]["connection"] = "8"
            with self.assertRaises(ValueError): active_pipeline_witness(source, altered)
            self.assertIsNone(active_pipeline_witness(None, native))

        def test_representation_and_bytes(self):
            self.assertEqual(native_value({"bytes": ["1", "255"]}, BYTES), {"kind": "bytes", "data": [1, 255]})
            with self.assertRaises(ValueError): native_value({"bytes": ["256"]}, BYTES)
            with self.assertRaises(ValueError): native_value({"u64": str(2**64)}, U64)
            with self.assertRaises(ValueError): native_value({"array": []}, rec(U64))
            self.assertNotEqual(native_value({"bytes": ["1"]}, BYTES), native_value({"bytes": ["2"]}, BYTES))
        def test_order_and_lifetime(self):
            def obj(data, alive=True):
                return {"objectId": "1", "domain": "1", "instance": "2", "providerKind": "0",
                        "tag": "reference.object.memory", "valueSchema": "reference.object.memory", "alive": alive,
                        "value": {"array": [{"u64": "2"}, {"bytes": data}, {"bytes": ["1", "2"]}, {"u64": "0"}, {"bool": False}]}}
            self.assertNotEqual(semantic_object(obj(["1", "2"]), True), semantic_object(obj(["2", "1"]), True))
            self.assertNotEqual(semantic_object(obj(["1", "2"]), True), semantic_object(obj(["1", "2"], False), True))
        def test_owner_and_capacity(self):
            h = {"kind": "11", "domain": "1", "store": "3", "slot": "0", "generation": "1", "owner": "7"}
            changed = dict(h, owner="8")
            with self.assertRaises(ValueError): IdentityBijection([dict(role="ticket", model=h, exec=h, native=changed)])
            a = native_value({"array": [{"u64": "1"}]}, vec(U64))
            b = native_value({"array": [{"u64": "1"}, {"u64": "2"}]}, vec(U64))
            self.assertNotEqual(a, b)
        def test_full_memory_projection_controls(self):
            h = {"kind": "11", "domain": "1", "store": "300", "slot": "1", "generation": "1", "owner": "0"}
            value = record([bits(2), {"kind": "bytes", "data": [1, 2]}, {"kind": "bytes", "data": [1, 2]}, bits(0), boolean(False)])
            entry = {"identity": h, "tag": "reference.object.memory", "alive": True, "value": value}
            source = {"ok": True, "exit": "return", "returned": [], "committed": [],
                      "world": {"objects": [entry], "events": [], "observations": [], "maxEvents": 0,
                                "nextSequence": 1, "maxPins": 256, "maxObjects": 16,
                                "maxBytes": 4096, "nextGeneration": 2}}
            nentry = {"objectId": "1", "domain": "1", "instance": "2", "providerKind": "0",
                      "tag": entry["tag"], "valueSchema": entry["tag"], "alive": True,
                      "value": {"array": [{"u64": "2"}, {"bytes": ["1", "2"]}, {"bytes": ["1", "2"]}, {"u64": "0"}, {"bool": False}]}}
            native = {"ok": True, "exit": "return", "returned": [], "committed": [],
                      "provider": {"objects": [nentry], "events": [], "eventOccupied": 0,
                                   "nextSequence": 1, "resultPinLimit": 256}}
            baseline = project(source, deepcopy(source), native)
            self.assertEqual(baseline["model"], baseline["native"])
            self.assertEqual(baseline["referenceEnvelope"]["model"], baseline["referenceEnvelope"]["exec"])
            for mutate in (
                lambda n: n["provider"]["objects"][0]["value"]["array"][1].update(bytes=["2", "1"]),
                lambda n: n["provider"]["objects"][0].update(alive=False),
                lambda n: n["provider"]["objects"][0].update(domain="2"),
                lambda n: n["provider"]["objects"][0]["value"]["array"][3].update(u64="1"),
                lambda n: n["provider"]["objects"][0]["value"]["array"][4].update(bool=True),
            ):
                changed = deepcopy(native)
                mutate(changed)
                observed = project(source, deepcopy(source), changed)
                self.assertNotEqual(observed["model"], observed["native"])
            changed_source = deepcopy(source)
            changed_source["world"]["maxEvents"] = 1
            observed = project(changed_source, deepcopy(changed_source), native)
            self.assertNotEqual(observed["model"], observed["native"])
        def test_scope_authority_snapshot(self):
            h = {"kind": "7", "domain": "1", "store": "130", "slot": "0", "generation": "1", "owner": "0"}
            parent = dict(h, kind="0", domain="0", store="0", generation="0")
            provider = {"objects": [], "events": [], "eventOccupied": 0, "nextSequence": 1,
                        "resultPinLimit": 256, "scopeMetadata": {"sequence": 0, "capacity": 16, "observers": [], "nextAllocationGeneration": 2},
                        "scopes": [{"identity": h, "parent": parent, "state": 0, "owned": [],
                                    "hasPlan": False, "plan": None}]}
            projected = _native_world(provider, [])
            self.assertEqual(projected["auxiliaryObjects"][0]["value"]["fields"][5], bits(0))
            changed = deepcopy(provider)
            changed["scopeMetadata"]["sequence"] = 1
            with self.assertRaises(ValueError): _native_world(changed, [])
            changed = deepcopy(provider)
            changed["scopes"][0]["plan"] = {"reason": "cancelled"}
            with self.assertRaises(ValueError): _native_world(changed, [])
        def test_exact_failure_causes(self):
            ref = {"ok": False, "error": "RegisterFieldWidth"}
            native = {"ok": False, "error": {"code": "21", "message": "field value width (program 0, block 0, opcode 28, handler/0/body/0)"}}
            self.assertTrue(error_agreement(ref, ref, native)["verified"])
            changed = deepcopy(native)
            changed["error"]["code"] = "2"
            self.assertIsNone(error_agreement(ref, ref, changed))
            changed = deepcopy(native)
            changed["error"]["message"] = "another provider failed"
            self.assertIsNone(error_agreement(ref, ref, changed))
        def test_result_ownership_and_allocator_counters(self):
            result = {"kind": "5", "domain": "1", "store": "3", "slot": "0", "generation": "1", "owner": "7"}
            source = dict(result, kind="0", store="50")
            original = dict(result, kind="6")
            fresh = dict(original, slot="1", generation="2", owner="99")
            provider = {"objects": [], "events": [], "eventOccupied": 0, "nextSequence": 1,
                        "resultPinLimit": 256, "resultPins": 0, "nextResultGeneration": 1, "nextConsumerGeneration": 2,
                        "results": [{"identity": result, "alive": True, "producerAlive": True, "publishing": False,
                                     "value": None, "ready": None, "consumerCount": 1, "pinCount": 0, "source": source,
                                     "type": 0, "maxBytes": 128, "producerOwner": 7, "consumerOwner": 7}],
                        "consumers": [{"identity": original, "result": result, "active": False},
                                      {"identity": fresh, "result": result, "active": True}]}
            world = _native_world(provider, [])
            self.assertEqual(len(world["auxiliaryObjects"]), 3)
            self.assertEqual(world["allocationCounters"][0]["nextGeneration"], "3")
            self.assertEqual(world["allocationRules"][0]["capacity"], "2")
            changed = deepcopy(provider)
            changed["results"][0]["consumerCount"] = 2
            with self.assertRaises(ValueError): _native_world(changed, [])
            changed = deepcopy(provider)
            changed["nextConsumerGeneration"] = 3
            self.assertNotEqual(_native_world(changed, [])["allocationCounters"], world["allocationCounters"])
            rule = {"kind": 6, "store": "21", "group": "storage.consumer", "perSlot": False,
                    "persistent": True, "allowMax": True, "capacity": "2"}
            changed_rule = dict(rule, store="22")
            with self.assertRaises(ValueError): allocation_rules([changed_rule], [])
    result = unittest.TextTestRunner().run(unittest.defaultTestLoader.loadTestsFromTestCase(Controls))
    if not result.wasSuccessful(): raise SystemExit(1)

def _retained_case_controls(path):
    import hashlib
    import json
    from pathlib import Path
    from tools.leanat.opcode_replay import compare_case
    captured = Path(path).read_bytes()
    row = json.loads(captured.decode("utf-8-sig"))
    def compare(candidate):
        executions = candidate["executions"]
        case = deepcopy(candidate["catalogCase"])
        case["descriptorSummary"] = executions["runtimeDescription"]["value"]
        return compare_case(case, candidate["input"], executions["model"], executions["exec"],
                            executions["runtime"]["value"], executions["systemc"]["value"], Path.cwd())
    baseline = compare(row)
    if baseline["status"] != "Pass":
        raise AssertionError(baseline)
    checks = []
    for field, bad in (("group", "unknown.payload-view"), ("persistent", True), ("store", "23"), ("kind", 9),
                       ("capacity", "127"), ("perSlot", True), ("allowMax", False)):
        changed = deepcopy(row)
        for world in [changed["input"]["world"]] + [changed["executions"][side]["world"] for side in ("model", "exec")]:
            next(r for r in world["allocationRules"] if r["group"] == "storage.payload-view")[field] = bad
        checks.append(("payload-view-rule."+field, changed))
    for field, bad in (("domain", "2"), ("slot", "0"), ("nextGeneration", "3"), ("persistent", True)):
        changed = deepcopy(row)
        for world in [changed["input"]["world"]] + [changed["executions"][side]["world"] for side in ("model", "exec")]:
            next(c for c in world["allocationCounters"] if c["group"] == "storage.payload-view")[field] = bad
        checks.append(("payload-view-counter."+field, changed))
    for field, bad in (("kind", 1), ("store", "16"), ("domain", "2"), ("slot", "1"), ("generation", "2"), ("owner", "8")):
        changed = deepcopy(row)
        for world in [changed["input"]["world"]] + [changed["executions"][side]["world"] for side in ("model", "exec")]:
            next(o for o in world["objects"] if o["tag"] == "protocol.receipt")["identity"][field] = bad
        checks.append(("receipt-identity."+field, changed))
    native_changes = {
        "payload": lambda p: p["protocol"]["ledgers"][0]["admission"]["request"]["data"].__setitem__(0, "99"),
        "call": lambda p: p["protocol"]["setup"][0]["value"].__setitem__("id", "99"),
        "intent": lambda p: p["protocol"]["intents"][0].__setitem__("phase", "1"),
        "receipt-generation": lambda p: p["drainReceipt"]["identity"].__setitem__("generation", "2"),
        "queue-owner": lambda p: p["objects"][0]["value"]["array"][6]["array"][0]["array"][2]["array"][1]["array"][0]["handle"].__setitem__("owner", "8"),
    }
    for side in ("runtime", "systemc"):
        for name, mutate in native_changes.items():
            changed = deepcopy(row)
            mutate(changed["executions"][side]["value"]["provider"])
            checks.append((side+"."+name, changed))
        changed = deepcopy(row)
        changed["executions"][side]["value"]["hostEvents"][0]["value"]["generation"] = "99"
        checks.append((side+".host-observation", changed))
    rejected = []
    for name, candidate in checks:
        result = compare(candidate)
        if result["status"] == "Pass":
            raise AssertionError("mutation accepted: "+name)
        rejected.append(name)
    print(json.dumps({"captureSha256": hashlib.sha256(captured).hexdigest(),
        "descriptorHash": row["executions"]["runtime"]["value"]["descriptorHash"],
        "baseline": baseline["status"], "runtimeAndSystemC": True,
        "rejectedMutationCount": len(rejected), "rejectedMutations": rejected}, indent=2))


if __name__ == "__main__":
    import sys
    if len(sys.argv) == 3 and sys.argv[1] == "--retained-case":
        _retained_case_controls(sys.argv[2])
    else:
        _self_test()
