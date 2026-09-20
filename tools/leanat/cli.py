import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from . import __version__
from .io import ToolError, read_json, write_json, digest, canonical, unique_pairs, require
from .trace import read_transcript, compare_runs, validate_values
from .evidence import validate_manifest, release_gate
from .migration import migration_report, validate_decisions, apply_migration
from .benchmark import run_benchmark

ROOT=Path(__file__).resolve().parents[2]


def bounded_nat(text):
    if not text.isascii() or not text.isdecimal() or int(text)>2**64-1: raise argparse.ArgumentTypeError("expected finite UInt64 decimal")
    return int(text)


def parser():
    result=argparse.ArgumentParser(prog="leanat",description="LeanAT development compiler, full-value evidence and replay tools. Command success never implies release acceptance.")
    result.add_argument("--version",action="version",version=__version__)
    commands=result.add_subparsers(dest="command",required=True)
    for command in ("check","dump-model-ir","dump-exec-ir","emit","build","run-ref","test","diff"):
        p=commands.add_parser(command)
        p.add_argument("module",help="Lean module name or .lean source path")
        if command in {"check","emit","build","test","diff"}:
            p.add_argument("--managed-access",choices=("off","on"),default="off")
            p.add_argument("--dmi",choices=("disabled","raw-functional"),default="disabled")
        if command in {"emit","build"}: p.add_argument("-o","--out",required=True)
        if command=="dump-exec-ir": p.add_argument("--table",choices=("programs","types","stateTypes","initialState"))
        if command in {"run-ref","diff"}:
            p.add_argument("--transcript",required=True)
            p.add_argument("--fuel",required=True,type=bounded_nat)
            p.add_argument("--until",type=bounded_nat)
        if command=="diff":
            p.add_argument("--systemc",required=True)
            p.add_argument("--cpp-runtime",help="compiled leanat_descriptor executable (required if discovery is ambiguous)")
            p.add_argument("--backend-results",help="JSON object of actual backend result paths (four layers required)")
        if command=="test":
            p.add_argument("--suite",required=True,help="versioned fixture commands and exact assertions")
            p.add_argument("--report",required=True,help="write actual execution report")
    for command in ("inspect","inspect-evidence"):
        p=commands.add_parser(command);p.add_argument("manifest");p.add_argument("--host-policy")
    p=commands.add_parser("validate-descriptor");p.add_argument("descriptor");p.add_argument("--validator",help="compiled descriptor validator executable")
    for command in ("check-protocol","graph-protocol"):
        p=commands.add_parser(command);p.add_argument("protocol",help="versioned finite exchange JSON")
    p=commands.add_parser("trace");p.add_argument("file");p.add_argument("--kind",required=True,choices=("waits","managed","protocol"))
    p=commands.add_parser("migrate");p.add_argument("project");p.add_argument("--report",required=True);p.add_argument("--decisions");p.add_argument("--out")
    p=commands.add_parser("benchmark");p.add_argument("spec");p.add_argument("--report",required=True)
    p=commands.add_parser("inventory");p.add_argument("--out",required=True)
    for command in ("transport-capture", "transport-replay"):
        p=commands.add_parser(command, help="bounded real Runtime/SystemC base-protocol transport")
        p.add_argument("input",help="transport fixture or captured transcript JSON")
        p.add_argument("--cpp-runtime",required=True)
        p.add_argument("--systemc",required=True)
        p.add_argument("--out",required=True)
    p=commands.add_parser("opcode-native",help="execute a source catalog through native and SystemC providers; reference acceptance is separate")
    p.add_argument("catalog")
    p.add_argument("--cpp-runtime",required=True)
    p.add_argument("--systemc",required=True)
    p.add_argument("--out",required=True)
    return result


def invoke(argv, failure=3, timeout=120, cwd=ROOT):
    try:
        run=subprocess.run([str(x) for x in argv],cwd=cwd,stdin=subprocess.DEVNULL,timeout=timeout,shell=False)
    except FileNotFoundError as exc: raise ToolError("MissingTool",str(argv[0]),5) from exc
    except subprocess.TimeoutExpired as exc: raise ToolError("HostTimeout",str(argv[0]),5) from exc
    if run.returncode: raise ToolError("SubprocessFailed",json.dumps({"command":[str(x) for x in argv],"exitCode":run.returncode}),failure)


def lean_command(module, mode, extra=(), cwd=ROOT):
    invoke(lean_argv(module,mode,extra,cwd),cwd=cwd)


def lean_argv(module, mode, extra=(), cwd=ROOT):
    path=Path(module)
    if path.suffix!=".lean":
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*",module): raise ToolError("InvalidModule",module)
        path=Path(cwd) / (module.replace(".","/")+".lean")
    path=path.resolve()
    if not path.is_file(): raise ToolError("MissingModule",str(path))
    lake=shutil.which("lake")
    if not lake:
        candidates=sorted((ROOT/".deps").glob("lean-*/bin/lake.exe"))
        if not candidates: raise ToolError("MissingTool","lake",5)
        lake=str(candidates[-1])
    return [lake,"env","lean","--run",str(path),mode,*extra]


def capture(argv, timeout=120, max_bytes=8_388_608, parse_json=True, cwd=ROOT):
    """Drain both pipes concurrently with hard byte bounds and a finite process timeout."""
    try: process=subprocess.Popen([str(x) for x in argv],cwd=cwd,stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE,shell=False)
    except OSError as exc: raise ToolError("MissingTool",str(exc),5) from exc
    buffers=[bytearray(),bytearray()];overflow=[]
    def drain(stream,index):
        while True:
            block=stream.read(4096)
            if not block:break
            if len(buffers[index])+len(block)>max_bytes:
                overflow.append(index);process.kill();break
            buffers[index].extend(block)
        stream.close()
    readers=[threading.Thread(target=drain,args=(stream,index),daemon=True) for index,stream in enumerate((process.stdout,process.stderr))]
    for thread in readers:thread.start()
    try: code=process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill();process.wait();raise ToolError("HostTimeout",str(argv[0]),5)
    finally:
        for thread in readers:thread.join(timeout=5)
    if overflow:raise ToolError("OutputLimit","backend output exceeded host byte limit",5)
    stdout,stderr=(bytes(x).decode("utf-8") for x in buffers)
    if not parse_json:return stdout,{"command":[str(x) for x in argv],"exitCode":code,"stderr":stderr,"stdout":stdout}
    try: result=json.loads(stdout,object_pairs_hook=unique_pairs)
    except (ValueError,UnicodeDecodeError) as exc: raise ToolError("BackendOutput",json.dumps({"command":[str(x) for x in argv],"exitCode":code,"stderr":stderr,"stdout":stdout[:2000]}),4) from exc
    return result,{"command":[str(x) for x in argv],"exitCode":code,"stderr":stderr,"stdout":stdout}


def scalar_plan(transcript):
    require(transcript,("segment",),"scalar transcript")
    plan=transcript["segment"]
    require(plan,("programId","inputs","scope"),"segment")
    if plan["scope"]!="scalar-segment" or transcript["records"] or plan["inputs"]: raise ToolError("UnsupportedReplayScope","current live four-backend harness accepts a closed, no-input scalar segment only",5)
    if type(plan["programId"]) is not int or not 0<=plan["programId"]<=2**32-1: raise ToolError("InvalidProgram","programId")
    if transcript["profile"]!="AT-Core-1.1-draft" or transcript["effectiveConfig"].get("scope")!="scalar-segment":raise ToolError("ConfigurationMismatch","scalar profile/scope")
    require(transcript["artifacts"],("descriptorHash",),"scalar artifact binding")
    return plan


def scalar_diff(args,transcript):
    plan=scalar_plan(transcript)
    from .toolchain import select_toolchain_lock
    from .artifacts import validate_emitted_package
    toolchain_lock,toolchain_selection=select_toolchain_lock(ROOT)
    if canonical(transcript["toolchainLock"])!=canonical(toolchain_lock):raise ToolError("ToolchainLockMismatch","transcript lock differs from the current host-specific lock",4)
    if args.until is not None:raise ToolError("UnsupportedReplayScope","scalar segment has no timed horizon",5)
    scratch=ROOT/".tmp/tools";scratch.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="diff-",dir=scratch) as directory:
        output=Path(directory)/"generated"
        _,emit_execution=capture(lean_argv(args.module,"emit",[str(output)]))
        if emit_execution["exitCode"]!=0:raise ToolError("EmitFailed",str(emit_execution),3)
        package_hash=digest(canonical({path.relative_to(output).as_posix():digest(path.read_bytes()) for path in sorted(output.rglob("*")) if path.is_file()}))
        retained=ROOT/"build/integration/tool-artifacts"/package_hash
        if not retained.exists():retained.parent.mkdir(parents=True,exist_ok=True);output.rename(retained)
        elif digest(canonical({path.relative_to(retained).as_posix():digest(path.read_bytes()) for path in sorted(retained.rglob("*")) if path.is_file()}))!=package_hash:raise ToolError("HashMismatch","retained emitted package was modified",4)
        descriptor=retained/"model.execir.bin"
        package_validation=validate_emitted_package(retained)
        for field in ("sourceMapHash","packageHash"):
            if field in transcript["artifacts"] and transcript["artifacts"][field]!=package_validation[field]:raise ToolError("HashMismatch","transcript "+field+" differs from fresh emitted package",4)
        emit_execution["publishedArtifactDirectory"]=str(retained)
        candidates=[Path(args.cpp_runtime)] if args.cpp_runtime else list((ROOT/"build").glob("**/leanat_descriptor.exe"))+list((ROOT/"build").glob("**/leanat_descriptor"))
        if len(candidates)!=1:raise ToolError("MissingTool","compiled leanat_descriptor missing or ambiguous",5)
        if digest(descriptor.read_bytes())!=transcript["artifacts"]["descriptorHash"]:raise ToolError("HashMismatch","transcript is bound to a different descriptor",4)
        initial,initial_execution=capture([candidates[0],"--run",descriptor,str(plan["programId"]),"0"])
        if initial["state"]!=transcript["initialState"] or initial["stopReason"]!="FuelExhausted":raise ToolError("InitialStateMismatch","transcript initial fixture differs from descriptor",4)
        commands={"modelir-lean":lean_argv(args.module,"run-model-segment",["--program",str(plan["programId"]),"--fuel",str(args.fuel)]),"execir-lean":lean_argv(args.module,"run-exec-segment",["--program",str(plan["programId"]),"--fuel",str(args.fuel)]),"cpp-runtime":[candidates[0],"--run",descriptor,str(plan["programId"]),str(args.fuel)],"systemc":[args.systemc,"--run",descriptor,str(plan["programId"]),str(args.fuel)]}
        runs={};executions={}
        for name,command in commands.items():
            executable_hash=digest(Path(command[0]).read_bytes())
            result,execution=capture(command)
            if digest(Path(command[0]).read_bytes())!=executable_hash:raise ToolError("ArtifactChanged","backend executable changed during run",4)
            executions[name]=dict(execution,result=result,executableHash=executable_hash,fuelUnit="model-statement-v1" if name=="modelir-lean" else "exec-instruction-and-terminator-v1")
            require(result,("result","state","complete","stopReason"),name)
            runs[name]={"trace":[{"kind":"commit","state":result["state"]},{"kind":"output","result":result["result"]}],"feedback":[],"stopReason":result["stopReason"],"complete":result["complete"] and execution["exitCode"]==0,"dataMode":"FullValue","transcriptHash":digest(canonical(transcript)),"observationEndpoint":"scalar-segment-return"}
        report=compare_runs(runs,commands)
        report.update(toolchainSelection=toolchain_selection,generatedPackageValidation=package_validation)
        report.update(scope="scalar-segment full result and committed state",executions=executions,emit=emit_execution,initialStateValidation=initial_execution,descriptorHash=digest(descriptor.read_bytes()),transcript=transcript,limitations=["no transport/callback environment","SystemC executes the same C++ interpreter inside its kernel; this is not an independent algorithm oracle","fuel units differ across ModelIR and ExecIR","not full profile conformance"])
        return report


def protocol_graph(doc):
    from .io import require
    require(doc,("schema","states","initial","exchanges"),"protocol")
    if doc["schema"]!="leanat.protocol.v1" or not isinstance(doc["states"],list) or len(doc["states"])>4096 or len(doc["exchanges"])>65536: raise ToolError("SchemaMismatch","protocol bounds/schema")
    states=set(doc["states"])
    if len(states)!=len(doc["states"]) or doc["initial"] not in states: raise ToolError("InvalidProtocol","states")
    seen=set();reachable={doc["initial"]};terminal=set()
    for row in doc["exchanges"]:
        require(row,("pre","post","callFlow","callPhase","sync","returnPhase","laneActions","terminal"),"exchange")
        if row["pre"] not in states or row["post"] not in states: raise ToolError("InvalidProtocol","unknown state")
        key=tuple(row[k] for k in ("pre","callFlow","callPhase","sync","returnPhase"))
        if key in seen: raise ToolError("GuardConflict","duplicate unconditional exchange")
        seen.add(key)
        if row["terminal"]: terminal.add(row["post"])
    for _ in states:
        for row in doc["exchanges"]:
            if row["pre"] in reachable: reachable.add(row["post"])
    diagnostics=[]
    if states-reachable: diagnostics.append({"code":"Unreachable","states":sorted(states-reachable)})
    backwards=set(terminal)
    for _ in states:
        for row in doc["exchanges"]:
            if row["post"] in backwards: backwards.add(row["pre"])
    if reachable-backwards: diagnostics.append({"code":"NoTerminalPath","states":sorted(reachable-backwards)})
    return {"exchanges":doc["exchanges"],"diagnostics":diagnostics,"scope":"finite graph reachability and unconditional duplicate exchange checks","notChecked":["guard semantics","lane leak","cancel-drain closure"]}


def inventory():
    source = "tests/conformance/requirements.json"
    definitions = read_json(ROOT / source)
    if definitions.get("schema") != "leanat.requirements.v1":
        raise ToolError("InvalidRegressionInventory", "unsupported requirement catalog")
    entries = [dict(row, source=source, status="not-run", testIds=[], evidence=[])
               for row in definitions["entries"]]
    ids = [row["id"] for row in entries]
    if len(ids) != 598 or len(ids) != len(set(ids)):
        raise ToolError("DuplicateAcceptance", "acceptance inventory missing or duplicated")
    regressions = [dict(id=row["id"], scenario=row["scenario"], acceptance=row["requirement"],
                        source=source, status="not-run", testIds=[], evidence=[])
                   for row in definitions["regressions"]]
    expected = {f"C-T{i:02}" for i in range(1,31)} | {f"E-T{i:02}" for i in range(1,41)}
    if len(regressions) != 70 or {r["id"] for r in regressions} != expected:
        raise ToolError("InvalidRegressionInventory", "original C/E regression IDs missing or duplicated")
    return {"schema":"leanat.acceptance-inventory.v1", "kind":"requirement-inventory",
            "count":len(entries), "release_gate":"not-evaluated", "entries":entries,
            "originalRegressions":regressions}


def _dispatch(args):
    command=args.command
    if getattr(args,"managed_access","off")!="off" or getattr(args,"dmi","disabled")!="disabled": raise ToolError("UnsupportedCapability","compiler optional capability plumbing is not available; no silent downgrade",5)
    if command in {"inspect","inspect-evidence"}:
        manifest=read_json(args.manifest)
        validation=validate_manifest(manifest,Path(args.manifest).resolve().parent,read_json(args.host_policy) if args.host_policy else None)
        return {"manifest":manifest,"validation":validation,"gate":release_gate(manifest,True,args.host_policy is not None)}
    if command=="inventory":
        report=inventory();write_json(args.out,report);return {"count":report["count"],"output":str(Path(args.out).resolve())}
    if command=="trace":
        document=read_json(args.file)
        validate_values(document)
        kinds={"waits":{"wait","task","scope","drain"},"managed":{"managed"},"protocol":{"wireCall","wireReturn","milestone"}}[args.kind]
        return {"kind":"filtered-view","sourcePreserved":True,"events":[row for row in document["trace"] if row.get("kind") in kinds]}
    if command in {"check-protocol","graph-protocol"}:
        report=protocol_graph(read_json(args.protocol))
        if command=="check-protocol" and report["diagnostics"]: raise ToolError("InvalidProtocol",json.dumps(report),3)
        return report
    if command=="migrate":
        report=migration_report(args.project);write_json(args.report,report)
        if bool(args.decisions)!=bool(args.out): raise ToolError("InvalidArguments","--decisions and --out must be provided together")
        if args.decisions:
            def checker(staging,entry):
                lake=lean_argv(entry,"check",cwd=staging)[0]
                _,build=capture([lake,"build"],parse_json=False,cwd=staging)
                if build["exitCode"]:raise ToolError("LeanBuildFailed",json.dumps(build),3)
                _,checked=capture(lean_argv(entry,"check",cwd=staging),cwd=staging)
                if checked["exitCode"]:raise ToolError("MigrationCheckFailed",json.dumps(checked),3)
                _,emitted=capture(lean_argv(entry,"emit",[str(staging/"generated")],cwd=staging),cwd=staging)
                if emitted["exitCode"]:raise ToolError("MigrationEmitFailed",json.dumps(emitted),3)
                generated=staging/"generated/manifest.json"
                if not generated.exists(): raise ToolError("MissingArtifact","migration emit did not produce manifest",3)
                validation=validate_manifest(read_json(generated),generated.parent)
                return {"leanBuild":build,"check":checked,"emit":emitted,"manifest":validation,"cppBuild":"not-run","conformance":"not-run"}
            return apply_migration(args.project,args.out,report,read_json(args.decisions),checker)
        return report
    if command=="benchmark":
        report=run_benchmark(read_json(args.spec));write_json(args.report,report)
        if report["status"]!="pass": raise ToolError("BenchmarkFailed","raw failure samples retained in report",4)
        return report
    if command=="validate-descriptor":
        validator=args.validator
        if not validator:
            paths=list((ROOT/"build").glob("**/leanat_descriptor.exe"))+list((ROOT/"build").glob("**/leanat_descriptor"))
            if len(paths)!=1: raise ToolError("MissingTool","provide --validator compiled descriptor validator (default missing or ambiguous)",5)
            validator=paths[0]
        invoke([validator,args.descriptor],3);return None
    if command=="test":
        suite=read_json(args.suite)
        require(suite,("schema","module","fixtures","oracle"),"suite")
        if suite["schema"]!="leanat.suite.v1" or suite["module"]!=args.module or not suite["oracle"]:raise ToolError("SuiteMismatch","module/schema/oracle")
        fixtures=suite["fixtures"]
        if not isinstance(fixtures,list) or not 0<len(fixtures)<=10000:raise ToolError("InvalidSuite","finite nonempty fixtures required")
        ids=set()
        for row in fixtures:
            require(row,("id","command","expectedExit","expectedJSON","requirement"),"fixture")
            if row["id"] in ids or not row["expectedJSON"] or not row["requirement"]:raise ToolError("InvalidSuite","duplicate ID or empty assertions")
            if not isinstance(row["command"],list) or not row["command"] or any(not isinstance(x,str) for x in row["command"]):raise ToolError("InvalidSuite","command must be argument array")
            ids.add(row["id"])
        rows=[]
        for row in fixtures:
            try:
                substitutions={"{python}":sys.executable,"{root}":str(ROOT)}
                if any("{lake}" in arg for arg in row["command"]):substitutions["{lake}"]=lean_argv(args.module,"check")[0]
                command=[]
                for argument in row["command"]:
                    for token,value in substitutions.items():argument=argument.replace(token,value)
                    command.append(argument)
                actual,execution=capture(command)
                status="pass" if actual==row["expectedJSON"] and execution["exitCode"]==row["expectedExit"] else "fail"
                rows.append(dict(row,status=status,actual=actual,execution=execution,evidenceKind="implementation-test"))
            except ToolError as exc:rows.append(dict(row,status="fail",error={"code":exc.code,"message":str(exc)},evidenceKind="implementation-test"))
        report={"schema":"leanat.suite-report.v1","module":args.module,"oracle":suite["oracle"],"suiteHash":digest(canonical(suite)),"tests":rows,"status":"pass" if all(row["status"]=="pass" for row in rows) else "fail","comparisonScope":"selected exact-output fixtures","release_gate":"not-evaluated"}
        write_json(args.report,report)
        if report["status"]!="pass":raise ToolError("SuiteFailed","actual failures saved in report",4)
        return report
    if command in {"run-ref","diff"}:
        transcript=read_transcript(args.transcript)
        if command=="diff":
            if not args.backend_results:return scalar_diff(args,transcript)
            files=read_json(args.backend_results)
            runs={key:read_json(path) for key,path in files.items()}
            expected_hash=digest(canonical(transcript))
            feedback=[record for record in transcript["records"] if record["recordKind"]=="return"]
            for name,run in runs.items():
                if run.get("transcriptHash")!=expected_hash or run.get("feedback")!=feedback:raise ToolError("ReplayInputMismatch",name+" does not match the supplied complete environment",4)
            report=compare_runs(runs,("modelir-lean","execir-lean","cpp-runtime","systemc"))
            report["method"]="compare-supplied-results; backends were not executed by this command"
            return report
        plan=scalar_plan(transcript)
        if args.until is not None:raise ToolError("UnsupportedReplayScope","scalar segment has no timed horizon",5)
        scratch=ROOT/".tmp/tools";scratch.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="reference-",dir=scratch) as directory:
            output=Path(directory)/"generated"
            _,emission=capture(lean_argv(args.module,"emit",[str(output)]))
            if emission["exitCode"] or digest((output/"model.execir.bin").read_bytes())!=transcript["artifacts"]["descriptorHash"]:raise ToolError("HashMismatch","reference model differs from transcript descriptor",4)
        initial,_=capture(lean_argv(args.module,"run-model-segment",["--program",str(plan["programId"]),"--fuel","0"]))
        if initial["state"]!=transcript["initialState"]:raise ToolError("InitialStateMismatch","reference initial state differs from transcript",4)
        result,execution=capture(lean_argv(args.module,"run-model-segment",["--program",str(plan["programId"]),"--fuel",str(args.fuel)]))
        return {"result":result,"execution":execution,"scope":"scalar-segment","release_gate":"not-evaluated"}
    if command=="build":
        lean_command(args.module,"emit",[str(Path(args.out).resolve())])
        invoke(["cmake","-S",str(Path(args.out).resolve()),"-B",str(Path(args.out).resolve()/"build")])
        invoke(["cmake","--build",str(Path(args.out).resolve()/"build")])
        return {"status":"built","tests":"not-run","release_gate":"not-evaluated"}
    extra=[]
    if command=="emit": extra=[str(Path(args.out).resolve())]
    if command=="dump-exec-ir" and args.table: extra=["--table",args.table]
    lean_command(args.module,command,extra)
    return None


def dispatch(args):
    if args.command=="opcode-native":
        from .opcode_replay import execute_catalog_native
        return execute_catalog_native(args.catalog,args.cpp_runtime,args.systemc,args.out,ROOT)
    if args.command in {"transport-capture", "transport-replay"}:
        from .transport_replay import capture_transport, replay_transport
        action=capture_transport if args.command=="transport-capture" else replay_transport
        return action(read_json(args.input),args.cpp_runtime,args.systemc,args.out)
    if getattr(args,"managed_access","off")!="off" or getattr(args,"dmi","disabled")!="disabled":raise ToolError("UnsupportedCapability","compiler optional capability plumbing is not available; no silent downgrade",5)
    build_record=None
    if args.command in {"check","dump-model-ir","dump-exec-ir","emit","build","run-ref","diff"}:
        command=lean_argv(args.module,"check")
        _,build_record=capture([command[0],"build"],parse_json=False)
        if build_record["exitCode"]:raise ToolError("LeanBuildFailed",json.dumps(build_record),3)
    result=_dispatch(args)
    if build_record is not None:
        record_path=ROOT/"build/tool-reports/last-lean-build.json"
        write_json(record_path,build_record)
        if isinstance(result,dict):result["leanBuild"]=build_record
    return result


def main(argv=None):
    try:
        value=dispatch(parser().parse_args(argv))
        if value is not None: print(json.dumps(value,ensure_ascii=False,indent=2))
        return 0
    except ToolError as exc:
        print(json.dumps({"error":exc.code,"message":str(exc)},ensure_ascii=False),file=sys.stderr)
        return exc.exit_code
    except (OSError,ValueError,KeyError,TypeError) as exc:
        print(json.dumps({"error":"ToolInputError","message":str(exc)},ensure_ascii=False),file=sys.stderr)
        return 2
