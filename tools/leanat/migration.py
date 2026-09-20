import re
import shutil
import tempfile
from pathlib import Path
from .io import ToolError, canonical, digest, write_json, read_json

RULES = [
 ("timed-return",r"returnTransport","Use transport entry or deferred response; future work cannot return synchronously"),
 ("cancel-free",r"cancelPending","Use abortLocalAndDrain/drainThenReset; preserve wire responsibility"),
 ("delay-base",r"effective\s*\+\s*returnedDelay","Compute callTime + full returnedDelay"),
 ("ordering",r"outOfOrderBy","Use readyOrder and optional businessKey"),
 ("reification",r"(?:PureFn\.)?reifiable\s*[=:]\s*(?:True|true)","Provide reification and checked semantic evidence"),
 ("suspend",r"\b(?:suspend|StmtIR|Opcode|WG_SUSPEND)\b","Use MStmtF/Op and Suspend terminator"),
 ("debug",r"modelDefined","Use untimed peek/poke without business effects"),
 ("domain",r"(?:perModuleRuntime|independentRuntime|独立\s*runtime)","Share Top RuntimeDomain or declare explicit domain boundaries"),
 ("no-mm",r"(?:noMM|withoutMM|无\s*MM)","Reject nb without MM; blocking uses fixed bridge"),
 ("ignorable",r"(?:unknownPhaseFatal|ignorable.*fatal)","TlmBase unknown ignorable phase: ACCEPTED+ignore without forwarding"),
 ("protocol",r"\b(?:extends|transition)\b","Declare builtin/ignorable/standalone and full call-return exchange"),
 ("release",r"releasePayload","Runtime owns payload and last-pin release"),
 ("dmi",r"instrumentedSerialized","Choose rawFunctional or private managedTimed explicitly"),
 ("evidence",r"\bverified\b|reference\s*\+\s*(?:tests?|测试)","Separate proved/tested/monitored/assumed; no inherited proof status"),
]


def migration_report(project, profile="AT-Core-1.1-draft"):
    project=Path(project).resolve()
    if not project.exists(): raise ToolError("MissingSource",str(project))
    paths=[project] if project.is_file() else sorted(p for p in project.rglob("*") if p.is_file() and not any(part in {".git","build",".lake",".deps",".tmp"} for part in p.relative_to(project).parts))
    if len(paths)>10000: raise ToolError("ReadLimit","migration source count")
    hashes,changes={},[]
    for path in paths:
        if path.stat().st_size>8_388_608: raise ToolError("ReadLimit",str(path))
        raw=path.read_bytes()
        name=path.name if project.is_file() else path.relative_to(project).as_posix()
        hashes[name]=digest(raw)
        if path.suffix not in {".lean",".toml",".json",".cpp",".hpp"}:continue
        try:source=raw.decode("utf-8")
        except UnicodeDecodeError:continue
        for rule,pattern,contract in RULES:
            for match in re.finditer(pattern,source):
                prefix=source[:match.start()]
                span={"path":name,"hash":hashes[name],"startByte":len(prefix.encode("utf-8")),"endByte":len(source[:match.end()].encode("utf-8")),"line":prefix.count("\n")+1,"column":len(prefix.rsplit("\n",1)[-1])+1}
                changes.append({"changeId":digest(canonical([name,rule,span["startByte"]]))[:24],"rule":rule,"sourceSpan":span,"old":match.group(),"newContract":contract,"semanticChange":True,"status":"decision-required"})
        if path.name.endswith("manifest.json"):
            changes.append({"changeId":digest(canonical([name,"old-manifest"]))[:24],"rule":"old-manifest","sourceSpan":{"path":name,"hash":hashes[name]},"old":"manifest evidence","newContract":"Rebuild manifest; discard inherited execution/proof statuses","semanticChange":True,"status":"decision-required"})
    report={"schema":"leanat.migration.v1","ruleVersion":"1","targetProfile":profile,"inputHashes":hashes,"changes":changes,"checkStatus":"not-run","sourcePreserved":True}
    report["reportHash"]=digest(canonical(report))
    return report


def validate_decisions(report, decisions):
    if any(decisions.get(key)!=report[key] for key in ("reportHash","inputHashes","targetProfile")): raise ToolError("StaleMigrationDecision","source/report/profile differs")
    expected={row["changeId"] for row in report["changes"]}
    choices=decisions.get("choices",{})
    if set(choices)!=expected or any(not isinstance(value,dict) or not value.get("replacement") or not value.get("rationale") for value in choices.values()): raise ToolError("UnresolvedMigration","each change requires explicit replacement and rationale")
    return choices


def apply_migration(project, output, report, decisions, checker):
    """Build and check an isolated candidate, then commit only the complete package."""
    project,output=Path(project).resolve(),Path(output).resolve()
    if not project.is_dir(): raise ToolError("MissingSource","migration application needs full project directory")
    if project.is_relative_to(output) or output.is_relative_to(project): raise ToolError("InvalidOutput","source and output must not contain one another")
    if output.exists(): raise ToolError("OutputExists","candidate output must not already exist")
    choices=validate_decisions(report,decisions)
    if not decisions.get("entryModule"): raise ToolError("MissingEntryModule","decisions must name a candidate Lean entryModule")
    output.parent.mkdir(parents=True,exist_ok=True)
    staging=Path(tempfile.mkdtemp(prefix=".leanat-migration-",dir=output.parent)).resolve()
    try:
        for source in project.rglob("*"):
            if source.is_symlink(): raise ToolError("UnsupportedSource","symlink in migration project")
            relative=source.relative_to(project)
            if any(part in {".git","build",".lake",".tmp",".deps"} for part in relative.parts): continue
            target=staging/relative
            if source.is_dir(): target.mkdir(parents=True,exist_ok=True)
            elif source.is_file():
                if source.stat().st_size>67_108_864: raise ToolError("ReadLimit",str(relative))
                target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        patches=[]
        by_file={}
        for change in report["changes"]:
            if change["rule"]=="old-manifest": continue
            by_file.setdefault(change["sourceSpan"]["path"],[]).append(change)
        for name,changes in by_file.items():
            raw=(staging/name).read_bytes()
            previous=len(raw)+1
            for change in sorted(changes,key=lambda x:x["sourceSpan"]["startByte"],reverse=True):
                start,end=change["sourceSpan"]["startByte"],change["sourceSpan"]["endByte"]
                if end>previous: raise ToolError("OverlappingMigration","overlapping decisions require a new source-level plan")
                previous=start
                replacement=choices[change["changeId"]]["replacement"].encode("utf-8")
                raw=raw[:start]+replacement+raw[end:]
                patches.append({"changeId":change["changeId"],"sourceSpan":change["sourceSpan"],"old":change["old"],"replacement":replacement.decode("utf-8")})
            (staging/name).write_bytes(raw)
        for path in staging.rglob("*manifest.json"):
            doc=read_json(path)
            doc["claims"]=[];doc["tests"]=[];doc["gates"]={};doc["execution_status"]="not-run";doc["release_gate"]="not-evaluated"
            write_json(path,doc)
        checks=checker(staging,decisions["entryModule"])
        applied=dict(report,checkStatus="checked",validation=checks,execution_status="not-run",release_gate="not-evaluated")
        write_json(staging/"migration-report.json",applied)
        write_json(staging/"applied-decisions.json",decisions)
        write_json(staging/"migration-patches.json",patches)
        write_json(staging/"migration-source-map.json",{"inputHashes":report["inputHashes"],"changes":patches})
        # Re-read originals immediately before publish, preventing stale decisions during validation.
        if migration_report(project,report["targetProfile"])["reportHash"]!=report["reportHash"]: raise ToolError("StaleMigrationDecision","source changed during candidate checking")
        staging.rename(output)
        return applied
    finally:
        if staging.exists() and staging.parent==output.parent and staging.name.startswith(".leanat-migration-"):
            shutil.rmtree(staging)
