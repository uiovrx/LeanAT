import tempfile
import unittest
from pathlib import Path
from tools.leanat.io import ToolError, write_json, digest
from tools.leanat.toolchain import select_toolchain_lock
from tools.leanat.artifacts import validate_emitted_package


class ToolchainSelection(unittest.TestCase):
    def test_linux_bootstrap_preferred_and_documented_fallback_verified(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock = {"lean": {"path": ".deps/lean-4.34.0-linux"}, "compiler": "c++ Linux"}
            write_json(root / ".deps/linux-toolchain-lock.json", lock)
            write_json(root / "docs/toolchain-linux-lock.json", lock)
            calls = []
            def verify(r, selected):
                calls.append((r, selected))
                return {"actual": "test verifier invoked"}
            _, provenance = select_toolchain_lock(root, "linux", verify)
            self.assertEqual(provenance["path"], ".deps/linux-toolchain-lock.json")
            self.assertEqual(calls, [])
            (root / ".deps/linux-toolchain-lock.json").unlink()
            _, provenance = select_toolchain_lock(root, "linux", verify)
            self.assertEqual(provenance["origin"], "verified-documented-fallback")
            self.assertEqual(len(calls), 1)

    def test_linux_never_relabels_windows_or_unverified_fallback(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            write_json(root / "docs/toolchain-lock.json", {"lean": {"path": ".deps/lean-windows"}})
            with self.assertRaises(ToolError):
                select_toolchain_lock(root, "linux")
            write_json(root / "docs/toolchain-linux-lock.json", {"lean": {"path": ".deps/lean-linux"}})
            def reject(*args):
                raise ToolError("ToolchainLockMismatch", "actual tools differ")
            with self.assertRaisesRegex(ToolError, "actual tools differ"):
                select_toolchain_lock(root, "linux", reject)
            write_json(root / ".deps/linux-toolchain-lock.json", {"lean": {"path": ".deps/lean-windows"}})
            with self.assertRaisesRegex(ToolError, "platform"):
                select_toolchain_lock(root, "linux", reject)

    def test_emitted_source_map_binds_descriptor_and_source_bytes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "model.execir.bin").write_bytes(b"descriptor")
            (root / "source.lean").write_text("alpha", encoding="utf-8")
            write_json(root / "source-map.json", {"schema": "leanat.source-map.v1", "artifactHash": digest(b"descriptor"),
                "entries": [{"location": {"node": 0}, "sourceSpan": {"path": "source.lean", "hash": digest(b"alpha"),
                "startByte": 0, "endByte": 5, "line": 1, "column": 1, "ancestry": []}}]})
            def manifest():
                write_json(root / "manifest.json", {"schema": "leanat.manifest.v1", "profile": "AT-Core-1.1-draft",
                    "topSystemId": 0, "optional_enabled": [], "effectiveConfig": {"topSystemId": 0}, "claims": [],
                    "artifacts": [{"id": name, "path": name, "sha256": digest((root/name).read_bytes())}
                                  for name in ("model.execir.bin", "source.lean", "source-map.json")]})
            manifest()
            self.assertEqual(validate_emitted_package(root)["sourceMapEntriesValidated"], 1)
            (root / "source.lean").write_text("bravo", encoding="utf-8")
            manifest()  # Even a refreshed manifest cannot excuse a stale source-map binding.
            with self.assertRaisesRegex(ToolError, "stale source"):
                validate_emitted_package(root)
            (root / "source.lean").write_text("alpha", encoding="utf-8")
            (root / "model.execir.bin").write_bytes(b"changed")
            manifest()
            with self.assertRaisesRegex(ToolError, "artifact/source-map"):
                validate_emitted_package(root)
