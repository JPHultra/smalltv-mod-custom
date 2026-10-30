#!/usr/bin/env python3
"""Push a normalized usage document to a SmallTV dashboard.

The device exposes POST /api/usage (open, because the daemon has no way to
carry the web-UI password). This script only moves JSON; provider-specific
collectors belong outside it.

Examples:
  python3 daemon/push_usage.py --device http://smalltv.local --demo --once
  python3 daemon/push_usage.py --device http://smalltv.local --file usage.json
  python3 daemon/push_usage.py --device http://smalltv.local --url http://127.0.0.1:8787/usage
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def demo_document() -> dict:
    return {
        "providers": [
            {
                "id": "claude",
                "name": "Claude",
                "ok": True,
                "status": "normal",
                "metrics": [
                    {"label": "5h", "pct": 29, "resetMin": 142},
                    {"label": "7d", "pct": 4, "resetMin": 9876},
                ],
            },
            {
                "id": "chatgpt",
                "name": "ChatGPT",
                "ok": True,
                "metrics": [{"label": "5h", "pct": 62, "resetMin": 88}],
            },
            {
                "id": "deepseek",
                "name": "DeepSeek",
                "ok": True,
                "metrics": [{"label": "balance", "pct": 81, "resetMin": 0}],
            },
        ]
    }


def load_document(args: argparse.Namespace) -> dict:
    if args.demo:
        return demo_document()
    if args.file:
        with open(args.file, encoding="utf-8") as handle:
            return json.load(handle)
    if args.url:
        with urllib.request.urlopen(args.url, timeout=args.timeout) as response:
            return json.load(response)
    raise SystemExit("provide --demo, --file or --url")


def push(device: str, document: dict, timeout: float) -> str:
    url = device.rstrip("/") + "/api/usage"
    body = json.dumps(document, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        url, data=body, method="POST",
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8", "replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="e.g. http://smalltv.local")
    parser.add_argument("--file", help="read the normalized document from this JSON file")
    parser.add_argument("--url", help="read the normalized document from this URL")
    parser.add_argument("--demo", action="store_true", help="push sample Claude/ChatGPT/DeepSeek data")
    parser.add_argument("--interval", type=float, default=60.0, help="seconds between pushes (default 60)")
    parser.add_argument("--once", action="store_true", help="push once and exit")
    parser.add_argument("--timeout", type=float, default=10.0, help="HTTP timeout in seconds (default 10)")
    args = parser.parse_args()

    first = True
    while True:
        try:
            document = load_document(args)
            reply = push(args.device, document, args.timeout)
            print(f"{time.strftime('%H:%M:%S')} pushed: {reply}")
        except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as error:
            print(f"{time.strftime('%H:%M:%S')} push failed: {error}", file=sys.stderr)
            if args.once and first:
                return 1
        first = False
        if args.once:
            return 0
        time.sleep(max(1.0, args.interval))


if __name__ == "__main__":
    raise SystemExit(main())
