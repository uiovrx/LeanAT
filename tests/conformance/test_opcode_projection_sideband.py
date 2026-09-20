"""Comparator mutation tests; these do not claim native execution coverage."""
import copy
import unittest
from opcode_projection_sideband import project, _world, _self_test, _error_agreement


def fixtures():
    def u(n):
        return {"kind": "bits", "width": 64, "value": str(n)}
    def rows(values):
        return {"kind": "vec", "values": [{"kind": "record", "fields": row} for row in values]}
    world = _world({"allocationRules": [dict(kind=2, store="4", group="event", perSlot=True,
        persistent=True, allowMax=False, capacity="8")], "allocationCounters": [dict(group="event",
        domain="1", slot="0", nextGeneration="3", persistent=True, retired=False)]})
    source = {"world": world, "context": {"domain": "1", "owner": "7", "environment": [
        {"name": "runtime.owners", "value": rows([[u(1), u(7)]])},
        {"name": "runtime.inputPorts", "value": rows([[u(1), u(2), u(1)]])},
        {"name": "runtime.inputs", "value": rows([])}]}}
    outcome = {"world": copy.deepcopy(world)}
    provider = {"inputs": [dict(instance="1", port="2", owner="7", typeId="1", ready=False, value=None)]}
    native = dict(initialProvider=copy.deepcopy(provider), provider=provider, identities={}, initialIdentities={},
                  actions=dict(actions=[], cursor="0", failed=False), hostEvents=[])
    return source, outcome, native


class SidebandProjectionTests(unittest.TestCase):
    def test_error_mapping_requires_actual_source_arguments_fuel_and_code(self):
        source, _, native = fixtures()
        source["context"]["instanceId"] = "1"
        source["context"]["owner"] = "8"
        args = [{"kind": "bits", "width": 64, "value": "2"}]
        error = {"code": "4", "message": "input instance owner (program 0, block 0, opcode 22, handler/0/body/1)"}
        row = dict(stage="error", opcode=22, program=0, block=0, source="handler/0/body/1", arguments=args,
                   error=error, fuelBefore="97", fuelAfter="96")
        trace = dict(operation="error:loadInput", location=row["source"], program="0", args=args, results=[], fuelBefore="97", fuelAfter="96")
        executable = dict(ok=False, error="InputOwnerMismatch", remainingFuel="96", trace=[trace])
        model = copy.deepcopy(executable)
        model["trace"][0]["operation"] = "error:leanat.core.input.read"
        native.update(ok=False, error=error, remainingFuel="96", opcodeObservationComplete=True, opcodeEvents=[row])
        self.assertTrue(_error_agreement(model, executable, native, source)["verified"])
        for field, value in (("opcode", 21), ("source", "wrong"), ("fuelAfter", "95"), ("arguments", []), ("stage", "completed")):
            changed = copy.deepcopy(native)
            changed["opcodeEvents"][-1][field] = value
            self.assertIsNone(_error_agreement(model, executable, changed, source), field)
        for field, value in (("code", "0"), ("message", "input instance owner")):
            changed = copy.deepcopy(native)
            changed["error"][field] = value
            changed["opcodeEvents"][-1]["error"] = changed["error"]
            self.assertIsNone(_error_agreement(model, executable, changed, source), field)
        changed = copy.deepcopy(executable)
        changed["trace"][-1]["fuelBefore"] = "98"
        self.assertIsNone(_error_agreement(model, changed, native, source))

    def test_context_rejection_does_not_claim_execution(self):
        source, _, native = fixtures()
        source["context"]["kind"] = "2"
        source["fuel"] = "100"
        outcome = dict(ok=False, error="ExecutionContextMismatch", trace=[], remainingFuel="100")
        native.update(ok=False, error=dict(code="8", message="program context mismatch (program 0)"), opcodeEvents=[], remainingFuel="100")
        self.assertFalse(_error_agreement(outcome, outcome, native, source)["opcodeExecuted"])
        changed = copy.deepcopy(outcome)
        changed["trace"] = [{"operation": "enter:loadInput"}]
        self.assertIsNone(_error_agreement(changed, outcome, native, source))
        changed = copy.deepcopy(native)
        changed["opcodeEvents"] = [{"stage": "entered", "opcode": 22}]
        self.assertIsNone(_error_agreement(outcome, outcome, changed, source))
        native["remainingFuel"] = "99"
        self.assertIsNone(_error_agreement(outcome, outcome, native, source))

    def test_existing_controls(self):
        _self_test()

    def test_complete_unchanged_journal_and_context(self):
        source, outcome, native = fixtures()
        originals = copy.deepcopy((source, outcome, native))
        result = project(outcome, outcome, native, source)
        self.assertFalse(result["unmapped"])
        self.assertEqual(result["allocationEvidence"]["model"], source["world"])
        self.assertEqual(result["contextEvidence"], source["context"])
        self.assertEqual((source, outcome, native), originals)

    def test_every_counter_and_rule_field_matters(self):
        for key, changes in (("allocationCounters", dict(group="other", domain="2", slot="1", nextGeneration="4", persistent=False, retired=True)),
                             ("allocationRules", dict(kind="3", store="5", group="other", perSlot=False, persistent=False, allowMax=True, capacity="9"))):
            for field, value in changes.items():
                with self.subTest(key=key, field=field):
                    source, outcome, native = fixtures()
                    outcome["world"][key][0][field] = value
                    self.assertTrue(project(outcome, outcome, native, source)["unmapped"])

    def test_unknown_missing_duplicate_journal(self):
        for mutation in ("unknown", "missing", "duplicate"):
            for key in ("allocationRules", "allocationCounters"):
                source, outcome, native = fixtures()
                if mutation == "unknown": outcome["world"][key][0]["unknown"] = 1
                elif mutation == "missing": del outcome["world"][key]
                else: outcome["world"][key] *= 2
                with self.assertRaises(ValueError): project(outcome, outcome, native, source)

    def test_unknown_world_context_and_native_journal(self):
        source, outcome, native = fixtures()
        outcome["world"]["unknown"] = []
        with self.assertRaises(ValueError): project(outcome, outcome, native, source)
        source, outcome, native = fixtures()
        source["context"]["unknown"] = 1
        with self.assertRaises(ValueError): project(outcome, outcome, native, source)
        source, outcome, native = fixtures()
        native["allocationCounters"] = []
        self.assertTrue(project(outcome, outcome, native, source)["unmapped"])

    def test_shared_mutation_does_not_become_an_oracle(self):
        source, outcome, native = fixtures()
        for key in ("nextGeneration", "nextSequence", "generationLimit", "maxBytes"):
            changed = copy.deepcopy(outcome)
            changed["world"][key] = str(int(changed["world"][key]) + 1)
            self.assertTrue(project(changed, changed, native, source)["unmapped"])


if __name__ == "__main__":
    unittest.main()
