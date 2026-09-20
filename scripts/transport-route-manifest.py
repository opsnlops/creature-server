#!/usr/bin/env python3
"""Generate or verify the frozen HTTP route manifest.

The manifest (docs/transport-route-manifest.json) is the contract for the
server's public route surface: it feeds /api/openapi.json and the API browser,
and CMake's --check target fails the build when the routes registered in
UWebSocketsServer.cpp drift from it. Regenerate with --write only when a route
is deliberately added or removed, and review the manifest diff.

HEAD registrations and trailing-slash aliases are not part of the manifest.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REGISTRATION_PATTERN = re.compile(
    r"run(?:Bodyless|Body)Route\(\s*response,\s*request,\s*"
    r'"(?P<method>[A-Z]+)",\s*"(?P<path>[^"]+)",\s*"(?P<handler>[A-Za-z0-9_]+)",\s*'
    r'"(?P<controller>[A-Za-z0-9_]+)"',
    re.MULTILINE,
)
WEBSOCKET_PATTERN = re.compile(r'\.ws<[A-Za-z_]+>\(\s*"(?P<path>[^"]+)"', re.MULTILINE)
SERVER_SOURCE = "src/server/transport/UWebSocketsServer.cpp"


def collect_routes(source_root: pathlib.Path) -> list[dict[str, object]]:
    source = source_root / SERVER_SOURCE
    contents = source.read_text(encoding="utf-8")
    routes: list[dict[str, object]] = []
    seen: dict[tuple[str, str], int] = {}
    for match in REGISTRATION_PATTERN.finditer(contents):
        method = match.group("method")
        if method == "HEAD":
            continue
        path = match.group("path")
        line = contents.count("\n", 0, match.start()) + 1
        route_key = (method, path)
        if route_key in seen:
            raise ValueError(f"duplicate route {method} {path}: lines {seen[route_key]} and {line}")
        seen[route_key] = line
        routes.append(
            {
                "method": method,
                "path": path,
                "handler": match.group("handler"),
                "controller": match.group("controller"),
                "source": SERVER_SOURCE,
            }
        )
    for match in WEBSOCKET_PATTERN.finditer(contents):
        routes.append(
            {
                "method": "GET",
                "path": match.group("path"),
                "handler": "websocket",
                "controller": "WebSocketController",
                "source": SERVER_SOURCE,
            }
        )
    # A trailing-slash alias of another registered route (for example
    # /api/docs/ beside /api/docs) is a convenience, not a contract entry.
    registered = {(str(route["method"]), str(route["path"])) for route in routes}
    routes = [
        route
        for route in routes
        if not (str(route["path"]).endswith("/") and len(str(route["path"])) > 1
                and (str(route["method"]), str(route["path"])[:-1]) in registered)
    ]
    routes.sort(key=lambda route: (str(route["path"]), str(route["method"])))
    return routes


def render_manifest(source_root: pathlib.Path) -> str:
    routes = collect_routes(source_root)
    document = {
        "description": "Frozen public HTTP and WebSocket route surface served by UWebSocketsServer.cpp.",
        "route_count": len(routes),
        "routes": routes,
    }
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--source-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path("docs/transport-route-manifest.json"),
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    source_root = arguments.source_root.resolve()
    manifest = arguments.manifest
    if not manifest.is_absolute():
        manifest = source_root / manifest
    try:
        rendered = render_manifest(source_root)
    except (OSError, ValueError) as error:
        print(f"route manifest generation failed: {error}", file=sys.stderr)
        return 1
    if arguments.write:
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(rendered, encoding="utf-8")
        print(f"wrote {manifest}")
        return 0
    try:
        existing = manifest.read_text(encoding="utf-8")
    except FileNotFoundError:
        print(f"route manifest is missing: {manifest}", file=sys.stderr)
        print("regenerate it with --write", file=sys.stderr)
        return 1
    if existing != rendered:
        print(f"route manifest is stale: {manifest}", file=sys.stderr)
        print("regenerate it with --write and review the route changes", file=sys.stderr)
        return 1
    print(f"route manifest is current ({len(collect_routes(source_root))} routes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
