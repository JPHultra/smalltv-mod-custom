import json
import os
import tempfile
import unittest

from deepseek_proxy import UsageTracker, usage_from_json, usage_from_sse


class DeepSeekProxyTests(unittest.TestCase):
    def test_usage_json(self):
        self.assertEqual(
            usage_from_json({"usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}}),
            (10, 5, 15),
        )
        self.assertEqual(
            usage_from_json({"usage": {"prompt_tokens": 3, "completion_tokens": 2}}),
            (3, 2, 5),
        )
        self.assertIsNone(usage_from_json({"usage": None}))

    def test_usage_sse(self):
        line = 'data: ' + json.dumps({"usage": {"prompt_tokens": 7, "completion_tokens": 4, "total_tokens": 11}})
        self.assertEqual(usage_from_sse(line), (7, 4, 11))
        self.assertIsNone(usage_from_sse("data: [DONE]"))
        self.assertIsNone(usage_from_sse("event: ping"))

    def test_tracker_persists_and_resets(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "usage.json")
            tracker = UsageTracker(path)
            tracker.add(10, 5, 15)
            tracker.add(2, 3, 5)
            snapshot = tracker.snapshot()
            self.assertEqual(snapshot["prompt_tokens"], 12)
            self.assertEqual(snapshot["completion_tokens"], 8)
            self.assertEqual(snapshot["total_tokens"], 20)
            self.assertEqual(snapshot["requests"], 2)

            reloaded = UsageTracker(path)
            self.assertEqual(reloaded.snapshot()["total_tokens"], 20)
            reloaded.reset()
            self.assertEqual(reloaded.snapshot()["total_tokens"], 0)
            self.assertEqual(reloaded.snapshot()["requests"], 0)


if __name__ == "__main__":
    unittest.main()
