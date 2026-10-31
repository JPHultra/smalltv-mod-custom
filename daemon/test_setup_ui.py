import json
import os
import tempfile
import threading
import unittest
import urllib.error
import urllib.request

from credentials import CredentialsStore
from setup_ui import make_server

TOKEN = "test-setup-token"


class SetupUITests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.store = CredentialsStore(os.path.join(self.directory.name, "credentials.json"))
        self.server = make_server("127.0.0.1", 0, self.store, {"providers": []}, 2, TOKEN)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.directory.cleanup()

    def request(self, path, method="GET", data=None, token=None):
        headers = {}
        body = None
        if token:
            headers["X-Setup-Token"] = token
        if data is not None:
            body = json.dumps(data).encode()
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(self.base + path, data=body, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            try:
                return error.code, error.read()
            finally:
                error.close()

    def test_page_requires_token_for_api_but_not_for_html(self):
        status, body = self.request("/")
        self.assertEqual(status, 200)
        self.assertIn(b"SmallTV usage setup", body)

        status, _ = self.request("/api/status", token=None)
        self.assertEqual(status, 401)
        status, _ = self.request("/api/status", token="wrong")
        self.assertEqual(status, 401)
        status, _ = self.request("/api/credentials", "POST", {"provider": "claude"}, token=None)
        self.assertEqual(status, 401)

    def test_save_status_clear_and_test_endpoint(self):
        status, body = self.request("/api/test", "POST", {"provider": "claude"}, TOKEN)
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["ok"])

        status, body = self.request("/api/credentials", "POST", {
            "provider": "claude",
            "accessToken": "secret-access",
            "refreshToken": "secret-refresh",
            "clientId": "public-client",
        }, TOKEN)
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["status"]["configured"])
        self.assertNotIn(b"secret-access", body)
        self.assertEqual(self.store.get("claude")["refreshToken"], "secret-refresh")

        status, body = self.request("/api/status", token=TOKEN)
        self.assertEqual(status, 200)
        data = json.loads(body)
        self.assertTrue(data["providers"]["claude"]["configured"])
        self.assertNotIn(b"secret-access", body)
        self.assertNotIn(b"secret-refresh", body)
        self.assertIn("credentials.json", data["credentialsPath"])

        status, body = self.request("/api/credentials", "POST", {
            "provider": "claude", "clear": True,
        }, TOKEN)
        self.assertEqual(status, 200)
        self.assertFalse(json.loads(body)["status"]["configured"])
        self.assertEqual(self.store.get("claude"), {})

        status, _ = self.request("/api/credentials", "POST", {"provider": "nope"}, TOKEN)
        self.assertEqual(status, 400)


if __name__ == "__main__":
    unittest.main()
