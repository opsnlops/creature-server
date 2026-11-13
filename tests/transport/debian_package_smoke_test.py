#!/usr/bin/env python3
"""Smoke-test the exact Creature Server Debian package payload."""

from __future__ import annotations

import argparse
import glob
import gzip
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def strip_roff(page: str) -> str:
    """Reduce roff markup to the words a reader actually sees.

    help2man escapes every hyphen as \\- and wraps option names in font
    switches, so "--voice-api-key" appears in the body as
    \\fB\\-\\-voice\\-api\\-key\\fR. Searching the raw page for a plain option name
    therefore only ever matches the SYNOPSIS, which is a much weaker check
    than it looks.
    """
    plain = re.sub(r"\\f(?:\(CW|[BIRP])", "", page)
    plain = plain.replace("\\,", "").replace("\\/", "")
    return plain.replace("\\-", "-")


def check_manual_page(root: Path) -> None:
    """Check the packaged manual page against the options the parser defines.

    Debian Policy 12.1 wants a manual page for every program, and the fleet
    packaging policy wants it to cover every option plus the on-disk
    configuration. The options half is generated from --help by help2man at
    build time, so the interesting failures are not "someone forgot to write
    it" but the quiet ones: a generation step that half worked, or a heading
    rewrite that stopped matching.
    """
    page_path = root / "usr/share/man/man8/creature-server.8.gz"
    require(page_path.is_file(), "package does not ship /usr/share/man/man8/creature-server.8.gz")
    page = gzip.decompress(page_path.read_bytes()).decode("utf-8")
    plain = strip_roff(page)

    # help2man files argparse's "Optional arguments:" as a .SS subsection of
    # DESCRIPTION, so the build rewrites it into a real OPTIONS section. If
    # argparse ever changes that wording the rewrite silently stops matching
    # and the page ships with its options buried -- this is what catches it.
    for heading in (".SH OPTIONS", ".SH CONFIGURATION", ".SH FILES", ".SH SERVICE"):
        require(heading in page, f"manual page is missing its {heading} section")

    # Every long option the parser defines has to be documented, in the body
    # and not merely in the synopsis. This is the whole reason the page is
    # generated rather than hand-written: there are three dozen options and a
    # hand-maintained list goes stale within a release.
    parser_source = Path(__file__).parents[2] / "src/server/config/CommandLine.cpp"
    require(parser_source.is_file(), f"cannot find the option definitions at {parser_source}")
    options = sorted(set(re.findall(r'"(--[a-z0-9-]+)"', parser_source.read_text())))
    require(len(options) > 20, f"only found {len(options)} options in {parser_source.name}; regex is wrong")

    body = plain[plain.index(".SH OPTIONS"):]
    undocumented = [option for option in options if option not in body]
    require(not undocumented, f"manual page does not document {len(undocumented)} option(s): {undocumented}")

    # help2man generates the page by RUNNING the binary, so the build
    # machine's environment reaches a file we then publish. These two options
    # print their current value as their default, so a builder with the keys
    # exported would bake a live credential into the package.
    for option in ("--voice-api-key", "--honeycomb-api-key"):
        match = re.search(re.escape(option) + r".{0,400}?\[default:(.*?)\]", body, re.DOTALL)
        require(match is not None, f"{option} has no documented default, so it cannot be checked for a leak")
        assert match is not None
        default = match.group(1).strip()
        require(
            default in ('""', "''"),
            f"{option} documents a non-empty default ({default!r}) -- a credential from the build "
            f"environment has leaked into the manual page",
        )

    print(f"PASS: manual page documents all {len(options)} options and leaks no credentials")


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

        check_manual_page(root)

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
