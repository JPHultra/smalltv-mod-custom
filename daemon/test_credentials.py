import json
import os
import tempfile
import unittest

from credentials import CredentialsStore


class CredentialsStoreTests(unittest.TestCase):
    def test_roundtrip_status_and_permissions(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "nested", "credentials.json")
            store = CredentialsStore(path)
            status = store.update("claude", {
                "accessToken": "secret-access",
                "refreshToken": "secret-refresh",
                "expiresAt": "1234",
                "clientId": "public-client",
            })
            self.assertTrue(status["configured"])
            self.assertTrue(status["fields"]["accessToken"])
            self.assertNotIn("secret", json.dumps(status))

            mode = os.stat(path).st_mode & 0o777
            self.assertEqual(mode, 0o600)
            with open(path, encoding="utf-8") as handle:
                raw = handle.read()
            self.assertIn("secret-access", raw)

            reloaded = CredentialsStore(path)
            self.assertEqual(reloaded.get("claude")["refreshToken"], "secret-refresh")
            self.assertEqual(reloaded.get("claude")["expiresAt"], 1234)

            # Blank values delete individual fields; a remaining secret keeps it configured.
            status = reloaded.update("claude", {"accessToken": ""})
            self.assertFalse(status["fields"]["accessToken"])
            self.assertTrue(status["fields"]["refreshToken"])
            self.assertTrue(status["configured"])

            status = reloaded.clear("claude")
            self.assertFalse(status["configured"])
            self.assertEqual(reloaded.get("claude"), {})

    def test_invalid_files_and_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "credentials.json")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("not json")
            self.assertEqual(CredentialsStore(path).get("deepseek"), {})

            with open(path, "w", encoding="utf-8") as handle:
                json.dump({"deepseek": {"apiKey": "  sk-test  ", "ignored": "x"}}, handle)
            store = CredentialsStore(path)
            self.assertEqual(store.get("deepseek"), {"apiKey": "sk-test"})

            with self.assertRaises(ValueError):
                store.update("nope", {"apiKey": "x"})
            with self.assertRaises(ValueError):
                store.update("deepseek", {"ignored": "x"})
            with self.assertRaises(ValueError):
                store.clear("nope")


if __name__ == "__main__":
    unittest.main()
