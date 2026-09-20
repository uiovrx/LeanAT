import copy
import unittest
from tests.conformance.opcode_projection_payload import project


def handle(kind, store):
    return dict(kind=str(kind), domain="1", store=str(store), slot="0", generation="1", owner="9")


def ref(value):
    if value is None:
        return {"kind": "unit"}
    if isinstance(value, bool):
        return {"kind": "bool", "value": value}
    if isinstance(value, int):
        return {"kind": "bits", "width": 64, "value": str(value)}
    if isinstance(value, bytes):
        return {"kind": "bytes", "data": list(value)}
    if isinstance(value, dict):
        return {"kind": "handle", "identity": value}
    return {"kind": "record", "fields": [ref(x) for x in value]}


def native(value):
    if value is None:
        return {"unit": None}
    if isinstance(value, bool):
        return {"bool": value}
    if isinstance(value, int):
        return {"u64": str(value)}
    if isinstance(value, bytes):
        return {"bytes": [str(x) for x in value]}
    if isinstance(value, dict):
        return {"handle": value}
    return {"array": [native(x) for x in value]}


def fixture():
    txn, hop = handle(0, 40), handle(1, 41)
    record = [txn, hop, 0, 7, True, True, 0, 4096, 4, bytes([1, 2, 3, 4]),
              bytes([9, 2, 9, 4]), bytes([255, 0]), 0, False, 65536,
              [[b"trace", 4, True, True, True, bytes([8, 7])]], 0]
    outcome = {"world": {"objects": [{"identity": handle(1, 22), "tag": "storage.payload",
                                      "value": ref(record), "alive": True}],
                         "events": [], "observations": [], "allocationRules": [], "allocationCounters": [], "nextGeneration": "2048",
                         "generationLimit": str(2**64), "nextSequence": "1", "maxObjects": "1024",
                         "maxPins": "256", "maxEvents": "1024", "maxBytes": "1048576",
                         "maxObservations": "100000"}}
    actual = {"provider": {"payload": native(record), "events": []},
              "actions": {"actions": [], "cursor": "0", "failed": False}, "hostEvents": [],
              "identities": {"transaction": txn, "hop": hop}}
    return outcome, copy.deepcopy(outcome), copy.deepcopy(actual)


class PayloadProjectionTests(unittest.TestCase):
    def assert_different(self, sample):
        result = project(*sample)
        self.assertTrue(result["unmapped"] or result["model"] != result["native"])

    def test_complete_semantics(self):
        result = project(*fixture())
        self.assertEqual(result["unmapped"], [])
        self.assertEqual(result["model"], result["exec"])
        self.assertEqual(result["model"], result["native"])
        self.assertEqual(len(result["identities"]), 2)
        self.assertIn("referenceRepresentation", result["diagnostics"])

    def test_every_field_mutation_is_visible(self):
        changes = {2: 1, 3: 8, 4: False, 5: False, 6: 1, 7: 4097, 8: 3,
                   9: bytes([0, 2, 3, 4]), 10: bytes([9, 2, 0, 4]), 11: bytes([0, 255]),
                   12: 1, 13: True, 14: 65535, 16: 1}
        for field, changed in changes.items():
            with self.subTest(field=field):
                sample = fixture()
                sample[2]["provider"]["payload"]["array"][field] = native(changed)
                self.assert_different(sample)

    def test_authority_and_generation_never_erased(self):
        for field in ("kind", "domain", "store", "slot", "generation", "owner"):
            sample = fixture()
            raw = sample[2]["provider"]["payload"]["array"][0]["handle"]
            raw[field] = str(int(raw[field]) + 1)
            sample[2]["identities"]["transaction"] = copy.deepcopy(raw)
            self.assert_different(sample)

    def test_extension_rules_capacity_and_owned_bytes(self):
        for field, changed in ((0, b"other"), (1, 5), (2, False), (3, False), (4, False),
                               (5, None), (5, bytes([8, 6]))):
            sample = fixture()
            ext = sample[2]["provider"]["payload"]["array"][15]["array"][0]["array"]
            ext[field] = native(changed)
            self.assert_different(sample)

    def test_unmapped_fields_and_external_effects_fail_closed(self):
        for change in (lambda x: x["provider"].update(extra=[]),
                       lambda x: x["provider"]["payload"]["array"].append(native(0)),
                       lambda x: x["provider"]["events"].append({"unexpected": True}),
                       lambda x: x["actions"]["actions"].append({"send": True}),
                       lambda x: x["hostEvents"].append({"output": True}),
                       lambda x: x["identities"].update(extra=handle(5, 90))):
            sample = fixture()
            change(sample[2])
            self.assertTrue(project(*sample)["unmapped"])

    def test_error_agreement_requires_exact_cause(self):
        sample = fixture()
        for ref_error, code, message in (("PayloadWrongLocalView", "4", "payload transaction is not current view"), ("PayloadDataLength", "21", "payload data shape"), ("PayloadWritePermission", "8", "extension response write permission")):
            sample[0].update(ok=False, error=ref_error)
            sample[1].update(ok=False, error=ref_error)
            sample[2].update(ok=False, error={"code": code, "message": message})
            self.assertTrue(project(*sample)["errorAgreement"]["verified"] )
            sample[2]["error"]["message"] = "different cause"
            self.assertNotIn("errorAgreement", project(*sample))
        sample[2]["error"] = {"code": "2", "message": "capacity"}
        self.assertNotIn("errorAgreement", project(*sample))

    def test_reference_bookkeeping_retained_and_checked(self):
        sample = fixture()
        sample[1]["world"]["nextGeneration"] = "2049"
        self.assertTrue(project(*sample)["unmapped"])

    def test_error_source_suffix_requires_matching_execution_witnesses(self):
        sample = fixture()
        error = {"code": "4", "message": "payload local view (program 0, block 2, opcode 24, handler/0/body/0)"}
        for outcome, operation in zip(sample[:2], ("error:leanat.payload.data.get", "error:payloadGet")):
            outcome.update(ok=False, error="PayloadWrongLocalView", trace=[{
                "program": "0", "location": "handler/0/body/0", "operation": operation}])
        sample[2].update(ok=False, error=error, opcodeEvents=[{
            "stage": "error", "program": 0, "block": 2, "opcode": 24,
            "source": "handler/0/body/0", "error": copy.deepcopy(error)}])
        self.assertTrue(project(*sample)["errorAgreement"]["verified"])
        for mutate in (lambda s: s[2]["error"].update(code="8"),
                       lambda s: s[2]["error"].update(message="other cause (program 0, block 2, opcode 24, handler/0/body/0)"),
                       lambda s: s[2]["opcodeEvents"][0].update(block=3),
                       lambda s: s[2]["opcodeEvents"][0].update(opcode=25),
                       lambda s: s[0]["trace"][0].update(location="handler/0/body/1"),
                       lambda s: s[1]["trace"][0].update(operation="error:extensionGet")):
            changed = copy.deepcopy(sample)
            mutate(changed)
            self.assertNotIn("errorAgreement", project(*changed))
        sample = fixture()
        sample[0]["world"]["unknown"] = 0
        self.assertTrue(project(*sample)["unmapped"])


if __name__ == "__main__":
    unittest.main()


