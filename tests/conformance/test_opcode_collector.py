import unittest
import copy
from pathlib import Path
from unittest.mock import patch

from tests.conformance.opcode_cases import _validate_targets, _return_mutation_control, _validate_source_inventory
from tools.leanat import opcode_replay
from tools.leanat.opcode_bounds import bind_segment_bytes, read_segment_bytes
from tools.leanat.opcode_replay import actual_coverage


class OpcodeCollectorTests(unittest.TestCase):
    def test_source_inventory_rejects_omitted_variant_and_spoofed_metadata(self):
        source = dict(id="case", variant="positive", profile="AT-Core-1.1-draft", handlerId=0,
                      programId=0, expectedOutcome="success", expectedOpcodeTags=[1], providerFamily="pure",
                      sourceInputSha256="input", descriptorSha256="descriptor", sourceMapSha256="map",
                      modelSha256="model", sourceWrapperSha256="wrapper", modelNodeInventory=[], sourceFiles=["source.lean"])
        negative = {**source, "variant": "negative", "expectedOutcome": "failure"}
        inventory = {"schema": "leanat.opcode-source-inventory.v1", "cases": [source, negative]}
        catalog = {"cases": [{**item, "referenceInput": {"sha256": item["sourceInputSha256"]},
                               "descriptor": {"sha256": item["descriptorSha256"]}, "sourceMap": {"sha256": "map"},
                               "model": {"sha256": "model"}, "sourceWrapper": {"sha256": "wrapper"},
                               "sources": [{"originalPath": "source.lean"}] } for item in inventory["cases"]]}
        self.assertTrue(_validate_source_inventory(catalog, inventory)["exactMatch"])
        with self.assertRaises(ValueError):
            _validate_source_inventory({"cases": catalog["cases"][:1]}, inventory)
        for key, value in (("expectedOutcome", "success"), ("expectedOpcodeTags", []),
                           ("providerFamily", "sideband"), ("programId", True),
                           ("referenceInput", {"sha256": "forged"}), ("descriptor", {"sha256": "forged"}),
                           ("sourceMap", {"sha256": "forged"}), ("modelNodeInventory", [{}])):
            altered = copy.deepcopy(catalog)
            altered["cases"][1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                _validate_source_inventory(altered, inventory)

    def test_production_comparator_rejects_return_mutation(self):
        from test_opcode_projection_pure import fixtures
        model, executable, native, inputs = fixtures()
        inputs = bind_segment_bytes(inputs, 8 * 1024 * 1024)
        for value in (model, executable):
            value.update(schema="leanat.reference-outcome.v1", returned=[], committed=[], exit="returned", wait=None, outcomeType=None, runtimeFuelRemaining=None)
        native.update(schema="leanat.opcode-native-output.v1", descriptorHash="test-descriptor", returned=[], committed=[], exit="returned",
                      stageBudget={"bytes": str(read_segment_bytes(inputs)), "writes": "4096", "actions": "4096", "events": "4096"})
        case = {"expectedOpcodeTags": [15], "expectedOutcome": "success", "providerFamily": "pure", "programId": 0}
        case["descriptorSummary"] = {"schema": "leanat.opcode-static.v1", "descriptorHash": "test-descriptor", "programs": [
            {"id": "0", "instructionFuel": "0", "blocks": [{"id": "0", "instructions": [{"opcode": "15", "source": "handler/0/body/0", "args": [0], "destType": None}]}]}]}
        root = Path(__file__).resolve().parents[2]
        systemc = copy.deepcopy(native)
        control = opcode_replay.compare_case(case, inputs, model, executable, native, systemc, root)
        self.assertEqual(control["status"], "Pass", control)
        result = _return_mutation_control(opcode_replay, case, inputs, model, executable, native, systemc, root)
        self.assertEqual(result["comparison"]["status"], "Fail")
        self.assertTrue(result["returnMismatches"])
        self.assertEqual(native["returned"], [])
        with patch.object(opcode_replay, "compare_case", return_value={"status": "Pass", "differences": []}):
            with self.assertRaises(ValueError):
                _return_mutation_control(opcode_replay, case, inputs, model, executable, native, systemc, root)

    def test_return_only_has_no_positive_opcode_credit(self):
        _validate_targets([])
        result = actual_coverage({"schema": "leanat.opcode-native-output.v1",
                                  "opcodeObservationComplete": True, "opcodeEvents": []}, [], "success")
        self.assertTrue(result["complete"])
        self.assertEqual(result["completed"], [])
        self.assertEqual(set(range(65)) - set(result["completed"]), set(range(65)))

    def test_return_only_still_requires_actual_complete_observation(self):
        self.assertFalse(actual_coverage({}, [], "success")["complete"])

    def test_invalid_targets_are_rejected(self):
        for targets in (None, "", [True], [-1], [65], ["0"]):
            with self.subTest(targets=targets), self.assertRaises(ValueError):
                _validate_targets(targets)


if __name__ == "__main__":
    unittest.main()
