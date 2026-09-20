"""Record locally executed facade profile runs; publish only complete stable evidence."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sources(root):
    files = []
    for folder in ("LeanAT", "runtime", "stdlib", "systemc", "templates", "tests/lean", "scripts", ".deps/systemc-linux-install/include"):
        files.extend(p for p in (root / folder).rglob("*") if p.is_file() and p.suffix in
                     {".lean", ".hpp", ".h", ".cpp", ".c", ".sh", ".py", ".cmake", ".txt"})
    files.extend(root / p for p in ("lakefile.lean", "lake-manifest.json", "lean-toolchain", "CMakeLists.txt") if (root / p).is_file())
    files.extend(root.glob("*.lean"))
    return {p.relative_to(root).as_posix(): digest(p) for p in sorted(set(files))}


def save(path, data):
    path.write_text(json.dumps(data, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def main():
    action, root_arg, results_arg, scratch_arg, *args = sys.argv[1:]
    root, results, scratch = map(lambda p: Path(p).resolve(), (root_arg, results_arg, scratch_arg))
    pending = scratch / "manifest.pending.json"
    if action == "begin":
        # An empty file cannot be mistaken for the previous successful manifest.
        (results / "runs.json").write_text("", encoding="utf-8")
        (results / "failed-runs.json").write_text("", encoding="utf-8")
        lock = ".deps/linux-toolchain-lock.json" if (root / ".deps/linux-toolchain-lock.json").is_file() else "docs/toolchain-linux-lock.json"
        save(pending, {"schema": "leanat.facade-profile-runs.v1", "toolchainLock":
             {"path": lock, "sha256": digest(root / lock)}, "sourceSha256": sources(root), "runs": []})
        return 0
    data = json.loads(pending.read_text(encoding="utf-8"))
    if action == "step":
        profile, scenario, stage, *command = args
        row = next((r for r in data["runs"] if (r["profile"], r["scenario"]) == (profile, scenario)), None)
        if row is None:
            row = {"profile": profile, "scenario": scenario}
            data["runs"].append(row)
        key = {"generate": "generateCommand", "compile": "compileCommand", "run": "command"}[stage]
        row[key] = command
        executable = shutil.which(command[0])
        if executable:
            row.setdefault("toolSha256", {})[str(Path(executable).resolve())] = digest(executable)
        if stage == "run":
            row["binarySha256"] = digest(command[0])
        if stage == "compile":
            row["linkedInputSha256"] = {str(Path(p).resolve()): digest(p) for p in command if p.endswith(".a")}
        try:
            code = subprocess.run(command, cwd=root, check=False).returncode
        except OSError as error:
            print(error, file=sys.stderr)
            code = 127
        row[{"generate": "generateExitCode", "compile": "compileExitCode", "run": "exitCode"}[stage]] = code
        if code == 0 and stage == "generate":
            generated = scratch / f"{profile}-{scenario}"
            retained = results / "generated" / f"{profile}-{scenario}"
            row["generatedSourceSha256"] = {}
            for path in sorted(generated.rglob("*")):
                if path.is_file() and path.suffix in {".hpp", ".cpp"}:
                    destination = retained / path.relative_to(generated)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, destination)
                    row["generatedSourceSha256"][destination.relative_to(results).as_posix()] = digest(destination)
        if code == 0 and stage == "run":
            row["resultSha256"] = digest(results / f"{profile}-{scenario}.json")
            row["descriptorSha256"] = digest(results / f"{profile}-{scenario}.execir.bin")
            if digest(command[0]) != row["binarySha256"]:
                row["exitCode"] = code = 1
        save(pending, data)
        return code
    if action != "finish":
        raise ValueError("unknown manifest action")
    expected = {(p, s) for p in ("core", "ext") for s in ("scalar", "wire", "signal", "process")}
    complete = {(r["profile"], r["scenario"]) for r in data["runs"] if all(
        r.get(k) == 0 for k in ("generateExitCode", "compileExitCode", "exitCode"))}
    stable = sources(root) == data["sourceSha256"] and digest(root / data["toolchainLock"]["path"]) == data["toolchainLock"]["sha256"]
    for row in data["runs"]:
        for key in ("linkedInputSha256", "toolSha256"):
            stable = stable and all(Path(p).is_file() and digest(p) == h for p, h in row.get(key, {}).items())
        for p, h in row.get("generatedSourceSha256", {}).items():
            stable = stable and (results / p).is_file() and digest(results / p) == h
    if complete != expected or not stable:
        save(results / "failed-runs.json", {"sourceStable": stable, "runs": data["runs"]})
        print("Facade manifest rejected: incomplete execution or changed sources", file=sys.stderr)
        return 1
    save(results / "runs.json", data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
