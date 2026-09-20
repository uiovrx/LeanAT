"""Independent base-protocol observation checker; never mutates a runtime ledger."""
from .io import ToolError, canonical, u64
import copy


class Monitor:
    def __init__(self, pending_capacity, snapshot_bytes, hooks):
        if pending_capacity<=0 or snapshot_bytes<=0: raise ToolError("InvalidBudget", "positive monitor capacities required")
        if not {"call", "return"}.issubset(hooks): raise ToolError("MissingHook", "call and return hooks required")
        self.capacity, self.byte_capacity = pending_capacity, snapshot_bytes
        self.pending, self.used, self.executed = {}, 0, 0
        self.last_call={}
        self.stopped = False

    def observe(self, record):
        if self.stopped: raise ToolError("MonitorStopped", "cannot continue after violation", 4)
        try: self._observe(record)
        except ToolError:
            self.stopped=True
            raise

    def _observe(self, record):
        body=record["body"]
        if record["apiKind"]!="nbTransport": return
        key=(record["domain"],body["connection"],body["localSide"],body["callId"])
        if record["recordKind"]=="call":
            if body.get("hasMM") is not True: raise ToolError("MissingMM", repr(key), 4)
            if key in self.pending: raise ToolError("DuplicateCall", repr(key),4)
            ledger=key[:3]
            identity=u64(body["callId"],"callId")
            if identity<=self.last_call.get(ledger,-1):raise ToolError("ReusedCallId",repr(key),4)
            if ledger not in self.last_call and len(self.last_call)>=self.capacity:raise ToolError("MonitorOverflow","local ledger identity capacity",4)
            size=len(canonical(body))
            if len(self.pending)>=self.capacity or self.used+size>self.byte_capacity: raise ToolError("MonitorOverflow", repr(key),4)
            phase,flow=body["phase"],body["flow"]
            if (flow,phase) not in {("Forward","BEGIN_REQ"),("Forward","END_RESP"),("Backward","END_REQ"),("Backward","BEGIN_RESP")}:
                if not body.get("ignorable"): raise ToolError("IllegalCall", repr(key),4)
            if u64(body["callTime"],"callTime")+u64(body["inDelay"],"inDelay")>2**64-1: raise ToolError("TimeOverflow", repr(key),4)
            self.pending[key]=(copy.deepcopy(body),size)
            self.last_call[ledger]=identity
            self.used+=size
        elif record["recordKind"]=="return":
            if key not in self.pending: raise ToolError("DuplicateOrMissingReturn",repr(key),4)
            call,size=self.pending[key]
            phase,sync,returned=call["phase"],body["sync"],body["phase"]
            legal=False
            if phase=="BEGIN_REQ": legal=sync in {"Accepted","Completed"} or (sync=="Updated" and returned in {"END_REQ","BEGIN_RESP"})
            elif phase=="END_REQ": legal=sync=="Accepted"
            elif phase=="BEGIN_RESP": legal=sync in {"Accepted","Completed"} or (sync=="Updated" and returned=="END_RESP")
            elif phase=="END_RESP": legal=sync in {"Accepted","Completed"}
            elif call.get("ignorable"): legal=sync=="Accepted" and body.get("ignored") is True
            if not legal: raise ToolError("IllegalReturn",repr(key)+" "+phase+"/"+sync,4)
            effective=u64(call["callTime"],"callTime")+u64(body["outDelay"],"outDelay")
            if effective>2**64-1: raise ToolError("TimeOverflow",repr(key),4)
            if phase=="BEGIN_REQ" and (sync=="Completed" or (sync=="Updated" and returned=="BEGIN_RESP")):
                if body.get("response") is None: raise ToolError("MissingResponse",repr(key),4)
            del self.pending[key]
            self.used-=size
            self.executed+=1

    def finish(self, reason):
        return {"stopReason":reason,"checkedExchanges":self.executed,"pendingCalls":len(self.pending),"stopped":self.stopped,"coverage":["base call/return matrix","MM declaration","time overflow","local return association"],"unobserved":["physical GP registry","lane transitions","raw pointer effects","opaque algorithms","runtime store hooks"]}
