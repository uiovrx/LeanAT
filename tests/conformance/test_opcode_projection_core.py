"""Mutation checks for the comparator, not conformance execution evidence."""
import copy
import unittest
from opcode_projection_core import project, _error_agreement


def handle(store=1, owner=7):
    return dict(kind="2", domain="1", store=str(store), slot="0", generation="2", owner=str(owner))


def fixtures():
    h = handle()
    event = dict(identity=h, time="9", turn="0", stage="3", instanceId="1", connection="1",
                 sequence="2", kind="storage.event", values=[dict(kind="bytes", data=[4, 5])],
                 source="", cancelled=False)
    rules = [dict(kind=kind, store=str(store), group=group, perSlot=per_slot, persistent=True,
                  allowMax=allow_max, capacity=str(capacity)) for kind, store, group, per_slot, allow_max, capacity in
             [(2, 1, "runtime.event", True, True, 8), (3, 10, "runtime.process", False, False, 64),
              (4, 11, "runtime.process", False, False, 128)]]
    counters = [dict(group="runtime.process", domain="1", slot=None, nextGeneration="1", persistent=True, retired=False),
                dict(group="runtime.event", domain="1", slot="0", nextGeneration="3", persistent=True, retired=False)]
    reference = dict(world=dict(maxEvents="8", nextGeneration="3", nextSequence="3", allocationRules=rules,
                    allocationCounters=counters, events=[event], objects=[dict(identity=h, tag="reference.event",
                    alive=True, value=dict(kind="bits", width=64, value="2"))], observations=[]))
    native = dict(provider=dict(eventCapacity="8", events=[dict(token=handle(80), key=dict(time="9", turn="0", stage="3",
        instance="1", connection="1", sequence="2"), value=dict(bytes=["4", "5"]), owner="7",
        epoch="0", cancelled=False, state="queued")], process=None, wait=None, frames="0", waits="0"))
    native["provider"].update(nextSequence="3", processCounters=dict(domain="1", store="81", nextGeneration="1",
        frameCapacity="64", waitCapacity="128", liveCapacity="64", frames="0", waits="0"),
        eventSlots=[dict(token=handle(80), state="queued")] +
          [dict(token=dict(handle(80), slot=str(slot), generation="0"), state="free") for slot in range(1, 8)])
    return reference, copy.deepcopy(reference), native


class ProjectionTests(unittest.TestCase):
    def test_error_equivalence_requires_exact_cause(self):
        reference = dict(ok=False, error="TimeRegression")
        native = dict(ok=False, error=dict(code="10", message="past tick (program 0, block 0, opcode 35)"))
        self.assertTrue(_error_agreement(reference, reference, native)["verified"])
        native["error"]["code"] = "1"
        self.assertIsNone(_error_agreement(reference, reference, native))
        unsupported = dict(ok=False, error="UnsupportedRuntimeOperands")
        self.assertIsNone(_error_agreement(unsupported, unsupported, native))

    def test_equivalent_key_and_payload(self):
        result = project(*fixtures())
        self.assertEqual(result["model"], result["exec"])
        self.assertEqual(result["model"], result["native"])
        self.assertFalse(result["unmapped"])

    def test_each_key_payload_and_cancel_mutation_is_visible(self):
        for field in ("time", "turn", "stage", "instance", "connection", "sequence"):
            model, executable, native = fixtures()
            native["provider"]["events"][0]["key"][field] = "99"
            result = project(model, executable, native)
            self.assertNotEqual(result["model"], result["native"], field)
        for field, value in (("cancelled", True), ("value", {"bytes": ["4", "6"]})):
            model, executable, native = fixtures()
            native["provider"]["events"][0][field] = value
            result = project(model, executable, native)
            self.assertNotEqual(result["model"], result["native"], field)

    def test_authority_and_occupancy_are_not_erased(self):
        model, executable, native = fixtures()
        native["provider"]["events"][0]["token"]["owner"] = "8"
        with self.assertRaises(ValueError):
            project(model, executable, native)
        model, executable, native = fixtures()
        native["provider"]["waits"] = "1"
        self.assertIn("wait capacity occupancy differs", project(model, executable, native)["unmapped"])

    def test_missing_event_is_unmapped(self):
        model, executable, native = fixtures()
        native["provider"]["events"] = []
        self.assertIn("event count differs", project(model, executable, native)["unmapped"])

    def test_allocator_burns_and_retirement_are_not_erased(self):
        model, executable, native = fixtures()
        native["provider"]["processCounters"]["nextGeneration"] = "2"
        result = project(model, executable, native)
        self.assertNotEqual(result["model"]["allocator"], result["native"]["allocator"])
        model, executable, native = fixtures()
        native["provider"]["eventSlots"][0]["token"]["generation"] = "3"
        result = project(model, executable, native)
        self.assertNotEqual(result["model"]["allocator"], result["native"]["allocator"])
        model, executable, native = fixtures()
        native["provider"]["eventSlots"][1]["state"] = "retired"
        self.assertIn("native event slot retired before exhaustion", project(model, executable, native)["unmapped"])

    def test_process_liveness_and_ordinal_are_observable(self):
        model, executable, native = fixtures()
        proc = dict(handle(10), kind="3", generation="1")
        none = dict(kind="variant", tag=0, fields=[])
        fields = [dict(kind="bits", width=64, value="0"), dict(kind="bits", width=64, value="0"),
                  none, none, none, dict(kind="bits", width=64, value="0")]
        entry = dict(identity=proc, tag="reference.process", alive=True,
                     value=dict(kind="record", fields=fields))
        model["world"]["objects"].append(entry)
        executable["world"]["objects"].append(copy.deepcopy(entry))
        model["world"]["allocationCounters"][0]["nextGeneration"] = "2"
        executable["world"]["allocationCounters"][0]["nextGeneration"] = "2"
        native["provider"]["processCounters"].update(nextGeneration="2", frames="1")
        native["provider"].update(frames="1", process=dict(identity=dict(proc, store="81"),
                                  program="0", state="1", ordinal="0", wait=None))
        matched = project(model, executable, native)
        self.assertEqual(matched["model"], matched["native"])
        native["preparedInputs"] = [dict(handle=dict(proc, store="81", owner="8"))]
        variants = project(model, executable, native)["identities"]
        mutation = next(entry for entry in variants if ".input." in entry["role"])
        self.assertEqual(mutation["model"], dict(proc, owner="8"))
        self.assertEqual(mutation["native"], dict(proc, store="81", owner="8"))
        native["provider"]["process"]["ordinal"] = "1"
        self.assertNotEqual(project(model, executable, native)["model"],
                            project(model, executable, native)["native"])
        native["provider"]["process"].update(ordinal="0", state="2")
        self.assertNotEqual(project(model, executable, native)["model"],
                            project(model, executable, native)["native"])

    def test_completion_requires_retirement_and_owned_result(self):
        model, executable, native = fixtures()
        proc = dict(handle(10), kind="3", generation="1")
        bit = lambda n: dict(kind="bits", width=64, value=str(n))
        for outcome in (model, executable):
            outcome["world"]["objects"].append(dict(identity=proc, tag="reference.process",
                alive=False, value=dict(kind="unit")))
            outcome["world"]["observations"].append(dict(kind="process.completed", opcode="return",
                source="", time="4", turn="2", values=[dict(kind="handle", identity=proc),
                bit(0), bit(0), dict(kind="record", fields=[bit(41)])]))
            outcome["world"]["allocationCounters"][0]["nextGeneration"] = "2"
        native["provider"]["processCounters"].update(nextGeneration="2", frames="0")
        native["provider"]["process"] = dict(identity=dict(proc, store="81"), retired=True,
            program="0", ordinal="0", completedValues=[dict(integer="41")],
            completedReady=dict(time="4", turn="2"))
        result = project(model, executable, native)
        self.assertFalse(result["unmapped"])
        self.assertEqual(result["model"], result["native"])
        native["provider"]["process"]["completedReady"]["turn"] = "3"
        result = project(model, executable, native)
        self.assertNotEqual(result["model"], result["native"])
        model["world"]["objects"][-1]["value"] = dict(kind="record", fields=[])
        self.assertTrue(project(model, executable, native)["unmapped"])


if __name__ == "__main__":
    unittest.main()


