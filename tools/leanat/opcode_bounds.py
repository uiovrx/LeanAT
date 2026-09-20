"""Capacity-profile checks; these do not infer bounds from execution observations.

Reference owned storage and native transaction staging are different resources.
The envelope checker below is conditional on caller-supplied, statically justified
per-store/effect bounds. It deliberately never emits a conformance Pass/proof.
"""

from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path
import re

from .io import ToolError, digest, canonical


def descriptor_value_bounds(types, node_bytes):
    """Derive maximum abstract/reference and compact/native Value bytes by type.

    Schema type references must be acyclic. Variant native representation includes
    its array wrapper, tag and fields wrapper (three Value nodes).
    """
    node_bytes = _nat(node_bytes, "valueNodeBytes")
    if not 1 <= node_bytes <= 1024:
        raise ToolError("CapacityProfile", "unsupported Value ABI")
    result, active = {}, set()

    def visit(index):
        index = _nat(index, "type index")
        if index >= len(types) or index in active:
            raise ToolError("CapacityProfile", "cyclic or missing descriptor type")
        if index in result:
            return result[index]
        active.add(index)
        typ = types[index]
        kind = _nat(typ["kind"], "type kind")
        bound = _nat(typ.get("bound", 0), "type bound")
        fields = typ.get("fields", [])
        if kind in (0, 1):
            pair = (1, node_bytes)
        elif kind in (2, 3):
            if kind == 2 and not 1 <= bound <= 64:
                raise ToolError("CapacityProfile", "bits width outside supported domain")
            pair = (16 if kind == 3 else 8 + (bound + 7) // 8, node_bytes)
        elif kind == 4:
            children = [visit(i) for i in fields]
            pair = (8 + sum(a for a, _ in children), node_bytes + sum(b for _, b in children))
        elif kind == 5:
            alternatives = [[visit(i) for i in ctor] for ctor in typ.get("constructors", [])]
            pair = (16 + max((sum(a for a, _ in c) for c in alternatives), default=0),
                    3 * node_bytes + max((sum(b for _, b in c) for c in alternatives), default=0))
        elif kind in (6, 7):
            if len(fields) != 1:
                raise ToolError("CapacityProfile", "vector element type missing")
            abstract, native = visit(fields[0])
            pair = (8 + bound * abstract, node_bytes + bound * native)
        elif kind == 8:
            pair = (8 + bound, node_bytes + bound)
        elif kind == 9:
            pair = (29, node_bytes)
        else:
            raise ToolError("CapacityProfile", "unsupported descriptor type")
        if max(pair) >= 2**64:
            raise ToolError("CapacityProfile", "descriptor Value bound exceeds uint64")
        active.remove(index)
        result[index] = pair
        return pair

    return [visit(i) for i in range(len(types))]


def verify_static_descriptor(summary, descriptor, expected_hash, expected_profile):
    """Bind native validated metadata to the exact catalog artifact on disk."""
    if summary.get("schema") != "leanat.opcode-static.v1":
        raise ToolError("CapacityProfile", "validated static descriptor summary required")
    if summary.get("descriptorHash") != expected_hash or digest(Path(descriptor).read_bytes()) != expected_hash:
        raise ToolError("CapacityProfile", "static descriptor artifact hash differs")
    if summary.get("profile") != expected_profile:
        raise ToolError("CapacityProfile", "static descriptor profile differs")
    return descriptor_value_bounds(summary["types"], summary["valueNodeBytes"])


def prove_pure_case(input_value, summary, descriptor, expected_hash, expected_profile, program_id=0):
    """Finite pure-program harness bound, including traces and buffered writes.

    All programs are checked, overapproximating every reachable call. No provider
    instructions, suspend or transport-return terminators are accepted. Thus the
    reference world can grow only by trace observations; state cells are separate.
    """
    layouts = verify_static_descriptor(summary, descriptor, expected_hash, expected_profile)
    fuel = _nat(input_value.get("fuel", 10000), "fuel")
    abstract_trace = native_trace = 0
    trace_present = False
    for program in summary["programs"]:
        for block in program["blocks"]:
            for instruction in block["instructions"]:
                opcode = _nat(instruction["opcode"], "opcode")
                if opcode > 20:
                    raise ToolError("CapacityProfile", "provider operation in pure bound")
                if opcode == 15:
                    trace_present = True
                    arg_types = instruction["args"]
                    text_bytes = _text_bytes(instruction.get("text", ""))
                    # Source evaluator appends at most one fixed path component
                    # and a <=20-digit index per fuel-consuming descent. Exec
                    # fallback program/block/instruction paths are shorter.
                    source_bytes = max(_text_bytes(instruction.get("source", "")), 128 + 32 * fuel)
                    abstract_trace = max(abstract_trace, 37 + text_bytes + source_bytes
                                         + sum(layouts[_nat(t, "argument type")][0] for t in arg_types))
                    native_trace = max(native_trace, text_bytes
                                       + sum(layouts[_nat(t, "argument type")][1] for t in arg_types))
            # Static native description uses the C++ Terminator::Kind ordinal.
            if _nat(block["terminator"]["kind"], "terminator kind") not in (0, 1, 2, 3, 4):
                raise ToolError("CapacityProfile", "non-pure terminator in pure bound")
    world = input_value.get("world", {})
    trace_count = instruction_effect_counts(summary, program_id, fuel).get(15, 0) if trace_present else 0
    observation_bound = len(world.get("observations", [])) + trace_count
    world_bytes = owned_state_bytes(world) + trace_count * abstract_trace
    state_types = summary["stateTypes"]
    if any(_nat(summary["types"][_nat(t, "state type")]["kind"], "state kind") not in (0, 1, 2, 3, 9) for t in state_types):
        raise ToolError("CapacityProfile", "aggregate state needs native spare-capacity witness")
    stage_bytes = sum(layouts[_nat(t, "state type")][1] for t in state_types)
    if stage_bytes > read_segment_bytes(input_value) or len(state_types) > 4096:
        raise ToolError("CapacityProfile", "pure staging envelope exceeds actual budget")
    if trace_present and (trace_count > 65536 or trace_count * native_trace > 1048576):
        raise ToolError("CapacityProfile", "pure native trace envelope exceeds actual budget")
    return {"schema": "leanat.opcode-capacity-proof.v1", "status": "Proven",
            "family": "pure", "descriptorHash": expected_hash,
            "inputHash": digest(canonical(input_value)),
            "referenceRequired": {"maxObjects": str(len(world.get("objects", []))),
                                  "maxObservations": str(observation_bound), "maxBytes": str(world_bytes)},
            "nativeRequired": {"segmentBytes": str(stage_bytes), "writes": str(len(state_types)),
                               "traceBytes": str(trace_count * native_trace), "traceEntries": str(trace_count)},
            "basis": "validated all-program pure opcode closure; fuel bounds trace count; descriptor types bound values"}


def instruction_effect_counts(summary, program_id, fuel):
    """Maximum per-opcode counts on acyclic CFG/call paths, bounded again by fuel.

    Cycles conservatively return fuel for every descriptor opcode. Individual
    opcode maxima need not occur on the same path, which only overestimates.
    """
    fuel = _nat(fuel, "fuel")
    programs = {_nat(p["id"], "program id"): p for p in summary["programs"]}
    opcodes = {_nat(i["opcode"], "opcode") for p in programs.values() for b in p["blocks"] for i in b["instructions"]}
    active = set()
    memo = {}

    def add(left, right):
        out = dict(left)
        for op, count in right.items():
            out[op] = min(fuel, out.get(op, 0) + count)
        return out

    def visit(pid, bid):
        key = pid, bid
        if key in active:
            raise RuntimeError("cycle")
        if key in memo:
            return memo[key]
        active.add(key)
        program = programs[pid]
        blocks = {_nat(b["id"], "block id"): b for b in program["blocks"]}
        block = blocks[bid]
        result = {}
        for instruction in block["instructions"]:
            opcode = _nat(instruction["opcode"], "opcode")
            result = add(result, {opcode: 1})
            if opcode == 19:
                callee = _nat(instruction["immediate"], "callee")
                result = add(result, visit(callee, _nat(programs[callee]["entry"], "entry")))
        branches = [visit(pid, _nat(target, "target")) for target in block["terminator"].get("targets", [])]
        tail = {op: max((branch.get(op, 0) for branch in branches), default=0) for op in opcodes}
        result = add(result, tail)
        active.remove(key)
        memo[key] = result
        return result

    pid = _nat(program_id, "program id")
    try:
        return visit(pid, _nat(programs[pid]["entry"], "entry"))
    except RuntimeError:
        return {op: fuel for op in opcodes}
    except KeyError as exc:
        raise ToolError("CapacityProfile", "missing program/block/callee in static summary") from exc


def bind_pure_harness(input_value, summary, descriptor, expected_hash, expected_profile, program_id=0):
    """Raise only proven nonbinding aggregate harness caps, before all executors."""
    proof = prove_pure_case(input_value, summary, descriptor, expected_hash, expected_profile, program_id)
    result = deepcopy(input_value)
    world = result.setdefault("world", {})
    defaults = {"maxObjects": 1024, "maxObservations": 100000, "maxBytes": 1048576}
    for name, required in proof["referenceRequired"].items():
        amount = max(_nat(world.get(name, defaults[name]), name), _nat(required, name))
        if amount >= 2**64:
            raise ToolError("CapacityProfile", "finite reference envelope exceeds representable host cap")
        world[name] = str(amount)
    proof["inputHash"] = digest(canonical(result))
    proof["referenceConfigured"] = {name: world[name] for name in defaults}
    return result, proof


def _nat(value, name):
    if type(value) is int and value >= 0:
        return value
    if isinstance(value, str) and re.fullmatch(r"0|[1-9][0-9]*", value):
        return int(value)
    raise ToolError("CapacityProfile", f"{name} must be a canonical natural")


def _text_bytes(value):
    if not isinstance(value, str):
        raise ToolError("CapacityProfile", "storage text must be a string")
    return len(value.encode("utf-8"))


def value_bytes(value, depth=64):
    """Contract.valueBytes, not sizeof(Value) or native retained capacity."""
    if depth == 0 or not isinstance(value, dict):
        raise ToolError("CapacityProfile", "invalid or excessively nested Value")
    kind = value.get("kind")
    if kind in ("unit", "bool"):
        return 1
    if kind == "bits":
        width = _nat(value["width"], "width")
        if not 1 <= width <= 64:
            raise ToolError("CapacityProfile", "bits width outside reference domain")
        return 8 + (width + 7) // 8
    if kind == "bytes":
        data = value["data"]
        if not isinstance(data, list) or any(type(b) is not int or not 0 <= b <= 255 for b in data):
            raise ToolError("CapacityProfile", "invalid byte array")
        return 8 + len(data)
    if kind == "handle":
        return 29
    if kind in ("record", "vec", "variant"):
        fields = value["values" if kind == "vec" else "fields"]
        if not isinstance(fields, list):
            raise ToolError("CapacityProfile", "Value fields must be a list")
        return (16 if kind == "variant" else 8) + sum(value_bytes(v, depth - 1) for v in fields)
    raise ToolError("CapacityProfile", f"unknown Value kind: {kind}")


def owned_state_bytes(world):
    """Exact abstract charge for parsed Reference.State, including dead entries."""
    total = sum(32 + _text_bytes(r["group"]) for r in world.get("allocationRules", []))
    total += sum(40 + _text_bytes(r["group"]) for r in world.get("allocationCounters", []))
    total += sum(64 + _text_bytes(o["tag"]) + value_bytes(o["value"])
                 for o in world.get("objects", []))
    total += sum(96 + _text_bytes(e["kind"]) + _text_bytes(e.get("source", ""))
                 + sum(value_bytes(v) for v in e.get("values", []))
                 for e in world.get("events", []))
    total += sum(32 + sum(_text_bytes(o.get(k, "")) for k in ("kind", "opcode", "source"))
                 + sum(value_bytes(v) for v in o.get("values", []))
                 for o in world.get("observations", []))
    return total


def read_segment_bytes(input_value):
    entries = input_value.get("context", {}).get("environment", [])
    names = [e.get("name") for e in entries]
    if len(names) != len(set(names)):
        raise ToolError("CapacityProfile", "duplicate environment name")
    values = [e["value"] for e in entries if e.get("name") == "profile.segmentBytes"]
    if len(values) != 1 or values[0].get("kind") != "bits" or values[0].get("width") != 64:
        raise ToolError("CapacityProfile", "explicit bits64 profile.segmentBytes required")
    size = _nat(values[0].get("value"), "profile.segmentBytes")
    if size >= 2**64:
        raise ToolError("CapacityProfile", "segment budget exceeds uint64")
    return size


def bind_segment_bytes(input_value, size):
    """Bind an explicit host choice; never fall back to world.maxBytes."""
    size = _nat(size, "profile.segmentBytes")
    result = deepcopy(input_value)
    entries = result.setdefault("context", {}).setdefault("environment", [])
    if any(e.get("name") == "profile.segmentBytes" for e in entries):
        if read_segment_bytes(result) != size:
            raise ToolError("CapacityProfile", "source segment budget differs from host choice")
    else:
        entries.append({"name": "profile.segmentBytes",
                        "value": {"kind": "bits", "width": 64, "value": str(size)}})
    read_segment_bytes(result)
    return result


@dataclass(frozen=True)
class StoreBound:
    """One reference (kind, domain, store) slot namespace, including tombstones.

    entry_bytes includes object header/tag/value; counter_bytes includes all
    counters attributable to this namespace. Shared counters may be overcounted.
    slot_capacity must come from actual configuration, never a measured peak.
    """
    namespace: tuple
    slot_capacity: int
    entry_bytes: int
    counter_bytes: int


def check_envelope(world, stores, *, event_count, event_bytes,
                   observation_count, observation_bytes, rule_bytes):
    """Check arithmetic sufficiency of a proposed finite static envelope.

    Every argument bounds the entire execution (including imported entries,
    preflight failures and retired counters), not merely the final state.
    Unrepresented stores and provider growth remain proof obligations. Return
    status is explicitly conditional even when every arithmetic check succeeds.
    """
    seen = set()
    store_index = {}
    object_count = object_bytes = counter_bytes = 0
    for store in stores:
        if len(store.namespace) != 3 or store.namespace in seen:
            raise ToolError("CapacityProfile", "duplicate or malformed store namespace")
        seen.add(store.namespace)
        store_index[store.namespace] = store
        for number in store.namespace:
            _nat(number, "namespace")
        count = _nat(store.slot_capacity, "slot capacity")
        object_count += count
        object_bytes += count * _nat(store.entry_bytes, "entry bytes")
        counter_bytes += _nat(store.counter_bytes, "counter bytes")
    for entry in world.get("objects", []):
        identity = entry["identity"]
        namespace = tuple(_nat(identity[k], k) for k in ("kind", "domain", "store"))
        if namespace not in seen:
            raise ToolError("CapacityProfile", "imported object namespace absent from envelope")
        bound = store_index[namespace]
        if _nat(identity["slot"], "slot") >= bound.slot_capacity or 64 + _text_bytes(entry["tag"]) + value_bytes(entry["value"]) > bound.entry_bytes:
            raise ToolError("CapacityProfile", "imported object exceeds namespace envelope")
    events = _nat(event_count, "event count")
    observations = _nat(observation_count, "observation count")
    byte_bound = (object_bytes + counter_bytes + _nat(rule_bytes, "rule bytes")
                  + events * _nat(event_bytes, "event bytes")
                  + observations * _nat(observation_bytes, "observation bytes"))
    bounds = {"maxObjects": object_count, "maxEvents": events,
              "maxObservations": observations, "maxBytes": byte_bound}
    defaults = {"maxObjects": 1024, "maxEvents": 1024,
                "maxObservations": 100000, "maxBytes": 1048576}
    if len(world.get("objects", [])) > object_count or len(world.get("events", [])) > events or len(world.get("observations", [])) > observations or owned_state_bytes(world) > byte_bound:
        raise ToolError("CapacityProfile", "envelope does not cover imported world")
    deficits = {name: {"required": str(bound), "configured": str(_nat(world.get(name, defaults[name]), name))}
                for name, bound in bounds.items() if bound > _nat(world.get(name, defaults[name]), name)}
    return {"schema": "leanat.opcode-capacity-envelope.v1", "status": "Conditional",
            "arithmeticSufficient": not deficits, "bounds": {k: str(v) for k, v in bounds.items()},
            "deficits": deficits, "certified": False}
