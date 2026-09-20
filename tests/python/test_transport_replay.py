import copy
import unittest
from pathlib import Path
from tools.leanat.io import read_json
from tools.leanat.io import ToolError
from tools.leanat.transport_replay import fixture, script, replay_transport, compare_model


class TransportReplayValidation(unittest.TestCase):
    def test_both_callback_orders_are_preserved(self):
        forward, reverse = script(fixture(False)), script(fixture(True))
        self.assertNotEqual(forward, reverse)
        self.assertIn("3 2 2 1 4 8 8 8 8", reverse)

    def test_reject_missing_feedback_configuration_and_bounds(self):
        mutations = [lambda p: p["feedback"].pop(),
                     lambda p: p["effectiveConfig"].update(rawDmi=True),
                     lambda p: p["requests"][0].update(data=[9]*65),
                     lambda p: p["callbacks"][0].update(time="03"),
                     lambda p: p["callbacks"][0].update(delay="18446744073709551616"),
                     lambda p: p["feedback"][0].update(sync=1)]
        for mutate in mutations:
            with self.subTest(mutation=mutate):
                plan = copy.deepcopy(fixture())
                mutate(plan)
                with self.assertRaises(ToolError):
                    script(plan)

    def test_capture_hash_checked_before_executing(self):
        with self.assertRaises(ToolError) as failure:
            replay_transport({"schema": "leanat.transport-capture.v1", "coverage": "FullForDeclaredScope",
                              "effectiveConfig": fixture()["effectiveConfig"], "contentSha256": "forged"},
                             "does-not-exist", "does-not-exist", "does-not-exist")
        self.assertIn("hash mismatch", str(failure.exception))

    def test_independent_oracle_compares_wire_and_timed_fields(self):
        root = Path(__file__).parent / "fixtures/transport"
        observed = read_json(root / "forward-runtime.json")
        model = read_json(root / "forward-model.json")
        self.assertEqual(compare_model(observed, model)["status"], "Pass")
        corruptions = [lambda m: m["trace"][0]["payload"]["data"].__setitem__(0, 255),
                       lambda m: m["trace"].remove(next(e for e in m["trace"] if e.get("event") == "requestReleased")),
                       lambda m: next(e for e in m["trace"] if e.get("kind") == "timed").update(keyConnection="999"),
                       lambda m: m["hops"][0]["response"]["data"].__setitem__(0, 254)]
        for mutate in corruptions:
            changed = copy.deepcopy(model)
            mutate(changed)
            self.assertEqual(compare_model(observed, changed)["status"], "Fail")

    def test_historical_sequence_only_oracle_order_is_rejected(self):
        root = Path(__file__).parent / "fixtures/transport"
        result = compare_model(read_json(root / "reverse-runtime.json"), read_json(root / "reverse-model.json"))
        self.assertEqual(result["status"], "Fail")
        self.assertIn("callbackSemanticOrderAndValues", [d["field"] for d in result["differences"]])

    def test_actual_runtime_milestone_keys_and_snapshots_are_compared(self):
        root = Path(__file__).parent / "fixtures/transport"
        observed = read_json(root / "forward-runtime.json")
        model = read_json(root / "forward-model.json")
        mutations = [lambda o: o["milestones"][-1].update(milestoneTime="99"),
                     lambda o: o["milestones"][0]["key"].update(turn="1"),
                     lambda o: o["milestones"][0]["key"].update(connection=2),
                     lambda o: o["milestones"][1]["key"].update(sequence=o["milestones"][0]["key"]["sequence"]),
                     lambda o: o["milestones"][2]["response"]["data"].__setitem__(0, 253)]
        for mutate in mutations:
            changed = copy.deepcopy(observed)
            mutate(changed)
            result = compare_model(changed, model)
            self.assertEqual(result["status"], "Fail")
            self.assertEqual(result["dutMilestones"]["status"], "Fail")


if __name__ == "__main__":
    unittest.main()
