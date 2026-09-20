import unittest
import copy
from unittest.mock import patch
from types import SimpleNamespace
from tools.leanat.opcode_replay import actual_coverage, bind_host_profile, differences, compare_case, validate_opcode_observations, compare_instruction_values, runtime_fuel_receipts, instruction_fuel_scopes
from tools.leanat.io import ToolError
from tests.conformance.opcode_projection_common import IdentityBijection


class OpcodeEvidenceTests(unittest.TestCase):
    def test_observer_fuel_offset_is_declared_not_inferred(self):
        case = {"programId": 0, "descriptorSummary": {"programs": [{"id": "0", "instructionFuel": "100"}]}}
        scopes = instruction_fuel_scopes(case, {"fuel": "10000"}, {})
        self.assertEqual(scopes["0"]["observerOffset"], "9900")
        row = {"stage": "completed", "opcode": 0, "program": 0, "source": "site", "arguments": [],
               "result": {"kind": "unit"}, "fuelBefore": "100", "fuelAfter": "99"}
        executable = {"trace": [{"operation": "const", "program": "0", "location": "site", "args": [],
                                  "results": [{"kind": "unit"}], "fuelBefore": "10000", "fuelAfter": "9999"}]}
        self.assertEqual(compare_instruction_values(executable, {"opcodeEvents": [row]}, IdentityBijection([]), scopes), [])
        wrong = {**row, "fuelBefore": "99", "fuelAfter": "98"}
        self.assertTrue(compare_instruction_values(executable, {"opcodeEvents": [wrong]}, IdentityBijection([]), scopes))
        child = {**row, "program": 1}
        self.assertTrue(compare_instruction_values(executable, {"opcodeEvents": [child]}, IdentityBijection([]), scopes))
        case["descriptorSummary"]["programs"].append({"id": "1", "instructionFuel": "100"})
        projection = {"fuelScopes": [{"program": "1", "verified": True, "execInitialFuel": "9997",
                         "nativeInitialFuel": "100", "runtimeCap": "100", "evidence": {"nativeConstructorReceipt": {
                             "program": "1", "inputFuel": "100", "runtimeCap": "100", "remainingFuel": "98"}}}]}
        scopes = instruction_fuel_scopes(case, {"fuel": "10000"}, projection)
        self.assertEqual(scopes["1"]["observerOffset"], "9897")
        projection["fuelScopes"][0]["nativeInitialFuel"] = "99"
        with self.assertRaises(ValueError):
            instruction_fuel_scopes(case, {"fuel": "10000"}, projection)

    def test_runtime_receipts_bind_descriptor_cap_and_actual_spent(self):
        case = {"expectedOutcome": "success", "programId": 7,
                "descriptorSummary": {"programs": [{"id": "7", "instructionFuel": "100"}]}}
        source = {"fuel": "10000"}
        model = {"runtimeFuelRemaining": "97", "remainingFuel": "9980"}
        executable = {"runtimeFuelRemaining": "97", "remainingFuel": "9997", "trace": [{"program": "7", "fuelAfter": "9997"}]}
        native = {"remainingFuel": "9997"}
        errors, evidence = runtime_fuel_receipts(case, source, model, executable, native, native)
        self.assertEqual(errors, [])
        self.assertEqual(evidence["runtime"]["segmentSpent"], "3")
        for changed in ({}, {"runtimeFuelRemaining": None}, {"runtimeFuelRemaining": "097"}, {"runtimeFuelRemaining": "96"}):
            self.assertTrue(runtime_fuel_receipts(case, source, changed, executable, native, native)[0])
        self.assertTrue(runtime_fuel_receipts(case, source, model, executable, {"remainingFuel": "9899"}, native)[0])
        case["descriptorSummary"]["programs"][0]["instructionFuel"] = "0"
        self.assertEqual(runtime_fuel_receipts(case, source, {"runtimeFuelRemaining": None}, {**executable, "runtimeFuelRemaining": None}, native, native)[0], [])
        case["expectedOutcome"] = "preflight-failure"
        self.assertEqual(runtime_fuel_receipts(case, source, {"runtimeFuelRemaining": None}, {"runtimeFuelRemaining": None}, {}, {})[0], [])
        self.assertTrue(runtime_fuel_receipts(case, source, model, executable, {}, {})[0])

    def test_full_instruction_values_and_fuel_are_independently_bound(self):
        value = {"kind": "bits", "width": 64, "value": "9"}
        row = {"stage": "completed", "opcode": 35, "program": 0, "source": "site", "arguments": [value],
               "result": value, "fuelBefore": "10", "fuelAfter": "9"}
        executable = {"trace": [{"operation": "scheduleEvent", "program": "0", "location": "site", "args": [value],
                                  "results": [value], "fuelBefore": "10", "fuelAfter": "9"}]}
        self.assertEqual(compare_instruction_values(executable, {"opcodeEvents": [row]}, IdentityBijection([])), [])
        for field, changed in (("arguments", []), ("result", None), ("fuelAfter", "8"), ("source", "different")):
            altered = {**row, field: changed}
            self.assertTrue(compare_instruction_values(executable, {"opcodeEvents": [altered]}, IdentityBijection([])))

    def test_observer_records_require_artifact_opcode_and_exact_pairing(self):
        summary = {"schema": "leanat.opcode-static.v1", "descriptorHash": "hash", "programs": [
            {"id": "0", "blocks": [{"id": "0", "instructions": [{"opcode": "35", "source": "source", "args": [], "destType": "0"}]}]}]}
        entry = {"stage": "entered", "program": 0, "block": 0, "instruction": 0, "callDepth": 0,
                 "opcode": 35, "source": "source", "fuelBefore": "10", "fuelAfter": "10", "arguments": [], "result": None, "error": None}
        completed = {**entry, "stage": "completed", "fuelAfter": "9", "result": {"kind": "unit"}}
        native = {"descriptorHash": "hash", "opcodeEvents": [entry, completed]}
        self.assertEqual(validate_opcode_observations(native, summary), [])
        for events in ([entry, completed, completed], [completed], [entry, {**completed, "opcode": 64}],
                       [entry, {**completed, "arguments": [1]}], [entry]):
            self.assertTrue(validate_opcode_observations({**native, "opcodeEvents": events}, summary))

    def test_preflight_failure_requires_unchanged_state_and_no_execution_credit(self):
        source = {"fuel": "10", "inputs": [], "committed": [],
                  "world": {"allocationRules": [], "allocationCounters": []},
                  "context": {"environment": [{"name": "profile.segmentBytes", "value": {"value": "100"}}]}}
        reference = {"schema": "leanat.reference-outcome.v1", "ok": False, "error": "ExecutionContextMismatch",
                     "returned": [], "committed": [], "world": source["world"], "exit": "returned",
                     "wait": None, "outcomeType": None, "remainingFuel": "10", "trace": [], "runtimeFuelRemaining": None}
        native = {"schema": "leanat.opcode-native-output.v1", "ok": False, "error": {"code": "8", "message": "program context mismatch (program 0)"},
                  "returned": [], "committed": [], "preparedInputs": [], "remainingFuel": "10", "exit": "failed",
                  "initialProvider": {}, "provider": {}, "hostEvents": [],
                  "actions": {"actions": [], "cursor": "0", "failed": False},
                  "stageBudget": {"bytes": "100", "writes": "4096", "actions": "4096", "events": "4096"},
                  "opcodeObservationComplete": True, "opcodeEvents": []}
        case = {"providerFamily": "sideband", "expectedOutcome": "preflight-failure", "expectedOpcodeTags": [22],
                "descriptorSummary": {"schema": "leanat.opcode-static.v1", "programs": []}}
        module = SimpleNamespace(project=lambda *args: {"model": {}, "exec": {}, "native": {}, "errorAgreement": {"verified": True}})
        with patch("importlib.import_module", return_value=module):
            result = compare_case(case, source, reference, reference, native, native, ".")
            self.assertEqual(result["status"], "Pass")
            self.assertEqual(result["coverage"]["runtime"]["completed"], [])
            for field, changed in (("remainingFuel", "9"), ("hostEvents", [1]), ("opcodeEvents", [{"stage": "completed", "opcode": 22}])):
                bad = copy.deepcopy(native)
                bad[field] = changed
                self.assertNotEqual(compare_case(case, source, reference, reference, bad, bad, ".")["status"], "Pass")

    def test_internal_projection_dependency_failure_is_not_hidden(self):
        error = ModuleNotFoundError("internal dependency missing", name="broken_dependency")
        # Import failures in a projection are implementation errors, not missing coverage.
        with patch("importlib.import_module", side_effect=error):
            with self.assertRaises(ModuleNotFoundError):
                compare_case({"providerFamily": "sideband", "expectedOutcome": "binding-failure", "expectedOpcodeTags": [22]}, {}, {}, {}, {}, {}, ".")

    def test_missing_projection_or_null_error_agreement_is_not_run(self):
        reference = {"schema": "leanat.reference-outcome.v1", "ok": False, "error": "cause",
                     "returned": [], "committed": [], "world": {}, "exit": "returned",
                     "wait": None, "outcomeType": None, "remainingFuel": "9"}
        native = {"schema": "leanat.opcode-native-output.v1", "ok": False, "error": "cause",
                  "returned": [], "committed": [], "remainingFuel": "9", "exit": "failed",
                  "opcodeObservationComplete": True, "opcodeEvents": [{"stage": "error", "opcode": 0}]}
        for projection in (None, {"model": {}, "exec": {}, "native": {}, "errorAgreement": None}):
            module = SimpleNamespace(project=lambda *args: projection)
            with patch("importlib.import_module", return_value=module):
                result = compare_case({"providerFamily": "nullable", "expectedOutcome": "failure", "expectedOpcodeTags": [0]},
                                      {}, reference, reference, native, native, ".")
            self.assertEqual(result["status"], "NotRun")
            self.assertTrue(result["unmapped"])

    def test_matching_failed_backends_cannot_satisfy_success_case(self):
        reference = {"schema": "leanat.reference-outcome.v1", "ok": False, "error": "failed",
                     "returned": [], "committed": [], "world": {}, "exit": "returned",
                     "wait": None, "outcomeType": None, "remainingFuel": "9"}
        native = {"schema": "leanat.opcode-native-output.v1", "ok": False, "error": "failed",
                  "returned": [], "committed": [], "remainingFuel": "9", "exit": "failed",
                  "provider": {}, "initialProvider": {}, "opcodeObservationComplete": True,
                  "opcodeEvents": [{"stage": "completed", "opcode": 0}]}
        result = compare_case({"providerFamily": "unavailable", "expectedOutcome": "success", "expectedOpcodeTags": [0]},
                              {}, reference, reference, native, native, ".")
        self.assertEqual(result["status"], "Fail")
        self.assertTrue(any(row["path"].endswith(".ok") for row in result["differences"]))

    def test_identity_mapping_preserves_authority_lifetime_and_bijection(self):
        def handle(generation, owner="9"):
            return {"kind": 1, "domain": "1", "store": "2", "slot": "3", "generation": generation, "owner": owner}
        entries = [{"role": "lease", "model": handle("2"), "exec": handle("2"), "native": handle("1")}]
        mapping = IdentityBijection(entries)
        native = {"kind": "handle", "identity": handle("1")}
        stale = {"kind": "handle", "identity": handle("3")}
        self.assertNotEqual(mapping.normalize(native, "native"), mapping.normalize(stale, "native"))
        with self.assertRaises(ValueError):
            IdentityBijection(entries + [{**entries[0], "role": "different-consumer"}])
        with self.assertRaises(ValueError):
            IdentityBijection([{**entries[0], "native": handle("1", "10")}])

    def test_observed_opcode_required_and_failure_prefix_preserved(self):
        output = {"schema": "leanat.opcode-native-output.v1", "opcodeObservationComplete": True,
                  "opcodeEvents": [{"stage": "entered", "opcode": 7},
                                   {"stage": "error", "opcode": 7}]}
        self.assertFalse(actual_coverage(output, [7], "success")["complete"])
        self.assertTrue(actual_coverage(output, [7], "failure")["complete"])
        output["opcodeObservationComplete"] = False
        self.assertFalse(actual_coverage(output, [7], "failure")["complete"])

    def test_full_order_and_handle_generation_are_observable(self):
        a = {"events": [{"owner": "1", "generation": "2"}, {"owner": "3", "generation": "4"}]}
        self.assertTrue(differences(a, {"events": list(reversed(a["events"]))}))
        self.assertTrue(differences(a, {"events": [{"owner": "1", "generation": "3"}, a["events"][1]]}))
        self.assertTrue(differences(a, {"events": a["events"], "feedback": []}))

    def test_host_measurement_rebinds_source_input_without_mutation(self):
        original = {"context": {"environment": [{"name": "profile.valueNodeBytes", "value": {"kind": "bits", "width": 64, "value": "1"}}]}}
        bound = bind_host_profile(original, {"schema": "leanat.opcode-host.v1", "valueNodeBytes": "40"})
        self.assertEqual(bound["context"]["environment"][0]["value"]["value"], "40")
        self.assertEqual(original["context"]["environment"][0]["value"]["value"], "1")
        with self.assertRaises(ToolError):
            bind_host_profile(original, {"schema": "leanat.opcode-host.v1", "valueNodeBytes": "040"})
