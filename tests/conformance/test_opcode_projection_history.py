"""Reject forged zero-cost control flow and rolled-back capability histories."""
import copy
import unittest
from opcode_projection_core import _discarded_wait_identity
from opcode_projection_pure import _trace_cost


class JoinHistoryTests(unittest.TestCase):
    def fixture(self):
        def row(operation, before, after, path="handler/0/body/0"):
            return dict(layer="ModelIR", program="0", location=path, operation=operation,
                        args=[], results=[], fuelBefore=str(before), fuelAfter=str(after))
        return dict(remainingFuel="96", ok=True, trace=[row("branch", 100, 99),
            row("literal", 99, 98, "handler/0/body/0/yes/0/expr/0"),
            row("writeState", 98, 97, "handler/0/body/0/yes/0"), row("join", 97, 97),
            row("ret", 97, 96, "handler/0/body/1")])

    def test_legitimate_zero_host_cost_join(self):
        gaps = []
        _trace_cost(self.fixture(), 100, "ModelIR", gaps)
        self.assertEqual(gaps, [])

    def test_join_shape_location_cost_and_omission_mutations(self):
        for field, value in (("operation", "literal"), ("location", "other"), ("args", [1]),
                             ("results", [1]), ("fuelBefore", "98"), ("fuelAfter", "98")):
            outcome = self.fixture()
            outcome["trace"][3][field] = value
            gaps = []
            _trace_cost(outcome, 100, "ModelIR", gaps)
            self.assertTrue(gaps, field)
        for duplicated in (False, True):
            outcome = self.fixture()
            if duplicated:
                outcome["trace"].insert(4, copy.deepcopy(outcome["trace"][3]))
            else:
                outcome["trace"].pop(3)
            gaps = []
            _trace_cost(outcome, 100, "ModelIR", gaps)
            self.assertTrue(gaps)


def wait_fixture():
    def handle(kind, store, generation):
        return dict(kind=kind, store=str(store), generation=str(generation), domain="1", slot="0", owner="7")
    logical_process, actual_process = handle(3, 10, 1), handle(3, 4, 1)
    logical_wait, actual_wait = handle(4, 11, 2), handle(4, 4, 2)
    typed = lambda h: dict(kind="handle", identity=h)
    entry = dict(role="process", model=logical_process, exec=logical_process, native=actual_process)
    args = [typed(logical_process), dict(kind="bits", width=64, value="3"), dict(kind="bits", width=64, value="1")]
    actual_args = [typed(actual_process), *args[1:]]
    counter = lambda n: dict(group="runtime.process", domain="1", slot=None, nextGeneration=str(n), persistent=True, retired=False)
    source = dict(world=dict(objects=[dict(identity=logical_process, tag="reference.process", alive=True)],
        allocationRules=[dict(kind=4, store="11", group="runtime.process", perSlot=False, persistent=True)],
        allocationCounters=[counter(2)]))
    def reference(operation):
        world = copy.deepcopy(source["world"])
        world["allocationCounters"] = [counter(3)]
        return dict(ok=False, error="RegisteredWaitWithoutSuspension", world=world,
                    trace=[dict(operation=operation, program="0", location="site", args=args, results=[typed(logical_wait)])])
    initial = dict(wait=None, waits="0", process=dict(identity=actual_process),
                   processCounters=dict(domain="1", store="4", nextGeneration="2"))
    final = copy.deepcopy(initial)
    final["processCounters"]["nextGeneration"] = "3"
    issued = dict(stage="completed", program=0, block=0, instruction=0, callDepth=0, opcode=39,
                  source="site", fuelBefore="100", arguments=actual_args, result=typed(actual_wait), error=None)
    native = dict(ok=False, error=dict(code="8", message="registered wait lacks atomic Suspend"),
                  initialProvider=initial, provider=final,
                  opcodeEvents=[dict(issued, stage="entered", result=None), issued],
                  rawSegment=dict(kind="0", values=[dict(handle=actual_wait)]))
    return reference("serviceCall:leanat.core.wait.timer"), reference("registerWait"), native, source, entry


class DiscardedWaitTests(unittest.TestCase):
    def test_actual_issuance_burn_and_rollback_produce_full_identity_role(self):
        entry = _discarded_wait_identity(*wait_fixture())
        self.assertEqual(entry["role"], "discarded.wait")
        self.assertEqual(entry["native"]["store"], "4")
        self.assertEqual(entry["exec"]["store"], "11")
        self.assertEqual(entry["native"]["generation"], "2")

    def test_identity_authority_liveness_and_history_mutations_rejected(self):
        for field, value in (("owner", "8"), ("generation", "3"), ("store", "5"), ("domain", "2"), ("slot", "1"), ("kind", 3)):
            values = wait_fixture()
            values[2]["opcodeEvents"][1]["result"]["identity"][field] = value
            with self.assertRaises(ValueError, msg=field):
                _discarded_wait_identity(*values)
        for mutation in (lambda n: n["provider"]["processCounters"].update(nextGeneration="2"),
                         lambda n: n["provider"].update(waits="1"),
                         lambda n: n["opcodeEvents"].reverse(),
                         lambda n: n["rawSegment"].update(values=[])):
            values = wait_fixture()
            mutation(values[2])
            with self.assertRaises(ValueError):
                _discarded_wait_identity(*values)
        values = wait_fixture()
        values[3]["world"]["allocationCounters"][0]["nextGeneration"] = "1"
        with self.assertRaises(ValueError):
            _discarded_wait_identity(*values)
