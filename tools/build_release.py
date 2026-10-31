#!/usr/bin/env python3
"""Build release images with the fewest possible framework rebuilds.

The pioarduino ESP32 platform reinstalls and rebuilds the Arduino/IDF framework
when the MCU or the custom_sdkconfig state changes. Building envs in an
arbitrary order therefore pays that cost repeatedly. This script orders the
envs by shared framework state, runs each target, stages the release asset
immediately (so a later env cannot remove it), and runs the same 8 KB slot gate
as the local release gate.

Usage:
  python3 tools/build_release.py
  python3 tools/build_release.py --envs smalltv,smalltv_c2
  python3 tools/build_release.py --pio /path/to/pio --jobs 8
  python3 tools/build_release.py --clean
  python3 tools/build_release.py --dry-run
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Order matters: group by the pioarduino framework configuration. Only a group
# boundary should trigger a framework rebuild.
GROUPS = [
    ("ESP8266", ["smalltv", "smalltv_loader"]),
    ("ESP32-C2, custom SDKConfig", ["smalltv_c2"]),
    ("classic ESP32, custom SDKConfig", ["smalltv_esp32"]),
    ("classic ESP32, stock SDKConfig", ["smalltv_esp32_8mb"]),
]

# env -> [(file in .pio/build/<env>, release asset name)]
ASSETS = {
    "smalltv": [("firmware.bin", "smalltv-mod-firmware.bin")],
    "smalltv_c2": [
        ("firmware.bin", "smalltv-mod-firmware-c2.bin"),
        ("firmware.factory.bin", "smalltv-mod-firmware-c2.factory.bin"),
    ],
    "smalltv_esp32": [
        ("firmware.bin", "smalltv-mod-firmware-esp32.bin"),
        ("firmware.factory.bin", "smalltv-mod-firmware-esp32.factory.bin"),
    ],
    "smalltv_esp32_8mb": [
        ("firmware.bin", "smalltv-mod-firmware-esp32-pro.bin"),
        ("firmware.factory.bin", "smalltv-mod-firmware-esp32-pro.factory.bin"),
    ],
    "smalltv_loader": [("firmware.bin", "smalltv-mod-loader.bin")],
}

# The loader has no OTA slot to measure.
SIZE_ENVS = {"smalltv", "smalltv_c2", "smalltv_esp32", "smalltv_esp32_8mb"}

CONSTRAINTS = "platformio==6.1.19\npioarduino==6.1.19\n"


def run(command, env, dry_run):
    print("+", " ".join(str(part) for part in command))
    if dry_run:
        return
    subprocess.run(command, cwd=ROOT, env=env, check=True)


def stage(env_name, dist):
    for source_name, asset_name in ASSETS[env_name]:
        source = ROOT / ".pio" / "build" / env_name / source_name
        if not source.is_file():
            raise SystemExit(f"missing build output: {source}")
        shutil.copy2(source, dist / asset_name)
        print(f"  staged {asset_name}")


def check_pio(pio):
    if os.path.sep in pio or (os.altsep and os.altsep in pio):
        return Path(pio).is_file()
    return shutil.which(pio) is not None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--envs", help="comma-separated subset (default: every release env)")
    parser.add_argument("--pio", default="pio", help="PlatformIO executable (default: pio)")
    parser.add_argument("--jobs", type=int, default=max(1, os.cpu_count() or 4),
                        help="parallel compile jobs (default: CPU count)")
    parser.add_argument("--dist", default="dist", help="release staging directory (default: dist)")
    parser.add_argument("--clean", action="store_true", help="remove .pio/build before building")
    parser.add_argument("--no-size", action="store_true", help="skip the 8 KB slot gate")
    parser.add_argument("--no-constraints", action="store_true",
                        help="do not pin platformio/pioarduino to 6.1.19")
    parser.add_argument("--dry-run", action="store_true", help="print the plan, build nothing")
    args = parser.parse_args()

    all_envs = [env for _, group in GROUPS for env in group]
    if args.envs:
        wanted = [item.strip() for item in args.envs.split(",") if item.strip()]
        unknown = [item for item in wanted if item not in ASSETS]
        if unknown:
            parser.error("unknown env: " + ", ".join(unknown))
    else:
        wanted = all_envs

    if not check_pio(args.pio):
        parser.exit(1, f"PlatformIO not found: {args.pio}\n"
                       "Install it with: pip install platformio==6.1.19\n")
    if not args.dry_run:
        try:
            version = subprocess.run([args.pio, "--version"], cwd=ROOT, capture_output=True,
                                     text=True, check=True).stdout.strip()
            print(version)
            if "6.1.19" not in version:
                print("warning: the project is pinned to PlatformIO 6.1.19; "
                      "other versions may select the broken SCons.", file=sys.stderr)
        except (OSError, subprocess.CalledProcessError):
            parser.exit(1, f"cannot run {args.pio} --version")

    dist = Path(args.dist)
    if not dist.is_absolute():
        dist = ROOT / dist
    if args.clean and not args.dry_run:
        build = ROOT / ".pio" / "build"
        shutil.rmtree(build, ignore_errors=True)
        print(f"cleaned {build}")
    if not args.dry_run:
        if dist.exists():
            shutil.rmtree(dist)
        dist.mkdir(parents=True)

    env = os.environ.copy()
    constraint_path = None
    if not args.no_constraints:
        handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
        handle.write(CONSTRAINTS)
        handle.close()
        constraint_path = handle.name
        env["PIP_CONSTRAINT"] = constraint_path
        env["UV_CONSTRAINT"] = constraint_path

    started = time.time()
    try:
        run([sys.executable, ROOT / "tools" / "ci_checks.py", "assets"], env, args.dry_run)
        for label, group in GROUPS:
            pending = [name for name in group if name in wanted]
            if not pending:
                continue
            print(f"\n== {label} ==")
            for name in pending:
                run([args.pio, "run", "-e", name, "--jobs", str(args.jobs)], env, args.dry_run)
                if not args.no_size and name in SIZE_ENVS:
                    run([sys.executable, ROOT / "tools" / "ci_checks.py", "size",
                         "--pio", args.pio, "--min-free", "8192", "--envs", name],
                        env, args.dry_run)
                if not args.dry_run:
                    stage(name, dist)
    finally:
        if constraint_path:
            try:
                os.unlink(constraint_path)
            except OSError:
                pass

    elapsed = time.time() - started
    print(f"\nDone in {int(elapsed // 60)}m {int(elapsed % 60)}s")
    if not args.dry_run:
        for path in sorted(dist.iterdir()):
            print(f"  {path.name:48} {path.stat().st_size:>9} B")


if __name__ == "__main__":
    main()
