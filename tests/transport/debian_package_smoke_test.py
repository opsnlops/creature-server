#!/usr/bin/env python3
"""Smoke-test the exact Creature Server Debian package payload."""

from __future__ import annotations

import argparse
import glob
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, help="path or glob for exactly one .deb")
    parser.add_argument("--network-device", default="lo")
    arguments = parser.parse_args()

    matches = [Path(path).resolve() for path in glob.glob(arguments.package)]
    require(len(matches) == 1, f"expected exactly one package, found {len(matches)}: {matches}")
    package = matches[0]

    with tempfile.TemporaryDirectory(prefix="creature-server-package-gate-") as temporary:
        root = Path(temporary)
        subprocess.run(["dpkg-deb", "--extract", str(package), str(root)], check=True)

        executable_candidates = (
            root / "usr/bin/creature-server",
            root / "bin/creature-server",
        )
        executable = next((candidate for candidate in executable_candidates if candidate.is_file()), None)
        require(executable is not None, "package does not contain /usr/bin/creature-server or /bin/creature-server")
        assert executable is not None
        require((executable.stat().st_mode & 0o111) != 0, "packaged creature-server is not executable")

        notices = root / "usr/share/doc/creature-server"
        require((notices / "uWebSockets.LICENSE").is_file(), "uWebSockets license notice is missing")
        require((notices / "uSockets.LICENSE").is_file(), "uSockets license notice is missing")

        links = subprocess.run(
            ["ldd", str(executable)], check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        ).stdout
        require("not found" not in links, f"packaged binary has unresolved runtime libraries:\n{links}")
        require("/build/creature-server" not in links, f"packaged binary links into the build tree:\n{links}")

        production_gate = Path(__file__).with_name("uwebsockets_production_gate_test.py")
        packaged_share = root / "usr/share/creature-server"
        installed_share = Path("/usr/share/creature-server")
        require(packaged_share.is_dir(), "package does not contain its runtime share directory")
        require(not installed_share.exists(), f"package gate refuses to replace existing {installed_share}")
        installed_share.parent.mkdir(parents=True, exist_ok=True)
        installed_share.symlink_to(packaged_share, target_is_directory=True)
        try:
            subprocess.run(
                [
                    sys.executable,
                    str(production_gate),
                    "--server",
                    str(executable),
                    "--network-device",
                    arguments.network_device,
                ],
                check=True,
            )
        finally:
            installed_share.unlink()

    architecture = subprocess.run(
        ["dpkg-deb", "--field", str(package), "Architecture"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()
    version = subprocess.run(
        ["dpkg-deb", "--field", str(package), "Version"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout.strip()
    print(f"PASS: deployable Debian package {package.name} ({architecture}, version {version})")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
