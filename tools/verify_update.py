#!/usr/bin/env python3
"""Verify the GitHub release contract used by the firmware self-updater.

Checks the release metadata, asset names, public download URLs, SHA-256 hashes,
embedded firmware versions, and the version ordering that decides whether a
device should update. With --local-only it checks the staged dist/ directory
without contacting GitHub.

Examples:
  python3 tools/verify_update.py --local-only --from-version 4.4.2-custom
  python3 tools/verify_update.py --tag v4.5.2-custom --from-version 4.4.2-custom
  python3 tools/verify_update.py --latest --from-version 4.4.2-custom
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REPO = "JPHultra/smalltv-mod-custom"
USER_AGENT = "smalltv-mod-update-verifier"


def fail(message: str) -> None:
    print("FAIL:", message, file=sys.stderr)
    raise SystemExit(1)


def read_config() -> tuple[str, list[str]]:
    text = (ROOT / "src" / "config.h").read_text(encoding="utf-8")
    version = re.search(r'#\s*define\s+FW_VERSION\s+"([^"]+)"', text)
    assets = re.findall(r'#\s*define\s+UPDATE_ASSET\s+"([^"]+)"', text)
    if not version or not assets:
        fail("FW_VERSION or UPDATE_ASSET missing from src/config.h")
    return version.group(1), assets


def parse_version(text: str) -> tuple[int, int, int]:
    match = re.search(r"(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        fail(f"cannot parse version: {text}")
    return tuple(int(part) for part in match.groups())


def local_assets(dist: Path) -> dict[str, Path]:
    if not dist.is_dir():
        fail(f"missing local release directory: {dist}")
    files = {path.name: path for path in dist.iterdir() if path.is_file()}
    if not files:
        fail(f"no release assets in {dist}")
    return files


def api_json(url: str) -> dict:
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "application/vnd.github+json",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except Exception as error:
        fail(f"GitHub API request failed: {url}: {error}")


def download(url: str, destination: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            destination.write_bytes(response.read())
    except Exception as error:
        fail(f"asset download failed: {url}: {error}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_firmware_version(path: Path, version: str) -> None:
    name = path.name
    if "firmware" not in name or "loader" in name:
        return
    if version.encode() not in path.read_bytes():
        fail(f"{name}: embedded firmware version {version} not found")


def check_update_order(version: str, from_version: str | None) -> None:
    if not from_version:
        return
    current = parse_version(from_version)
    available = parse_version(version)
    if available <= current:
        fail(f"release {version} is not newer than {from_version}")
    print(f"update eligibility: {from_version} -> {version} is eligible")


def check_local(dist: Path, version: str, required: list[str],
                from_version: str | None) -> dict[str, Path]:
    assets = local_assets(dist)
    for name in required:
        if name not in assets:
            fail(f"local dist is missing UPDATE_ASSET: {name}")
    for name, path in assets.items():
        check_firmware_version(path, version)
    print(f"local assets: {len(assets)}")
    print(f"firmware version: {version}")
    print("required UPDATE_ASSET files: " + ", ".join(required))
    check_update_order(version, from_version)
    return assets


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--tag", help="release tag to verify (v4.5.2-custom)")
    parser.add_argument("--latest", action="store_true",
                        help="verify the published latest release")
    parser.add_argument("--local-only", action="store_true",
                        help="verify dist/ without contacting GitHub")
    parser.add_argument("--dist", default="dist", help="local release directory")
    parser.add_argument("--from-version",
                        help="assert the release is newer than this version")
    args = parser.parse_args()

    version, required = read_config()
    dist = Path(args.dist)
    if not dist.is_absolute():
        dist = ROOT / dist
    local = check_local(dist, version, required, args.from_version)

    if args.local_only:
        print("local verification passed")
        return

    tag = args.tag
    if tag and not tag.startswith("v"):
        tag = "v" + tag

    if tag:
        release = api_json(f"https://api.github.com/repos/{args.repo}/releases/tags/{tag}")
    else:
        release = api_json(f"https://api.github.com/repos/{args.repo}/releases/latest")

    if release.get("draft"):
        fail("release is still a draft")
    if args.latest:
        latest = api_json(f"https://api.github.com/repos/{args.repo}/releases/latest")
        if release.get("prerelease"):
            fail("release is still a prerelease")
        if tag and latest.get("tag_name") != tag:
            fail(f"latest release is {latest.get('tag_name')}, expected {tag}")
        release = latest

    release_tag = release.get("tag_name", "")
    if args.tag and release_tag != tag:
        fail(f"release tag is {release_tag}, expected {tag}")
    if parse_version(release_tag) != parse_version(version):
        fail(f"release tag {release_tag} does not match firmware {version}")

    remote = {asset["name"]: asset for asset in release.get("assets", [])}
    missing = sorted(set(local) - set(remote))
    extra = sorted(set(remote) - set(local))
    if missing:
        fail("release is missing local assets: " + ", ".join(missing))
    if extra:
        fail("release has unexpected assets: " + ", ".join(extra))
    for name in required:
        if name not in remote:
            fail(f"release is missing UPDATE_ASSET: {name}")

    with tempfile.TemporaryDirectory(prefix="smalltv-update-verify-") as temporary:
        temporary = Path(temporary)
        for name, local_path in sorted(local.items()):
            remote_path = temporary / name
            download(remote[name]["browser_download_url"], remote_path)
            check_firmware_version(remote_path, version)
            if sha256(local_path) != sha256(remote_path):
                fail(f"checksum mismatch: {name}")
            print(f"verified {name} ({local_path.stat().st_size} bytes)")

    check_update_order(version, args.from_version)
    print(f"remote release {release_tag} verification passed")


if __name__ == "__main__":
    main()
