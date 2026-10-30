#!/usr/bin/env python3
"""Aggregate per-provider usage endpoints and push the normalized document.

Each configured provider is fetched independently, mapped through an adapter,
and sent to the device as one POST /api/usage document. A failed provider is
reported as {"ok": false}, so the device keeps its previous metrics and shows
the error state instead of blanking the card.

Adapters:
  claude-legacy  the old clawdmeter {s,sr,w,wr,st,ok} payload
  generic        a JSON object plus dotted paths for each metric
  normalized     the provider already returns {id,name,ok,status,metrics}

Example:
  python3 daemon/usage_daemon.py --config daemon/usage-daemon.json
  python3 daemon/usage_daemon.py --config daemon/usage-daemon.json --once
  python3 daemon/usage_daemon.py --config daemon/usage-daemon.json --dry-run
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def path_get(obj, path):
    """Return a value from a dotted object path, or None."""
    if not path:
        return None
    current = obj
    for part in str(path).split("."):
        if not isinstance(current, dict) or part not in current:
            return None
        current = current[part]
    return current


def numeric(value):
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError:
            return None
    return None


def make_metric(label, pct, reset_min):
    value = numeric(pct)
    if value is None:
        return None
    reset = numeric(reset_min)
    return {
        "label": str(label or "--")[:11],
        "pct": round(max(0.0, min(100.0, value)), 1),
        "resetMin": max(0, int(reset or 0)),
    }


def provider_error(spec):
    return {
        "id": str(spec.get("id", "unknown")),
        "name": str(spec.get("name", spec.get("id", "unknown"))),
        "ok": False,
        "status": "unavailable",
    }


def claude_legacy(source, spec):
    if not isinstance(source, dict):
        raise ValueError("expected a JSON object")
    if source.get("ok") is False:
        return provider_error(spec)
    if numeric(source.get("s")) is None:
        raise ValueError("missing s")
    metrics = [
        make_metric("5h", source.get("s"), source.get("sr")),
        make_metric("7d", source.get("w"), source.get("wr")),
    ]
    return {
        "id": str(spec["id"]),
        "name": str(spec.get("name", "Claude")),
        "ok": True,
        "status": str(source.get("st", ""))[:15],
        "metrics": [metric for metric in metrics if metric],
    }


def generic(source, spec):
    if not isinstance(source, dict):
        raise ValueError("expected a JSON object")
    ok_path = spec.get("okPath")
    if ok_path and path_get(source, ok_path) is False:
        return provider_error(spec)
    if not ok_path and source.get("ok") is False:
        return provider_error(spec)

    metrics = []
    for item in spec.get("metrics", []):
        if not isinstance(item, dict) or not item.get("pctPath"):
            continue
        reset = path_get(source, item["resetPath"]) if item.get("resetPath") else 0
        metric = make_metric(item.get("label", "--"), path_get(source, item["pctPath"]), reset)
        if metric:
            metrics.append(metric)
    if not metrics:
        raise ValueError("no metric paths matched")

    status = path_get(source, spec["statusPath"]) if spec.get("statusPath") else ""
    return {
        "id": str(spec["id"]),
        "name": str(spec.get("name", spec["id"])),
        "ok": True,
        "status": str(status or "")[:15],
        "metrics": metrics,
    }


def normalized(source, spec):
    if not isinstance(source, dict):
        raise ValueError("expected a JSON object")
    result = dict(source)
    result["id"] = str(spec["id"])
    result.setdefault("name", str(spec.get("name", spec["id"])))
    result.setdefault("ok", True)
    if not isinstance(result.get("metrics"), list):
        raise ValueError("normalized source has no metrics array")
    return result


ADAPTERS = {
    "claude-legacy": claude_legacy,
    "generic": generic,
    "normalized": normalized,
}


def fetch_json(url, timeout, headers=None):
    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def build_payload(config, timeout):
    providers = []
    for spec in config["providers"]:
        adapter = ADAPTERS.get(spec.get("adapter", "generic"))
        try:
            if adapter is None:
                raise ValueError("unknown adapter")
            source = fetch_json(spec["url"], timeout, spec.get("headers"))
            providers.append(adapter(source, spec))
        except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as error:
            print(f"{spec.get('id', '?')}: {error}", file=sys.stderr)
            providers.append(provider_error(spec))
    return {"providers": providers}


def push(device, document, timeout):
    url = device.rstrip("/") + "/api/usage"
    body = json.dumps(document, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read().decode("utf-8", "replace")


def validate(config):
    providers = config.get("providers")
    if not isinstance(providers, list) or not providers:
        raise ValueError("providers must be a non-empty array")
    for spec in providers:
        if not isinstance(spec, dict) or not spec.get("id") or not spec.get("url"):
            raise ValueError("every provider needs id and url")
        if spec.get("adapter", "generic") not in ADAPTERS:
            raise ValueError(f"{spec['id']}: unknown adapter {spec.get('adapter')}")
        if spec.get("adapter", "generic") == "generic":
            metrics = spec.get("metrics")
            if not isinstance(metrics, list) or not metrics:
                raise ValueError(f"{spec['id']}: generic providers need metrics")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="JSON daemon config")
    parser.add_argument("--device", help="override the config device URL")
    parser.add_argument("--interval", type=float, help="override seconds between pushes")
    parser.add_argument("--timeout", type=float, help="override HTTP timeout in seconds")
    parser.add_argument("--once", action="store_true", help="push once and exit")
    parser.add_argument("--dry-run", action="store_true", help="print the document and exit")
    args = parser.parse_args()

    try:
        with open(args.config, encoding="utf-8") as handle:
            config = json.load(handle)
        validate(config)
        device = (args.device or config.get("device") or "").strip()
        if not device:
            raise ValueError("no device URL in the config or --device")
        interval = max(1.0, float(args.interval if args.interval is not None else config.get("interval", 60)))
        timeout = max(1.0, float(args.timeout if args.timeout is not None else config.get("timeout", 10)))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.exit(1, f"usage daemon: {error}\n")

    if args.dry_run:
        print(json.dumps(build_payload(config, timeout), indent=2))
        return 0

    while True:
        document = build_payload(config, timeout)
        try:
            reply = push(device, document, timeout)
            print(f"{time.strftime('%H:%M:%S')} pushed: {reply}")
        except (OSError, urllib.error.URLError) as error:
            print(f"{time.strftime('%H:%M:%S')} push failed: {error}", file=sys.stderr)
            if args.once:
                return 1
        if args.once:
            return 0
        time.sleep(interval)


if __name__ == "__main__":
    raise SystemExit(main())
