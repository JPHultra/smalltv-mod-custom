import http.server
import json
import os
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from credentials import CredentialsStore
from usage_daemon import (
    claude_legacy,
    compact_number,
    compact_tokens,
    generic,
    make_metric,
    map_codex_usage,
    map_deepseek_balance,
    normalized,
    path_get,
    collect_claude,
    collect_codex,
    deepseek_key,
    map_claude_usage,
    provider_error,
    read_claude_token,
    read_codex_auth,
    refresh_codex_auth,
    window_label,
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

    def test_codex_mapping(self):
        self.assertEqual(window_label(18000), "5h")
        self.assertEqual(window_label(604800), "7d")
        payload = {
            "plan_type": "plus",
            "rate_limit": {
                "allowed": True,
                "limit_reached": False,
                "primary_window": {
                    "used_percent": 1,
                    "limit_window_seconds": 18000,
                    "reset_after_seconds": 17692,
                    "reset_at": 0,
                },
                "secondary_window": {
                    "used_percent": 45,
                    "limit_window_seconds": 604800,
                    "reset_after_seconds": 382486,
                    "reset_at": 0,
                },
            },
        }
        out = map_codex_usage(payload, {"id": "chatgpt", "name": "Codex"})
        self.assertTrue(out["ok"])
        self.assertEqual(out["status"], "plus")
        self.assertEqual([m["label"] for m in out["metrics"]], ["5h", "7d", "plan"])
        self.assertEqual(out["metrics"][0]["pct"], 1.0)
        self.assertEqual(out["metrics"][0]["resetMin"], 295)
        self.assertEqual(out["metrics"][1]["resetMin"], 6375)
        self.assertEqual(out["metrics"][2]["value"], "plus")

    def test_claude_mapping(self):
        payload = {
            "five_hour": {
                "utilization": 29,
                "resets_at": "2099-01-01T00:00:00+00:00",
            },
            "seven_day": {
                "utilization": 4,
                "resets_at": "2099-01-02T00:00:00+00:00",
            },
            "limits": [{"kind": "session", "severity": "normal"}],
        }
        out = map_claude_usage(payload, {"id": "claude", "name": "Claude"})
        self.assertTrue(out["ok"])
        self.assertEqual(out["status"], "normal")
        self.assertEqual([m["label"] for m in out["metrics"]], ["5h", "7d"])
        self.assertEqual(out["metrics"][0]["pct"], 29.0)
        self.assertEqual(out["metrics"][1]["pct"], 4.0)

    def test_hostile_provider_payloads(self):
        for bad in (
            {"five_hour": "x", "seven_day": []},
            {"five_hour": {"utilization": "nope"}},
            {"five_hour": {"utilization": "inf"}},
            {"limits": {}},
        ):
            with self.assertRaises(ValueError):
                map_claude_usage(bad, {"id": "claude"})
        for bad in (
            {"balance_infos": "x"},
            {"balance_infos": [None]},
            {"balance_infos": [{"total_balance": "nope"}]},
        ):
            with self.assertRaises(ValueError):
                map_deepseek_balance(bad, {"id": "deepseek"})
        with self.assertRaises(ValueError):
            map_codex_usage({"rate_limit": "x"}, {"id": "chatgpt"})
        # Additional windows are opt-in and malformed entries are ignored.
        payload = {
            "rate_limit": {"primary_window": {"used_percent": 1, "limit_window_seconds": 18000}},
            "additional_rate_limits": [
                None, "x", {"limit_name": "extra", "rate_limit": {
                    "primary_window": {"used_percent": 2, "limit_window_seconds": 60},
                }},
            ],
        }
        out = map_codex_usage(payload, {"id": "chatgpt", "includeAdditional": True})
        self.assertEqual([m["label"] for m in out["metrics"]], ["5h", "plan", "extra 1m"])

    def test_no_implicit_home_credentials(self):
        with tempfile.TemporaryDirectory() as directory, patch.dict(
                os.environ, {"DEEPSEEK_API_KEY": "", "CLAUDE_CODE_OAUTH_TOKEN": ""}):
            store = CredentialsStore(os.path.join(directory, "empty.json"))
            with self.assertRaises(ValueError):
                read_claude_token({"id": "claude"}, store)
            with self.assertRaises(ValueError):
                read_codex_auth({"id": "chatgpt"}, store)
            with self.assertRaises(ValueError):
                deepseek_key({"id": "deepseek"}, store)

    def test_store_credentials_and_refresh(self):
        with tempfile.TemporaryDirectory() as directory:
            store = CredentialsStore(os.path.join(directory, "credentials.json"))
            store.update("deepseek", {"apiKey": "sk-store"})
            self.assertEqual(deepseek_key({"id": "deepseek"}, store), "sk-store")

            store.update("claude", {
                "accessToken": "old", "refreshToken": "old-refresh", "expiresAt": 1,
            })
            with patch("usage_daemon._request_claude_refresh", return_value={
                    "access_token": "new", "refresh_token": "new-refresh",
                    "expires_in": 3600}) as refresh:
                self.assertEqual(read_claude_token({"id": "claude"}, store)[2], "new")
                self.assertTrue(refresh.called)
            self.assertEqual(store.get("claude")["accessToken"], "new")
            self.assertEqual(store.get("claude")["refreshToken"], "new-refresh")

            store.update("codex", {
                "accessToken": "old", "refreshToken": "old-refresh", "accountId": "acct",
            })
            with patch("usage_daemon._request_codex_refresh", return_value={
                    "access_token": "new"}) as refresh:
                access, account = refresh_codex_auth(
                    None, {"tokens": {"refresh_token": "old-refresh"}},
                    {"id": "chatgpt"}, 5, store,
                )
                self.assertTrue(refresh.called)
            self.assertEqual((access, account), ("new", "acct"))
            self.assertEqual(store.get("codex")["accessToken"], "new")
            _, _, _, access, account = read_codex_auth({"id": "chatgpt"}, store)
            self.assertEqual((access, account), ("new", "acct"))

    def test_claude_refresh(self):
        payload = {
            "five_hour": {"utilization": 7, "resets_at": "2099-01-01T00:00:00+00:00"},
            "seven_day": {"utilization": 2, "resets_at": "2099-01-02T00:00:00+00:00"},
        }

        class Mock(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _json(self, code, body):
                data = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path == "/usage" and self.headers.get("Authorization") == "Bearer new":
                    self._json(200, payload)
                else:
                    self._json(401, {"error": "expired"})

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length)
                try:
                    data = json.loads(body)
                except ValueError:
                    data = {}
                if (self.path == "/oauth/token"
                        and data.get("grant_type") == "refresh_token"
                        and data.get("client_id") == "9d1c250a-e61b-44d9-88ed-5944d1962f5e"):
                    self._json(200, {"access_token": "new", "refresh_token": "new-refresh",
                                     "expires_in": 3600})
                else:
                    self._json(404, {})

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Mock)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                path = os.path.join(directory, "credentials.json")
                with open(path, "w", encoding="utf-8") as handle:
                    json.dump({"claudeAiOauth": {
                        "accessToken": "old", "refreshToken": "old-refresh",
                        "expiresAt": int(time.time() * 1000) + 3600 * 1000,
                    }}, handle)
                spec = {
                    "id": "claude", "name": "Claude", "adapter": "claude",
                    "authFile": path, "tokenEnv": "SMALLTV_TEST_CLAUDE_TOKEN",
                    "url": f"http://127.0.0.1:{server.server_address[1]}/usage",
                    "tokenUrl": f"http://127.0.0.1:{server.server_address[1]}/oauth/token",
                }
                out = collect_claude(spec, 5)
                self.assertTrue(out["ok"])
                self.assertEqual(out["metrics"][0]["pct"], 7.0)
                with open(path, encoding="utf-8") as handle:
                    credentials = json.load(handle)
                self.assertEqual(credentials["claudeAiOauth"]["accessToken"], "new")
                self.assertEqual(credentials["claudeAiOauth"]["refreshToken"], "new-refresh")
        finally:
            server.shutdown()
            server.server_close()

    def test_codex_refresh(self):
        payload = {
            "plan_type": "plus",
            "rate_limit": {
                "primary_window": {
                    "used_percent": 2, "limit_window_seconds": 18000,
                    "reset_after_seconds": 600, "reset_at": 0,
                }
            },
        }

        class Mock(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _json(self, code, body):
                data = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path == "/usage" and self.headers.get("Authorization") == "Bearer new":
                    self._json(200, payload)
                else:
                    self._json(401, {"error": "expired"})

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                self.rfile.read(length)
                if self.path == "/oauth/token":
                    self._json(200, {"access_token": "new", "refresh_token": "new-refresh"})
                else:
                    self._json(404, {})

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Mock)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory() as directory:
                path = os.path.join(directory, "auth.json")
                with open(path, "w", encoding="utf-8") as handle:
                    json.dump({
                        "auth_mode": "chatgpt",
                        "tokens": {
                            "access_token": "old", "refresh_token": "old-refresh",
                            "account_id": "account",
                        },
                    }, handle)
                spec = {
                    "id": "chatgpt", "name": "Codex", "adapter": "codex",
                    "authFile": path,
                    "url": f"http://127.0.0.1:{server.server_address[1]}/usage",
                    "tokenUrl": f"http://127.0.0.1:{server.server_address[1]}/oauth/token",
                }
                out = collect_codex(spec, 5)
                self.assertTrue(out["ok"])
                self.assertEqual(out["metrics"][0]["label"], "5h")
                with open(path, encoding="utf-8") as handle:
                    auth = json.load(handle)
                self.assertEqual(auth["tokens"]["access_token"], "new")
                self.assertEqual(auth["tokens"]["refresh_token"], "new-refresh")
                self.assertIn("last_refresh", auth)
        finally:
            server.shutdown()
            server.server_close()

    def test_codex_plan_credits_and_additional(self):
        payload = {
            "plan_type": "pro",
            "rate_limit": {
                "primary_window": {"used_percent": 10, "limit_window_seconds": 18000,
                                   "reset_after_seconds": 600},
                "secondary_window": {"used_percent": 20, "limit_window_seconds": 604800,
                                     "reset_after_seconds": 6000},
            },
            "credits": {"has_credits": True, "unlimited": False, "balance": "5.00"},
            "additional_rate_limits": [
                {"limit_name": "codex-mini", "rate_limit": {
                    "primary_window": {"used_percent": 30, "limit_window_seconds": 300,
                                       "reset_after_seconds": 60},
                }},
            ],
        }
        out = map_codex_usage(payload, {
            "id": "chatgpt", "includeCredits": True, "includeAdditional": True,
        })
        self.assertEqual([m["label"] for m in out["metrics"]],
                         ["5h", "7d", "plan", "credits", "codex 5m"])
        self.assertEqual(out["metrics"][2]["value"], "pro")
        self.assertEqual(out["metrics"][3]["value"], "5.00")
        self.assertEqual(out["metrics"][4]["pct"], 30.0)

    def test_deepseek_mapping(self):
        self.assertEqual(compact_number("110.00"), "110")
        self.assertEqual(compact_tokens(1_234_567), "1.2M")
        payload = {
            "is_available": True,
            "balance_infos": [
                {"currency": "CNY", "total_balance": "110.00",
                 "granted_balance": "10.00", "topped_up_balance": "100.00"}
            ],
        }
        out = map_deepseek_balance(
            payload, {"id": "deepseek", "name": "DeepSeek"},
            {"total_tokens": 1_234_567},
        )
        self.assertTrue(out["ok"])
        self.assertEqual([m["label"] for m in out["metrics"]], ["balance", "tokens"])
        self.assertEqual(out["metrics"][0]["value"], "110 CNY")
        self.assertEqual(out["metrics"][1]["value"], "1.2M")
        self.assertEqual(out["metrics"][1]["pct"], 0.0)

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
