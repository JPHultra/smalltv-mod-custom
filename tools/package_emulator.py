#!/usr/bin/env python3
"""Stage the SmallTV desktop packages in dist/ for a GitHub release.

The script always builds the Linux package and optionally adds the Windows
package when a cross-built executable and SDL2 DLL are supplied. The staging
tree includes the bundled Inter fonts and every third-party license needed to
run the app, so the package does not depend on files elsewhere in the repo.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import subprocess
import tarfile
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINUX_NAME = "smalltv-emulator-linux-x86_64"
WINDOWS_NAME = "smalltv-emulator-windows-x86_64"

COMMON_FILES = [
    ("emulator/README.md", "README.md"),
    ("LICENSE", "LICENSE"),
    ("FONT-LICENSE.txt", "FONT-LICENSE.txt"),
    ("emulator/assets/fonts/OFL.txt", "INTER-LICENSE.txt"),
    ("emulator/third_party/httplib-LICENSE.txt", "HTTPLIB-LICENSE.txt"),
    ("emulator/third_party/imgui/LICENSE.txt", "IMGUI-LICENSE.txt"),
    ("emulator/third_party/SDL2-LICENSE.txt", "SDL2-LICENSE.txt"),
    ("emulator/third_party/STB-LICENSE.txt", "STB-LICENSE.txt"),
    ("examples/view.json", "examples/view.json"),
    ("daemon/usage.example.json", "usage.example.json"),
]

LINUX_RUN = """#!/bin/sh
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$DIR"
exec "$DIR/smalltv-emulator" --view "$DIR/examples/view.json" --usage "$DIR/usage.example.json" "$@"
"""

WINDOWS_README = """SmallTV native desktop app for Windows.
Launch smalltv-emulator.exe directly. Keep SDL2.dll and the assets folder beside
the executable. No installation is required. Use Scan or enter the device URL
and press Connect.
"""


def version_from_config() -> str:
    text = (ROOT / "src" / "config.h").read_text(encoding="utf-8")
    match = re.search(r'#\s*define\s+FW_VERSION\s+"([^"]+)"', text)
    if not match:
        raise SystemExit("FW_VERSION not found in src/config.h")
    return match.group(1)


def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise SystemExit(f"missing package input: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def stage_common(package: Path, version: str) -> None:
    for source, target in COMMON_FILES:
        copy_file(ROOT / source, package / target)

    assets = ROOT / "emulator" / "assets"
    for source in sorted(assets.rglob("*")):
        if source.is_file():
            copy_file(source, package / "assets" / source.relative_to(assets))

    (package / "VERSION.txt").write_text(version + "\n", encoding="utf-8")


def stage_linux(package: Path, binary: Path) -> None:
    copy_file(binary, package / "smalltv-emulator")
    executable = package / "smalltv-emulator"
    os.chmod(executable, executable.stat().st_mode | stat.S_IXUSR |
             stat.S_IXGRP | stat.S_IXOTH)
    strip = shutil.which("strip")
    if not strip:
        raise SystemExit("strip not found; cannot build a release-sized Linux package")
    subprocess.run([strip, "--strip-unneeded", str(executable)], check=True)
    run = package / "run.sh"
    run.write_text(LINUX_RUN, encoding="utf-8")
    os.chmod(run, 0o755)


def stage_windows(package: Path, binary: Path, sdl2_dll: Path) -> None:
    copy_file(binary, package / "smalltv-emulator.exe")
    copy_file(sdl2_dll, package / "SDL2.dll")
    (package / "README-WINDOWS.txt").write_text(WINDOWS_README, encoding="utf-8")


def archive_linux(package: Path, destination: Path) -> None:
    destination.unlink(missing_ok=True)
    with tarfile.open(destination, "w:gz") as archive:
        archive.add(package, arcname=LINUX_NAME)


def archive_windows(package: Path, destination: Path) -> None:
    destination.unlink(missing_ok=True)
    with zipfile.ZipFile(destination, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(package.rglob("*")):
            if path.is_file():
                archive.write(path, (Path(WINDOWS_NAME) / path.relative_to(package)).as_posix())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", default="dist", help="release staging directory")
    parser.add_argument("--version", help="package version (default: FW_VERSION)")
    parser.add_argument("--linux-bin", default="emulator/build/smalltv-emulator",
                        help="Linux executable to package")
    parser.add_argument("--windows-bin", help="cross-built Windows executable")
    parser.add_argument("--sdl2-dll", help="SDL2.dll to include with Windows")
    args = parser.parse_args()

    if bool(args.windows_bin) != bool(args.sdl2_dll):
        parser.error("--windows-bin and --sdl2-dll must be supplied together")

    version = args.version or version_from_config()
    dist = Path(args.dist)
    if not dist.is_absolute():
        dist = ROOT / dist
    dist.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="smalltv-package-") as temporary:
        temporary = Path(temporary)

        linux = temporary / LINUX_NAME
        stage_common(linux, version)
        stage_linux(linux, Path(args.linux_bin))
        archive_linux(linux, dist / f"{LINUX_NAME}.tar.gz")

        if args.windows_bin:
            windows = temporary / WINDOWS_NAME
            stage_common(windows, version)
            stage_windows(windows, Path(args.windows_bin), Path(args.sdl2_dll))
            archive_windows(windows, dist / f"{WINDOWS_NAME}.zip")

    print(f"packaged SmallTV emulator {version}")
    for pattern in (f"{LINUX_NAME}.tar.gz", f"{WINDOWS_NAME}.zip"):
        path = dist / pattern
        if path.exists():
            print(f"  {path.name:48} {path.stat().st_size:>9} B")


if __name__ == "__main__":
    main()
