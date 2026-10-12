import json
import unittest

from tools.stretch_input import stretch_native, stretch_oracle


class StretchInputTests(unittest.TestCase):
    def test_native_preserves_start_and_holds_buttons_and_touch(self):
        source = "10 8 -1 -1\n12 1 40 50\n"
        rows = [tuple(map(int, line.split())) for line in stretch_native(source, 3).splitlines()]
        self.assertEqual(rows, [
            (10, 8, -1, -1), (11, 8, -1, -1),
            (12, 9, 40, 50), (13, 1, 40, 50), (14, 1, 40, 50),
        ])

    def test_oracle_preserves_start_and_merges_overlapping_events(self):
        source = json.dumps([
            {"frame": 10, "buttons": ["START"]},
            {"frame": 12, "buttons": ["A"], "touch": [40, 50]},
        ])
        actions = json.loads(stretch_oracle(source, 3))
        self.assertEqual(actions, [
            {"frame": 10, "buttons": ["START"]},
            {"frame": 11, "buttons": ["START"]},
            {"frame": 12, "buttons": ["A", "START"], "touch": [40, 50]},
            {"frame": 13, "buttons": ["A"], "touch": [40, 50]},
            {"frame": 14, "buttons": ["A"], "touch": [40, 50]},
        ])



    def test_touch_precedence_uses_latest_overlapping_touch(self):
        source = "0 0 10 11\n1 0 -1 -1\n2 0 20 21\n"
        rows = [tuple(map(int, line.split())) for line in stretch_native(source, 4).splitlines()]
        self.assertEqual(rows[2], (2, 0, 20, 21))
        self.assertEqual(rows[4], (4, 0, 20, 21))
        oracle = json.loads(stretch_oracle(json.dumps([
            {"frame": 0, "touch": [10, 11]},
            {"frame": 1, "buttons": ["A"]},
            {"frame": 2, "touch": [20, 21]},
        ]), 4))
        self.assertEqual(oracle[2]["touch"], [20, 21])
        self.assertEqual(oracle[4]["touch"], [20, 21])

    def test_native_malformed_rows_include_line_context(self):
        for source in ("0 1 -1\n", "0 1 nope -1\n", "-1 1 -1 -1\n", "0 -1 -1 -1\n"):
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, "line 1"):
                stretch_native(source)

    def test_oracle_malformed_actions_include_action_context(self):
        for source in ('[{}]', '[{"frame": -1}]', '[{"frame": 0, "buttons": "A"}]',
                       '[{"frame": 0, "touch": [256, 1]}]', '[null]'):
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, "action 1"):
                stretch_oracle(source)

    def test_oracle_action_errors_report_source_line(self):
        source = '[\n  {"frame": 0, "unknown": 1}\n]'
        with self.assertRaisesRegex(ValueError, "line 2 action 1"):
            stretch_oracle(source)
if __name__ == "__main__":
    unittest.main()
