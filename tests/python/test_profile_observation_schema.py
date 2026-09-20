import copy
import json
import re
from pathlib import Path
import unittest

from tests.conformance.profile_observation_schema import (
    KINDS, ObservationSchemaError, symmetric_deletion_control,
    validate_observations, validate_record,
)


def _walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


class ProfileObservationSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        report = Path(__file__).resolve().parents[2] / "tests/python/fixtures/profile-observations.json"
        cls.report = json.loads(report.read_text(encoding="utf-8"))
        # Observation-only schema fixtures are test inputs, not release evidence.
        # Normalize legacy causal.ready defaults for the current schema.
        for obj in _walk(cls.report):
            if set(obj) == {"key", "event", "identity", "causal"}:
                obj["causal"].setdefault("ready", {"time": obj["key"]["time"], "turn": obj["key"]["turn"]})
        cls.rows = cls.report["rows"]
        cls.records = {}
        for obj in _walk(cls.report):
            if set(obj) == {"ordinal", "kind", "value"}:
                previous = cls.records.get(obj["kind"])
                if previous is None or len(json.dumps(obj)) < len(json.dumps(previous)):
                    cls.records[obj["kind"]] = obj

    def test_all_published_shapes_and_symmetric_controls(self):
        self.assertEqual(len(self.rows), 64)
        self.assertEqual(set(KINDS) - set(self.records), {
            "afterCallError", "callError", "host.transport.error", "wire.start.error",
            "commit.error", "output", "runtimeTrace", "trace"})
        for identity, observations in self.rows:
            with self.subTest(identity=identity):
                before = copy.deepcopy(observations)
                validate_observations(identity, observations)
                self.assertTrue(symmetric_deletion_control(identity, observations)["rejected"])
                self.assertEqual(observations, before)

    def test_every_kind_rejects_missing_envelope_and_value_fields(self):
        for kind, record in self.records.items():
            for field in ("ordinal", "kind", "value"):
                changed = copy.deepcopy(record)
                del changed[field]
                with self.subTest(kind=kind, field=field), self.assertRaises(ObservationSchemaError):
                    validate_record(changed)
            if isinstance(record["value"], dict):
                for field in record["value"]:
                    changed = copy.deepcopy(record)
                    del changed["value"][field]
                    with self.subTest(kind=kind, field="value." + field), self.assertRaises(ObservationSchemaError):
                        validate_record(changed)

    def test_nested_payload_and_milestone_fields_are_mandatory(self):
        call = copy.deepcopy(self.records["wireCall"])
        for field in call["value"]["request"]:
            changed = copy.deepcopy(call)
            del changed["value"]["request"][field]
            with self.subTest(field=field), self.assertRaises(ObservationSchemaError):
                validate_record(changed)
        milestone = copy.deepcopy(self.records["milestone.full"])
        for field in milestone["value"]["causal"]:
            changed = copy.deepcopy(milestone)
            del changed["value"]["causal"][field]
            with self.subTest(field=field), self.assertRaises(ObservationSchemaError):
                validate_record(changed)

    def test_empty_unknown_reordered_and_bad_numeric_records_rejected(self):
        identity, original = self.rows[0]
        stream = "records" if "records" in original else "events"
        for replacement in ([], [{}]):
            changed = copy.deepcopy(original)
            changed[stream] = replacement
            with self.assertRaises(ObservationSchemaError):
                validate_observations(identity, changed)
        changed = copy.deepcopy(original)
        changed[stream][0]["kind"] = "unregistered-output"
        with self.assertRaises(ObservationSchemaError):
            validate_observations(identity, changed)
        changed = copy.deepcopy(original)
        changed[stream][0], changed[stream][1] = changed[stream][1], changed[stream][0]
        with self.assertRaises(ObservationSchemaError):
            validate_observations(identity, changed)
        for number in (True, 1, "01", "-1", str(2**64)):
            changed = copy.deepcopy(self.records["wireCall"])
            changed["value"]["id"] = number
            with self.subTest(number=number), self.assertRaises(ObservationSchemaError):
                validate_record(changed)
        changed = copy.deepcopy(self.records["wireCall"])
        changed["value"]["request"]["data"] = ["256"]
        with self.assertRaises(ObservationSchemaError):
            validate_record(changed)

    def test_callback_subrun_and_outer_fields_are_mandatory(self):
        observations = next(o for identity, o in self.rows if identity == "C-T24")
        for direction in ("forward", "reverse"):
            changed = copy.deepcopy(observations)
            del changed[direction]
            with self.assertRaises(ObservationSchemaError):
                validate_observations("C-T24", changed)
        for field in ("stop", "fuel", "records"):
            changed = copy.deepcopy(observations)
            del changed["reverse"][field]
            with self.assertRaises(ObservationSchemaError):
                validate_observations("C-T24", changed)

    def test_scoped_output_cannot_lose_instance_by_switching_to_unscoped_shape(self):
        row = {"ordinal": "0", "kind": "output", "value": {"port": "1", "instance": "2", "value": {"bool": True}}}
        observations = {"events": [row], "stop": "Completed", "fuel": "0"}
        validate_observations("C-T10", observations)
        del row["value"]["instance"]
        with self.assertRaises(ObservationSchemaError):
            validate_observations("C-T10", observations)
        validate_observations("C-T01", {"records": [row], "stop": "Completed", "fuel": "0"})

    def test_all_literal_cpp_emitter_kinds_are_registered(self):
        directory = Path(__file__).resolve().parents[1] / "conformance"
        files = list(directory.glob("profile_*.cpp")) + list(directory.glob("profile_*.hpp"))
        source = "\n".join(path.read_text(encoding="utf-8") for path in files)
        emitted = set(re.findall(r'\.add\("([^"\n]+)"', source))
        self.assertFalse(emitted - set(KINDS))


if __name__ == "__main__":
    unittest.main()
