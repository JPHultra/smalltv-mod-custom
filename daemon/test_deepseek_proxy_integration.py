import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROXY = Path(__file__).with_name("deepseek_proxy.py")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class MockUpstream(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _send(self, body, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        self.rfile.read(length)
        if self.path.endswith("/stream"):
            self._send(
                b'data: {"usage":{"prompt_tokens":3,"completion_tokens":2,"total_tokens":5}}\n\n'
                b'data: [DONE]\n\n',
                "text/event-stream",
            )
        elif self.path.startswith("/anthropic"):
            self._send(
                b'event: message_start\n'
                b'data: {"type":"message_start","message":{"usage":{"input_tokens":10,"output_tokens":1}}}\n\n'
                b'event: message_delta\n'
                b'data: {"type":"message_delta","usage":{"output_tokens":5}}\n\n'
                b'event: message_stop\n'
                b'data: {"type":"message_stop"}\n\n',
                "text/event-stream",
            )
        else:
            self._send(
                json.dumps({"choices": [], "usage": {
                    "prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15,
                }}).encode(),
                "application/json",
            )


class DeepSeekProxyIntegrationTests(unittest.TestCase):
    def test_proxy_counts_openai_and_anthropic_usage(self):
        upstream = http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockUpstream)
        threading.Thread(target=upstream.serve_forever, daemon=True).start()
        state = tempfile.mktemp(suffix=".json")
        port = free_port()
        proxy = subprocess.Popen(
            [sys.executable, str(PROXY), "--port", str(port),
             "--upstream", f"http://127.0.0.1:{upstream.server_address[1]}",
             "--state", state],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            for _ in range(60):
                try:
                    urllib.request.urlopen(f"http://127.0.0.1:{port}/usage", timeout=1).read()
                    break
                except Exception:
                    time.sleep(0.1)
            else:
                self.fail("proxy did not start")

            def post(path, payload):
                request = urllib.request.Request(
                    f"http://127.0.0.1:{port}{path}",
                    data=json.dumps(payload).encode(),
                    headers={"Content-Type": "application/json", "Authorization": "Bearer test"},
                    method="POST",
                )
                with urllib.request.urlopen(request, timeout=5) as response:
                    return response.read()

            post("/chat/completions", {"model": "deepseek-flash", "messages": []})
            post("/chat/completions/stream", {"model": "deepseek-flash", "messages": [], "stream": True})
            post("/anthropic/v1/messages", {"model": "deepseek-flash", "messages": []})

            with urllib.request.urlopen(f"http://127.0.0.1:{port}/usage", timeout=5) as response:
                snapshot = json.load(response)
            self.assertEqual(snapshot["total_tokens"], 35)
            self.assertEqual(snapshot["requests"], 3)
            self.assertEqual(snapshot["today"]["total_tokens"], 35)
            with open(state, encoding="utf-8") as handle:
                self.assertEqual(json.load(handle)["total_tokens"], 35)
        finally:
            proxy.terminate()
            proxy.wait(timeout=5)
            upstream.shutdown()
            upstream.server_close()


if __name__ == "__main__":
    unittest.main()
