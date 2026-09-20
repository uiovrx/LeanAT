import unittest
from unittest.mock import patch

from tools.leanat.io import ToolError
from tools.leanat.opcode_bounds import (
    StoreBound, bind_segment_bytes, check_envelope, owned_state_bytes,
    read_segment_bytes, value_bytes, descriptor_value_bounds, prove_pure_case,
    instruction_effect_counts,
)


class OpcodeBoundsTests(unittest.TestCase):
    def test_effect_counts_take_branch_max_include_calls_and_bound_cycles(self):
        def block(i, ops, targets=()):
            return {"id": i, "instructions": [{"opcode": op, "immediate": 1} for op in ops],
                    "terminator": {"targets": list(targets)}}
        summary = {"programs": [
            {"id": 0, "entry": 0, "blocks": [block(0, [19], [1, 2]),
                                                 block(1, [15, 15]), block(2, [15])]},
            {"id": 1, "entry": 0, "blocks": [block(0, [15])]}]}
        result = instruction_effect_counts(summary, 0, 100)
        self.assertEqual(result[15], 3)
        self.assertEqual(result[19], 1)
        summary["programs"][0]["blocks"][1]["terminator"]["targets"] = [0]
        self.assertEqual(instruction_effect_counts(summary, 0, 7), {15: 7, 19: 7})

    def test_descriptor_layout_bounds_and_cycles(self):
        types = [{"kind": "2", "bound": "64"},
                 {"kind": "8", "bound": "256"},
                 {"kind": "4", "fields": ["0", "1"]},
                 {"kind": "5", "constructors": [[], ["2"]]},
                 {"kind": "7", "bound": "4", "fields": ["3"]}]
        self.assertEqual(descriptor_value_bounds(types, "40"),
                         [(16, 40), (264, 296), (288, 376), (304, 496), (1224, 2024)])
        with self.assertRaises(ToolError):
            descriptor_value_bounds([{"kind": 4, "fields": [0]}], 40)

    def test_pure_proof_checks_actual_hash_and_rejects_service(self):
        from tools.leanat.io import digest
        blob = b"actual descriptor fixture"
        summary = {"schema": "leanat.opcode-static.v1", "descriptorHash": digest(blob),
                   "profile": "p", "valueNodeBytes": "40", "types": [{"kind": 2, "bound": 64}],
                   "stateTypes": [0], "programs": [{"id": 0, "entry": 0, "blocks": [{"id": 0, "instructions": [
                       {"opcode": 15, "args": [0], "text": "trace", "source": "x"}],
                       "terminator": {"kind": 3}}]}]}
        source = bind_segment_bytes({"fuel": "2"}, 40)
        with patch("pathlib.Path.read_bytes", return_value=blob):
            proof = prove_pure_case(source, summary, "unused", digest(blob), "p")
            self.assertEqual(proof["status"], "Proven")
            self.assertEqual(proof["nativeRequired"]["traceBytes"], "45")
            with self.assertRaises(ToolError):
                prove_pure_case(source, summary, "unused", "0" * 64, "p")
            summary["programs"][0]["blocks"][0]["instructions"][0]["opcode"] = 21
            with self.assertRaises(ToolError):
                prove_pure_case(source, summary, "unused", digest(blob), "p")

    def test_staging_budget_is_explicit_and_independent(self):
        source = {"world": {"maxBytes": "1"}}
        with self.assertRaises(ToolError):
            read_segment_bytes(source)
        bound = bind_segment_bytes(source, 8192)
        self.assertEqual(read_segment_bytes(bound), 8192)
        self.assertEqual(bound["world"]["maxBytes"], "1")
        self.assertNotIn("context", source)
        self.assertEqual(bind_segment_bytes(bound, 8192), bound)
        with self.assertRaises(ToolError):
            bind_segment_bytes(bound, 4096)
        for invalid in (True, -1, "01", "1.0", 2**64):
            with self.assertRaises(ToolError):
                bind_segment_bytes(source, invalid)

    def test_duplicate_environment_rejected(self):
        source = {"context": {"environment": [{"name": "x"}, {"name": "x"}]}}
        with self.assertRaises(ToolError):
            bind_segment_bytes(source, 1)

    def test_contract_charge_includes_utf8_tombstones_cancelled_and_metadata(self):
        bits = {"kind": "bits", "width": 9, "value": "0"}
        world = {
            "objects": [{"tag": "死", "alive": False, "value": bits}],
            "events": [{"kind": "e", "source": "源", "cancelled": True, "values": [bits]}],
            "observations": [{"kind": "o", "opcode": "x", "source": "源", "values": [bits]}],
            "allocationRules": [{"group": "组"}],
            "allocationCounters": [{"group": "组", "retired": True}],
        }
        self.assertEqual(owned_state_bytes(world), 77 + 110 + 47 + 35 + 43)
        self.assertEqual(value_bytes({"kind": "variant", "tag": 0, "fields": [
            {"kind": "vec", "values": [{"kind": "bytes", "data": [1, 2]}]},
            {"kind": "handle", "identity": {}}, {"kind": "unit"}]}), 64)

    def test_aggregate_capacity_is_sum_of_namespaces(self):
        stores = [StoreBound((4, 1, 1), 1024, 96, 0),
                  StoreBound((5, 1, 20), 128, 96, 0),
                  StoreBound((6, 1, 21), 256, 96, 0)]
        result = check_envelope({}, stores, event_count=1024, event_bytes=96,
                                observation_count=0, observation_bytes=0, rule_bytes=0)
        self.assertEqual(result["bounds"]["maxObjects"], "1408")
        self.assertFalse(result["arithmeticSufficient"])
        self.assertIn("maxObjects", result["deficits"])
        result = check_envelope({"maxObjects": "1408"}, stores, event_count=1024,
                                event_bytes=96, observation_count=0, observation_bytes=0,
                                rule_bytes=0)
        self.assertTrue(result["arithmeticSufficient"])
        self.assertFalse(result["certified"])
        self.assertEqual(result["status"], "Conditional")

    def test_bad_or_missing_store_envelopes_rejected(self):
        store = StoreBound((5, 1, 20), 1, 100, 0)
        options = dict(event_count=0, event_bytes=0, observation_count=0,
                       observation_bytes=0, rule_bytes=0)
        with self.assertRaises(ToolError):
            check_envelope({}, [store, store], **options)
        obj = {"identity": {"kind": 5, "domain": "1", "store": "20", "slot": "1"},
               "tag": "r", "value": {"kind": "unit"}}
        with self.assertRaises(ToolError):
            check_envelope({"objects": [obj]}, [], **options)
        with self.assertRaises(ToolError):
            check_envelope({"objects": [obj]}, [store], **options)


if __name__ == "__main__":
    unittest.main()
