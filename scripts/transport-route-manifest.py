#!/usr/bin/env python3
"""Generate or verify the frozen oat++ HTTP route manifest."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys


ENDPOINT_PATTERN = re.compile(
    r'\bENDPOINT\s*\(\s*"(?P<method>[A-Z]+)"\s*,\s*"(?P<path>[^"]+)"\s*,\s*'
    r"(?P<handler>[A-Za-z_][A-Za-z0-9_]*)",
    re.MULTILINE,
)


def collect_routes(source_root: pathlib.Path) -> list[dict[str, object]]:
    controller_root = source_root / "src/server/ws/controller"
    routes: list[dict[str, object]] = []
    seen: dict[tuple[str, str], str] = {}

    for source in sorted(controller_root.glob("*.h")):
        contents = source.read_text(encoding="utf-8")
        for match in ENDPOINT_PATTERN.finditer(contents):
            method = match.group("method")
            path = "/" + match.group("path").lstrip("/")
            handler = match.group("handler")
            line = contents.count("\n", 0, match.start()) + 1
            relative_source = source.relative_to(source_root).as_posix()
            route_key = (method, path)
            if route_key in seen:
                raise ValueError(
                    f"duplicate route {method} {path}: {seen[route_key]} and "
                    f"{relative_source}:{line}"
                )
            seen[route_key] = f"{relative_source}:{line}"
            routes.append(
                {
                    "method": method,
                    "path": path,
                    "handler": handler,
                    "controller": source.stem,
                    "source": relative_source,
                }
            )

    routes.sort(key=lambda route: (str(route["path"]), str(route["method"])))
    return routes


def render_manifest(source_root: pathlib.Path) -> str:
    routes = collect_routes(source_root)
    document = {
        "schema_version": 1,
        "source_framework": "oatpp",
        "explicit_route_count": len(routes),
        "routes": routes,
    }
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
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
