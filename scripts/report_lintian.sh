#!/usr/bin/env bash
#
# Run lintian on the built Debian package(s) and REPORT what it finds.
#
# This is deliberately report-only: it always exits 0, even when lintian
# reports errors. April's ruling (2026-11-12) was to adopt lintian without a
# failing gate first, because a gate switched on before anyone has read the
# output is a gate that gets disabled within a day. Per-tag fix-versus-override
# decisions are hers, and tightening this into a real gate is a later, separate
# choice.
#
# lintian runs inside Debian rather than on the host: the GitHub runners are
# Ubuntu, and Debian tooling belongs on Debian. That is the same reason the
# package itself is built in a debian:trixie container.
#
# Usage:
#   scripts/report_lintian.sh [directory-of-debs]
#
# Defaults to out/debs (where the documented local Docker build drops them).
# CI passes its own directory, since the workflow copies the .deb out of the
# container image into package/.

set -uo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
package_dir=${1:-"$repo_root/out/debs"}

if [ ! -d "$package_dir" ]; then
    echo "No such directory: $package_dir" >&2
    echo "Build a package first, or pass the directory holding the .deb files." >&2
    exit 0
fi

package_dir=$(cd -- "$package_dir" && pwd)

shopt -s nullglob
packages=("$package_dir"/*.deb)
if [ "${#packages[@]}" -eq 0 ]; then
    echo "No .deb files in $package_dir; nothing to check." >&2
    exit 0
fi

echo "Running lintian on ${#packages[@]} package(s) in $package_dir"

# --display-info and --tag-display-limit 0 so nothing is hidden: the whole
# point of the first runs is seeing the complete list. (--no-tag-display-limit
# is deprecated.) lintian refuses to run as root, hence runuser.
output_file=$(mktemp)
trap 'rm -f "$output_file"' EXIT

docker run --rm -v "$package_dir:/package:ro" debian:trixie sh -c \
    'apt-get update -qq >/dev/null 2>&1 &&
     apt-get install -y -qq --no-install-recommends lintian >/dev/null 2>&1 &&
     lintian --version &&
     runuser -u nobody -- lintian --display-info --tag-display-limit 0 /package/*.deb' \
    2>&1 | tee "$output_file"

# Count real tag lines (E:/W:/I:/P: followed by a package name), not the
# version banner or apt noise.
findings=$(grep -cE '^[EWIP]: ' "$output_file" || true)

if [ "$findings" -gt 0 ]; then
    echo
    echo "lintian reported $findings finding(s). This step is report-only and does not fail the build."
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::warning title=lintian::$findings finding(s) in the Debian package; see this step's log. Report-only (issue #266)."
    fi
else
    echo
    echo "lintian reported nothing."
fi

exit 0
