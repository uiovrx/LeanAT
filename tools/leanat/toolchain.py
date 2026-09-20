"""Select host-specific lock provenance without relabeling another platform's lock."""
import platform
import subprocess
import sys
from pathlib import Path
from .io import ToolError, read_json, digest


def verify_linux_fallback(root, lock):
    def first(command):
        try:
            return subprocess.check_output(command, cwd=root, text=True, stderr=subprocess.STDOUT, timeout=15).splitlines()[0]
        except (OSError, subprocess.SubprocessError, IndexError) as error:
            raise ToolError("ToolchainLockUnverified", str(error), 5) from error
    actual = {"compiler": first(["c++", "--version"]), "cmake": first(["cmake", "--version"]),
              "python": platform.python_version(), "machine": platform.machine()}
    if any(lock.get(key) != value for key, value in actual.items()):
        raise ToolError("ToolchainLockMismatch", "documented Linux fallback differs from actual host tools", 5)
    lean = root / lock["lean"]["path"] / "bin/lean"
    if lock["lean"]["version"] not in first([str(lean), "--version"]):
        raise ToolError("ToolchainLockMismatch", "documented Linux Lean version differs", 5)
    version_file = root / lock["systemc"]["path"] / "lib/cmake/SystemCLanguage/SystemCLanguageConfigVersion.cmake"
    if not version_file.is_file() or ('set(PACKAGE_VERSION "' + lock["systemc"]["version"] + '")') not in version_file.read_text(encoding="utf-8"):
        raise ToolError("ToolchainLockUnverified", "documented Linux SystemC installation/version not verified", 5)
    return actual


def select_toolchain_lock(root, host_platform=None, fallback_verifier=None):
    root = Path(root).resolve()
    host_platform = sys.platform if host_platform is None else host_platform
    linux = host_platform.startswith("linux")
    if linux:
        local = root / ".deps/linux-toolchain-lock.json"
        path = local if local.is_file() else root / "docs/toolchain-linux-lock.json"
    elif host_platform == "win32":
        path = root / "docs/toolchain-lock.json"
    else:
        raise ToolError("UnsupportedToolchainHost", host_platform, 5)
    if not path.is_file():
        raise ToolError("MissingToolchainLock", str(path), 5)
    lock = read_json(path)
    lean_path = lock.get("lean", {}).get("path", "").lower()
    if (linux and ("linux" not in lean_path or "windows" in lean_path or "mingw" in str(lock.get("compiler", "")).lower())) or (not linux and "windows" not in lean_path):
        raise ToolError("ToolchainLockMismatch", "lock platform differs from execution host", 5)
    fallback = linux and path.parent.name == "docs"
    verified = (fallback_verifier or verify_linux_fallback)(root, lock) if fallback else None
    return lock, {"path": path.relative_to(root).as_posix(), "sha256": digest(path.read_bytes()),
                  "hostPlatform": host_platform, "origin": "verified-documented-fallback" if fallback else "bootstrap-lock",
                  "fallbackVerification": verified}
