#!/usr/bin/env python3
"""Local DeepSeek proxy that counts token usage for the dashboard.

DeepSeek's public API returns token usage per response but has no aggregate
usage endpoint. Point an OpenAI-compatible DeepSeek client at this proxy and it
forwards every request to api.deepseek.com, counts the usage fields, and serves:

  GET  /usage        accumulated prompt/completion/total tokens and request count
  POST /usage/reset  clear the counters

The proxy listens on 127.0.0.1:8789 by default. Set the client base URL to
http://127.0.0.1:8789 (or http://127.0.0.1:8789/anthropic for the Anthropic
format). The Authorization header is forwarded as-is and is never written to
the state file.

Usage:
  python3 daemon/deepseek_proxy.py
  python3 daemon/deepseek_proxy.py --port 8790 --state ~/.local/state/smalltv/deepseek.json
"""
from __future__ import annotations

import argparse
import datetime
import http.client
import http.server
import json
import os
import sys
import threading
import urllib.parse


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def numeric(value):
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, (int, float)):
        return int(value)
    if isinstance(value, str):
        try:
            return int(float(value.strip()))
        except ValueError:
            return None
    return None


def usage_from_json(value):
    """Return (prompt, completion, total) from an OpenAI usage object, or None."""
    if not isinstance(value, dict):
        return None
    usage = value.get("usage")
    if not isinstance(usage, dict):
        return None
    prompt = numeric(usage.get("prompt_tokens"))
    completion = numeric(usage.get("completion_tokens"))
    total = numeric(usage.get("total_tokens"))
    if prompt is None and completion is None and total is None:
        return None
    prompt = prompt or 0
    completion = completion or 0
    total = total if total is not None else prompt + completion
    if total < prompt + completion:
        total = prompt + completion
    return prompt, completion, total


def usage_from_sse(line):
    """Return usage from an SSE `data: {...}` line, or None."""
    line = line.strip()
    if not line.startswith("data:"):
        return None
    payload = line[5:].strip()
    if not payload or payload == "[DONE]":
        return None
    try:
        return usage_from_json(json.loads(payload))
    except (TypeError, ValueError):
        return None


class UsageTracker:
    def __init__(self, path):
        self.path = os.path.expanduser(path)
        self.lock = threading.Lock()
        self.prompt = 0
        self.completion = 0
        self.total = 0
        self.requests = 0
        self.since = now_iso()
        self.updated = self.since
        self._load()

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError, json.JSONDecodeError):
            return
        self.prompt = int(data.get("prompt_tokens", 0))
        self.completion = int(data.get("completion_tokens", 0))
        self.total = int(data.get("total_tokens", 0))
        self.requests = int(data.get("requests", 0))
        self.since = data.get("since", self.since)
        self.updated = data.get("updated", self.updated)

    def _save(self):
        os.makedirs(os.path.dirname(self.path) or ".", exist_ok=True)
        temporary = self.path + ".tmp"
        with open(temporary, "w", encoding="utf-8") as handle:
            json.dump(self.snapshot_locked(), handle)
        os.chmod(temporary, 0o600)
        os.replace(temporary, self.path)

    def add(self, prompt, completion, total):
        with self.lock:
            self.prompt += prompt
            self.completion += completion
            self.total += total
            self.requests += 1
            self.updated = now_iso()
            self._save()

    def snapshot_locked(self):
        return {
            "prompt_tokens": self.prompt,
            "completion_tokens": self.completion,
            "total_tokens": self.total,
            "requests": self.requests,
            "since": self.since,
            "updated": self.updated,
        }

    def snapshot(self):
        with self.lock:
            return self.snapshot_locked()

    def reset(self):
        with self.lock:
            self.prompt = self.completion = self.total = self.requests = 0
            self.since = self.updated = now_iso()
            self._save()


class ProxyHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    tracker: UsageTracker
    upstream: str
    timeout: float

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")

    def do_PUT(self):
        self._handle("PUT")

    def do_DELETE(self):
        self._handle("DELETE")

    def do_OPTIONS(self):
        self._send_json(204, b"")

    def _local(self, method):
        path = urllib.parse.urlsplit(self.path).path
        if method == "GET" and path == "/usage":
            self._send_json(200, json.dumps(self.tracker.snapshot()).encode())
            return True
        if method == "POST" and path == "/usage/reset":
            self.tracker.reset()
            self._send_json(200, b'{"ok":true}')
            return True
        if method == "GET" and path == "/":
            self._send_json(200, b'{"proxy":"deepseek","usage":"/usage"}')
            return True
        return False

    def _handle(self, method):
        if self._local(method):
            return
        body = self._read_body()
        if body is None:
            return
        body = self._enable_stream_usage(body)
        headers = self._forward_headers(body)
        target = urllib.parse.urlsplit(self.upstream)
        connection_class = http.client.HTTPSConnection if target.scheme == "https" else http.client.HTTPConnection
        connection = connection_class(target.hostname, target.port, timeout=self.timeout)
        path = urllib.parse.urlsplit(self.path)
        url = target.path.rstrip("/") + path.path + (("?" + path.query) if path.query else "")
        try:
            connection.request(method, url, body=body, headers=headers)
            response = connection.getresponse()
        except (OSError, http.client.HTTPException) as error:
            self._send_json(502, json.dumps({"error": "upstream: " + str(error)}).encode())
            connection.close()
            return

        if "text/event-stream" in response.getheader("Content-Type", ""):
            self._relay_sse(response)
        else:
            data = response.read()
            usage = usage_from_json(self._safe_json(data))
            if usage:
                self.tracker.add(*usage)
            self._send_bytes(response.status, response.reason, response.getheaders(), data)
        connection.close()

    def _read_body(self):
        length = self.headers.get("Content-Length")
        if length:
            try:
                count = int(length)
            except ValueError:
                self._send_json(400, b'{"error":"bad Content-Length"}')
                return None
            return self.rfile.read(count)
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            chunks = []
            while True:
                line = self.rfile.readline().strip()
                if not line:
                    continue
                size = int(line.split(b";", 1)[0], 16)
                if size == 0:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(size))
                self.rfile.read(2)
            return b"".join(chunks)
        return b""

    def _enable_stream_usage(self, body):
        if not body or urllib.parse.urlsplit(self.path).path.startswith("/anthropic"):
            return body
        try:
            request = json.loads(body)
        except (TypeError, ValueError):
            return body
        if isinstance(request, dict) and request.get("stream") is True:
            options = request.get("stream_options")
            if not isinstance(options, dict):
                options = {}
            options["include_usage"] = True
            request["stream_options"] = options
            return json.dumps(request, separators=(",", ":")).encode()
        return body

    def _forward_headers(self, body):
        headers = {}
        for key, value in self.headers.items():
            if key.lower() in ("host", "content-length", "connection", "transfer-encoding", "accept-encoding"):
                continue
            headers[key] = value
        headers["Accept-Encoding"] = "identity"
        if body:
            headers["Content-Length"] = str(len(body))
        return headers

    def _relay_sse(self, response):
        self.send_response(response.status, response.reason)
        for key, value in response.getheaders():
            if key.lower() in ("transfer-encoding", "content-length", "connection"):
                continue
            self.send_header(key, value)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        pending = b""
        try:
            while True:
                chunk = response.read(1024)
                if not chunk:
                    break
                self.wfile.write(("%X\r\n" % len(chunk)).encode() + chunk + b"\r\n")
                self.wfile.flush()
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    usage = usage_from_sse(line.decode("utf-8", "replace"))
                    if usage:
                        self.tracker.add(*usage)
            if pending.strip():
                usage = usage_from_sse(pending.decode("utf-8", "replace"))
                if usage:
                    self.tracker.add(*usage)
            self.wfile.write(b"0\r\n\r\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_bytes(self, status, reason, headers, data):
        self.send_response(status, reason)
        for key, value in headers:
            if key.lower() in ("transfer-encoding", "content-length", "connection"):
                continue
            self.send_header(key, value)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)
        self.close_connection = True

    def _send_json(self, status, data):
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        if data:
            self.wfile.write(data)
        self.close_connection = True

    @staticmethod
    def _safe_json(data):
        try:
            return json.loads(data)
        except (TypeError, ValueError):
            return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8789)
    parser.add_argument("--upstream", default="https://api.deepseek.com")
    parser.add_argument("--state", default="~/.local/state/smalltv/deepseek-usage.json")
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args()

    tracker = UsageTracker(args.state)
    ProxyHandler.tracker = tracker
    ProxyHandler.upstream = args.upstream
    ProxyHandler.timeout = args.timeout
    server = http.server.ThreadingHTTPServer((args.host, args.port), ProxyHandler)
    server.daemon_threads = True
    print(f"DeepSeek usage proxy on http://{args.host}:{args.port} -> {args.upstream}")
    print(f"usage: http://{args.host}:{args.port}/usage   state: {os.path.expanduser(args.state)}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
