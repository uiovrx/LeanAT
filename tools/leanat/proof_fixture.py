"""Reproduce positive and precise negative actual-kernel evidence checks."""
import argparse
import copy
import shutil
from pathlib import Path
from .proof import ROOT,run_proof_audit
from .io import read_json,write_json,digest,ToolError
from .evidence import validate_manifest


def create_fixture(output,report_path):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    audit=run_proof_audit("LeanAT.Proofs","LeanAT.Proofs.transportSafety")
    actual=audit["actual"]
    shutil.copy2(ROOT/"LeanAT/Proofs.lean",output/"Proofs.lean")
    shutil.copy2(ROOT/".lake/build/lib/lean/LeanAT/Proofs.olean",output/"Proofs.olean")
    write_json(output/"kernel-record.json",actual)
    artifacts=[{"id":identity,"path":path,"sha256":digest((output/path).read_bytes())} for identity,path in (("module","Proofs.lean"),("certificate","Proofs.olean"),("kernel","kernel-record.json"))]
    claim={"property":actual["theorem"],"scope":actual["theoremType"],"assumptions":[],"evidence":"proved","coveredArtifacts":{a["id"]:a["sha256"] for a in artifacts},"module":"LeanAT.Proofs","theorem":actual["theorem"],"moduleArtifact":"module","certificateArtifact":"certificate","kernelRecord":"kernel","leanVersion":actual["leanVersion"],"semanticsVersion":"LeanAT.source-set.v1","semanticsHash":audit["semanticsHash"],"axioms":actual["axioms"]}
    manifest={"schema":"leanat.manifest.v1","profile":"AT-Core-1.1-draft","topSystemId":0,"optional_enabled":[],"artifacts":artifacts,"effectiveConfig":{"topSystemId":0},"claims":[claim]}
    write_json(output/"manifest.json",manifest)
    validation=validate_manifest(manifest,output)
    rows=[{"case":"known-theorem","status":"pass","audit":validation["proofAudits"][0]}]
    from .cli import capture,lean_argv
    negative_source=output/"sorry-negative.lean"
    negative_source.write_text("import LeanAT.ModelIR.ProofAuditCommand\ntheorem leanatRejectedSorry : False := by sorry\n#leanat_audit leanatRejectedSorry\n",encoding="utf-8")
    lake=lean_argv("LeanAT.Proofs","check")[0]
    stdout,execution=capture([lake,"env","lean",str(negative_source.resolve())],parse_json=False)
    expected_message="Proof audit rejected sorryAx dependency"
    rows.append({"case":"actual-sorry-theorem","status":"pass" if execution["exitCode"]==1 and expected_message in stdout else "fail","expectedExit":1,"expectedDiagnostic":expected_message,"execution":execution})
    for case,expected in (("unknown-theorem","KernelAuditFailed"),("forged-axiom-record","KernelRecordMismatch"),("scope-overclaim","ProofScopeMismatch")):
        bad=copy.deepcopy(manifest)
        if case=="unknown-theorem":bad["claims"][0]["theorem"]=bad["claims"][0]["property"]="LeanAT.Proofs.DoesNotExist"
        elif case=="scope-overclaim":bad["claims"][0]["scope"]="all C++ and SystemC programs are safe"
        else:
            record=copy.deepcopy(actual);record["axioms"]=["sorryAx"];write_json(output/"forged-kernel.json",record)
            bad["artifacts"][2]["path"]="forged-kernel.json";bad["artifacts"][2]["sha256"]=digest((output/"forged-kernel.json").read_bytes());bad["claims"][0]["coveredArtifacts"]["kernel"]=bad["artifacts"][2]["sha256"]
        try:
            validate_manifest(bad,output)
            rows.append({"case":case,"status":"fail","expectedError":expected,"actualError":None})
        except ToolError as exc:rows.append({"case":case,"status":"pass" if exc.code==expected else "fail","expectedError":expected,"actualError":exc.code,"detail":str(exc)})
    report={"schema":"leanat.proof-fixture-report.v1","status":"pass" if all(row["status"]=="pass" for row in rows) else "fail","cases":rows,"manifest":str((output/"manifest.json").resolve()),"release_gate":"not-evaluated"}
    write_json(report_path,report)
    return report


def main():
    parser=argparse.ArgumentParser();parser.add_argument("--out",required=True);parser.add_argument("--report",required=True)
    args=parser.parse_args();result=create_fixture(args.out,args.report)
    print(result["status"])
    return 0 if result["status"]=="pass" else 4


if __name__=="__main__":raise SystemExit(main())
