"""Original Lean frontend context regression with actual positive/negative compilation."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def run(root, output_dir):
    root, output_dir = Path(root), Path(output_dir)
    source = root / "tests/lean/fail/HandlerAwaitSlot.lean"
    positive = "import LeanAT.Frontend.BlockSyntax\nat_component Allowed where\n  on internal.tick event do\n    emit \"accepted\"\n    return\n"
    candidates = ([Path(os.environ["LEANAT_LAKE"])] if os.environ.get("LEANAT_LAKE") else [])
    candidates += sorted((root / ".deps").glob("lean*/bin/lake.exe")) if os.name == "nt" else sorted((root / ".deps").glob("lean*/bin/lake"))
    if shutil.which("lake"):
        candidates.append(Path(shutil.which("lake")))
    lake = next((p for p in candidates if p.is_file()), None)
    row = {"id": "E-T07", "backend": "lean-compiler", "input": {"positive": positive, "negative": source.read_text(encoding="utf-8")}, "expected": {"positiveExit": 0, "negativeExit": 1, "negativeDiagnostic": "AwaitSlotForbiddenInContext"}, "actual": [], "status": "NotRun", "stop": "MissingDependency", "reason": "Actual Lean compiler required for timed-handler awaitSlot context rejection"}
    if lake is None:
        return [row]
    scratch = output_dir / "lean_cases"
    scratch.mkdir(parents=True, exist_ok=True)
    control = scratch / "Allowed.lean"
    control.write_text(positive, encoding="utf-8")
    commands = [[str(lake), "build", "LeanAT.Frontend.BlockSyntax"], [str(lake), "env", "lean", str(control.resolve())], [str(lake), "env", "lean", str(source.resolve())]]
    try:
        for command in commands:
            result = subprocess.run(command, cwd=root, text=True, encoding="utf-8", errors="replace", capture_output=True, timeout=120)
            row["actual"].append({"command": command, "exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
            if len(row["actual"]) == 1 and result.returncode != 0:
                row["reason"] = "Frontend module build failed before positive/negative scenarios; see actual build diagnostics"
                break
        if len(row["actual"]) == 3:
            build, good, bad = row["actual"]
            passed = build["exit_code"] == good["exit_code"] == 0 and bad["exit_code"] == 1 and "AwaitSlotForbiddenInContext" in bad["stdout"] + bad["stderr"]
            row.update(status="Pass" if passed else "Fail", stop="CompilerDiagnostics", reason="")
    except (OSError, subprocess.TimeoutExpired) as exc:
        row.update(reason=str(exc), stop="CompilerUnavailable")
    finally:
        control.unlink(missing_ok=True)
    row["source_sha256"] = {str(source.relative_to(root)): hashlib.sha256(source.read_bytes()).hexdigest(), "positive": hashlib.sha256(positive.encode()).hexdigest(), "frontend": hashlib.sha256((root / "LeanAT/Frontend/BlockSyntax.lean").read_bytes()).hexdigest()}
    evidence = scratch / "E-T07.json"
    evidence.write_text(json.dumps(row, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    row["evidence_path"] = str(evidence.resolve())
    row["evidence_sha256"] = hashlib.sha256(evidence.read_bytes()).hexdigest()
    return [row]
