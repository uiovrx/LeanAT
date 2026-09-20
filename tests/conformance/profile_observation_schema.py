"""Finite E40 published observation schemas; no values are projected or reordered.

Shared structures mirror profile_observations.hpp. Kind registrations cover the
six profile runners' published record types. Unknown kinds fail closed. The
registry is static code, never learned from the execution being validated.
"""
import copy
from contextvars import ContextVar
import re


class ObservationSchemaError(ValueError):
    pass


_stream_context = ContextVar("profile_observation_stream", default=None)


def array(element):
    return ("array", element)


def optional(element):
    return ("union", "null", element)


SCHEMAS = {
    "handle": {k: "nat" for k in ("kind", "domain", "store", "slot", "generation", "owner")},
    "ready": {"time": "nat", "turn": "nat"},
    "wake": {"time": "nat", "turn": "nat", "generation": "nat"},
    "key": {k: "nat" for k in ("time", "turn", "stage", "instance", "connection", "sequence")},
    "error": {"code": "nat", "message": "text"},
    "extension": {"key": "text", "bytes": array("byte")},
    "payload": {"command": "nat", "address": "nat", "data": array("byte"),
                "streamingWidth": "nat", "byteEnable": array("byte"), "status": "nat",
                "dmiHint": "bool", "extensions": array("extension")},
    "response": {"status": "nat", "data": array("byte"), "dmiHint": "bool", "extensions": array("extension")},
    "wireCall": {"id": "nat", "connection": "nat", "transport": "nat", "flow": "nat",
                 "phase": "nat", "callTime": "nat", "incomingDelay": "nat", "request": "payload"},
    "wireReturn": {"sync": "nat", "phase": optional("nat"), "outgoingDelay": "nat", "response": optional("response")},
    "intent": {"connection": "nat", "transaction": "handle", "flow": "nat", "phase": "nat",
               "notBefore": "nat", "callId": "nat", "transport": "nat", "payload": "payload"},
    "trace": {"kind": "text", "ready": "ready", "instance": "nat", "connection": "nat",
              "values": array("value"), "detail": "text"},
    "cell": {"value": "value", "version": "nat", "epoch": "nat", "stableBytes": "bool", "epochIndependent": "bool"},
    "eventDraft": {"key": "key", "value": "value", "owner": "nat", "epoch": "nat"},
    "queuedEvent": {"token": "handle", "event": "eventDraft", "cancelled": "bool"},
    "protocolMilestone": {"kind": "nat", "time": "nat", "implicit": "bool", "response": optional("response")},
    "ledgerIdentity": {k: "nat" for k in ("domain", "localSide", "connection", "transport", "transportGeneration")},
    "ledger": {"identity": "ledgerIdentity", "state": "nat", "lastTiming": "nat", "faulted": "bool",
               "pending": "bool", "callOrdinal": "nat", "protocolState": "nat"},
    "causal": {"hop": "handle", "ready": "ready", "callId": "nat", "milestone": "protocolMilestone"},
    "milestone": {"key": "key", "event": "handle", "identity": "ledgerIdentity", "causal": "causal"},
    "exchange": {"callId": "nat", "hop": "handle", "nextState": "nat", "milestones": array("protocolMilestone"),
                 "traceTags": array("text"), "needsAck": "bool", "wireTerminal": "bool", "ignored": "bool"},
    "source": {"fileHash": "text", "byteStart": "nat", "byteEnd": "nat", "line": "nat", "column": "nat"},
    "execution": {"kind": "nat", "domain": "nat", "instance": "nat", "connection": "nat", "ready": "ready",
                  "owner": "nat", "epoch": "nat", "source": "source"},
    "commit": {"actions": array("intent"), "cursor": "nat", "failed": "bool"},
    "segment": {"kind": "nat", "values": array("value"), "error": "text", "fuelUsed": "nat", "traces": array("trace"),
                "suspendedWait": optional("handle"), "resumeBlock": "nat", "liveValues": array("value")},
    "runtimeConfig": {**{k: "nat" for k in (
        "domain", "instance", "instanceCapacity", "eventCapacity", "ledgerCapacity", "callCapacity", "callsPerLedger",
        "intentCapacity", "frameCapacity", "waitCapacity", "liveCapacity", "drainCapacity", "hopsPerTransaction",
        "resultCapacity", "consumerCapacity", "pinCapacity", "maxEvents", "maxEventsPerTick", "ingressBytes",
        "nestingCapacity", "blockingCapacity")}, "instances": array("nat"), "connections": array("nat"),
        "connectionBindings": array({"connection": "nat", "initiator": "nat", "target": "nat"})},
}


def _fail(path, message):
    raise ObservationSchemaError(path + ": " + message)


def _validate(schema, value, path, depth=0):
    if depth > 128:
        _fail(path, "observation nesting exceeds limit")
    if isinstance(schema, str) and schema in SCHEMAS:
        return _validate(SCHEMAS[schema], value, path, depth + 1)
    if schema == "records":
        return validate_records(value, path, depth + 1)
    if schema == "output":
        stream = _stream_context.get()
        if stream is None:
            _fail(path, "output requires its published observation context")
        fields = {"port": "nat", "value": "value"}
        if stream == "events":
            fields["instance"] = "nat"
        return _validate(fields, value, path, depth + 1)
    if schema == "value":
        if not isinstance(value, dict) or len(value) != 1:
            _fail(path, "owning Value must have exactly one discriminant")
        key = next(iter(value))
        choices = {"unit": "null", "bool": "bool", "u64": "nat", "i64": "int", "bytes": array("byte"),
                   "array": array("value"), "handle": "handle"}
        if key not in choices:
            _fail(path, "unknown owning Value discriminant")
        return _validate(choices[key], value[key], path + "." + key, depth + 1)
    if isinstance(schema, dict):
        if not isinstance(value, dict) or set(value) != set(schema):
            _fail(path, "required fields differ: " + ", ".join(schema))
        for key, child in schema.items():
            _validate(child, value[key], path + "." + key, depth + 1)
    elif isinstance(schema, tuple):
        if schema[0] == "union":
            for choice in schema[1:]:
                try:
                    _validate(choice, value, path, depth + 1)
                    return
                except ObservationSchemaError:
                    pass
            _fail(path, "no permitted variant matches")
        elif schema[0] == "array":
            if not isinstance(value, list):
                _fail(path, "array required")
            for index, item in enumerate(value):
                _validate(schema[1], item, f"{path}[{index}]", depth + 1)
        else:
            _fail(path, "unknown schema constructor")
    elif schema in ("nat", "byte", "int"):
        signed = schema == "int"
        pattern = r"0|-[1-9][0-9]*|[1-9][0-9]*" if signed else r"0|[1-9][0-9]*"
        if not isinstance(value, str) or not re.fullmatch(pattern, value):
            _fail(path, "canonical decimal string required")
        number = int(value)
        low, high = (-2**63, 2**63 - 1) if signed else (0, 255 if schema == "byte" else 2**64 - 1)
        if not low <= number <= high:
            _fail(path, "integer outside field range")
    elif schema == "text":
        if not isinstance(value, str):
            _fail(path, "text required")
    elif schema == "bool":
        if type(value) is not bool:
            _fail(path, "boolean required")
    elif schema == "null":
        if value is not None:
            _fail(path, "null required")
    elif schema == "empty":
        if value != [] or not isinstance(value, list):
            _fail(path, "published empty collection required")
    else:
        _fail(path, "unregistered schema")


def validate_record(record, path="record", depth=0):
    if not isinstance(record, dict) or set(record) != {"ordinal", "kind", "value"}:
        _fail(path, "Recorder envelope requires ordinal, kind and value")
    _validate("nat", record["ordinal"], path + ".ordinal")
    kind = record["kind"]
    if not isinstance(kind, str) or kind not in KINDS:
        _fail(path, "unknown published record kind")
    _validate(KINDS[kind], record["value"], path + ".value", depth + 1)


def validate_records(records, path="records", depth=0):
    if not isinstance(records, list) or not records:
        _fail(path, "nonempty Recorder stream required")
    for index, record in enumerate(records):
        validate_record(record, f"{path}[{index}]", depth + 1)
        if int(record["ordinal"]) != index:
            _fail(path, "Recorder ordinals must be contiguous in retained order")


def validate_observations(identity, observations):
    """Validate the complete published observation object, retaining every field."""
    if identity == "C-T24":
        if not isinstance(observations, dict) or set(observations) != {"forward", "reverse"}:
            _fail(identity, "both ordered callback subruns required")
        for direction in ("forward", "reverse"):
            _validate_observation_body(observations[direction], "records", False, identity + "." + direction)
        return
    if identity not in {f"C-T{i:02}" for i in range(1, 31)}:
        _fail("observations", "unknown Core identity")
    memory = identity in {"C-T20", "C-T21", "C-T22", "C-T23"}
    records = memory or identity in {f"C-T{i:02}" for i in range(1, 9)} | {"C-T25", "C-T26", "C-T29"}
    if identity == "C-T02" and isinstance(observations, dict) and "events" in observations:
        records = False  # Complementary native-wire and real-GP publications.
    _validate_observation_body(observations, "records" if records else "events", memory, identity)


def _validate_observation_body(observations, stream, memory, path):
    required = {stream, "stop", "fuel"} | ({"state", "operations"} if memory else set())
    if not isinstance(observations, dict) or set(observations) != required:
        _fail(path, "published observation envelope fields differ")
    _validate("text", observations["stop"], path + ".stop")
    if not observations["stop"]:
        _fail(path, "stop reason missing")
    _validate("nat", observations["fuel"], path + ".fuel")
    token = _stream_context.set(stream)
    try:
        validate_records(observations[stream], path + "." + stream)
    finally:
        _stream_context.reset(token)
    if memory:
        _validate(array("value"), observations["state"], path + ".state")
        _validate("nat", observations["operations"], path + ".operations")


def symmetric_deletion_control(identity, observations):
    """Use the production validator on an identical missing field in either side.

    The control deliberately corrupts a copied *record value*, when it has fields,
    rather than merely changing fuel or comparing two different JSON objects.
    """
    validate_observations(identity, observations)
    changed = copy.deepcopy(observations)
    body = changed["forward"] if identity == "C-T24" else changed
    stream = "records" if "records" in body else "events"
    candidates = [(index, record) for index, record in enumerate(body[stream])
                  if isinstance(record["value"], dict) and record["value"]]
    if candidates:
        index, record = candidates[0]
        field = next(iter(record["value"]))
        del record["value"][field]
        location = f"{stream}[{index}].value.{field}"
    else:
        del body[stream][0]["value"]
        location = f"{stream}[0].value"
    try:
        validate_observations(identity, changed)
    except ObservationSchemaError:
        return {"mutation": "same required record field deleted from both profiles",
                "field": ("forward." if identity == "C-T24" else "") + location,
                "rejected": True}
    _fail(identity, "validator accepted symmetric required-field deletion")


# KINDS is the finite emitter registry below; no input/output report is read here.
KINDS = {
    # Additional literal emitter paths, not exercised in the retained round-7 report.
    'afterCallError': 'ledger',
    'callError': 'error',
    'commit.error': 'error',
    'host.transport.error': 'error',
    'wire.start.error': 'error',
    'output': 'output',
    'runtimeTrace': 'trace',
    'trace': 'trace',
    'accepted.ledger': 'ledger',
    'actual.byteorder': 'nat',
    'add.error': 'error',
    'admission': {'connection': 'nat', 'generation': 'nat', 'hop': 'handle', 'inTime': 'nat', 'pending': 'bool', 'request': 'payload', 'semanticTerminal': 'bool', 'sequence': 'nat', 'servicing': 'bool', 'transport': 'nat', 'txn': 'handle', 'wireTerminal': 'bool'},
    'admission.request': 'payload',
    'admittedHop': 'ledger',
    'admittedTransaction': 'handle',
    'afterCall': 'ledger',
    'afterCallLedger': 'ledger',
    'afterCallRequestLaneFree': 'bool',
    'afterCallResponseLaneFree': 'bool',
    'afterExternalResponse': 'ledger',
    'afterReturn': 'ledger',
    'arguments': ('array', 'value'),
    'arm': ('union', 'null', 'ready'),
    'batch': {'complete': 'bool', 'cursor': 'nat', 'id': 'nat', 'members': ('array', 'queuedEvent'), 'ready': 'ready'},
    'batch.error': 'error',
    'beforeCall': 'ledger',
    'binding.ledger': 'ledger',
    'blocking.arrival': 'nat',
    'blocking.request': 'payload',
    'blocking.return': 'payload',
    'blockingArrival': 'nat',
    'blockingCommit': 'commit',
    'blockingFuelUsed': 'nat',
    'blockingInputDelay': 'nat',
    'blockingResponse': 'response',
    'blockingReturn': 'payload',
    'blockingReturnDelay': 'nat',
    'businessInputs': 'nat',
    'businessMilestone': 'causal',
    'callTicket': 'handle',
    'callbackOrder': 'text',
    'calls.received': ('array', 'nat'),
    'calls.sent': ('array', 'nat'),
    'cancelDrain': {'hops': ('array', {'callPin': 'bool', 'cleanupReturned': 'bool', 'hop': 'handle', 'timingConsumed': 'bool', 'wireTerminal': 'bool'}), 'reason': 'nat', 'state': 'nat'},
    'cancelReceipt': 'handle',
    'commit': 'commit',
    'configuration': {'callsPerLedger': 'nat', 'connection': 'nat', 'domain': 'nat', 'instance': 'nat', 'ledgerCapacity': 'nat', 'target': 'nat', 'ticketCapacity': 'nat'},
    'createdLedger': 'ledger',
    'createdLedgerHandle': 'handle',
    'debugCount': 'nat',
    'debugNativeReturn': 'payload',
    'debugTick': 'nat',
    'descriptorTopology': {'connection': 'nat', 'domain': 'nat', 'initiator': 'nat', 'target': 'nat'},
    'dispatch': 'queuedEvent',
    'drain.outstanding': 'nat',
    'drain.reason': 'nat',
    'drain.receipt': 'handle',
    'drain.state': 'nat',
    'earlyTerminalRead': {'error': 'error', 'time': 'nat'},
    'effectiveMilestone': 'milestone',
    'endian.reject': 'error',
    'epoch': 'nat',
    'exchange': 'exchange',
    'execution': 'execution',
    'external.buffer.reused': 'nat',
    'externalCall': 'wireCall',
    'externalDelayedCall': 'wireCall',
    'externalPeerReturn': 'wireReturn',
    'externalReturn': 'wireReturn',
    'faulted.ledger': 'ledger',
    'finalBlocking': 'payload',
    'finalDelay': 'nat',
    'finalDrainBacklog': 'nat',
    'finalLedger': 'ledger',
    'finalModelState': ('array', 'cell'),
    'finalRoutes': 'nat',
    'finalWireState': 'ledger',
    'fuel': {'consumed': 'nat', 'initial': 'nat', 'remaining': 'nat'},
    'fuelUsed': 'nat',
    'generation.error': 'error',
    'generation.final': 'handle',
    'gpRefCount': 'nat',
    'handlerExecution': 'execution',
    'host.arm': ('union', 'null', 'wake'),
    'host.intent': 'intent',
    'host.state.epoch.transition': ('array', 'cell'),
    'host.trace': 'trace',
    'host.wire.return': 'wireReturn',
    'hostCalls': 'nat',
    'hostLimits': {'admissionHops': 'nat', 'admissionTransactions': 'nat', 'grants': 'nat', 'handlerInstructionFuel': 'nat', 'routes': 'nat'},
    'immediate.applied': 'nat',
    'immediate.calls': 'nat',
    'immediate.gp': 'payload',
    'independentIllegalReturnCase': 'records',
    'ingress.rejected': 'error',
    'initialMemory': ('array', 'nat'),
    'initialState': ('array', 'cell'),
    'initiatorDelayedReturn': 'wireReturn',
    'input': {'call': 'wireCall', 'ready': 'ready'},
    'input.call': 'wireCall',
    'input.ready': 'ready',
    'instructionFuelBranch': 'records',
    'intent': 'intent',
    'irqCount': 'nat',
    'kernel.observed': 'nat',
    'kernel.observer': {'count': 'nat', 'delta': 'nat', 'time': 'nat'},
    'ledger': 'ledger',
    'ledgerHandle': 'handle',
    'loop.resumes': 'nat',
    'loop.stop': 'text',
    'memory': ('array', 'nat'),
    'memory.after': ('array', 'nat'),
    'memory.before': ('array', 'nat'),
    'milestone': 'milestone',
    'milestone.call': 'nat',
    'milestone.full': 'milestone',
    'milestone.hop': 'handle',
    'milestone.ready': 'ready',
    'milestone.value': 'protocolMilestone',
    'mm.free': {'count': 'nat', 'refs': 'nat', 'time': 'nat'},
    'mm.release.owner': {'frees': 'nat', 'refs': 'nat'},
    'mmFreeCount': 'nat',
    'modelHandler': 'segment',
    'modelState': ('array', 'cell'),
    'mul.error': 'error',
    'mutation': {'branch': 'text', 'integrityRecomputed': 'bool', 'path': 'text', 'value': 'nat', 'width': 'nat'},
    'native.call': 'wireCall',
    'native.outgoing': {'connection': 'nat', 'delay': 'nat', 'phase': 'nat', 'refs': 'nat', 'time': 'nat'},
    'native.return': {'delay': 'nat', 'phase': 'nat', 'refs': 'nat', 'sync': 'nat'},
    'nativeRequest': 'payload',
    'nativeResponse': 'payload',
    'nativeResponseDelay': 'nat',
    'nativeResponsePhase': 'nat',
    'nativeResponseTick': 'nat',
    'nativeSendIntent': 'intent',
    'nativeWireReturn': {'callId': 'nat', 'return': 'wireReturn'},
    'nbInputDelay': 'nat',
    'nbReturnDelay': 'nat',
    'nbReturnPhase': 'nat',
    'nbReturnSync': 'nat',
    'needsAck': 'bool',
    'open.binding': 'ledger',
    'operation': {'arguments': ('array', 'value'), 'handler': 'nat', 'ready': 'ready'},
    'outboundCall': 'wireCall',
    'payload': 'payload',
    'peer.final': ('array', 'nat'),
    'peer.request': 'payload',
    'peer.response': 'response',
    'peer.visibleWrite': ('array', 'nat'),
    'pin': {'byte': 'nat', 'connection': 'nat', 'frees': 'nat', 'refs': 'nat'},
    'positiveContext': 'execution',
    'process': {'bound': 'bool', 'instance': 'nat', 'ordinal': 'nat', 'program': 'nat', 'state': 'nat', 'suspension': {'ordinal': 'nat', 'process': 'handle', 'wait': 'handle'}},
    'process.created': 'handle',
    'process.startToken': 'handle',
    'protocolMilestone': 'milestone',
    'publishedIntent': 'intent',
    'pump': {'batch': 'nat', 'complete': 'bool', 'executed': 'nat', 'limit': 'text', 'progress': 'nat', 'stop': 'nat'},
    'pump.error': 'error',
    'queue.occupied': 'nat',
    'queue.preexisting': 'handle',
    'receiverAndCallerReturn': 'wireReturn',
    'receiverCall': 'wireCall',
    'recordedReturn': {'callId': 'nat', 'terminal': 'bool', 'tick': 'nat'},
    'registerReset': ('array', 'nat'),
    'registeredIgnorablePhase': 'nat',
    'rejectedMutation': {'branch': 'text', 'error': {'code': 'nat', 'fieldPath': 'text', 'reason': 'text'}, 'fuel': 'nat', 'handlerInvocations': 'nat'},
    'requestLaneFree': 'bool',
    'response.afterWait': 'value',
    'response.durable': {'ready': 'ready', 'value': 'value'},
    'response.owned': 'response',
    'response.ready': 'ready',
    'responseCommit': 'commit',
    'responseIntent': 'intent',
    'responseLaneFree': 'bool',
    'result.consumer': 'handle',
    'result.handle': 'handle',
    'resume': {'ordinal': 'nat', 'process': 'handle', 'ready': 'ready', 'wait': 'handle'},
    'retainedFailedCall': 'wireCall',
    'retainedFailedReturn': 'wireReturn',
    'retainedFailure': 'error',
    'retainedRoute': {'downstream': 'nat', 'egressAddress': 'nat', 'hop': 'handle', 'ingressAddress': 'nat', 'payload': 'payload', 'transaction': 'handle', 'upstream': 'nat'},
    'retire.call': 'wireCall',
    'retire.ledger': 'ledger',
    'retire.return': 'wireReturn',
    'return': ('array', 'value'),
    'returnError': 'error',
    'runtime.config': 'runtimeConfig',
    'runtime.stop': 'text',
    'runtimeConfig': 'runtimeConfig',
    'runtimeEffectiveOrder': 'records',
    'runtimeIgnoredNoForward': 'records',
    'runtimeTerminalConsumption': 'records',
    'schedule': 'eventDraft',
    'segment': 'segment',
    'selectedMap': {'bindingIndex': 'nat', 'decoder': 'nat', 'downstream': 'nat', 'id': 'nat', 'outputEndpoint': 'nat', 'size': 'nat', 'source': 'nat', 'target': 'nat'},
    'selectedProgram': 'nat',
    'sendIntent': 'intent',
    'sequence.error': 'error',
    'service.active': 'nat',
    'start.event': 'queuedEvent',
    'state': ('array', 'value'),
    'state.after': ('array', 'cell'),
    'state.after.stale': ('array', 'cell'),
    'state.before': ('array', 'cell'),
    'state.final': ('array', 'cell'),
    'state.initial': ('array', 'cell'),
    'stimulus.blocking': {'delay': 'nat', 'payload': 'payload'},
    'stimulus.gp': 'payload',
    'stimulus.incoming': {'connection': 'nat', 'delay': 'nat', 'payload': 'payload', 'phase': 'nat', 'time': 'nat'},
    'stimulus.no_mm': 'payload',
    'stimulus.reset.stage': 'nat',
    'stop': 'text',
    'targetDelayedCallTicket': 'handle',
    'targetDelayedExchange': 'exchange',
    'targetDelayedReturn': 'wireReturn',
    'terminalAt25': 'protocolMilestone',
    'terminalConsumptions': 'nat',
    'time.error': 'error',
    'timeout.consumer': 'handle',
    'timeout.final.reason': 'nat',
    'timeout.final.state': 'nat',
    'timeout.local.after': 'value',
    'timeout.local.before': 'value',
    'timeout.receipt': 'handle',
    'timeout.result': 'handle',
    'timeout.results.remaining': 'nat',
    'transaction.committed': 'commit',
    'transaction.discarded': 'bool',
    'validCommit': 'commit',
    'validExecution': 'segment',
    'validState': ('array', 'cell'),
    'vm.commit': 'commit',
    'vm.context': 'execution',
    'vm.error': 'error',
    'vm.fuel.remaining': 'nat',
    'vm.result': 'segment',
    'vm.resumes': 'nat',
    'vm.state': ('array', 'cell'),
    'wake': 'wake',
    'width.reject': 'error',
    'wire.call': 'wireCall',
    'wire.ingress': 'wireCall',
    'wire.ingress.return': 'wireReturn',
    'wire.return': 'wireReturn',
    'wireCall': 'wireCall',
    'wireReturn': 'wireReturn',
}

