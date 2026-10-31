#!/usr/bin/env python3
"""Aggregate per-provider usage endpoints and push the normalized document.

Each configured provider is fetched independently, mapped through an adapter,
and sent to the device as one POST /api/usage document. A failed provider is
reported as {"ok": false}, so the device keeps its previous metrics and shows
the error state instead of blanking the card.

Adapters:
  claude         Claude Code's OAuth usage endpoint, using ~/.claude/.credentials.json
  claude-legacy  the old clawdmeter {s,sr,w,wr,st,ok} payload
  codex          the ChatGPT/Codex subscription usage endpoint, using ~/.codex/auth.json
  deepseek       DeepSeek balance, plus optional token usage from a proxy
  generic        a JSON object plus dotted paths for each metric
  normalized     the provider already returns {id,name,ok,status,metrics}

Values:
  A metric may carry "value" instead of (or in addition to) a percentage. The
  dashboard shows it as text, e.g. "110 CNY" or "1.2M tokens". A metric with a
  value and pct 0 has no fill bar; pct > 0 adds one.

Example:
  python3 daemon/usage_daemon.py --config daemon/usage-daemon.json
  python3 daemon/usage_daemon.py --config daemon/usage-daemon.json --once
  python3 daemon/usage_daemon.py --config daemon/usage-daemon.json --dry-run
"""
from __future__ import annotations

import argparse
import contextlib
import datetime
import json
import math
import os
import sys
import time
import urllib.error
import urllib.request

try:
    import fcntl
except ImportError:  # Windows: no advisory file lock; atomic replace still applies.
    fcntl = None


@contextlib.contextmanager
def file_lock(path, timeout=10.0):
    """Advisory lock for the auth files; the Codex/Claude refresh path is serialized."""
    if fcntl is None:
        yield
        return
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        deadline = time.time() + timeout
        while True:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.time() >= deadline:
                    raise TimeoutError("timed out waiting for the auth file lock")
                time.sleep(0.1)
        yield
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


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
        number = float(value)
        return number if math.isfinite(number) else None
    if isinstance(value, str):
        try:
            number = float(value.strip())
        except ValueError:
            return None
        return number if math.isfinite(number) else None
    return None


def make_metric(label, pct, reset_min, value=None):
    amount = numeric(pct)
    if amount is None and not value:
        return None
    reset = numeric(reset_min)
    metric = {
        "label": str(label or "--")[:11],
        "pct": round(max(0.0, min(100.0, amount or 0.0)), 1),
        "resetMin": max(0, int(round(reset or 0))),
    }
    if value:
        metric["value"] = str(value)[:19]
    return metric


def compact_number(value):
    text = str(value).strip()
    if "." in text:
        text = text.rstrip("0").rstrip(".")
    return text or "0"


def compact_tokens(value):
    count = numeric(value)
    if count is None:
        return None
    if abs(count) >= 1_000_000:
        return f"{count / 1_000_000:.1f}M"
    if abs(count) >= 1_000:
        return f"{count / 1_000:.1f}k"
    return str(int(count))


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
        if not isinstance(item, dict) or not (item.get("pctPath") or item.get("valuePath")):
            continue
        reset = path_get(source, item["resetPath"]) if item.get("resetPath") else 0
        value = path_get(source, item["valuePath"]) if item.get("valuePath") else None
        metric = make_metric(item.get("label", "--"), path_get(source, item["pctPath"]) if item.get("pctPath") else None,
                             reset, value)
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


# ---------------------------------------------------------------------------
# Codex / ChatGPT subscription usage
# ---------------------------------------------------------------------------
CODEX_DEFAULT_URL = "https://chatgpt.com/backend-api/wham/usage"
CODEX_TOKEN_URL = "https://auth.openai.com/oauth/token"
CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"
CODEX_USER_AGENT = "codex-cli"


def window_label(seconds):
    seconds = int(numeric(seconds) or 0)
    if seconds >= 86400:
        return f"{seconds // 86400}d"
    if seconds >= 3600:
        return f"{seconds // 3600}h"
    return f"{max(1, seconds // 60)}m"


def codex_window_metric(window, prefix=""):
    if not isinstance(window, dict):
        return None
    label = window_label(window.get("limit_window_seconds"))
    if prefix:
        label = f"{prefix[:5]} {label}"[:11]
    return make_metric(
        label,
        window.get("used_percent"),
        (numeric(window.get("reset_after_seconds")) or 0) / 60,
    )


def map_codex_usage(payload, spec):
    if not isinstance(payload, dict):
        raise ValueError("expected a JSON object")
    rate = payload.get("rate_limit")
    if not isinstance(rate, dict):
        raise ValueError("missing rate_limit")
    metrics = []
    for key in ("primary_window", "secondary_window"):
        metric = codex_window_metric(rate.get(key))
        if metric:
            metrics.append(metric)
    if not metrics:
        raise ValueError("no rate-limit windows")

    plan = str(payload.get("plan_type", ""))
    if spec.get("includePlan", True):
        metrics.append(make_metric("plan", None, 0, plan or "unknown"))

    credits = payload.get("credits")
    if spec.get("includeCredits", False) and isinstance(credits, dict):
        balance = credits.get("balance")
        if credits.get("has_credits") or (balance not in (None, "", "0", 0)):
            value = "unlimited" if credits.get("unlimited") else (str(balance) if balance not in (None, "") else "credits")
            metrics.append(make_metric("credits", None, 0, value))

    if spec.get("includeAdditional", False):
        for additional in payload.get("additional_rate_limits") or []:
            if not isinstance(additional, dict):
                continue
            details = additional.get("rate_limit") or {}
            name = str(additional.get("limit_name") or additional.get("metered_feature") or "extra")
            for key in ("primary_window", "secondary_window"):
                metric = codex_window_metric(details.get(key), name)
                if metric:
                    metrics.append(metric)

    metrics = [metric for metric in metrics if metric][:5]
    return {
        "id": str(spec["id"]),
        "name": str(spec.get("name", "Codex")),
        "ok": True,
        "status": plan[:15],
        "metrics": metrics,
    }


def read_codex_auth(spec):
    path = os.path.expanduser(spec.get("authFile", "~/.codex/auth.json"))
    with open(path, encoding="utf-8") as handle:
        auth = json.load(handle)
    if not isinstance(auth, dict):
        raise ValueError("Codex auth is not a JSON object")
    tokens = auth.get("tokens")
    if not isinstance(tokens, dict):
        tokens = {}
    access = tokens.get("access_token")
    account = tokens.get("account_id")
    if not access or not account:
        raise ValueError("Codex auth has no ChatGPT tokens")
    return path, auth, tokens.get("refresh_token"), access, account


def write_codex_auth(path, auth):
    temporary = path + ".smalltv-tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(auth, handle, indent=2)
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)


def refresh_codex_auth(path, auth, spec, timeout):
    with file_lock(path + ".lock"):
        return _refresh_codex_locked(path, auth, spec, timeout)


def _refresh_codex_locked(path, auth, spec, timeout):
    # Another Codex process may have refreshed first; prefer its token.
    with open(path, encoding="utf-8") as handle:
        current = json.load(handle)
    if not isinstance(current, dict):
        current = {}
    tokens = current.get("tokens")
    if not isinstance(tokens, dict):
        tokens = {}
    current_refresh = tokens.get("refresh_token")
    original_tokens = auth.get("tokens") if isinstance(auth, dict) else {}
    old_refresh = original_tokens.get("refresh_token") if isinstance(original_tokens, dict) else None
    if current_refresh and current_refresh != old_refresh and tokens.get("access_token"):
        return tokens["access_token"], tokens.get("account_id")

    if not current_refresh:
        raise ValueError("Codex auth has no refresh token")
    body = json.dumps({
        "grant_type": "refresh_token",
        "client_id": spec.get("clientId", CODEX_CLIENT_ID),
        "refresh_token": current_refresh,
        "refresh_reason": "scheduled",
    }).encode("utf-8")
    request = urllib.request.Request(
        spec.get("tokenUrl", CODEX_TOKEN_URL),
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": spec.get("userAgent", CODEX_USER_AGENT),
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        refreshed = json.load(response)
    access = refreshed.get("access_token")
    if not access:
        raise ValueError("Codex token refresh returned no access token")
    tokens["access_token"] = access
    if refreshed.get("refresh_token"):
        tokens["refresh_token"] = refreshed["refresh_token"]
    if refreshed.get("id_token"):
        tokens["id_token"] = refreshed["id_token"]
    current["tokens"] = tokens
    current["last_refresh"] = datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
    write_codex_auth(path, current)
    return access, tokens.get("account_id")


def fetch_codex_usage(spec, timeout):
    path, auth, refresh, access, account = read_codex_auth(spec)
    url = spec.get("url", CODEX_DEFAULT_URL)
    headers = {
        "Authorization": "Bearer " + access,
        "chatgpt-account-id": account,
        "User-Agent": spec.get("userAgent", CODEX_USER_AGENT),
        "Accept": "application/json",
    }
    for attempt in (1, 2):
        request = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if attempt == 1 and error.code in (401, 403) and refresh:
                error.close()
                access, account = refresh_codex_auth(path, auth, spec, timeout)
                headers["Authorization"] = "Bearer " + access
                headers["chatgpt-account-id"] = account
                continue
            error.close()
            raise
    raise ValueError("Codex usage request failed")


def collect_codex(spec, timeout):
    return map_codex_usage(fetch_codex_usage(spec, timeout), spec)


# ---------------------------------------------------------------------------
# Claude Code subscription usage
# ---------------------------------------------------------------------------
CLAUDE_DEFAULT_CREDENTIALS = "~/.claude/.credentials.json"
CLAUDE_DEFAULT_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
CLAUDE_DEFAULT_TOKEN_URL = "https://platform.claude.com/v1/oauth/token"
CLAUDE_OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"
CLAUDE_USER_AGENT = "claude-code/2.1.146"
CLAUDE_REFRESH_MARGIN = 300


def minutes_until(iso_timestamp):
    if not iso_timestamp:
        return 0
    try:
        text = str(iso_timestamp).replace("Z", "+00:00")
        when = datetime.datetime.fromisoformat(text)
        now = datetime.datetime.now(datetime.timezone.utc)
        return max(0, int(round((when - now).total_seconds() / 60)))
    except (TypeError, ValueError):
        return 0


def write_claude_credentials(path, credentials):
    temporary = path + ".smalltv-tmp"
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(credentials, handle, indent=2)
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)


def refresh_claude_token(path, credentials, oauth, spec, timeout):
    refresh = oauth.get("refreshToken")
    if not refresh:
        raise ValueError("Claude token expired and has no refresh token; run `claude setup-token`")
    with file_lock(path + ".lock"):
        try:
            with open(path, encoding="utf-8") as handle:
                current = json.load(handle)
        except (OSError, ValueError, json.JSONDecodeError):
            current = credentials
        if not isinstance(current, dict):
            current = credentials
        current_oauth = current.get("claudeAiOauth")
        if not isinstance(current_oauth, dict):
            current_oauth = {}
        if current_oauth.get("accessToken") and current_oauth.get("accessToken") != oauth.get("accessToken"):
            return current_oauth["accessToken"]

        body = json.dumps({
            "grant_type": "refresh_token",
            "client_id": spec.get("clientId", CLAUDE_OAUTH_CLIENT_ID),
            "refresh_token": refresh,
        }).encode("utf-8")
        request = urllib.request.Request(
            spec.get("tokenUrl", CLAUDE_DEFAULT_TOKEN_URL),
            data=body,
            method="POST",
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "User-Agent": spec.get("userAgent", CLAUDE_USER_AGENT),
            },
        )
        with urllib.request.urlopen(request, timeout=timeout) as response:
            refreshed = json.load(response)
        access = refreshed.get("access_token")
        if not access:
            raise ValueError("Claude token refresh returned no access token")
        current_oauth["accessToken"] = access
        if refreshed.get("refresh_token"):
            current_oauth["refreshToken"] = refreshed["refresh_token"]
        if "expires_in" in refreshed:
            current_oauth["expiresAt"] = int((time.time() + float(refreshed["expires_in"])) * 1000)
        elif "expires_at" in refreshed:
            current_oauth["expiresAt"] = int(float(refreshed["expires_at"]) * 1000)
        current["claudeAiOauth"] = current_oauth
        write_claude_credentials(path, current)
        return access


def read_claude_token(spec):
    path = os.path.expanduser(spec.get("authFile", CLAUDE_DEFAULT_CREDENTIALS))
    env_name = spec.get("tokenEnv", "CLAUDE_CODE_OAUTH_TOKEN")
    if os.environ.get(env_name):
        return path, None, os.environ[env_name].strip()
    with open(path, encoding="utf-8") as handle:
        credentials = json.load(handle)
    if not isinstance(credentials, dict):
        raise ValueError("Claude credentials are not a JSON object")
    oauth = credentials.get("claudeAiOauth")
    if not isinstance(oauth, dict):
        oauth = {}
    access = oauth.get("accessToken")
    if not access:
        raise ValueError("Claude credentials have no access token; run `claude setup-token` or set CLAUDE_CODE_OAUTH_TOKEN")
    expires = oauth.get("expiresAt")
    if isinstance(expires, (int, float)) and time.time() >= (expires / 1000.0 - CLAUDE_REFRESH_MARGIN):
        access = refresh_claude_token(path, credentials, oauth, spec, 20)
    return path, credentials, access


def map_claude_usage(payload, spec):
    if not isinstance(payload, dict):
        raise ValueError("expected a JSON object")
    five = payload.get("five_hour")
    seven = payload.get("seven_day")
    five = five if isinstance(five, dict) else {}
    seven = seven if isinstance(seven, dict) else {}
    metrics = [
        make_metric("5h", five.get("utilization"), minutes_until(five.get("resets_at"))),
        make_metric("7d", seven.get("utilization"), minutes_until(seven.get("resets_at"))),
    ]
    metrics = [metric for metric in metrics if metric]
    if not metrics:
        raise ValueError("no five_hour/seven_day utilization")
    status = "allowed"
    limits = payload.get("limits")
    for limit in limits if isinstance(limits, list) else []:
        if isinstance(limit, dict) and limit.get("kind") == "session" and limit.get("severity"):
            status = str(limit["severity"])
            break
    return {
        "id": str(spec["id"]),
        "name": str(spec.get("name", "Claude")),
        "ok": True,
        "status": status[:15],
        "metrics": metrics,
    }


def fetch_claude_usage(spec, timeout):
    path, credentials, token = read_claude_token(spec)
    url = spec.get("url", CLAUDE_DEFAULT_USAGE_URL)
    headers = {
        "Authorization": "Bearer " + token,
        "anthropic-version": "2023-06-01",
        "anthropic-beta": "oauth-2025-04-20",
        "Accept": "application/json",
        "User-Agent": spec.get("userAgent", CLAUDE_USER_AGENT),
    }
    for attempt in (1, 2):
        request = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            oauth = (credentials or {}).get("claudeAiOauth") or {}
            if attempt == 1 and error.code in (401, 403) and oauth.get("refreshToken"):
                error.close()
                token = refresh_claude_token(path, credentials, oauth, spec, timeout)
                headers["Authorization"] = "Bearer " + token
                continue
            error.close()
            raise
    raise ValueError("Claude usage request failed")


def collect_claude(spec, timeout):
    return map_claude_usage(fetch_claude_usage(spec, timeout), spec)


# ---------------------------------------------------------------------------
# DeepSeek balance (+ optional token usage from a local proxy)
# ---------------------------------------------------------------------------
DEEPSEEK_BALANCE_URL = "https://api.deepseek.com/user/balance"


def deepseek_key(spec):
    if spec.get("apiKey"):
        return str(spec["apiKey"]).strip()
    env_name = spec.get("apiKeyEnv", "DEEPSEEK_API_KEY")
    if os.environ.get(env_name):
        return os.environ[env_name].strip()
    if spec.get("apiKeyFile"):
        with open(os.path.expanduser(spec["apiKeyFile"]), encoding="utf-8") as handle:
            return handle.read().strip()
    raise ValueError(f"no DeepSeek API key (set {env_name}, apiKeyFile or apiKey)")


def fetch_deepseek_balance(spec, timeout):
    request = urllib.request.Request(
        spec.get("url", DEEPSEEK_BALANCE_URL),
        headers={
            "Authorization": "Bearer " + deepseek_key(spec),
            "Accept": "application/json",
            "User-Agent": "smalltv-usage-daemon",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def map_deepseek_balance(payload, spec, usage_source=None):
    if not isinstance(payload, dict):
        raise ValueError("expected a JSON object")
    infos = [item for item in (payload.get("balance_infos") or []) if isinstance(item, dict)]
    if not infos:
        raise ValueError("missing balance_infos")
    currency = spec.get("currency")
    info = next((item for item in infos if item.get("currency") == currency), infos[0]) if currency else infos[0]
    total = info.get("total_balance")
    if numeric(total) is None:
        raise ValueError("missing total_balance")
    metrics = [make_metric(
        spec.get("balanceLabel", "balance"), None, 0,
        f"{compact_number(total)} {info.get('currency', '')}".strip(),
    )]

    if usage_source is not None:
        tokens = path_get(usage_source, spec.get("usagePath", "total_tokens"))
        if numeric(tokens) is not None:
            budget = numeric(spec.get("usageBudget")) or 0
            pct = (numeric(tokens) * 100.0 / budget) if budget > 0 else 0
            metrics.append(make_metric(
                spec.get("usageLabel", "tokens"), pct, 0, compact_tokens(tokens),
            ))

    return {
        "id": str(spec["id"]),
        "name": str(spec.get("name", "DeepSeek")),
        "ok": True,
        "status": "available" if payload.get("is_available", True) else "low balance",
        "metrics": [metric for metric in metrics if metric],
    }


def collect_deepseek(spec, timeout):
    payload = fetch_deepseek_balance(spec, timeout)
    usage_source = None
    if spec.get("usageUrl"):
        try:
            usage_source = fetch_json(spec["usageUrl"], timeout, spec.get("usageHeaders"))
        except urllib.error.HTTPError as error:
            error.close()
            print(f"{spec.get('id', 'deepseek')}: token usage source: {error}", file=sys.stderr)
        except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as error:
            print(f"{spec.get('id', 'deepseek')}: token usage source: {error}", file=sys.stderr)
    return map_deepseek_balance(payload, spec, usage_source)


ADAPTERS = {
    "claude-legacy": claude_legacy,
    "generic": generic,
    "normalized": normalized,
}


def fetch_json(url, timeout, headers=None):
    request = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def collect(spec, timeout):
    adapter = spec.get("adapter", "generic")
    if adapter == "claude":
        return collect_claude(spec, timeout)
    if adapter == "codex":
        return collect_codex(spec, timeout)
    if adapter == "deepseek":
        return collect_deepseek(spec, timeout)
    function = ADAPTERS.get(adapter)
    if function is None:
        raise ValueError(f"unknown adapter {adapter}")
    return function(fetch_json(spec["url"], timeout, spec.get("headers")), spec)


def build_payload(config, timeout):
    providers = []
    for spec in config["providers"]:
        try:
            providers.append(collect(spec, timeout))
        except urllib.error.HTTPError as error:
            error.close()
            print(f"{spec.get('id', '?')}: {error}", file=sys.stderr)
            providers.append(provider_error(spec))
        except (OSError, ValueError, TypeError, AttributeError, KeyError,
                urllib.error.URLError, json.JSONDecodeError) as error:
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
        if not isinstance(spec, dict) or not spec.get("id"):
            raise ValueError("every provider needs an id")
        adapter = spec.get("adapter", "generic")
        if adapter not in ADAPTERS and adapter not in ("claude", "codex", "deepseek"):
            raise ValueError(f"{spec['id']}: unknown adapter {adapter}")
        if adapter in ADAPTERS and not spec.get("url"):
            raise ValueError(f"{spec['id']}: {adapter} providers need a url")
        if adapter == "generic":
            metrics = spec.get("metrics")
            if not isinstance(metrics, list) or not metrics:
                raise ValueError(f"{spec['id']}: generic providers need metrics")
        if adapter == "deepseek":
            # The key is checked from the environment/file at collect time.
            if not (spec.get("apiKey") or spec.get("apiKeyEnv") or spec.get("apiKeyFile")):
                spec["apiKeyEnv"] = "DEEPSEEK_API_KEY"
        if adapter == "codex":
            spec.setdefault("authFile", "~/.codex/auth.json")
        if adapter == "claude":
            spec.setdefault("authFile", CLAUDE_DEFAULT_CREDENTIALS)


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
