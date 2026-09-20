import copy
import unittest
from tests.conformance.opcode_projection_protocol import project


def handle(kind, store, slot=0):
    return dict(kind=str(kind), domain="1", store=str(store), slot=str(slot), generation="1", owner="9")


def value(v):
    if isinstance(v, bool):
        return {"kind": "bool", "value": v}
    if isinstance(v, dict):
        return {"kind": "handle", "identity": v}
    return {"kind": "bits", "width": "64", "value": str(v)}


def fixture():
    source, result, consumer = handle(3, 10), handle(5, 20), handle(6, 21)
    record = [value(source), value(2), value(4096), value(True), value(True),
              {"kind": "bytes", "data": [4, 9]}, value(0), value(0), value(0)]
    outcome = {"world": {"maxPins": "256", "objects": [
        {"identity": result, "tag": "storage.result", "alive": True,
         "value": {"kind": "record", "fields": record}},
        {"identity": consumer, "tag": "storage.consumer", "alive": True,
         "value": {"kind": "record", "fields": [value(result), value(False)]}}]}}
    identities = [{"name": n, "identity": h} for n, h in
                  [("result", result), ("consumer", consumer), ("resultSource", source)]]
    provider = {"ledgers": [], "drains": [], "ack": None, "resultStore": {
        "results": [{"identity": result, "alive": True, "producerAlive": True,
                     "publishing": False, "value": {"bytes": [4, 9]},
                     "ready": {"time": "0", "turn": "0"}, "consumerCount": "1", "pinCount": "0",
                     "create": {"source": source, "type": "2", "maxBytes": "4096",
                                "producerOwner": "9", "consumerOwner": "9"}}],
        "consumers": [{"identity": consumer, "result": result, "active": True}],
        "nextResultGeneration": "2", "nextConsumerGeneration": "2", "pinCount": "0", "pinLimit": "256"}}
    native = {"provider": provider, "initialProvider": copy.deepcopy(provider),
              "identities": identities, "initialIdentities": copy.deepcopy(identities),
              "actions": {}, "hostEvents": []}
    return outcome, copy.deepcopy(outcome), native


class ProtocolProjectionControls(unittest.TestCase):
    def test_host_observation_order_and_initial_prefix_are_authoritative(self):
        from tests.conformance.opcode_projection_protocol_state import host_reference
        event = lambda time: {"kind": "protocol.host.arm", "values": [value(time), value(0), value(2)]}
        initial = {"observations": [{"kind": "input", "values": []}]}
        world = {"observations": initial["observations"] + [event(3), event(9)]}
        first = host_reference(world, initial)
        world["observations"][1:] = reversed(world["observations"][1:])
        self.assertNotEqual(first, host_reference(world, initial))
        world["observations"][0] = event(0)
        with self.assertRaises(ValueError):
            host_reference(world, initial)

    def test_unused_result_slots_preserve_capacity_and_invalid_state(self):
        from tests.conformance.opcode_projection_protocol_state import result_native
        _, _, output = fixture()
        provider = output["provider"]
        pool = provider["resultStore"]
        # Result and consumer are one physical store, but distinct typed slot namespaces.
        pool["consumers"][0]["identity"]["store"] = "20"
        blank = copy.deepcopy(pool["results"][0])
        blank.update(alive=False, producerAlive=False, publishing=False, value=None,
                     ready=None, consumerCount="0", pinCount="0")
        blank["identity"].update(slot="1", generation="0", owner="0")
        zero = {key: "0" for key in ("kind", "domain", "store", "slot", "generation", "owner")}
        blank["create"] = dict(source=zero, type="0", maxBytes="4096", producerOwner="0", consumerOwner="0")
        pool["results"].append(blank)
        clean = result_native(provider, {"domain": "1"})
        self.assertEqual(clean["unusedSlotViolations"], [])
        self.assertEqual(clean["unusedResultSlots"], ["1"])
        for mutation in ("publishing", "alive"):
            blank[mutation] = True
            self.assertNotEqual(clean, result_native(provider, {"domain": "1"}))
            blank[mutation] = False
        pool["nextConsumerGeneration"] = "999"
        self.assertNotEqual(clean, result_native(provider, {"domain": "1"}))
    def test_typed_result_query_detects_publication_and_consumer_retirement(self):
        from tests.conformance.opcode_projection_protocol_state import result_query_reference
        outcome, _, _ = fixture()
        world = outcome["world"]
        world["allocationRules"] = [{"group": group, "capacity": "1"}
            for group in ("storage.result", "storage.consumer")]
        world["allocationCounters"] = [{"group": group, "nextGeneration": "2"}
            for group in ("storage.result", "storage.consumer")]
        fields = world["objects"][0]["value"]["fields"]
        fields.extend([value(9), value(9), value(False)])
        ready = result_query_reference(world)
        self.assertEqual(ready["read"], {"value": [4, 9]})
        world["objects"][1]["alive"] = False
        released = result_query_reference(world)
        self.assertEqual(released["read"]["error"]["code"], "6")
        self.assertEqual(released["ownership"]["consumers"], "0")
        world["objects"][1]["alive"] = True
        fields[4] = value(False)
        unpublished = result_query_reference(world)
        self.assertEqual(unpublished["read"]["error"]["code"], "7")
        self.assertFalse(unpublished["ownership"]["published"])
    def test_invalid_input_identity_mapping_preserves_displacement(self):
        a, b, native = fixture()
        baseline = copy.deepcopy(a["world"]["objects"][1]["identity"])
        forged = {**baseline, "generation": "2"}
        native["preparedInputs"] = [{"kind": "handle", "identity": copy.deepcopy(forged)}]
        input_value = {"inputs": [{"kind": "handle", "identity": forged}], "world": a["world"]}
        projected = project(a, b, native, input_value)
        self.assertTrue(any(e["role"] == "invalidInput.0" for e in projected["identities"]))
        native["preparedInputs"][0]["identity"]["generation"] = baseline["generation"]
        with self.assertRaisesRegex(ValueError, "displacement"):
            project(a, b, native, input_value)
    def test_setup_codec_preserves_return_delay_and_milestone_order(self):
        from tests.conformance.opcode_projection_protocol_state import setup_reference
        record = lambda fields: {"kind": "record", "fields": fields}
        vec = lambda fields: {"kind": "vec", "values": fields}
        unit = {"kind": "unit"}
        data = {"kind": "bytes", "data": [4, 9]}
        response = record([value(1), data, value(False), vec([])])
        request = record([value(0), value(42), data, value(2), {"kind": "bytes", "data": []}, value(0), value(False), vec([])])
        call = record([value(1), value(1), value(1), value(0), value(1), value(10), value(2), request])
        returned = record([value(1), value(3), value(5), response])
        milestones = vec([record([value(0), value(15), value(True), unit]), record([value(1), value(15), value(True), response])])
        exchange = record([value(1), value(handle(1, 13)), value(3), milestones, vec([]), value(True), value(False), value(False)])
        world = {"observations": [{"kind": "protocol.setup.exchange", "values": [call, returned, exchange]}]}
        first = setup_reference(world)
        self.assertEqual(first[1]["value"]["outgoingDelay"], "5")
        returned["fields"][2] = value(6)
        self.assertNotEqual(first, setup_reference(world))
        returned["fields"][2] = value(5)
        milestones["values"].reverse()
        self.assertNotEqual(first, setup_reference(world))
    def test_semantic_values_match_but_unmapped_blocks_complete_claim(self):
        p = project(*fixture())
        self.assertEqual(p["model"], p["native"])
        self.assertTrue(p["unmapped"])

    def test_lifecycle_and_capacity_changes_are_detected(self):
        for key, replacement in [("active", False)]:
            a, b, n = fixture()
            n["provider"]["resultStore"]["consumers"][0][key] = replacement
            p = project(a, b, n)
            self.assertNotEqual(p["model"], p["native"])
        a, b, n = fixture()
        n["provider"]["resultStore"]["results"][0]["create"]["maxBytes"] = "4095"
        p = project(a, b, n)
        self.assertNotEqual(p["model"], p["native"])

    def test_owner_and_generation_mutations_cannot_be_normalized_away(self):
        for field in ("owner", "generation"):
            a, b, n = fixture()
            n["identities"][0]["identity"][field] = "99"
            with self.assertRaises(ValueError):
                project(a, b, n)

    def test_actual_causal_order_remains_in_unmapped_evidence(self):
        a, b, n = fixture()
        n["hostEvents"] = [{"ordinal": "1"}, {"ordinal": "2"}]
        first = project(a, b, n)
        n["hostEvents"].reverse()
        second = project(a, b, n)
        self.assertNotEqual(first["unmapped"], second["unmapped"])


if __name__ == "__main__":
    unittest.main()
