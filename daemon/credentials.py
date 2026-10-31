"""Runtime credential store for the usage daemon.

Credentials are entered through the local setup UI and saved on the daemon host
with mode 0600. Nothing here reads Claude/Codex auth files or environment
variables; those remain explicit legacy fallbacks in the adapters only.
"""
from __future__ import annotations

import copy
import json
import os
import tempfile
import threading

from fslock import file_lock

DEFAULT_CREDENTIALS_PATH = "~/.config/smalltv/credentials.json"

FIELDS = {
    "claude": ("accessToken", "refreshToken", "expiresAt", "clientId", "tokenUrl", "userAgent"),
    "codex": ("accessToken", "refreshToken", "accountId", "clientId", "tokenUrl", "userAgent"),
    "deepseek": ("apiKey",),
}
SECRET_FIELDS = {
    "claude": ("accessToken", "refreshToken"),
    "codex": ("accessToken", "refreshToken", "accountId"),
    "deepseek": ("apiKey",),
}
MAX_LENGTHS = {
    "accessToken": 8192,
    "refreshToken": 8192,
    "apiKey": 8192,
    "accountId": 512,
    "clientId": 128,
    "tokenUrl": 1024,
    "userAgent": 256,
}
MAX_EXPIRES_AT = 10**15


def credentials_path(path=None):
    return os.path.abspath(os.path.expanduser(
        path or os.environ.get("SMALLTV_CREDENTIALS") or DEFAULT_CREDENTIALS_PATH
    ))


def _clean_value(key, value):
    if key == "expiresAt":
        try:
            number = int(float(value))
        except (TypeError, ValueError, OverflowError):
            return None
        return number if 0 <= number <= MAX_EXPIRES_AT else None
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    limit = MAX_LENGTHS.get(key, 1024)
    return text[:limit]


class CredentialsStore:
    """A tiny atomic JSON store; status exposes booleans, never secret values."""

    def __init__(self, path=None):
        self.path = credentials_path(path)
        self._lock = threading.RLock()
        self.data = self._load()

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, ValueError, json.JSONDecodeError):
            return {}
        if not isinstance(data, dict):
            return {}
        clean = {}
        for provider, values in data.items():
            if provider not in FIELDS or not isinstance(values, dict):
                continue
            row = {}
            for key in FIELDS[provider]:
                value = _clean_value(key, values.get(key))
                if value is not None:
                    row[key] = value
            if row:
                clean[provider] = row
        return clean

    def _ensure_dir(self):
        os.makedirs(os.path.dirname(self.path) or ".", mode=0o700, exist_ok=True)

    def _write(self):
        directory = os.path.dirname(self.path) or "."
        os.makedirs(directory, mode=0o700, exist_ok=True)
        temporary = None
        try:
            fd, temporary = tempfile.mkstemp(prefix=".credentials-", dir=directory)
            with os.fdopen(fd, "w", encoding="utf-8") as handle:
                json.dump(self.data, handle, indent=2, sort_keys=True)
                handle.write("\n")
            os.chmod(temporary, 0o600)
            os.replace(temporary, self.path)
            temporary = None
        finally:
            if temporary:
                try:
                    os.unlink(temporary)
                except OSError:
                    pass

    def reload(self):
        with self._lock:
            self.data = self._load()
            return copy.deepcopy(self.data)

    def get(self, provider):
        with self._lock:
            self.data = self._load()
            return copy.deepcopy(self.data.get(provider, {}))

    def status(self, provider=None):
        with self._lock:
            self.data = self._load()
            providers = [provider] if provider in FIELDS else list(FIELDS)
            return {
                name: {
                    "configured": any(self.data.get(name, {}).get(key) for key in SECRET_FIELDS[name]),
                    "fields": {key: bool(self.data.get(name, {}).get(key)) for key in SECRET_FIELDS[name]},
                }
                for name in providers
            }

    def update(self, provider, values, replace=False):
        if provider not in FIELDS or not isinstance(values, dict):
            raise ValueError("unknown provider")
        if not any(key in FIELDS[provider] for key in values):
            raise ValueError("no credential fields supplied")
        self._ensure_dir()
        with self._lock, file_lock(self.path + ".lock"):
            self.data = self._load()
            row = {} if replace else dict(self.data.get(provider, {}))
            for key in FIELDS[provider]:
                if key not in values:
                    continue
                value = _clean_value(key, values.get(key))
                if value is None:
                    row.pop(key, None)
                else:
                    row[key] = value
            if row:
                self.data[provider] = row
            else:
                self.data.pop(provider, None)
            self._write()
        return self.status(provider)[provider]

    def clear(self, provider):
        if provider not in FIELDS:
            raise ValueError("unknown provider")
        self._ensure_dir()
        with self._lock, file_lock(self.path + ".lock"):
            self.data = self._load()
            self.data.pop(provider, None)
            self._write()
        return self.status(provider)[provider]
