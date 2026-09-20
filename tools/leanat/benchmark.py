import platform
import statistics
import subprocess
import time
import sys
from pathlib import Path
from .io import ToolError, digest, require, read_json, canonical
from .trace import compare_runs


def run_benchmark(spec):
    require(spec,("command","warmup","samples","timeoutSeconds","seed","input","toolchainLock","buildArtifact","sampleStatePolicy","successDefinition","observationEndpoint","outputReport","correctnessReport"),"benchmark")
    if spec["sampleStatePolicy"]!="ResetToFixture": raise ToolError("UnsupportedStatePolicy","only fresh-process ResetToFixture is implemented")
    if spec["successDefinition"]!="scalar-segment-completed-and-equal-v1" or spec["seed"]!="0" or spec["input"].get("seed")!="0":raise ToolError("UnsupportedBenchmarkScope","only the fixed closed scalar completion definition and deterministic seed0 are supported")
    if type(spec["samples"]) is not int or not 1<=spec["samples"]<=10000 or type(spec["warmup"]) is not int or not 0<=spec["warmup"]<=10000: raise ToolError("InvalidBudget","sample/warmup bounds")
    command=spec["command"]
    if not isinstance(command,list) or not command or any(not isinstance(x,str) for x in command): raise ToolError("InvalidCommand","command must be argument array")
    command=[sys.executable if argument=="{python}" else argument for argument in command]
    if not 0<spec["timeoutSeconds"]<=3600: raise ToolError("InvalidBudget","timeoutSeconds")
    artifact=Path(spec["buildArtifact"])
    build_hash=digest(artifact.read_bytes())
    correctness=read_json(spec["correctnessReport"])
    if correctness.get("status")!="pass" or correctness.get("descriptorHash")!=spec["input"].get("artifacts",{}).get("descriptorHash"):raise ToolError("SemanticMismatch","benchmark correctness report must match the input descriptor")
    expected=correctness.get("executions",{}).get("cpp-runtime",{}).get("result")
    if not expected or expected.get("complete") is not True:raise ToolError("IncompleteRun","missing completed correctness baseline")
    if correctness.get("executions",{}).get("cpp-runtime",{}).get("executableHash")!=build_hash:raise ToolError("ConfigurationMismatch","benchmark binary differs from correctness run")
    input_hash=digest(canonical({"descriptorHash":correctness["descriptorHash"],"programId":spec["input"]["segment"]["programId"],"inputs":spec["input"]["segment"]["inputs"]}))
    rows=[]
    baseline={"trace":[{"kind":"commit","state":expected["state"]},{"kind":"output","result":expected["result"]}],"feedback":[],"complete":True,"stopReason":"Completed","observationEndpoint":spec["observationEndpoint"],"dataMode":"FullValue","transcriptHash":input_hash}
    for index in range(spec["warmup"]+spec["samples"]):
        output=Path(spec["outputReport"])
        if output.exists(): raise ToolError("OutputExists","benchmark outputReport must be absent before each run")
        start=time.perf_counter_ns()
        row={"index":index,"warmup":index<spec["warmup"],"wallNanoseconds":"0","exitCode":None,"run":None,"status":"fail"}
        try:
            if digest(artifact.read_bytes())!=build_hash:raise ToolError("ConfigurationMismatch","build artifact changed between samples")
            completed=subprocess.run(command,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=spec["timeoutSeconds"],shell=False)
            elapsed=time.perf_counter_ns()-start
            run=read_json(output) if output.exists() else None
            row={"index":index,"warmup":index<spec["warmup"],"wallNanoseconds":str(elapsed),"exitCode":completed.returncode,"run":run,"status":"fail"}
            if completed.returncode==0 and run:
                if run.get("benchmarkInput")!=spec["input"] or run.get("seed")!=spec["seed"] or run.get("successDefinition")!=spec["successDefinition"] or run.get("buildHash")!=build_hash:raise ToolError("ConfigurationMismatch","measured run does not bind input, seed, success definition and binary")
                if run.get("observationEndpoint")!=spec["observationEndpoint"]: raise ToolError("SemanticMismatch","benchmark observation endpoint")
                compare_runs({"baseline":baseline,"sample":run})
                row["status"]="pass"
        except subprocess.TimeoutExpired:
            row={"index":index,"warmup":index<spec["warmup"],"wallNanoseconds":str(time.perf_counter_ns()-start),"exitCode":None,"run":None,"status":"timeout"}
        except ToolError as exc:
            row["wallNanoseconds"]=str(time.perf_counter_ns()-start)
            row["error"]={"code":exc.code,"message":str(exc)}
        except OSError as exc:
            row["wallNanoseconds"]=str(time.perf_counter_ns()-start)
            row["error"]={"code":"BackendLaunchFailed","message":str(exc)}
        row["command"]=command
        rows.append(row)
        if output.exists(): output.unlink()
    measured=[row for row in rows if not row["warmup"]]
    valid=all(row["status"]=="pass" for row in rows)
    wall=[int(row["wallNanoseconds"]) for row in measured]
    return {"schema":"leanat.benchmark.v1","status":"pass" if valid else "fail","spec":spec,"buildHash":build_hash,"correctnessReportHash":digest(Path(spec["correctnessReport"]).read_bytes()),"host":{"os":platform.platform(),"cpu":platform.processor(),"python":platform.python_version(),"timer":"perf_counter_ns","timerResolutionSeconds":time.get_clock_info("perf_counter").resolution},"samples":rows,"statistics":{"method":"arithmetic mean and sample standard deviation; no confidence interval","meanWallNanoseconds":statistics.mean(wall),"sampleStddevNanoseconds":statistics.stdev(wall) if len(wall)>1 else None,"insufficientSamples":len(wall)<2,"failedSamplesIncluded":True},"unmeasured":["child CPU","peak memory","runtime counters unless supplied by backend"],"release_gate":"not-evaluated"}
