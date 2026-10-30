import unittest

from usage_daemon import (
    claude_legacy,
    generic,
    make_metric,
    normalized,
    path_get,
    provider_error,
)


class UsageDaemonTests(unittest.TestCase):
    def test_path_and_number(self):
        source = {"a": {"b": [{"c": 42}]}, "flag": False}
        self.assertEqual(path_get(source, "a.b"), [{"c": 42}])
        self.assertIsNone(path_get(source, "a.missing"))
        self.assertIsNone(path_get(source, "flag.deeper"))
        self.assertEqual(make_metric("5h", "29.5", "142")["pct"], 29.5)
        self.assertEqual(make_metric("5h", 140, -3)["pct"], 100.0)
        self.assertIsNone(make_metric("5h", None, 0))

    def test_claude_legacy(self):
        spec = {"id": "claude", "name": "Claude"}
        out = claude_legacy({"s": 29, "sr": 142, "w": 4, "wr": 9876, "st": "normal", "ok": True}, spec)
        self.assertTrue(out["ok"])
        self.assertEqual(out["id"], "claude")
        self.assertEqual([m["label"] for m in out["metrics"]], ["5h", "7d"])
        self.assertEqual(out["metrics"][0]["pct"], 29.0)
        self.assertEqual(out["metrics"][0]["resetMin"], 142)
        self.assertFalse(claude_legacy({"ok": False}, spec)["ok"])

    def test_generic_paths(self):
        spec = {
            "id": "chatgpt",
            "name": "ChatGPT",
            "statusPath": "status",
            "metrics": [
                {"label": "5h", "pctPath": "limits.5h.pct", "resetPath": "limits.5h.resetMin"},
                {"label": "7d", "pctPath": "limits.7d.pct"},
            ],
        }
        source = {"status": "normal", "limits": {"5h": {"pct": 62, "resetMin": 88}}}
        out = generic(source, spec)
        self.assertTrue(out["ok"])
        self.assertEqual(out["status"], "normal")
        self.assertEqual(len(out["metrics"]), 1)
        self.assertEqual(out["metrics"][0]["label"], "5h")
        self.assertEqual(out["metrics"][0]["resetMin"], 88)
        with self.assertRaises(ValueError):
            generic({"limits": {}}, spec)

    def test_normalized_and_error(self):
        spec = {"id": "deepseek", "name": "DeepSeek"}
        out = normalized({"name": "DeepSeek API", "metrics": [{"label": "balance", "pct": 81}]}, spec)
        self.assertEqual(out["id"], "deepseek")
        self.assertEqual(out["name"], "DeepSeek API")
        self.assertTrue(out["ok"])
        error = provider_error(spec)
        self.assertFalse(error["ok"])
        self.assertEqual(error["id"], "deepseek")


if __name__ == "__main__":
    unittest.main()
