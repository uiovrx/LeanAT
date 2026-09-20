"""Full-value transcript validation. Schema is versioned independently of runtime IR."""
import base64
import binascii
import json
import os
import tempfile
from pathlib import Path
from .io import ToolError, canonical, digest, read_json, require, u64

APIS = {"nbTransport", "blockingTransport", "debugTransport", "dmiQuery", "dmiInvalidate", "managedAccess", "sideband", "reset"}
KINDS = {"call", "return", "event"}
TIME_FIELDS = {"callTime", "inDelay", "outDelay", "time", "effectiveTime", "turn", "epoch", "appEpoch", "observedOrdinal", "callId", "invocationId", "generation", "transport"}


def validate_values(value, data_limit=1_048_576):
    if isinstance(value, dict):
        if "digest" in value and "base64" not in value:
            if value.get("data") is None:raise ToolError("MissingFullValue", "digest cannot replace observable bytes")
            if value["digest"]!=digest(canonical(value["data"])):raise ToolError("HashMismatch","full data digest mismatch")
        if "base64" in value:
            require(value, ("declaredLength",), "bytes")
            if not isinstance(value["base64"],str) or len(value["base64"])>((data_limit+2)//3)*4:raise ToolError("TraceOverflow","encoded bytes exceed host data limit")
            try: raw = base64.b64decode(value["base64"], validate=True)
            except (ValueError, binascii.Error, TypeError) as exc: raise ToolError("TraceSchemaMismatch", "invalid base64") from exc
            if type(value["declaredLength"]) is not int or len(raw) != value["declaredLength"] or len(raw) > data_limit:
                raise ToolError("TraceOverflow", "byte length mismatch or host data limit exceeded")
            if "digest" in value and value["digest"] != digest(raw): raise ToolError("HashMismatch", "byte digest mismatch")
        for key, child in value.items():
            if key in TIME_FIELDS and child is not None: u64(child, key)
            validate_values(child, data_limit)
    elif isinstance(value, list):
        if len(value) > data_limit: raise ToolError("TraceOverflow", "value array exceeds host limit")
        for child in value: validate_values(child, data_limit)


def validate_transcript(document, record_limit=100_000, data_limit=1_048_576, strict=True):
    require(document, ("schema", "profile", "phaseMapping", "artifacts", "toolchainLock", "initialState", "effectiveConfig", "timebase", "captureState", "stopReason", "pendingResponsibilities", "replayCoverage", "records"), "transcript")
    if document["schema"] != "leanat.transcript.v1": raise ToolError("TraceSchemaMismatch", "unsupported transcript schema")
    validate_values(document,data_limit)
    if document["captureState"] not in {"Complete", "Prefix", "Interrupted"}: raise ToolError("TraceSchemaMismatch", "captureState")
    records = document["records"]
    if not isinstance(records, list) or len(records)>record_limit: raise ToolError("TraceOverflow", "record capacity")
    coverage = document["replayCoverage"]
    require(coverage, ("eligibility", "scope", "unobservedEffects", "assumptions"), "replayCoverage")
    if strict and (coverage["eligibility"] != "FullForScope" or coverage["unobservedEffects"]):
        raise ToolError("ReplayCoverageGap", "declared observation scope is not fully replayable")
    pending, seen, stacks = {}, set(), {}
    previous = -1
    for index, record in enumerate(records):
        require(record, ("recordKind", "apiKind", "observedOrdinal", "domain", "time", "role", "body"), "record")
        if set(record)-{"recordKind", "apiKind", "observedOrdinal", "domain", "time", "role", "body", "diagnostics"}:
            raise ToolError("TraceSchemaMismatch", "unknown semantic envelope field")
        ordinal = u64(record["observedOrdinal"], "observedOrdinal")
        if ordinal <= previous: raise ToolError("CallReturnMismatch", "observation order is not strictly increasing")
        previous = ordinal
        api, kind, body = record["apiKind"], record["recordKind"], record["body"]
        if api not in APIS or kind not in KINDS or record["role"] not in {"environment", "model"}: raise ToolError("TraceSchemaMismatch", "unsupported API/kind/role")
        validate_values(record, data_limit)
        if kind == "event":
            if api not in {"sideband", "reset", "dmiInvalidate"}: raise ToolError("TraceSchemaMismatch", "API requires call and return")
            require(body, ("value",), "event")
            continue
        if api == "nbTransport":
            require(body, ("connection", "localSide", "callId"), "nb identity")
            key = (record["domain"], api, body["connection"], body["localSide"], body["callId"])
            require(body, ("flow", "phase", "callTime", "inDelay", "transport", "generation", "payload") if kind=="call" else ("sync", "phase", "outDelay", "response"), "nb body")
            allowed={"connection","localSide","callId","flow","phase","callTime","inDelay","transport","generation","payload","hasMM","ignorable","sync","outDelay","response","ignored","nativeStack","parentCallRef","parentInvocationRef","diagnostics"}
            if set(body)-allowed:raise ToolError("TraceSchemaMismatch","unknown mandatory nb body field")
            if body["localSide"] not in {"Incoming","Outgoing"}:raise ToolError("TraceSchemaMismatch","localSide")
            if kind=="call" and (body["flow"] not in {"Forward","Backward"} or not isinstance(body["phase"],str) or not isinstance(body["payload"],dict)):raise ToolError("TraceSchemaMismatch","nb call fields")
            if kind=="return" and body["sync"] not in {"Accepted","Updated","Completed"}:raise ToolError("TraceSchemaMismatch","sync")
            if kind=="return" and body["sync"]=="Updated" and not isinstance(body["phase"],str):raise ToolError("TraceSchemaMismatch","UPDATED requires returned phase")
        else:
            require(body, ("invocationId",), "invocation")
            key = (record["domain"], api, body["invocationId"])
            require(body, ("request",) if kind=="call" else ("result",), api)
            if set(body)-{"invocationId","request","result","nativeStack","parentInvocationRef","diagnostics"}:raise ToolError("TraceSchemaMismatch","unknown mandatory invocation body field")
            field=body["request" if kind=="call" else "result"]
            fields={"blockingTransport":(("payload","inDelay"),("payload","outDelay")),"debugTransport":(("payload",),("count","data")),"dmiQuery":(("address","command"),("granted",)),"dmiInvalidate":(("start","end"),("acknowledged",)),"managedAccess":(("command","value"),("value",)),"sideband":(("value",),("value",)),"reset":(("value",),("value",))}
            require(field,fields[api][0 if kind=="call" else 1],api+" value")
            if api=="debugTransport" and kind=="return" and (type(field["count"]) is not int or not 0<=field["count"]<=2**32-1):raise ToolError("TraceSchemaMismatch","debug count")
            if api=="dmiQuery" and kind=="return":
                if type(field["granted"]) is not bool:raise ToolError("TraceSchemaMismatch","DMI result boolean")
                if field["granted"]:require(field,("grant",),"DMI grant")
        if kind == "call":
            if key in seen: raise ToolError("CallReturnMismatch", "duplicate call identity")
            seen.add(key)
            pending[key] = record
            if "nativeStack" in body: stacks.setdefault((record["domain"],body["nativeStack"]), []).append(key)
        else:
            if key not in pending: raise ToolError("CallReturnMismatch", "return without pending call")
            call = pending.pop(key)
            if record["role"] == call["role"]: raise ToolError("CallReturnMismatch", "return role must be opposite caller")
            if u64(record["time"], "time") < u64(call["time"], "time"): raise ToolError("CallReturnMismatch", "return precedes call")
            if "nativeStack" in call["body"]:
                stack = stacks[(record["domain"],call["body"]["nativeStack"])]
                if not stack or stack.pop()!=key: raise ToolError("CallReturnMismatch", "native return stack order")
    if strict and document["captureState"] != "Complete": raise ToolError("TranscriptIncomplete", "strict replay needs complete capture")
    if document["captureState"] == "Complete" and pending: raise ToolError("TranscriptIncomplete", "missing synchronous feedback")
    return document


def read_transcript(path, **kwargs):
    return validate_transcript(read_json(path), **kwargs)


def compare_runs(runs, expected_backends=None):
    if expected_backends is not None and set(runs)!=set(expected_backends): raise ToolError("MissingBackend", "all declared backends must execute", 4)
    if len(runs)<2: raise ToolError("MissingBackend", "comparison needs at least two backends", 4)
    baseline = None
    for name, run in runs.items():
        require(run, ("trace", "feedback", "stopReason", "observationEndpoint", "complete", "dataMode", "transcriptHash"), name)
        if run["dataMode"] != "FullValue": raise ToolError("MissingFullValue", name)
        validate_values(run)
        if run["complete"] is not True or run["stopReason"] not in {"Quiescent", "Completed"}: raise ToolError("IncompleteRun", name + ": " + str(run["stopReason"]), 4)
        observed = {key: run[key] for key in ("trace", "feedback", "observationEndpoint", "transcriptHash")}
        if baseline is None: baseline = observed
        elif baseline != observed:
            raise ToolError("SemanticMismatch", "full observable values or environment feedback differ in " + name, 4)
    return {"status":"pass", "scope":"strict-full-data", "backends":list(runs), "release_gate":"not-evaluated"}


def write_trace(path, events, record_limit, byte_limit):
    """Commit a complete JSONL stream atomically; never publish a truncated success file."""
    if record_limit<0 or byte_limit<0: raise ToolError("InvalidBudget","finite nonnegative trace budgets required")
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    fd,temp=tempfile.mkstemp(prefix=".leanat-trace-",dir=path.parent)
    count=used=0
    try:
        with os.fdopen(fd,"wb") as stream:
            for event in events:
                validate_values(event)
                encoded=canonical(event)+b"\n"
                if count>=record_limit or used+len(encoded)>byte_limit: raise ToolError("TraceOverflow","trace sink budget exhausted")
                stream.write(encoded);count+=1;used+=len(encoded)
        os.replace(temp,path)
        return {"captureState":"Complete","records":count,"bytes":used}
    except OSError as exc: raise ToolError("TraceWriteFailure",str(exc),5) from exc
    finally:
        if os.path.exists(temp):os.unlink(temp)


def lookup_source(source_map, artifact_bytes, location, source_root):
    require(source_map,("schema","artifactHash","entries"),"source map")
    if source_map["schema"]!="leanat.source-map.v1" or source_map["artifactHash"]!=digest(artifact_bytes): raise ToolError("MissingSourceMap","artifact/source-map mismatch")
    matches=[entry for entry in source_map["entries"] if entry.get("location")==location]
    if len(matches)!=1: raise ToolError("MissingSourceMap","location missing or ambiguous")
    span=matches[0]["sourceSpan"]
    require(span,("path","hash","startByte","endByte","line","column","ancestry"),"source span")
    root=Path(source_root).resolve();path=(root/span["path"]).resolve()
    if not path.is_relative_to(root) or path.stat().st_size>8_388_608: raise ToolError("MissingSourceMap","source path or size")
    raw=path.read_bytes()
    if digest(raw)!=span["hash"] or not 0<=span["startByte"]<=span["endByte"]<=len(raw): raise ToolError("MissingSourceMap","stale source hash/range")
    try: prefix=raw[:span["startByte"]].decode("utf-8");raw[span["startByte"]:span["endByte"]].decode("utf-8")
    except UnicodeDecodeError as exc:raise ToolError("MissingSourceMap","range splits UTF-8 scalar") from exc
    if span["line"]!=prefix.count("\n")+1 or span["column"]!=len(prefix.rsplit("\n",1)[-1])+1: raise ToolError("MissingSourceMap","source line/column does not match byte range")
    return span
