"""Comparator mutation tests, not evidence of executing the language corpus."""
import copy
import unittest
from opcode_projection_pure import project, WORLD_DEFAULTS


def row(layer, operation, before, after, args=None, results=None, location="handler/0/body/0"):
    return dict(layer=layer, program="0", location=location, operation=operation,
                args=args or [], results=results or [], fuelBefore=str(before), fuelAfter=str(after))


def fixtures():
    arg = {"kind": "bits", "width": 64, "value": "18446744073709551615"}
    observation = dict(kind="trace", opcode="actual", source="handler/0/body/0", time="9", turn="2", values=[arg])
    model = dict(ok=True, error="", remainingFuel="999", world=dict(objects=[], events=[], observations=[observation]),
                 trace=[row("ModelIR", "emit", 1000, 999, [arg])])
    model["world"].update(WORLD_DEFAULTS, allocationRules=[], allocationCounters=[])
    executable = copy.deepcopy(model)
    executable.update(remainingFuel="998", trace=[row("ExecIR", "enter:trace", 1000, 999, [arg], location="program/0/block/0/0"),
        row("ExecIR", "trace", 1000, 999, [arg]), row("ExecIR", "return", 999, 998, location="program/0/block/0/1")])
    event = dict(stage="entered", program=0, block=0, instruction=0, callDepth=0, opcode=15,
                 source="handler/0/body/0", fuelBefore="1000", fuelAfter="1000", arguments=[arg], result=None, error=None)
    native = dict(ok=True, error=None, remainingFuel="998", provider={}, initialProvider={}, hostEvents=[],
                  opcodeObservationComplete=True, opcodeEvents=[event, dict(event, stage="completed", fuelAfter="999")],
                  actions=dict(actions=[], cursor="0", failed=False), rawSegment=dict(traces=[dict(kind="actual",
                    ready=dict(time="9", turn="2"), instance="1", connection="3", values=[{"u64": arg["value"]}], detail="")]))
    input_value = dict(context=dict(instanceId="1", connection="3", now="9", turn="2"), fuel="1000", world={})
    return model, executable, native, input_value


def failure():
    model, executable, native, source = fixtures()
    arg = dict(kind="bits", width=64, value="256")
    for outcome, layer in ((model, "ModelIR"), (executable, "ExecIR")):
        outcome.update(ok=False, error="ArithmeticOverflow", remainingFuel="999",
                       trace=[row(layer, "error:convert", 1000, 999, [arg])])
        outcome["world"]["observations"] = []
    native.update(ok=False, remainingFuel="999", error=dict(code="1", message="ArithmeticOverflow (program 0, block 0, opcode 18, handler/0/body/0)"))
    native["rawSegment"] = None
    for event in native["opcodeEvents"]:
        event.update(opcode=18, arguments=[arg])
    native["opcodeEvents"][1].update(stage="error", error=copy.deepcopy(native["error"]))
    return model, executable, native, source


class PureProjectionTests(unittest.TestCase):
    def test_complete_allocation_journal_is_unchanged(self):
        args = fixtures()
        rule = dict(kind=4, store="7", group="task", perSlot=True, persistent=True, allowMax=False, capacity="9")
        counter = dict(group="task", domain="1", slot="3", nextGeneration="6", persistent=True, retired=False)
        journal = dict(allocationRules=[rule], allocationCounters=[counter])
        for world in (args[0]["world"], args[1]["world"], args[3]["world"]):
            world.update(copy.deepcopy(journal))
        result = project(*args)
        self.assertFalse(result["unmapped"])
        self.assertEqual(result["exec"]["allocationJournal"], journal)
        self.assertEqual(result["native"]["allocationJournal"], journal)
        changes = {
            "allocationRules": dict(kind=5, store="8", group="other", perSlot=False, persistent=False, allowMax=True, capacity="10"),
            "allocationCounters": dict(group="other", domain="2", slot="4", nextGeneration="7", persistent=False, retired=True),
        }
        for table, fields in changes.items():
            for field, change in fields.items():
                mutated = copy.deepcopy(args)
                mutated[1]["world"][table][0][field] = change
                self.assertIn("exec pure allocation journal changed", project(*mutated)["unmapped"], field)
        for table in journal:
            mutated = copy.deepcopy(args)
            mutated[1]["world"][table] = []
            self.assertTrue(project(*mutated)["unmapped"])
            mutated = copy.deepcopy(args)
            mutated[1]["world"][table][0]["extra"] = 0
            with self.assertRaises(ValueError):
                project(*mutated)
            mutated = copy.deepcopy(args)
            mutated[1]["world"][table].append(copy.deepcopy(mutated[1]["world"][table][0]))
            with self.assertRaises(ValueError):
                project(*mutated)

    def test_hidden_allocation_and_missing_journal_fail_closed(self):
        args = fixtures()
        del args[1]["world"]["allocationCounters"]
        with self.assertRaises(ValueError):
            project(*args)
        args = fixtures()
        args[2]["identities"] = {"unexpected": "allocated"}
        self.assertIn("pure execution has native allocation identities", project(*args)["unmapped"])

    def test_zero_fuel_rejection_is_not_an_executed_charge(self):
        model, executable, native, source = failure()
        source["fuel"] = "0"
        for outcome, layer in ((model, "ModelIR"), (executable, "ExecIR")):
            outcome.update(error="FuelExhausted", remainingFuel="0",
                           trace=[row(layer, "error:convert", 0, 0, native["opcodeEvents"][0]["arguments"])])
        native.update(error=dict(code="12", message="instruction fuel exhausted (program 0, block 0, opcode 18, handler/0/body/0)"), remainingFuel="0")
        for event in native["opcodeEvents"]:
            event.update(fuelBefore="0", fuelAfter="0")
        native["opcodeEvents"][-1]["error"] = copy.deepcopy(native["error"])
        p = project(model, executable, native, source)
        self.assertFalse(p["unmapped"])
        self.assertTrue(p["errorAgreement"]["verified"])
        native["opcodeEvents"][-1]["fuelBefore"] = "1"
        self.assertNotIn("errorAgreement", project(model, executable, native, source))

    def test_statement_entry_and_terminal_error_diagnostics_have_distinct_roles(self):
        args = fixtures()
        args[0]["trace"].insert(0, row("ModelIR", "enter:statement", 1000, 999))
        self.assertFalse(project(*args)["unmapped"])
        args[1]["trace"].append(row("ExecIR", "error:terminator", 999, 998, location="program/0/block/0/1"))
        self.assertIn("ExecIR successful outcome contains error trace", project(*args)["unmapped"])

    def test_actual_trace_and_distinct_source_costs(self):
        p = project(*fixtures())
        self.assertFalse(p["unmapped"])
        self.assertEqual(p["model"], p["exec"])
        self.assertEqual(p["exec"], p["native"])

    def test_full_trace_field_mutations_visible(self):
        mutations = [("kind", "other"), ("detail", "changed"), ("connection", "4"),
                     ("instance", "2"), ("ready", dict(time="9", turn="3")),
                     ("values", [{"u64": "18446744073709551614"}])]
        for field, changed in mutations:
            args = fixtures()
            args[2]["rawSegment"]["traces"][0][field] = changed
            p = project(*args)
            self.assertNotEqual(p["exec"], p["native"], field)

    def test_exact_numeric_error_mapping(self):
        p = project(*failure())
        self.assertFalse(p["unmapped"])
        self.assertTrue(p["errorAgreement"]["verified"])

    def test_wrong_code_message_opcode_and_source_never_verified(self):
        for field, changed in (("code", "0"), ("message", "ArithmeticOverflow"),
                ("message", "ArithmeticOverflow (program 0, block 0, opcode 18, wrong-source)")):
            args = failure()
            args[2]["error"][field] = changed
            args[2]["opcodeEvents"][1]["error"][field] = changed
            self.assertNotIn("errorAgreement", project(*args))
        args = failure()
        args[0]["error"] = args[1]["error"] = "unknown failure"
        self.assertNotIn("errorAgreement", project(*args))

    def test_fuel_source_arguments_and_missing_execution_fail_closed(self):
        for field, changed in (("fuelAfter", "998"), ("source", "other"),
                               ("arguments", []), ("program", 2)):
            args = fixtures()
            args[2]["opcodeEvents"][1][field] = changed
            self.assertTrue(project(*args)["unmapped"], field)
        args = fixtures()
        args[2]["opcodeEvents"] = []
        self.assertTrue(project(*args)["unmapped"])

    def test_trace_prefix_and_cost_gaps_not_erased(self):
        args = fixtures()
        args[2]["rawSegment"] = None
        self.assertIn("native emitted trace prefix not fully retained", project(*args)["unmapped"])
        args = fixtures()
        args[0]["remainingFuel"] = "997"
        self.assertTrue(any("unobserved fuel interval" in str(x) for x in project(*args)["unmapped"]))
        args = fixtures()
        args[2]["opcodeObservationComplete"] = False
        self.assertTrue(project(*args)["unmapped"])

    def test_trace_order_and_reference_bookkeeping_are_visible(self):
        model, executable, native, source = fixtures()
        for outcome in (model, executable):
            second = copy.deepcopy(outcome["world"]["observations"][0])
            second.update(opcode="second", source="handler/0/body/1")
            outcome["world"]["observations"].append(second)
        model["trace"].append(row("ModelIR", "emit", 999, 998, model["trace"][0]["args"], location="handler/0/body/1"))
        model["remainingFuel"] = "998"
        executable["trace"][-1:] = [row("ExecIR", "trace", 999, 998, model["trace"][0]["args"], location="handler/0/body/1"),
                                      row("ExecIR", "return", 998, 997, location="program/0/block/0/2")]
        executable["remainingFuel"] = native["remainingFuel"] = "997"
        entry = dict(native["opcodeEvents"][0], instruction=1, source="handler/0/body/1", fuelBefore="999", fuelAfter="999")
        native["opcodeEvents"] += [entry, dict(entry, stage="completed", fuelAfter="998")]
        native["rawSegment"]["traces"].append(dict(native["rawSegment"]["traces"][0], kind="second"))
        self.assertFalse(project(model, executable, native, source)["unmapped"])
        native["rawSegment"]["traces"].reverse()
        p = project(model, executable, native, source)
        self.assertNotEqual(p["exec"], p["native"])
        args = fixtures()
        args[1]["world"]["nextGeneration"] = "999"
        self.assertTrue(project(*args)["unmapped"])

    def test_provider_side_effects_and_unknown_fields_rejected(self):
        args = fixtures()
        args[2]["actions"]["actions"] = [{"unexpected": True}]
        self.assertTrue(project(*args)["unmapped"])
        args = fixtures()
        args[1]["world"]["observations"][0]["hidden"] = 1
        with self.assertRaises(ValueError):
            project(*args)
        with self.assertRaises(ValueError):
            project(*fixtures()[:3])


if __name__ == "__main__":
    unittest.main()
