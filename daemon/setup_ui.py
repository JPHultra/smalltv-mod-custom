#!/usr/bin/env python3
"""Local setup UI for usage-provider credentials.

Run this on the daemon host after the firmware is flashed, open the printed
URL, and paste credentials from any machine. The page never reads local auth
files or environment variables, and never displays saved secrets back.
"""
from __future__ import annotations

import argparse
import copy
import hmac
import json
import secrets
import sys
import urllib.error
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

import usage_daemon
from credentials import FIELDS, CredentialsStore

PROVIDERS = ("claude", "codex", "deepseek")
DEFAULT_SPECS = {
    "claude": {"id": "claude", "name": "Claude", "adapter": "claude"},
    "codex": {"id": "chatgpt", "name": "Codex", "adapter": "codex"},
    "deepseek": {"id": "deepseek", "name": "DeepSeek", "adapter": "deepseek"},
}
MAX_BODY = 64 * 1024
SETUP_HTML = Path(__file__).with_name("setup.html").read_text(encoding="utf-8")


def specs_from_config(config):
    specs = copy.deepcopy(DEFAULT_SPECS)
    providers = config.get("providers") if isinstance(config, dict) else None
    for spec in providers if isinstance(providers, list) else []:
        if not isinstance(spec, dict):
            continue
        adapter = spec.get("adapter")
        if adapter in specs:
            specs[adapter] = spec
    return specs


class SetupHandler(BaseHTTPRequestHandler):
    server_version = "SmallTVSetup/1.0"

    def log_message(self, fmt, *args):
        message = fmt % args
        token = getattr(self.server, "setup_token", "")
        if token:
            message = message.replace(token, "[redacted]")
        sys.stderr.write("setup: " + message + "\n")

    def _send(self, status, body, content_type="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, separators=(",", ":"))
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def _authorized(self):
        supplied = self.headers.get("X-Setup-Token", "")
        expected = getattr(self.server, "setup_token", "")
        return bool(supplied and expected and hmac.compare_digest(supplied, expected))

    def _read_json(self):
        try:
            length = int(self.headers.get("Content-Length", 0))
        except ValueError:
            self._send(400, {"error": "invalid content length"})
            return None
        if length < 0 or length > MAX_BODY:
            self._send(413, {"error": "request body too large"})
            return None
        try:
            data = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            self._send(400, {"error": "invalid JSON"})
            return None
        if not isinstance(data, dict):
            self._send(400, {"error": "expected a JSON object"})
            return None
        return data

    def do_GET(self):
        path = urlsplit(self.path).path
        if path in ("/", "/index.html"):
            self._send(200, SETUP_HTML, "text/html; charset=utf-8")
            return
        if path == "/api/status":
            if not self._authorized():
                self._send(401, {"error": "setup token required"})
                return
            status = self.server.store.status()
            providers = {name: status[name] for name in PROVIDERS if name in status}
            self._send(200, {
                "providers": providers,
                "credentialsPath": self.server.store.path,
            })
            return
        self._send(404, {"error": "not found"})

    def do_POST(self):
        if not self._authorized():
            self._send(401, {"error": "setup token required"})
            return
        path = urlsplit(self.path).path
        data = self._read_json()
        if data is None:
            return
        provider = data.get("provider")
        if provider not in PROVIDERS:
            self._send(400, {"error": "unknown provider"})
            return
        if path == "/api/credentials":
            try:
                if data.get("clear"):
                    status = self.server.store.clear(provider)
                else:
                    values = {
                        key: data[key] for key in FIELDS[provider]
                        if key in data and data[key] not in (None, "")
                    }
                    if not values:
                        self._send(400, {"error": "no non-empty credential fields"})
                        return
                    status = self.server.store.update(provider, values)
            except (OSError, ValueError, TimeoutError) as error:
                self._send(400, {"error": str(error)[:300]})
                return
            self._send(200, {"ok": True, "provider": provider, "status": status})
            return
        if path == "/api/test":
            spec = self.server.specs.get(provider)
            if not spec:
                self._send(400, {"error": "provider is not configured"})
                return
            try:
                result = usage_daemon.collect(spec, self.server.timeout_sec, self.server.store)
                self._send(200, {"ok": bool(result.get("ok")), "provider": result})
            except urllib.error.HTTPError as error:
                error.close()
                self._send(200, {"ok": False, "error": "provider returned HTTP %d" % error.code})
            except Exception as error:
                self._send(200, {"ok": False, "error": str(error)[:300]})
            return
        self._send(404, {"error": "not found"})


def make_server(host, port, store, config, timeout=10, setup_token=None):
    server = ThreadingHTTPServer((host, port), SetupHandler)
    server.store = store
    server.specs = specs_from_config(config)
    server.timeout_sec = max(1.0, float(timeout))
    server.setup_token = setup_token or secrets.token_urlsafe(18)
    return server


def load_config(path):
    if not path:
        return {"providers": []}
    with open(path, encoding="utf-8") as handle:
        config = json.load(handle)
    if not isinstance(config, dict):
        raise ValueError("config must be a JSON object")
    return config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", help="daemon JSON config (provider URLs/options)")
    parser.add_argument("--credentials", help="override the credentials JSON path")
    parser.add_argument("--bind", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8790, help="port (default 8790)")
    parser.add_argument("--timeout", type=float, default=10, help="provider test timeout")
    parser.add_argument("--token", help="fixed setup token instead of a generated one")
    args = parser.parse_args()

    try:
        config = load_config(args.config)
        store = CredentialsStore(args.credentials or config.get("credentialsFile"))
        server = make_server(args.bind, args.port, store, config, args.timeout, args.token)
    except (OSError, ValueError) as error:
        parser.exit(1, f"setup ui: {error}\n")

    host, port = server.server_address[:2]
    shown = "127.0.0.1" if host in ("0.0.0.0", "::") else host
    print(f"SmallTV usage setup: http://{shown}:{port}/")
    print(f"Setup token: {server.setup_token}")
    print("Open the page, enter the token, fill the provider fields, and press Save.")
    if host not in ("127.0.0.1", "::1"):
        print("warning: bound beyond localhost; anyone who can reach this port and has the token can update credentials.", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
