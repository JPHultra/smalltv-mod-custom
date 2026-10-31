import json
import os
import tempfile
import unittest

from deepseek_proxy import (
    UsageTracker,
    anthropic_event,
    usage_from_anthropic_json,
    usage_from_json,
    usage_from_sse,
)


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

    def test_usage_anthropic(self):
        self.assertEqual(
            usage_from_anthropic_json({"usage": {"input_tokens": 10, "output_tokens": 5}}),
            (10, 5, 15),
        )
        state = {}
        self.assertIsNone(anthropic_event(
            {"type": "message_start", "message": {"usage": {"input_tokens": 10, "output_tokens": 1}}},
            state,
        ))
        self.assertEqual(
            anthropic_event({"type": "message_delta", "usage": {"output_tokens": 5}}, state),
            (10, 5, 15),
        )
        self.assertIsNone(anthropic_event({"type": "message_stop"}, state))

    def test_usage_sse(self):
        line = 'data: ' + json.dumps({"usage": {"prompt_tokens": 7, "completion_tokens": 4, "total_tokens": 11}})
        self.assertEqual(usage_from_sse(line), (7, 4, 11))
        self.assertIsNone(usage_from_sse("data: [DONE]"))
        self.assertIsNone(usage_from_sse("event: ping"))

    def test_hostile_stream_and_state(self):
        self.assertIsNone(usage_from_sse(None))
        self.assertIsNone(usage_from_sse("data: {bad"))
        self.assertIsNone(usage_from_sse("data: null"))
        self.assertIsNone(usage_from_sse("data: []"))
        self.assertIsNone(usage_from_sse("event: error"))
        self.assertIsNone(usage_from_json({"usage": {"prompt_tokens": "inf"}}))
        self.assertIsNone(usage_from_json({"usage": {"prompt_tokens": float("inf")}}))
        self.assertIsNone(usage_from_json({"usage": {"prompt_tokens": []}}))
        state = {}
        self.assertIsNone(anthropic_event({"type": "message_start", "message": "x"}, state))
        self.assertIsNone(anthropic_event({"type": "message_delta", "usage": "x"}, state))
        self.assertIsNone(anthropic_event(None, state))
        self.assertIsNone(anthropic_event({"type": "message_delta", "usage": {"output_tokens": 5}}, None))

        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "usage.json")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("[1,2,3]")
            self.assertEqual(UsageTracker(path).snapshot()["total_tokens"], 0)
            with open(path, "w", encoding="utf-8") as handle:
                handle.write('{"prompt_tokens":"x","days":{"x":{"total_tokens":"y"}}}')
            tracker = UsageTracker(path)
            self.assertEqual(tracker.snapshot()["total_tokens"], 0)
            self.assertEqual(tracker.snapshot()["days"]["x"]["total_tokens"], 0)

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
            self.assertEqual(snapshot["today"]["total_tokens"], 20)

            reloaded = UsageTracker(path)
            self.assertEqual(reloaded.snapshot()["total_tokens"], 20)
            self.assertEqual(reloaded.snapshot()["today"]["total_tokens"], 20)
            reloaded.reset()
            self.assertEqual(reloaded.snapshot()["total_tokens"], 0)
            self.assertEqual(reloaded.snapshot()["requests"], 0)


if __name__ == "__main__":
    unittest.main()
