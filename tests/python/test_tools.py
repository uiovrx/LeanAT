import base64
import copy
import json
from pathlib import Path
import tempfile
import unittest
import sys
from tools.leanat.io import ToolError, read_json, digest
from tools.leanat.trace import validate_transcript, compare_runs, validate_values, write_trace, lookup_source
from tools.leanat.evidence import normalize_capabilities, release_gate, validate_manifest
from tools.leanat.monitor import Monitor
from tools.leanat.migration import migration_report, validate_decisions, apply_migration, RULES
from tools.leanat.benchmark import run_benchmark
from tools.leanat.cli import parser, inventory, protocol_graph


def transcript():
    identity={"connection":1,"localSide":"Outgoing","callId":"9007199254740993"}
    return {"schema":"leanat.transcript.v1","profile":"AT-Core-1.1-draft","phaseMapping":{},"artifacts":{},"toolchainLock":{},"initialState":{},"effectiveConfig":{},"timebase":{"tick":"1"},"captureState":"Complete","stopReason":"Quiescent","pendingResponsibilities":[],"replayCoverage":{"eligibility":"FullForScope","scope":"L1","unobservedEffects":[],"assumptions":[]},"records":[
        {"recordKind":"call","apiKind":"nbTransport","observedOrdinal":"0","domain":1,"time":"100","role":"model","body":dict(identity,flow="Forward",phase="BEGIN_REQ",callTime="100",inDelay="10",transport="1",generation="1",payload={"data":{"declaredLength":2,"base64":"AQI="}},hasMM=True)},
        {"recordKind":"return","apiKind":"nbTransport","observedOrdinal":"1","domain":1,"time":"100","role":"environment","body":dict(identity,sync="Completed",phase=None,outDelay="25",response={"status":"OK","data":{"declaredLength":2,"base64":"AwQ="}})}]}


class TraceTests(unittest.TestCase):
    def test_precise_ids_and_full_bytes(self):
        self.assertEqual(validate_transcript(transcript())["records"][0]["body"]["callId"],"9007199254740993")
        bad=transcript();bad["records"][0]["body"]["callId"]=9007199254740993
        with self.assertRaises(ToolError): validate_transcript(bad)

    def test_missing_feedback(self):
        bad=transcript();bad["records"].pop()
        with self.assertRaisesRegex(ToolError,"feedback"): validate_transcript(bad)

    def test_order_and_coverage(self):
        bad=transcript();bad["records"].reverse()
        with self.assertRaises(ToolError): validate_transcript(bad)
        bad=transcript();bad["replayCoverage"]["unobservedEffects"]=["raw-write"]
        with self.assertRaises(ToolError): validate_transcript(bad)

    def test_duplicate_json_and_depth(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"input.json";path.write_text('{"a":1,"a":2}')
            with self.assertRaises(ToolError): read_json(path)
            path.write_text('['*100+'0'+']'*100)
            with self.assertRaises(ToolError): read_json(path,max_depth=8)

    def test_hash_not_value_and_bounded_data(self):
        with self.assertRaises(ToolError): validate_values({"digest":"same"})
        with self.assertRaises(ToolError): validate_values({"base64":"AQI=","declaredLength":3})
        with self.assertRaises(ToolError): validate_values({"base64":"AQI=","declaredLength":2},1)
        with self.assertRaises(ToolError): validate_values({"digest":"incorrect","data":None})
        bad=transcript();bad["initialState"]={"digest":"fake","data":None}
        with self.assertRaises(ToolError):validate_transcript(bad)

    def test_backend_feedback_and_completion(self):
        run={"trace":[{"data":[1,2]}],"feedback":[{"status":"OK"}],"stopReason":"Quiescent","complete":True,"observationEndpoint":"done","dataMode":"FullValue","transcriptHash":"fixed"}
        self.assertEqual(compare_runs({"a":run,"b":copy.deepcopy(run)})["status"],"pass")
        other=copy.deepcopy(run);other["feedback"][0]["status"]="ERROR"
        with self.assertRaises(ToolError): compare_runs({"a":run,"b":other})
        other=copy.deepcopy(run);other["complete"]=False;other["stopReason"]="FuelExhausted"
        with self.assertRaises(ToolError): compare_runs({"a":other,"b":other})
        other=copy.deepcopy(run);del other["feedback"]
        with self.assertRaises(ToolError): compare_runs({"a":run,"b":other})

    def test_atomic_trace_overflow(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"trace.jsonl";path.write_text("original")
            with self.assertRaises(ToolError):write_trace(path,[{"kind":"output"},{"kind":"commit"}],1,4096)
            self.assertEqual(path.read_text(),"original")
            report=write_trace(path,[{"kind":"output"}],1,4096)
            self.assertEqual(report["records"],1)

    def test_source_unicode_and_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            raw="α\n模型".encode();path=Path(directory)/"a.lean";path.write_bytes(raw)
            location={"kind":"Instruction","programId":0,"blockId":0,"index":0}
            span={"path":"a.lean","hash":digest(raw),"startByte":3,"endByte":6,"line":2,"column":1,"ancestry":[]}
            source={"schema":"leanat.source-map.v1","artifactHash":digest(b"descriptor"),"entries":[{"location":location,"sourceSpan":span}]}
            self.assertEqual(lookup_source(source,b"descriptor",location,directory),span)
            span["startByte"]=4
            with self.assertRaises(ToolError):lookup_source(source,b"descriptor",location,directory)


class MonitorTests(unittest.TestCase):
    def test_complete_matrix(self):
        allowed={"BEGIN_REQ":{"Accepted":{None},"Updated":{"END_REQ","BEGIN_RESP"},"Completed":{None}},"END_REQ":{"Accepted":{None}},"BEGIN_RESP":{"Accepted":{None},"Updated":{"END_RESP"},"Completed":{None}},"END_RESP":{"Accepted":{None},"Completed":{None}}}
        for phase in allowed:
            for sync in ("Accepted","Updated","Completed"):
                for returned in (None,"END_REQ","BEGIN_RESP","END_RESP"):
                    with self.subTest(phase=phase,sync=sync,returned=returned):
                        doc=transcript();call,ret=doc["records"]
                        call["body"]["phase"]=phase;call["body"]["flow"]="Forward" if phase in {"BEGIN_REQ","END_RESP"} else "Backward"
                        ret["body"]["sync"]=sync;ret["body"]["phase"]=returned
                        monitor=Monitor(2,4096,{"call","return"});monitor.observe(call)
                        legal=sync in allowed[phase] and (sync!="Updated" or returned in allowed[phase][sync])
                        if legal:
                            monitor.observe(ret);self.assertEqual(monitor.finish("Quiescent")["checkedExchanges"],1)
                        else:
                            with self.assertRaises(ToolError):monitor.observe(ret)

    def test_no_mm_capacity_and_missing_hook(self):
        with self.assertRaises(ToolError): Monitor(1,1,{"call"})
        call=transcript()["records"][0]
        with self.assertRaises(ToolError): Monitor(1,1,{"call","return"}).observe(call)
        call["body"]["hasMM"]=False
        with self.assertRaisesRegex(ToolError,r"\("): Monitor(1,4096,{"call","return"}).observe(call)

    def test_double_return_and_dual_local_ledger(self):
        monitor=Monitor(2,4096,{"call","return"})
        a,b=transcript()["records"];c,d=copy.deepcopy(a),copy.deepcopy(b)
        c["body"]["localSide"]=d["body"]["localSide"]="Incoming"
        for record in (a,c,d,b):monitor.observe(record)
        self.assertEqual(monitor.executed,2)
        with self.assertRaises(ToolError):monitor.observe(b)

    def test_end_resp_without_response_and_reused_identity(self):
        monitor=Monitor(2,4096,{"call","return"})
        call,returned=transcript()["records"]
        call["body"]["phase"]="END_RESP";returned["body"]["response"]=None
        monitor.observe(call);monitor.observe(returned)
        self.assertEqual(monitor.executed,1)
        with self.assertRaisesRegex(ToolError,"Outgoing"):monitor.observe(call)


class EvidenceTests(unittest.TestCase):
    def test_optional_group_atomic(self):
        manifest={"profile":"AT-Ext-1.1-draft","optional_enabled":["managed-access"],"tests":[{"id":"E-T34","status":"pass","applicability":"N/A"}]}
        report=release_gate(manifest)
        self.assertTrue(all(f"E-T{i:02}" in report["requiredTests"] for i in range(26,35)))
        self.assertIn("E-T34",report["blockers"])
        self.assertEqual(report["optionalGroups"][0]["applicability"],"N/A")

    def test_alias_unknown(self):
        values,migrations=normalize_capabilities(["pure-ffi"])
        self.assertEqual(values,["extern-pure"]);self.assertEqual(len(migrations),1)
        with self.assertRaises(ToolError):normalize_capabilities(["unbounded"])

    def test_hash_tamper_and_unbacked_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"model.bin";path.write_bytes(b"original")
            doc={"schema":"leanat.manifest.v1","profile":"AT-Core-1.1-draft","topSystemId":1,"effectiveConfig":{"topSystemId":1},"optional_enabled":[],"artifacts":[{"id":"descriptor","path":"model.bin","sha256":digest(b"original")}],"claims":[]}
            self.assertTrue(validate_manifest(doc,directory)["valid"])
            with self.assertRaises(ToolError):validate_manifest(doc,directory,{})
            path.write_bytes(b"tampered")
            with self.assertRaises(ToolError):validate_manifest(doc,directory)

    def test_self_authored_kernel_report_is_not_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"certificate";path.write_bytes(b"not a proof")
            doc={"schema":"leanat.manifest.v1","profile":"AT-Core-1.1-draft","topSystemId":0,"optional_enabled":[],"effectiveConfig":{"topSystemId":0},"artifacts":[{"id":"certificate","path":"certificate","sha256":digest(path.read_bytes())}],"claims":[{"property":"everything","scope":"all","assumptions":[],"evidence":"proved","coveredArtifacts":{"certificate":digest(path.read_bytes())},"theorem":"Does.Not.Exist","axioms":[],"kernelRecord":{"axioms":["sorryAx"],"exitCode":0}}]}
            with self.assertRaises(ToolError) as raised:validate_manifest(doc,directory)
            self.assertEqual(raised.exception.code,"UnverifiedProofClaim")
            path.write_bytes(b"original");doc["claims"]=[{"property":"safe","scope":"all","assumptions":[],"evidence":"proved","coveredArtifacts":{}}]
            with self.assertRaises(ToolError):validate_manifest(doc,directory)


class CliMigrationTests(unittest.TestCase):
    def test_inventory_complete_honest(self):
        report=inventory()
        self.assertEqual(report["count"],598)
        self.assertTrue(all(row["status"]=="not-run" for row in report["entries"]))

    def test_all_migration_rules_source_preserved_and_stale_decisions(self):
        source="returnTransport cancelPending effective + returnedDelay outOfOrderBy PureFn.reifiable=True suspend modelDefined perModuleRuntime noMM unknownPhaseFatal extends releasePayload instrumentedSerialized verified"
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/"old.lean";path.write_text(source)
            report=migration_report(directory)
            self.assertEqual({row["rule"] for row in report["changes"]},{row[0] for row in RULES})
            self.assertEqual(path.read_text(),source)
            decisions={key:report[key] for key in ("reportHash","inputHashes","targetProfile")}
            decisions["choices"]={row["changeId"]:{"replacement":"explicit","rationale":"reviewed"} for row in report["changes"]}
            validate_decisions(report,decisions)
            path.write_text(source+" ")
            with self.assertRaises(ToolError):validate_decisions(migration_report(directory),decisions)

    def test_help_and_enums(self):
        args=parser().parse_args(["run-ref","Example","--transcript","x.json","--fuel","0"])
        self.assertEqual(args.fuel,0)
        with self.assertRaises(SystemExit):parser().parse_args(["trace","x","--kind","invalid"])

    def test_migration_changed_text_fixture_cannot_publish(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/"source";source.mkdir()
            (source/"Main.lean").write_text("def main := 0")
            fixture=source/"input.txt";fixture.write_text("before")
            report=migration_report(source)
            decisions={key:report[key] for key in ("reportHash","inputHashes","targetProfile")}
            decisions.update(choices={},entryModule="Main")
            def checker(staging,entry):
                fixture.write_text("after")
                return {"checked":True}
            with self.assertRaises(ToolError):apply_migration(source,root/"candidate",report,decisions,checker)
            self.assertFalse((root/"candidate").exists())


class BenchmarkTests(unittest.TestCase):
    def test_unrelated_success_output_is_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);output=root/"run.json";correctness=root/"correctness.json"
            fixture={"seed":"0","artifacts":{"descriptorHash":"descriptor"},"segment":{"programId":0,"inputs":[]}}
            state=[{"kind":"u64","value":"42"}]
            result={"state":state,"result":state,"complete":True,"stopReason":"Completed"}
            correctness.write_text(json.dumps({"status":"pass","descriptorHash":"descriptor","executions":{"cpp-runtime":{"result":result,"executableHash":digest(Path(sys.executable).read_bytes())}}}))
            unrelated={"trace":[{"kind":"commit","state":state},{"kind":"output","result":state}],"feedback":[],"complete":True,"stopReason":"Completed","observationEndpoint":"scalar-segment-return","dataMode":"FullValue","transcriptHash":"unrelated"}
            script="from pathlib import Path; Path("+repr(str(output))+").write_text("+repr(json.dumps(unrelated))+")"
            spec={"command":[sys.executable,"-c",script],"warmup":0,"samples":1,"timeoutSeconds":10,"seed":"0","input":fixture,"toolchainLock":{"version":"test"},"buildArtifact":sys.executable,"sampleStatePolicy":"ResetToFixture","successDefinition":"scalar-segment-completed-and-equal-v1","observationEndpoint":"scalar-segment-return","outputReport":str(output),"correctnessReport":str(correctness)}
            report=run_benchmark(spec)
            self.assertEqual(report["status"],"fail")
            self.assertEqual(report["samples"][0]["error"]["code"],"ConfigurationMismatch")


if __name__=="__main__":unittest.main()
